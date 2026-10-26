#include "aaf_export.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <set>

#include "aaf_file.h"
#include "rpp_reader.h"
#include "util.h"

namespace dawp {
namespace {

const double kEps = 1e-9;

// Definitions (AAF baseline dictionary, plus the channel combiner used by Avid and Resolve)
const char* kDataDefSound = "01030202-0200-0000-060e-2b3404010101";
const char* kDataDefTimecode = "01030201-0100-0000-060e-2b3404010101";
const char* kDataDefDescriptive = "01030201-1000-0000-060e-2b3404010101";
const char* kOpMonoAudioGain = "9d2ea894-0968-11d3-8a38-0050040ef7d2";
const char* kOpMonoAudioPan = "9d2ea893-0968-11d3-8a38-0050040ef7d2";
const char* kOpMonoAudioDissolve = "0c3bea41-fc05-11d2-8a29-0050040ef7d2";
const char* kOpChannelCombiner = "6b46dd7a-132d-4856-ab21-8b751d8462ec";
const char* kOpMotionControl = "9d2ea890-0968-11d3-8a38-0050040ef7d2";  // VideoSpeedControl
const char* kParamLevel = "e4962320-2267-11d3-8a4c-0050040ef7d2";
const char* kParamAmplitude = "e4962321-2267-11d3-8a4c-0050040ef7d2";
const char* kParamPan = "e4962322-2267-11d3-8a4c-0050040ef7d2";
const char* kParamSpeedRatio = "72559a80-24d7-11d3-8a50-0050040ef7d2";
const char* kInterpLinear = "5b6c85a4-0ede-11d3-80a9-006008143e6f";
const char* kInterpConstant = "5b6c85a5-0ede-11d3-80a9-006008143e6f";
const char* kInterpPower = "c09153f7-bd18-4e5a-ad09-cbdd654fa001";
const char* kContainerAaf = "4313b571-d8ba-11d2-809b-006008143e6f";
const char* kTypeRational = "03010100-0000-0000-060e-2b3401040101";
const char* kUsageTopLevel = "0d010102-0101-0700-060e-2b3404010101";
const char* kOpEditProtocol = "0d011201-0100-0000-060e-2b3404010105";

uint32_t LE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t LE16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint64_t LE64(const uint8_t* p) { return LE32(p) | ((uint64_t)LE32(p + 4) << 32); }
uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
void PutLE32(std::vector<uint8_t>& v, uint32_t x)
{
  for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
void PutBE32(std::vector<uint8_t>& v, uint32_t x)
{
  for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (8 * i)));
}
void PutTag(std::vector<uint8_t>& v, const char* tag) { v.insert(v.end(), tag, tag + 4); }

double Extended80(const uint8_t* p)
{
  int exponent = ((p[0] & 0x7F) << 8) | p[1];
  uint64_t mantissa = 0;
  for (int i = 0; i < 8; i++) mantissa = (mantissa << 8) | p[2 + i];
  if (exponent == 0 && mantissa == 0) return 0;
  double v = std::ldexp((double)mantissa, exponent - 16383 - 63);
  return (p[0] & 0x80) ? -v : v;
}

bool ReadAt(FILE* f, int64_t pos, uint8_t* buf, size_t n) { return SeekFile(f, pos) && fread(buf, 1, n, f) == n; }

struct Media
{
  std::string path;
  bool exists = false;
  enum Kind
  {
    kOther,
    kWave,
    kAiff
  } kind = kOther;
  int sampleRate = 0, channels = 0, bits = 16;
  int64_t frames = 0;
  std::vector<uint8_t> summary;  // WAVE/AIFC descriptor summary: the header up to the sample data
  MobId master{}, file{};
  bool embed = false;
  int64_t neededFrames = 0;  // for files that could not be read: how much the clips use
};

// Reads the format of a WAV/AIFF file and builds the descriptor summary (a canonical header:
// the format chunk copied from the file, followed by the sample data chunk header).
bool ReadPcmHeader(Media& m)
{
  int64_t fileSize = FileSize(m.path);
  if (fileSize < 12) return false;
  FILE* f = OpenFile(m.path, "rb");
  if (!f) return false;
  uint8_t head[12];
  bool ok = false;
  if (ReadAt(f, 0, head, 12))
  {
    bool riff = (memcmp(head, "RIFF", 4) == 0 || memcmp(head, "RF64", 4) == 0) && memcmp(head + 8, "WAVE", 4) == 0;
    bool aiff = memcmp(head, "FORM", 4) == 0 && (memcmp(head + 8, "AIFF", 4) == 0 || memcmp(head + 8, "AIFC", 4) == 0);
    std::vector<uint8_t> fmt, fver;
    uint64_t dataSize = 0, ds64Data = 0;
    bool haveData = false;
    int64_t pos = 12;
    uint8_t ch[8];
    while ((riff || aiff) && pos + 8 <= fileSize && ReadAt(f, pos, ch, 8))
    {
      uint32_t size = riff ? LE32(ch + 4) : BE32(ch + 4);
      bool known = riff ? memcmp(ch, "fmt ", 4) == 0 : (memcmp(ch, "COMM", 4) == 0 || memcmp(ch, "FVER", 4) == 0);
      if (known && size < 4096)
      {
        std::vector<uint8_t>& dst = memcmp(ch, "FVER", 4) == 0 ? fver : fmt;
        dst.resize(size);
        if (!ReadAt(f, pos + 8, dst.data(), size)) break;
      }
      else if (riff && memcmp(ch, "ds64", 4) == 0 && size >= 16)
      {
        uint8_t d[16];
        if (!ReadAt(f, pos + 8, d, 16)) break;
        ds64Data = LE64(d + 8);
      }
      else if (memcmp(ch, riff ? "data" : "SSND", 4) == 0)
      {
        dataSize = (riff && size == 0xFFFFFFFF) ? ds64Data : size;
        if (aiff) dataSize = dataSize >= 8 ? dataSize - 8 : 0;  // offset + block size
        int64_t avail = fileSize - pos - (aiff ? 16 : 8);
        if ((int64_t)dataSize > avail) dataSize = avail > 0 ? avail : 0;
        haveData = true;
        if (!fmt.empty()) break;
      }
      if (size == 0xFFFFFFFF) break;
      pos += 8 + (int64_t)size + (size & 1);
    }
    if (riff && fmt.size() >= 16 && haveData)
    {
      m.kind = Media::kWave;
      m.channels = LE16(&fmt[2]);
      m.sampleRate = (int)LE32(&fmt[4]);
      int blockAlign = LE16(&fmt[12]);
      m.bits = LE16(&fmt[14]);
      if (blockAlign > 0 && m.sampleRate > 0 && m.channels > 0)
      {
        m.frames = (int64_t)(dataSize / blockAlign);
        std::vector<uint8_t>& s = m.summary;
        PutTag(s, "RIFF");
        PutLE32(s, (uint32_t)std::min<uint64_t>(4 + 8 + fmt.size() + (fmt.size() & 1) + 8 + dataSize, 0xFFFFFFFFu));
        PutTag(s, "WAVE");
        PutTag(s, "fmt ");
        PutLE32(s, (uint32_t)fmt.size());
        s.insert(s.end(), fmt.begin(), fmt.end());
        if (fmt.size() & 1) s.push_back(0);
        PutTag(s, "data");
        PutLE32(s, (uint32_t)std::min<uint64_t>(dataSize, 0xFFFFFFFFu));
        ok = true;
      }
    }
    else if (aiff && fmt.size() >= 18 && haveData)
    {
      m.kind = Media::kAiff;
      m.channels = BE16(&fmt[0]);
      m.frames = BE32(&fmt[2]);
      m.bits = BE16(&fmt[6]);
      m.sampleRate = (int)std::lround(Extended80(&fmt[8]));
      if (m.sampleRate > 0 && m.channels > 0)
      {
        bool aifc = memcmp(head + 8, "AIFC", 4) == 0;
        std::vector<uint8_t> body;
        PutTag(body, aifc ? "AIFC" : "AIFF");
        if (aifc && fver.size() == 4)
        {
          PutTag(body, "FVER");
          PutBE32(body, 4);
          body.insert(body.end(), fver.begin(), fver.end());
        }
        PutTag(body, "COMM");
        PutBE32(body, (uint32_t)fmt.size());
        body.insert(body.end(), fmt.begin(), fmt.end());
        if (fmt.size() & 1) body.push_back(0);
        PutTag(body, "SSND");
        PutBE32(body, (uint32_t)std::min<uint64_t>(dataSize + 8, 0xFFFFFFFFu));
        PutBE32(body, 0);
        PutBE32(body, 0);
        PutTag(m.summary, "FORM");
        PutBE32(m.summary, (uint32_t)std::min<uint64_t>(body.size() + dataSize, 0xFFFFFFFFu));
        m.summary.insert(m.summary.end(), body.begin(), body.end());
        ok = true;
      }
    }
  }
  fclose(f);
  return ok;
}

// file:// URL of an absolute path (percent-encoded UTF-8)
std::string FileUrl(const std::string& path)
{
  std::string p = path;
  std::replace(p.begin(), p.end(), '\\', '/');
  std::string prefix = "file://";
  if (p.compare(0, 2, "//") == 0) p = p.substr(2);  // UNC: file://server/share/...
  else if (p.empty() || p[0] != '/') prefix += "/";  // C:/... -> file:///C:/...
  static const char* hex = "0123456789ABCDEF";
  std::string out = prefix;
  for (unsigned char c : p)
  {
    if (isalnum(c) || strchr("-._~/:", c)) out += (char)c;
    else
    {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

struct EnvPoint
{
  double t, v;  // seconds, value
};

double EnvAt(const std::vector<EnvPoint>& pts, double t)
{
  if (pts.empty()) return 1;
  if (t <= pts.front().t) return pts.front().v;
  if (t >= pts.back().t) return pts.back().v;
  for (size_t i = 1; i < pts.size(); i++)
    if (t <= pts[i].t)
    {
      double d = pts[i].t - pts[i - 1].t;
      return d > kEps ? pts[i - 1].v + (pts[i].v - pts[i - 1].v) * (t - pts[i - 1].t) / d : pts[i].v;
    }
  return pts.back().v;
}

// The envelope between a and b, shifted to start at 0, with values at both ends.
std::vector<EnvPoint> SliceEnv(const std::vector<EnvPoint>& pts, double a, double b)
{
  std::vector<EnvPoint> out;
  if (pts.empty()) return out;
  out.push_back({0, EnvAt(pts, a)});
  for (const EnvPoint& p : pts)
    if (p.t > a + kEps && p.t < b - kEps) out.push_back({p.t - a, p.v});
  out.push_back({b - a, EnvAt(pts, b)});
  return out;
}

bool IsFlat(const std::vector<EnvPoint>& pts)
{
  for (const EnvPoint& p : pts)
    if (std::fabs(p.v - pts.front().v) > 1e-6) return false;
  return true;
}

struct ClipOut
{
  int64_t s = 0, e = 0;  // composition samples
  int media = -1;
  std::vector<int> channels;  // file channels, in output order
  double offset = 0;          // source position at the clip start (seconds)
  double rate = 1;
  double gain = 1;
  std::vector<EnvPoint> env;  // gain envelope, seconds from the clip start
  int64_t fadeIn = 0, fadeOut = 0;
  int fadeInShape = 0, fadeOutShape = 0;
  bool mute = false;
  // crossfade with the neighbouring clip in the same lane (overlap in samples)
  int64_t xfadePrev = 0, xfadeNext = 0;
  int64_t Len() const { return e - s; }
};

struct TrackIn
{
  std::string name;
  double vol = 1, pan = 0;
  bool mute = false, solo = false;
  std::vector<EnvPoint> volEnv, panEnv;  // pan envelope already in AAF units (0 = left, 1 = right)
  std::vector<ClipOut> clips;
};

class AafExporter
{
public:
  AafExporter(const AafExportOptions& options, AafExportReport& report) : options_(options), report_(report) {}
  bool Run(const std::string& text, const std::string& outPath, std::string& error);

private:
  void Warn(const std::string& w) { warnings_[w]++; }
  bool ReadEnvelope(const RppNode* env, std::vector<EnvPoint>& pts);
  std::string ResolvePath(const std::string& file) const;
  int GetMedia(const std::string& path);
  void ReadTrack(const RppNode& n, TrackIn& t);
  void ReadItem(const RppNode& item, TrackIn& t);
  std::vector<std::vector<ClipOut>> AssignLanes(std::vector<ClipOut> clips);

  // object building
  MobId NewMobId();
  Auid NewAuid();
  int Component(const char* cls, const char* dataDef, int64_t length);
  int Filler(int64_t length) { return Component("Filler", kDataDefSound, length); }
  int SourceClip(const MobId& mob, int slot, int64_t start, int64_t length, const char* dataDef = kDataDefSound);
  int Rational(double v, int32_t den);
  int Param(const char* def, double constant, const std::vector<EnvPoint>& pts, double length, const char* interp);
  int OpGroup(const char* op, int64_t length, std::vector<int> inputs, std::vector<int> params);
  int Transition(int64_t length, int shape);
  int ClipSegment(const ClipOut& c);
  int LaneSequence(std::vector<ClipOut>& lane, int64_t total);
  int TrackSlot(const TrackIn& t, std::vector<ClipOut>& lane, const std::string& name, int slotId, int number,
                int64_t total);
  void BuildDictionary(int header);
  void BuildMediaMobs(int content);
  int MarkerSlot(int64_t& total);

  const AafExportOptions& options_;
  AafExportReport& report_;
  std::map<std::string, int> warnings_;
  RppNode root_;
  AafWriter w_;
  int sampleRate_ = 48000;
  std::vector<Media> media_;
  std::map<std::string, int> mediaIndex_;
  std::mt19937_64 rng_{std::random_device()() ^ (uint64_t)time(nullptr)};
  int year_ = 2000, month_ = 1, day_ = 1, hour_ = 0, minute_ = 0, second_ = 0;
  struct Marker
  {
    double pos, len;
    std::string name;
  };
  std::vector<Marker> markers_;
};

MobId AafExporter::NewMobId()
{
  static const uint8_t label[12] = {0x06, 0x0a, 0x2b, 0x34, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x0f, 0x00};
  MobId id{};
  memcpy(id.data(), label, 12);
  id[12] = 0x13;
  for (int i = 16; i < 32; i += 8)
  {
    uint64_t r = rng_();
    memcpy(id.data() + i, &r, 8);
  }
  return id;
}

Auid AafExporter::NewAuid()
{
  Auid a{};
  for (int i = 0; i < 16; i += 8)
  {
    uint64_t r = rng_();
    memcpy(a.data() + i, &r, 8);
  }
  a[7] = (uint8_t)((a[7] & 0x0F) | 0x40);  // version 4 (Data3 is stored little-endian)
  a[8] = (uint8_t)((a[8] & 0x3F) | 0x80);
  return a;
}

// ---------------------------------------------------------------------------------------------
// Reading the project

bool AafExporter::ReadEnvelope(const RppNode* env, std::vector<EnvPoint>& pts)
{
  pts.clear();
  if (!env) return false;
  if (const RppNode* act = env->Child("ACT"))
    if (act->Int(1, 1) == 0) return false;
  if (env->Child("POOLEDENVINST")) Warn("Automation items were not exported");
  for (const RppNode* p : env->Children("PT"))
  {
    if (p->Int(3, 0) != 0) Warn("Envelope segment shapes were exported as linear");
    pts.push_back({p->Num(1), p->Num(2)});
  }
  std::stable_sort(pts.begin(), pts.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.t < b.t; });
  return !pts.empty();
}

std::string AafExporter::ResolvePath(const std::string& file) const
{
  if (file.empty() || IsAbsolutePath(file)) return NativeSeparators(file);
  for (const std::string& dir : options_.mediaDirs)
  {
    std::string p = NativeSeparators(JoinPath(dir, file));
    if (FileExists(p)) return p;
  }
  return NativeSeparators(JoinPath(options_.mediaDirs.empty() ? std::string(".") : options_.mediaDirs[0], file));
}

int AafExporter::GetMedia(const std::string& path)
{
  auto it = mediaIndex_.find(path);
  if (it != mediaIndex_.end()) return it->second;
  Media m;
  m.path = path;
  m.exists = FileExists(path);
  if (!ReadPcmHeader(m))
  {
    m.kind = Media::kOther;
    m.summary.clear();
    MediaInfo mi;
    if (m.exists && (ProbeAudioFile(path, mi) || (options_.probeMedia && options_.probeMedia(path, mi))) &&
        mi.sampleRate > 0)
    {
      m.sampleRate = mi.sampleRate;
      m.channels = std::max(1, mi.channels);
      m.frames = (int64_t)std::llround(mi.duration * mi.sampleRate);
      Warn("Not a WAV/AIFF file, referenced as PCM (other applications may not open it): " + FileName(path));
    }
    else
    {
      m.sampleRate = sampleRate_;
      m.channels = 2;
      if (!m.exists) Warn("Media file not found, referenced by path only: " + path);
      else Warn("Could not read the format of " + FileName(path) + " (assumed stereo)");
    }
  }
  m.embed = options_.embedMedia && m.exists && m.kind != Media::kOther;
  m.master = NewMobId();
  m.file = NewMobId();
  media_.push_back(m);
  mediaIndex_[path] = (int)media_.size() - 1;
  return (int)media_.size() - 1;
}

void AafExporter::ReadTrack(const RppNode& n, TrackIn& t)
{
  if (const RppNode* name = n.Child("NAME")) t.name = name->Token(1);
  if (const RppNode* v = n.Child("VOLPAN"))
  {
    t.vol = v->Num(1, 1);
    t.pan = std::max(-1.0, std::min(1.0, v->Num(2, 0)));
  }
  if (const RppNode* m = n.Child("MUTESOLO"))
  {
    t.mute = m->Int(1) != 0;
    t.solo = m->Int(2) != 0;
  }
  ReadEnvelope(n.Child("VOLENV2"), t.volEnv);
  // REAPER pan envelopes run from +1 (left) to -1 (right)
  if (ReadEnvelope(n.Child("PANENV2"), t.panEnv))
    for (EnvPoint& p : t.panEnv) p.v = (1.0 - std::max(-1.0, std::min(1.0, p.v))) / 2.0;
  std::vector<EnvPoint> unused;
  if (ReadEnvelope(n.Child("MUTEENV"), unused)) Warn("Mute envelopes were not exported");
  if (ReadEnvelope(n.Child("VOLENV"), unused)) Warn("Pre-FX volume envelopes were not exported");
  if (ReadEnvelope(n.Child("PANENV"), unused)) Warn("Pre-FX pan envelopes were not exported");
  if (ReadEnvelope(n.Child("WIDTHENV2"), unused)) Warn("Width envelopes were not exported");
  if (const RppNode* chain = n.Child("FXCHAIN"))
    for (const RppNode& c : chain->children)
      if (c.block && c.Name() != "FXCHAIN_REC")
      {
        Warn("Track FX were not exported (AAF carries no plug-ins)");
        break;
      }
  if (n.Child("AUXRECV")) Warn("Sends were not exported (AAF has no routing)");
  for (const RppNode* item : n.Children("ITEM")) ReadItem(*item, t);
}

void AafExporter::ReadItem(const RppNode& item, TrackIn& t)
{
  double pos = 0, len = 0, fadeIn = 0, fadeOut = 0, itemVol = 1;
  int fadeInShape = 0, fadeOutShape = 0;
  bool loop = false, mute = false;
  if (const RppNode* n = item.Child("POSITION")) pos = n->Num(1);
  if (const RppNode* n = item.Child("LENGTH")) len = n->Num(1);
  if (const RppNode* n = item.Child("LOOP")) loop = n->Int(1) != 0;
  if (const RppNode* n = item.Child("MUTE")) mute = n->Int(1) != 0;
  if (const RppNode* n = item.Child("FADEIN"))
  {
    fadeInShape = n->Int(1);
    fadeIn = n->Num(3) > 0 ? n->Num(3) : n->Num(2);
  }
  if (const RppNode* n = item.Child("FADEOUT"))
  {
    fadeOutShape = n->Int(1);
    fadeOut = n->Num(3) > 0 ? n->Num(3) : n->Num(2);
  }
  if (!(len > kEps)) return;

  struct Take
  {
    double soffs = 0, rate = 1, vol = 1, pan = 0;
    int chanMode = 0;
    const RppNode* source = nullptr;
    const RppNode* volEnv = nullptr;
    bool selected = false, otherEnv = false;
  };
  std::vector<Take> takes(1);
  for (const RppNode& c : item.children)
  {
    const std::string& name = c.Name();
    Take& tk = takes.back();
    if (name == "TAKE")
    {
      takes.emplace_back();
      takes.back().selected = c.Token(1) == "SEL";
    }
    else if (name == "VOLPAN")
    {
      itemVol = c.Num(1, 1);
      tk.pan = c.Num(2, 0);
      tk.vol = c.Num(3, 1);
    }
    else if (name == "TAKEVOLPAN")
    {
      tk.pan = c.Num(1, 0);
      tk.vol = c.Num(2, 1);
    }
    else if (name == "SOFFS") tk.soffs = c.Num(1);
    else if (name == "PLAYRATE") tk.rate = c.Num(1, 1);
    else if (name == "CHANMODE") tk.chanMode = c.Int(1);
    else if (c.block && name == "SOURCE") tk.source = &c;
    else if (c.block && name == "VOLENV") tk.volEnv = &c;
    else if (c.block && (name == "PANENV" || name == "MUTEENV" || name == "PITCHENV")) tk.otherEnv = true;
    else if (c.block && name == "TAKEFX") Warn("Take FX were not exported");
  }
  Take take = takes[0];
  for (const Take& tk : takes)
    if (tk.selected) take = tk;
  if (takes.size() > 1) Warn("Only the active take of items with several takes was exported");
  if (!take.source) return;
  if (!(take.rate > kEps)) take.rate = 1;
  if (std::fabs(take.pan) > 1e-6) Warn("Take pan was not exported");
  std::vector<EnvPoint> takeEnv;
  if (take.otherEnv) Warn("Take pan/mute/pitch envelopes were not exported");

  const RppNode* src = take.source;
  double secStart = 0, secLength = 0;
  std::string type = src->Token(1);
  if (type == "SECTION")
  {
    if (const RppNode* n = src->Child("STARTPOS")) secStart = n->Num(1);
    if (const RppNode* n = src->Child("LENGTH")) secLength = n->Num(1);
    if (const RppNode* n = src->Child("MODE"))
      if (n->Int(1) & 2) Warn("Reversed items were exported playing forwards");
    if (const RppNode* inner = src->Child("SOURCE"))
    {
      src = inner;
      type = src->Token(1);
    }
  }
  const RppNode* file = src->Child("FILE");
  if (type == "MIDI" || type == "MIDIPOOL")
  {
    Warn("MIDI items were not exported (AAF has no MIDI)");
    return;
  }
  if (!file || type == "VIDEO")
  {
    if (type != "EMPTY") Warn("Items with " + type + " sources were not exported");
    return;
  }

  int mi = GetMedia(ResolvePath(file->Token(1)));
  Media& m = media_[mi];
  if (ReadEnvelope(take.volEnv, takeEnv))
    for (EnvPoint& p : takeEnv) p.t /= take.rate;  // take envelopes run in take time

  // channels: CHANMODE 0 normal, 1 reverse stereo, 2 mono downmix, 3 left, 4 right, 3+n channel n
  std::vector<int> channels;
  for (int c = 0; c < m.channels; c++) channels.push_back(c);
  if (take.chanMode == 1 && m.channels >= 2) std::swap(channels[0], channels[1]);
  else if (take.chanMode == 2 && m.channels > 1) Warn("Mono downmix of items was not exported (all channels kept)");
  else if (take.chanMode >= 3 && take.chanMode - 3 < m.channels) channels = {take.chanMode - 3};
  else if (take.chanMode >= 3) Warn("An item channel mode was not exported (all channels kept)");

  // a looped item repeats its source (or section); each pass becomes its own clip
  double fileLen = m.frames > 0 && m.sampleRate > 0 ? (double)m.frames / m.sampleRate : 0;
  double srcLen = secLength > kEps ? secLength : fileLen - secStart;
  struct Piece
  {
    double t0, t1, offset;  // item time range, source position at t0
  };
  std::vector<Piece> pieces;
  double rel = take.soffs;
  if (loop && srcLen > 1e-3 && rel + len * take.rate > srcLen + 1e-3)
  {
    rel = std::fmod(rel, srcLen);
    if (rel < 0) rel += srcLen;
    double tpos = 0;
    while (tpos < len - kEps && pieces.size() < 10000)
    {
      double l = std::min(len - tpos, (srcLen - rel) / take.rate);
      pieces.push_back({tpos, tpos + l, secStart + rel});
      tpos += l;
      rel = 0;
    }
  }
  else
  {
    double t0 = 0;
    if (rel < 0)  // the item starts before its source: silence
    {
      t0 = -rel / take.rate;
      rel = 0;
    }
    if (t0 < len - kEps) pieces.push_back({t0, len, secStart + rel});
  }
  if (!m.exists || m.kind == Media::kOther)
    for (const Piece& p : pieces)
      m.neededFrames = std::max(m.neededFrames, (int64_t)std::ceil((p.offset + (p.t1 - p.t0) * take.rate) * m.sampleRate));

  double gain = itemVol * take.vol;
  for (size_t i = 0; i < pieces.size(); i++)
  {
    const Piece& p = pieces[i];
    ClipOut c;
    c.s = std::llround((pos + p.t0) * sampleRate_);
    c.e = std::llround((pos + p.t1) * sampleRate_);
    if (c.e <= c.s || c.e <= 0) continue;
    if (c.s < 0)
    {
      Warn("Items before the project start were cut");
      double cut = -(double)c.s / sampleRate_;
      c.s = 0;
      c.offset = p.offset + cut * take.rate;
    }
    else
      c.offset = p.offset;
    c.media = mi;
    c.channels = channels;
    c.rate = take.rate;
    c.mute = mute;
    c.gain = gain;
    if (!takeEnv.empty())
    {
      double a = (double)c.s / sampleRate_ - pos, b = (double)c.e / sampleRate_ - pos;
      c.env = SliceEnv(takeEnv, a, b);
      if (IsFlat(c.env))
      {
        c.gain *= c.env.front().v;
        c.env.clear();
      }
      else
        for (EnvPoint& e : c.env) e.v *= gain;
    }
    if (i == 0 && fadeIn > kEps)
    {
      c.fadeIn = std::llround(std::max(0.0, fadeIn - p.t0) * sampleRate_);
      c.fadeInShape = fadeInShape;
    }
    if (i + 1 == pieces.size() && fadeOut > kEps)
    {
      c.fadeOut = std::llround(fadeOut * sampleRate_);
      c.fadeOutShape = fadeOutShape;
    }
    t.clips.push_back(c);
    report_.clips++;
  }
  if (std::fabs(take.rate - 1) > 1e-9) Warn("Item playback rates were exported as speed changes (pitch follows)");
}

// Clips that overlap go to extra lanes, except where the earlier clip fades out and the later one
// fades in: that overlap becomes a crossfade (an AAF Transition). A lane holds clips with one
// channel count (AAF tracks are mono, stereo, 5.1, ...).
std::vector<std::vector<ClipOut>> AafExporter::AssignLanes(std::vector<ClipOut> clips)
{
  std::stable_sort(clips.begin(), clips.end(), [](const ClipOut& a, const ClipOut& b) { return a.s < b.s; });
  std::vector<std::vector<ClipOut>> lanes;
  bool overlaps = false;
  for (ClipOut& c : clips)
  {
    bool placed = false;
    for (auto& lane : lanes)
    {
      ClipOut& p = lane.back();
      if (p.channels.size() != c.channels.size()) continue;
      int64_t overlap = p.e - c.s;
      if (overlap <= 0)
      {
        lane.push_back(c);
        placed = true;
      }
      else if (p.fadeOut > 0 && c.fadeIn > 0 && overlap + p.xfadePrev < p.Len() && overlap < c.Len() && c.e > p.e)
      {
        p.xfadeNext = overlap;
        c.xfadePrev = overlap;
        lane.push_back(c);
        placed = true;
      }
      if (placed) break;
    }
    if (!placed)
    {
      for (auto& lane : lanes)
        if (lane.back().channels.size() == c.channels.size()) overlaps = true;
      lanes.push_back({c});
    }
  }
  if (lanes.empty()) lanes.emplace_back();
  if (overlaps) Warn("Overlapping items were placed on extra tracks");
  if (lanes.size() > 1 && !overlaps) Warn("Items with different channel counts were placed on extra tracks");
  return lanes;
}

// ---------------------------------------------------------------------------------------------
// Objects

int AafExporter::Component(const char* cls, const char* dataDef, int64_t length)
{
  int o = w_.Create(cls);
  w_.SetWeakRef(o, "DataDefinition", AuidFromString(dataDef));
  if (length >= 0) w_.SetInt(o, "Length", length);
  return o;
}

int AafExporter::SourceClip(const MobId& mob, int slot, int64_t start, int64_t length, const char* dataDef)
{
  int sc = Component("SourceClip", dataDef, length);
  w_.SetMobId(sc, "SourceID", mob);
  w_.SetInt(sc, "SourceMobSlotID", slot);
  w_.SetInt(sc, "StartTime", start);
  return sc;
}

AafRational ToRational(double v, int32_t den)
{
  double limit = (double)INT32_MAX / den;
  v = std::max(-limit, std::min(limit, v));
  return {(int32_t)std::llround(v * den), den};
}

// A ConstantValue, or a VaryingValue when there are points (times in seconds from the start of
// the operation, which is 'length' seconds long; AAF control point times are 0..1).
int AafExporter::Param(const char* def, double constant, const std::vector<EnvPoint>& pts, double length,
                       const char* interp)
{
  int den = 1 << 24;
  if (pts.empty())
  {
    int cv = w_.Create("ConstantValue");
    w_.SetAuid(cv, "Definition", AuidFromString(def));
    w_.SetIndirectRational(cv, "Value", ToRational(constant, den));
    return cv;
  }
  int vv = w_.Create("VaryingValue");
  w_.SetAuid(vv, "Definition", AuidFromString(def));
  w_.SetWeakRef(vv, "Interpolation", AuidFromString(interp));
  for (const EnvPoint& p : pts)
  {
    double t = length > kEps ? std::max(0.0, std::min(1.0, p.t / length)) : 0.0;
    int cp = w_.Create("ControlPoint");
    w_.SetRational(cp, "Time", ToRational(t, 1 << 30));
    w_.SetIndirectRational(cp, "Value", ToRational(p.v, den));
    w_.Append(vv, "PointList", cp);
  }
  return vv;
}

int AafExporter::OpGroup(const char* op, int64_t length, std::vector<int> inputs, std::vector<int> params)
{
  int og = Component("OperationGroup", kDataDefSound, length);
  w_.SetWeakRef(og, "Operation", AuidFromString(op));
  for (int i : inputs) w_.Append(og, "InputSegments", i);
  for (int p : params) w_.Append(og, "Parameters", p);
  return og;
}

int AafExporter::Transition(int64_t length, int shape)
{
  int tr = Component("Transition", kDataDefSound, length);
  w_.SetInt(tr, "CutPoint", length / 2);
  std::vector<EnvPoint> curve = {{0, 0}, {1, 1}};
  int og = OpGroup(kOpMonoAudioDissolve, length, {}, {Param(kParamLevel, 0, curve, 1, shape == 0 ? kInterpLinear : kInterpPower)});
  w_.SetRef(tr, "OperationGroup", og);
  return tr;
}

int AafExporter::ClipSegment(const ClipOut& c)
{
  const Media& m = media_[c.media];
  int64_t len = c.Len();
  int64_t inner = std::fabs(c.rate - 1) > 1e-9 ? std::llround(len * c.rate) : len;
  int64_t start = std::llround(c.offset * sampleRate_);
  int seg;
  if (c.channels.size() == 1) seg = SourceClip(m.master, c.channels[0] + 1, start, inner);
  else
  {
    std::vector<int> inputs;
    for (int ch : c.channels) inputs.push_back(SourceClip(m.master, ch + 1, start, inner));
    seg = OpGroup(kOpChannelCombiner, inner, inputs, {});
  }
  if (inner != len)
  {
    // the speed opgroup's input is longer/shorter by the rate
    int speed = w_.Create("ConstantValue");
    w_.SetAuid(speed, "Definition", AuidFromString(kParamSpeedRatio));
    w_.SetIndirectRational(speed, "Value", ToRational(c.rate, 1 << 20));
    seg = OpGroup(kOpMotionControl, len, {seg}, {speed});
  }
  // muted clips get a gain operation even at unity: LibAAF ends a disabled clip's mute state there
  if (std::fabs(c.gain - 1) > 1e-7 || !c.env.empty() || c.mute)
    seg = OpGroup(kOpMonoAudioGain, len, {seg}, {Param(kParamAmplitude, c.gain, c.env, (double)len / sampleRate_, kInterpLinear)});
  if (c.mute)
  {
    // Media Composer's disabled clip: the Filler plays, the attributes mark the alternate as muted
    int sel = Component("Selector", kDataDefSound, len);
    for (auto& tv : {std::make_pair("_DISABLE_CLIP_FLAG", 1), std::make_pair("_AAF_SELECTED", 0)})
    {
      int o = w_.Create("TaggedValue");
      w_.SetString(o, "Name", tv.first);
      w_.SetIndirectInt32(o, "Value", tv.second);
      w_.Append(sel, "ComponentAttributeList", o);
    }
    w_.SetRef(sel, "Selected", Filler(len));
    w_.Append(sel, "Alternates", seg);
    seg = sel;
  }
  return seg;
}

// A lane's clips as a Sequence 'total' samples long. Fades become Transitions next to Fillers,
// crossfades Transitions between two clips (a Transition overlaps its neighbours).
int AafExporter::LaneSequence(std::vector<ClipOut>& lane, int64_t total)
{
  int seq = Component("Sequence", kDataDefSound, total);
  int64_t cursor = 0;
  bool lastIsTransition = false;
  auto add = [&](int comp, int64_t len, bool transition) {
    w_.Append(seq, "Components", comp);
    cursor += transition ? -len : len;
    lastIsTransition = transition;
  };
  for (size_t i = 0; i < lane.size(); i++)
  {
    ClipOut& c = lane[i];
    // fades share the clip with crossfades and each other
    int64_t room = c.Len() - c.xfadePrev - c.xfadeNext;
    int64_t in = c.xfadePrev ? 0 : c.fadeIn, out = c.xfadeNext ? 0 : c.fadeOut;
    if (in + out > room && in + out > 0)
    {
      double k = (double)std::max<int64_t>(room, 0) / (double)(in + out);
      in = (int64_t)(in * k);
      out = (int64_t)(out * k);
    }
    if (c.xfadePrev) add(Transition(c.xfadePrev, c.fadeInShape), c.xfadePrev, true);
    else if (in > 0)
    {
      add(Filler(c.s + in - cursor), c.s + in - cursor, false);
      add(Transition(in, c.fadeInShape), in, true);
    }
    else if (c.s > cursor)
      add(Filler(c.s - cursor), c.s - cursor, false);
    add(ClipSegment(c), c.Len(), false);
    if (out > 0) add(Transition(out, c.fadeOutShape), out, true);
  }
  if (cursor < total || lastIsTransition) add(Filler(total - cursor), total - cursor, false);
  return seq;
}

int AafExporter::TrackSlot(const TrackIn& t, std::vector<ClipOut>& lane, const std::string& name, int slotId,
                           int number, int64_t total)
{
  int slot = w_.Create("TimelineMobSlot");
  w_.SetInt(slot, "SlotID", slotId);
  w_.SetString(slot, "SlotName", name);
  w_.SetInt(slot, "PhysicalTrackNumber", number);
  w_.SetRational(slot, "EditRate", {sampleRate_, 1});
  w_.SetInt(slot, "Origin", 0);
  int seg = LaneSequence(lane, total);
  double seconds = (double)total / sampleRate_;
  if (std::fabs(t.pan) > 1e-6 || !t.panEnv.empty())
    seg = OpGroup(kOpMonoAudioPan, total, {seg}, {Param(kParamPan, (t.pan + 1) / 2, t.panEnv, seconds, kInterpLinear)});
  std::vector<EnvPoint> vol = t.volEnv;
  for (EnvPoint& p : vol) p.v *= t.vol;
  if (std::fabs(t.vol - 1) > 1e-7 || !vol.empty())
    seg = OpGroup(kOpMonoAudioGain, total, {seg}, {Param(kParamAmplitude, t.vol, vol, seconds, kInterpLinear)});
  w_.SetRef(slot, "Segment", seg);
  if (t.mute || t.solo)
    for (auto& tv : {std::make_pair("AudioMixerCompMute", t.mute), std::make_pair("AudioMixerCompSolo", t.solo)})
    {
      int o = w_.Create("TaggedValue");
      w_.SetString(o, "Name", tv.first);
      w_.SetIndirectInt32(o, "Value", tv.second ? 1 : 0);
      w_.Append(slot, "TimelineMobAttributeList", o);
    }
  return slot;
}

void AafExporter::BuildDictionary(int header)
{
  int dict = w_.Create("Dictionary");
  auto def = [&](const char* cls, const char* set, const char* id, const char* name, const char* description) {
    int d = w_.Create(cls);
    w_.SetAuid(d, "Identification", AuidFromString(id));
    w_.SetString(d, "Name", name);
    if (description) w_.SetString(d, "Description", description);
    w_.Insert(dict, set, d);
    return d;
  };
  def("DataDefinition", "DataDefinitions", kDataDefSound, "Sound", "Sound data");
  def("DataDefinition", "DataDefinitions", kDataDefTimecode, "Timecode", "Timecode data");
  def("DataDefinition", "DataDefinitions", kDataDefDescriptive, "Descriptive Metadata", "Descriptive metadata");
  struct OpInfo
  {
    const char* id;
    const char* name;
    int inputs;
    bool timeWarp;
  };
  for (const OpInfo& op : {OpInfo{kOpMonoAudioGain, "Mono Audio Gain", 1, false},
                           OpInfo{kOpMonoAudioPan, "Mono Audio Pan", 1, false},
                           OpInfo{kOpMonoAudioDissolve, "Mono Audio Dissolve", 2, false},
                           OpInfo{kOpChannelCombiner, "Audio Channel Combiner", -1, false},
                           OpInfo{kOpMotionControl, "Motion Control", 1, true}})
  {
    int d = def("OperationDefinition", "OperationDefinitions", op.id, op.name, nullptr);
    w_.SetWeakRef(d, "DataDefinition", AuidFromString(kDataDefSound));
    w_.SetInt(d, "NumberInputs", op.inputs);
    if (op.timeWarp) w_.SetBool(d, "IsTimeWarp", true);
  }
  for (auto& p : {std::make_pair(kParamLevel, "Level"), std::make_pair(kParamAmplitude, "Amplitude"),
                  std::make_pair(kParamPan, "Pan"), std::make_pair(kParamSpeedRatio, "SpeedRatio")})
  {
    int d = def("ParameterDefinition", "ParameterDefinitions", p.first, p.second, nullptr);
    w_.SetWeakRef(d, "Type", AuidFromString(kTypeRational));
  }
  def("InterpolationDefinition", "InterpolationDefinitions", kInterpLinear, "LinearInterp", nullptr);
  def("InterpolationDefinition", "InterpolationDefinitions", kInterpConstant, "ConstantInterp", nullptr);
  def("InterpolationDefinition", "InterpolationDefinitions", kInterpPower, "PowerInterp", nullptr);
  def("ContainerDefinition", "ContainerDefinitions", kContainerAaf, "AAF", "AAF container");
  w_.SetRef(header, "Dictionary", dict);
}

void AafExporter::BuildMediaMobs(int content)
{
  for (Media& m : media_)
  {
    if (m.frames <= 0) m.frames = std::max<int64_t>(m.neededFrames, 1);
    std::string name = FileStem(m.path);
    auto mob = [&](const char* cls, const MobId& id) {
      int o = w_.Create(cls);
      w_.SetMobId(o, "MobID", id);
      w_.SetString(o, "Name", name);
      w_.SetTimestamp(o, "LastModified", year_, month_, day_, hour_, minute_, second_);
      w_.SetTimestamp(o, "CreationTime", year_, month_, day_, hour_, minute_, second_);
      w_.Insert(content, "Mobs", o);
      return o;
    };
    auto slot = [&](int mobObj, int ch, int segment) {
      int s = w_.Create("TimelineMobSlot");
      w_.SetInt(s, "SlotID", ch + 1);
      w_.SetInt(s, "PhysicalTrackNumber", ch + 1);
      w_.SetRational(s, "EditRate", {m.sampleRate, 1});
      w_.SetInt(s, "Origin", 0);
      w_.SetRef(s, "Segment", segment);
      w_.Append(mobObj, "Slots", s);
    };
    // master mob: one slot per channel, each referring to the same slot of the file mob
    int master = mob("MasterMob", m.master);
    for (int ch = 0; ch < m.channels; ch++) slot(master, ch, SourceClip(m.file, ch + 1, 0, m.frames));

    int file = mob("SourceMob", m.file);
    int desc;
    if (m.kind == Media::kOther)
    {
      desc = w_.Create("PCMDescriptor");
      w_.SetRational(desc, "AudioSamplingRate", {m.sampleRate, 1});
      w_.SetInt(desc, "Channels", m.channels);
      w_.SetInt(desc, "QuantizationBits", m.bits);
      w_.SetInt(desc, "BlockAlign", m.channels * ((m.bits + 7) / 8));
      w_.SetInt(desc, "AverageBPS", m.sampleRate * m.channels * ((m.bits + 7) / 8));
    }
    else
    {
      desc = w_.Create(m.kind == Media::kWave ? "WAVEDescriptor" : "AIFCDescriptor");
      w_.SetBytes(desc, "Summary", m.summary);
    }
    w_.SetRational(desc, "SampleRate", {m.sampleRate, 1});
    w_.SetInt(desc, "Length", m.frames);
    w_.SetWeakRef(desc, "ContainerFormat", AuidFromString(kContainerAaf));
    if (!m.embed)
    {
      int loc = w_.Create("NetworkLocator");
      w_.SetString(loc, "URLString", FileUrl(m.path));
      w_.Append(desc, "Locator", loc);
    }
    w_.SetRef(file, "EssenceDescription", desc);
    MobId none{};
    for (int ch = 0; ch < m.channels; ch++) slot(file, ch, SourceClip(none, 0, 0, m.frames));
    report_.mediaFiles++;

    if (m.embed)
    {
      int ed = w_.Create("EssenceData");
      w_.SetMobId(ed, "MobID", m.file);
      w_.SetStream(ed, "Data", m.path, 0, (uint64_t)FileSize(m.path));
      w_.Insert(content, "EssenceData", ed);
      report_.embedded++;
    }
  }
}

int AafExporter::MarkerSlot(int64_t& total)
{
  if (markers_.empty()) return -1;
  int seq = Component("Sequence", kDataDefDescriptive, -1);
  int64_t end = 0;
  for (const Marker& mk : markers_)
  {
    int64_t pos = std::max<int64_t>(0, std::llround(mk.pos * sampleRate_));
    int64_t len = std::llround(mk.len * sampleRate_);
    int dm = w_.Create("DescriptiveMarker");
    w_.SetWeakRef(dm, "DataDefinition", AuidFromString(kDataDefDescriptive));
    w_.SetInt(dm, "Position", pos);
    if (len > 0) w_.SetInt(dm, "Length", len);
    if (!mk.name.empty()) w_.SetString(dm, "Comment", mk.name);
    w_.Append(seq, "Components", dm);
    end = std::max(end, pos + std::max<int64_t>(len, 0));
  }
  total = std::max(total, end);
  w_.SetInt(seq, "Length", total);
  int slot = w_.Create("EventMobSlot");
  w_.SetInt(slot, "SlotID", 1000);
  w_.SetInt(slot, "PhysicalTrackNumber", 1);
  w_.SetRational(slot, "EditRate", {sampleRate_, 1});
  w_.SetRef(slot, "Segment", seq);
  return slot;
}

bool AafExporter::Run(const std::string& text, const std::string& outPath, std::string& error)
{
  if (!ParseRpp(text, root_, error)) return false;
  if (root_.Name() != "REAPER_PROJECT")
  {
    error = "Not a REAPER project";
    return false;
  }
  time_t now = time(nullptr);
  struct tm lt;
#ifdef _WIN32
  localtime_s(&lt, &now);
#else
  localtime_r(&now, &lt);
#endif
  year_ = lt.tm_year + 1900, month_ = lt.tm_mon + 1, day_ = lt.tm_mday;
  hour_ = lt.tm_hour, minute_ = lt.tm_min, second_ = lt.tm_sec;

  if (const RppNode* sr = root_.Child("SAMPLERATE"))
    if (sr->Int(1) > 0) sampleRate_ = sr->Int(1);
  // frame rate for the timecode track (REAPER: TIMEMODE ... <fps> <drop>)
  AafRational fps{30, 1};
  int fpsInt = 30;
  bool drop = false;
  if (const RppNode* tm = root_.Child("TIMEMODE"))
  {
    double f = tm->Num(4, 30);
    fpsInt = std::max(1, (int)std::lround(f));
    fps = std::fabs(f - fpsInt) > 0.001 ? AafRational{fpsInt * 1000, 1001} : AafRational{fpsInt, 1};
    drop = tm->Int(5) != 0 && fps.den == 1001;
  }
  double projOffset = 0;
  if (const RppNode* po = root_.Child("PROJOFFS")) projOffset = po->Num(1);

  // tracks (folders are flattened; a folder parent without items is left out)
  std::vector<TrackIn> tracks;
  for (const RppNode* n : root_.Children("TRACK"))
  {
    TrackIn t;
    ReadTrack(*n, t);
    if (const RppNode* bus = n->Child("ISBUS"))
      if (bus->Int(1) == 1)
      {
        Warn("Folder tracks were flattened");
        if (t.clips.empty()) continue;
      }
    if (t.clips.empty() && n->Child("ITEM")) continue;  // only MIDI or other items that were not exported
    if (t.name.empty()) t.name = "Audio " + std::to_string(tracks.size() + 1);
    tracks.push_back(std::move(t));
  }
  if (const RppNode* v = root_.Child("MASTER_VOLUME"))
    if (std::fabs(v->Num(1, 1) - 1) > 1e-6 || std::fabs(v->Num(2, 0)) > 1e-6)
      Warn("Master volume/pan was not exported");
  {
    std::vector<EnvPoint> pts;
    if (ReadEnvelope(root_.Child("MASTERVOLENV2"), pts) || ReadEnvelope(root_.Child("MASTERPANENV2"), pts))
      Warn("Master envelopes were not exported");
  }
  if (const RppNode* chain = root_.Child("MASTERFXLIST"))
    for (const RppNode& c : chain->children)
      if (c.block)
      {
        Warn("Master FX were not exported");
        break;
      }

  // markers and regions (a region is listed twice: start with its name, end without)
  {
    std::map<int, size_t> openRegions;
    for (const RppNode* m : root_.Children("MARKER"))
    {
      int idx = m->Int(1), flags = m->Int(4);
      if (flags & 1)
      {
        auto it = openRegions.find(idx);
        if (it != openRegions.end() && m->Token(3).empty())
        {
          markers_[it->second].len = std::max(0.0, m->Num(2) - markers_[it->second].pos);
          openRegions.erase(it);
          continue;
        }
        openRegions[idx] = markers_.size();
      }
      markers_.push_back({m->Num(2), 0, m->Token(3)});
    }
    std::stable_sort(markers_.begin(), markers_.end(), [](const Marker& a, const Marker& b) { return a.pos < b.pos; });
  }

  // lanes and the composition length
  std::vector<std::vector<std::vector<ClipOut>>> lanes;
  int64_t total = 0;
  for (TrackIn& t : tracks)
  {
    lanes.push_back(AssignLanes(t.clips));
    for (const ClipOut& c : t.clips) total = std::max(total, c.e);
  }
  for (const Marker& mk : markers_) total = std::max(total, std::llround((mk.pos + mk.len) * sampleRate_));
  total = std::max<int64_t>(total, 1);

  // header
  // Avid extensions read by Media Composer, Pro Tools and LibAAF
  w_.DefineProperty("TimelineMobSlot", "TimelineMobAttributeList", "107f8331-1914-4234-b2c4-5a3eb755b7ca",
                    "05060800-0000-0000-060e-2b3401040101");
  w_.DefineProperty("Component", "ComponentAttributeList", "60958184-47b1-11d4-a01c-0004ac969f50",
                    "05060800-0000-0000-060e-2b3401040101");
  int header = w_.Create("Header");
  w_.SetInt(header, "ByteOrder", 0x4949);
  w_.SetTimestamp(header, "LastModified", year_, month_, day_, hour_, minute_, second_);
  w_.SetVersion(header, "Version", 1, 1);
  w_.SetInt(header, "ObjectModelVersion", 1);
  w_.SetAuid(header, "OperationalPattern", AuidFromString(kOpEditProtocol));
  int ident = w_.Create("Identification");
  w_.SetString(ident, "CompanyName", "DAWproject for REAPER");
  w_.SetString(ident, "ProductName", "REAPER AAF export");
  w_.SetProductVersion(ident, "ProductVersion", 1, 0, 0, 0, 1);
  w_.SetString(ident, "ProductVersionString", "1.0");
  w_.SetProductVersion(ident, "ToolkitVersion", 1, 0, 0, 0, 1);
  w_.SetAuid(ident, "ProductID", AuidFromString("5f3c1e4a-8d2b-4c6e-9a71-0b2d3e4f5a6b"));
  w_.SetTimestamp(ident, "Date", year_, month_, day_, hour_, minute_, second_);
#ifdef _WIN32
  w_.SetString(ident, "Platform", "Win32");
#elif defined(__APPLE__)
  w_.SetString(ident, "Platform", "MacOS");
#else
  w_.SetString(ident, "Platform", "Linux");
#endif
  w_.SetAuid(ident, "GenerationAUID", NewAuid());
  w_.Append(header, "IdentificationList", ident);
  BuildDictionary(header);
  int content = w_.Create("ContentStorage");
  w_.Insert(content, "EssenceData", -1);  // present even when empty (readers expect it)
  w_.SetRef(header, "Content", content);

  // composition: timecode, one slot per track lane, markers
  int comp = w_.Create("CompositionMob");
  w_.SetMobId(comp, "MobID", NewMobId());
  std::string name = options_.name;
  if (name.empty()) name = "REAPER project";
  w_.SetString(comp, "Name", name);
  w_.SetTimestamp(comp, "LastModified", year_, month_, day_, hour_, minute_, second_);
  w_.SetTimestamp(comp, "CreationTime", year_, month_, day_, hour_, minute_, second_);
  w_.SetAuid(comp, "UsageCode", AuidFromString(kUsageTopLevel));
  w_.Insert(content, "Mobs", comp);

  int markerSlot = MarkerSlot(total);
  {
    int slot = w_.Create("TimelineMobSlot");
    w_.SetInt(slot, "SlotID", 1);
    w_.SetString(slot, "SlotName", "TC1");
    w_.SetInt(slot, "PhysicalTrackNumber", 1);
    w_.SetRational(slot, "EditRate", fps);
    w_.SetInt(slot, "Origin", 0);
    int64_t tcLen = (int64_t)std::ceil((double)total / sampleRate_ * fps.num / fps.den);
    int tc = Component("Timecode", kDataDefTimecode, std::max<int64_t>(tcLen, 1));
    double startFrames = projOffset * fps.num / fps.den;
    if (std::fabs(startFrames - std::round(startFrames)) > 0.01)
      Warn("The project start offset was rounded to whole timecode frames");
    w_.SetInt(tc, "Start", std::llround(startFrames));
    w_.SetInt(tc, "FPS", fpsInt);
    w_.SetBool(tc, "Drop", drop);
    w_.SetRef(slot, "Segment", tc);
    w_.Append(comp, "Slots", slot);
  }
  int slotId = 2, number = 1;
  for (size_t i = 0; i < tracks.size(); i++)
    for (size_t l = 0; l < lanes[i].size(); l++)
    {
      std::string slotName = l == 0 ? tracks[i].name : tracks[i].name + " (" + std::to_string(l + 1) + ")";
      w_.Append(comp, "Slots", TrackSlot(tracks[i], lanes[i][l], slotName, slotId++, number++, total));
      report_.tracks++;
    }
  if (markerSlot >= 0) w_.Append(comp, "Slots", markerSlot);

  BuildMediaMobs(content);

  std::string tmp = outPath + ".tmp";
  if (!w_.Write(tmp, header, error))
  {
    RemoveFile(tmp);
    return false;
  }
  if (!RenameFile(tmp, outPath))
  {
    RemoveFile(tmp);
    error = "Could not write " + outPath;
    return false;
  }
  for (auto& w : warnings_)
    report_.warnings.push_back(w.second > 1 ? w.first + " (" + std::to_string(w.second) + "x)" : w.first);
  return true;
}

}  // namespace

bool ExportAaf(const std::string& rppText, const std::string& outPath, const AafExportOptions& options,
               AafExportReport& report, std::string& error)
{
  AafExporter e(options, report);
  return e.Run(rppText, outPath, error);
}

}  // namespace dawp
