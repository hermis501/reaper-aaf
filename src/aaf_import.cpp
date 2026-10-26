#include "aaf_import.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <set>

#include "aaf_file.h"
#include "media_info.h"
#include "pugixml.hpp"
#include "util.h"

namespace dawp {
namespace {

using Obj = AafReader::Obj;
using pugi::xml_node;

const double kTimeEps = 1e-6;  // seconds; well below one sample

// Baseline property ids (AAF object specification).
enum : uint16_t
{
  kComponentDataDef = 0x0201,
  kComponentLength = 0x0202,
  kEssenceGroupChoices = 0x0501,
  kEventPosition = 0x0601,
  kEventComment = 0x0602,
  kOpGroupOperation = 0x0b01,
  kOpGroupInputs = 0x0b02,
  kOpGroupParameters = 0x0b03,
  kNestedScopeSlots = 0x0c01,
  kSelectorSelected = 0x0f01,
  kSelectorAlternates = 0x0f02,
  kSequenceComponents = 0x1001,
  kSourceID = 0x1101,
  kSourceMobSlotID = 0x1102,
  kSourceClipStartTime = 0x1201,
  kTimecodeStart = 0x1501,
  kTransitionOpGroup = 0x1801,
  kContentMobs = 0x1901,
  kContentEssenceData = 0x1902,
  kControlPointValue = 0x1a02,
  kControlPointTime = 0x1a03,
  kEssenceDataMobID = 0x2701,
  kEssenceDataData = 0x2702,
  kDescriptorLocator = 0x2f01,
  kFileSampleRate = 0x3001,
  kFileLength = 0x3002,
  kAifcSummary = 0x3101,
  kWaveSummary = 0x3801,
  kHeaderContent = 0x3b03,
  kHeaderIdentificationList = 0x3b06,
  kIdentCompany = 0x3c01,
  kIdentProduct = 0x3c02,
  kIdentVersionString = 0x3c04,
  kSoundSamplingRate = 0x3d03,
  kSoundChannels = 0x3d07,
  kSoundBits = 0x3d01,
  kMultipleDescriptors = 0x3f01,
  kNetworkLocatorUrl = 0x4001,
  kTextLocatorName = 0x4101,
  kMobID = 0x4401,
  kMobName = 0x4402,
  kMobSlots = 0x4403,
  kMobUserComments = 0x4406,
  kMobUsageCode = 0x4408,
  kSourceMobDescriptor = 0x4701,
  kSlotID = 0x4801,
  kSlotName = 0x4802,
  kSlotSegment = 0x4803,
  kSlotPhysicalTrack = 0x4804,
  kEventSlotEditRate = 0x4901,
  kTimelineSlotEditRate = 0x4b01,
  kParameterDefinition = 0x4c01,
  kConstantValue = 0x4d01,
  kVaryingInterpolation = 0x4e01,
  kVaryingPoints = 0x4e02,
  kTaggedName = 0x5001,
  kTaggedValue = 0x5003,
};

const Auid kDataDefSound = AuidFromString("01030202-0200-0000-060e-2b3404010101");
const Auid kDataDefLegacySound = AuidFromString("78e1ebe1-6cef-11d2-807d-006008143e6f");
const Auid kDataDefPicture = AuidFromString("01030202-0100-0000-060e-2b3404010101");
const Auid kDataDefLegacyPicture = AuidFromString("6f3c8ce1-6cef-11d2-807d-006008143e6f");
const Auid kDataDefTimecode = AuidFromString("01030201-0100-0000-060e-2b3404010101");
const Auid kDataDefLegacyTimecode = AuidFromString("7f275e81-77e5-11d2-807f-006008143e6f");
const Auid kParamLevel = AuidFromString("e4962320-2267-11d3-8a4c-0050040ef7d2");
const Auid kParamAmplitude = AuidFromString("e4962321-2267-11d3-8a4c-0050040ef7d2");
const Auid kParamPan = AuidFromString("e4962322-2267-11d3-8a4c-0050040ef7d2");
const Auid kParamSpeedRatio = AuidFromString("72559a80-24d7-11d3-8a50-0050040ef7d2");
const Auid kOpChannelCombiner = AuidFromString("6b46dd7a-132d-4856-ab21-8b751d8462ec");
const Auid kInterpConstant = AuidFromString("5b6c85a5-0ede-11d3-80a9-006008143e6f");
const Auid kInterpLog = AuidFromString("15829ec3-1f24-458a-960d-c65bb23c2aa1");
const Auid kInterpPower = AuidFromString("c09153f7-bd18-4e5a-ad09-cbdd654fa001");
const Auid kUsageSubClip = AuidFromString("0d010102-0101-0500-060e-2b3404010101");
const Auid kUsageAdjustedClip = AuidFromString("0d010102-0101-0600-060e-2b3404010101");
const Auid kUsageTopLevel = AuidFromString("0d010102-0101-0700-060e-2b3404010101");

double Sec(int64_t editUnits, AafRational rate)
{
  return rate.num ? (double)editUnits * rate.den / rate.num : 0.0;
}

bool Near(double a, double b) { return std::fabs(a - b) < kTimeEps; }

// ---------------------------------------------------------------------------------------------
// Model built from the composition

struct Source
{
  int essence = -1;
  int channel = -1;  // channel of the file, -1 = all channels
  bool operator==(const Source& o) const { return essence == o.essence && channel == o.channel; }
  bool operator<(const Source& o) const { return essence != o.essence ? essence < o.essence : channel < o.channel; }
};

struct EnvPt
{
  double time, value;  // time: seconds (absolute while walking, relative to the clip start when stored)
  bool hold;
};

struct Clip
{
  double pos = 0, len = 0;
  double offset = 0;  // seconds into the source
  double rate = 1;    // playback rate
  std::vector<Source> sources;
  std::string name;        // shown name
  std::string sourceName;  // master clip name (Pro Tools marks rendered fades this way)
  double sourceLength = 0;  // seconds, 0 = unknown
  double gain = 1;
  std::vector<EnvPt> gainEnv;  // seconds from the clip start
  double fadeIn = 0, fadeOut = 0;
  int fadeInShape = 0, fadeOutShape = 0;
  bool mute = false;
  double End() const { return pos + len; }
};

struct Track
{
  std::string name;
  bool solo = false, mute = false;
  double gain = 1;
  double pan = 0.5;  // AAF pan: 0 = left, 0.5 = center, 1 = right
  std::vector<EnvPt> volumeEnv, panEnv;  // absolute seconds
  std::vector<Clip> clips;
};

struct Essence
{
  std::string name;
  std::string path;  // local file; empty when unavailable
  bool embedded = false;
  bool extracted = false;
  Obj essenceData = nullptr;
  Obj descriptor = nullptr;
  int channels = 0;
  double sampleRate = 0;
  double length = 0;  // seconds
};

struct MarkerInfo
{
  double pos, len;
  std::string name;
  std::string color;
};

struct Resolved
{
  std::vector<Source> sources;
  double offset = 0;
  std::string name;
  double length = 0;
};

// ---------------------------------------------------------------------------------------------
// Audio file helpers (embedded essence, multichannel merging)

void Put16(std::vector<uint8_t>& v, uint32_t x)
{
  v.push_back((uint8_t)x);
  v.push_back((uint8_t)(x >> 8));
}
void Put32(std::vector<uint8_t>& v, uint32_t x)
{
  Put16(v, x & 0xFFFF);
  Put16(v, x >> 16);
}
uint32_t Le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t Le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

double Extended80(const uint8_t* p)  // AIFF sample rate
{
  int exponent = ((p[0] & 0x7F) << 8) | p[1];
  uint64_t mantissa = 0;
  for (int i = 0; i < 8; i++) mantissa = (mantissa << 8) | p[2 + i];
  if (exponent == 0 && mantissa == 0) return 0;
  double v = std::ldexp((double)mantissa, exponent - 16383 - 63);
  return (p[0] & 0x80) ? -v : v;
}

std::vector<uint8_t> WavHeader(int channels, uint32_t sampleRate, int bits, bool isFloat, uint64_t dataBytes)
{
  std::vector<uint8_t> h;
  int blockAlign = channels * (bits / 8);
  h.insert(h.end(), {'R', 'I', 'F', 'F'});
  Put32(h, (uint32_t)std::min<uint64_t>(dataBytes + 36, 0xFFFFFFFFu));
  h.insert(h.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  Put32(h, 16);
  Put16(h, isFloat ? 3 : 1);
  Put16(h, channels);
  Put32(h, sampleRate);
  Put32(h, sampleRate * blockAlign);
  Put16(h, blockAlign);
  Put16(h, bits);
  h.insert(h.end(), {'d', 'a', 't', 'a'});
  Put32(h, (uint32_t)std::min<uint64_t>(dataBytes, 0xFFFFFFFFu));
  return h;
}

// PCM sample data inside a WAV or AIFF file.
struct PcmFile
{
  FILE* f = nullptr;
  uint64_t dataOffset = 0, frames = 0;
  int channels = 0, bits = 0;
  uint32_t sampleRate = 0;
  bool isFloat = false, bigEndian = false;
  ~PcmFile()
  {
    if (f) fclose(f);
  }
  int FrameBytes() const { return channels * (bits / 8); }
};

bool OpenPcm(const std::string& path, PcmFile& out)
{
  out.f = OpenFile(path, "rb");
  if (!out.f) return false;
  uint8_t h[12];
  if (fread(h, 1, 12, out.f) != 12) return false;
  bool riff = memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVE", 4) == 0;
  bool aiff = memcmp(h, "FORM", 4) == 0 && (memcmp(h + 8, "AIFF", 4) == 0 || memcmp(h + 8, "AIFC", 4) == 0);
  if (!riff && !aiff) return false;
  uint64_t pos = 12;
  bool haveFmt = false, haveData = false;
  uint64_t dataBytes = 0;
  while (!(haveFmt && haveData))
  {
    uint8_t c[8];
    if (!SeekFile(out.f, (int64_t)pos) || fread(c, 1, 8, out.f) != 8) break;
    uint64_t size = riff ? Le32(c + 4) : Be32(c + 4);
    uint8_t body[40] = {};
    size_t want = (size_t)std::min<uint64_t>(size, sizeof(body));
    if (riff && memcmp(c, "fmt ", 4) == 0)
    {
      if (fread(body, 1, want, out.f) != want || want < 16) return false;
      int tag = Le16(body);
      if (tag == 0xFFFE && want >= 26) tag = Le16(body + 24);  // WAVE_FORMAT_EXTENSIBLE sub format
      out.channels = Le16(body + 2);
      out.sampleRate = Le32(body + 4);
      out.bits = Le16(body + 14);
      out.isFloat = tag == 3;
      if (tag != 1 && tag != 3) return false;
      haveFmt = true;
    }
    else if (riff && memcmp(c, "data", 4) == 0)
    {
      out.dataOffset = pos + 8;
      dataBytes = size;
      haveData = true;
    }
    else if (aiff && memcmp(c, "COMM", 4) == 0)
    {
      if (fread(body, 1, want, out.f) != want || want < 18) return false;
      out.channels = Be16(body);
      out.bits = Be16(body + 6);
      out.sampleRate = (uint32_t)std::lround(Extended80(body + 8));
      out.bigEndian = true;
      if (memcmp(h + 8, "AIFC", 4) == 0 && want >= 22)
      {
        if (memcmp(body + 18, "sowt", 4) == 0) out.bigEndian = false;
        else if (memcmp(body + 18, "fl32", 4) == 0 || memcmp(body + 18, "FL32", 4) == 0) out.isFloat = true;
        else if (memcmp(body + 18, "NONE", 4) != 0) return false;
      }
      haveFmt = true;
    }
    else if (aiff && memcmp(c, "SSND", 4) == 0)
    {
      uint8_t ss[8];
      if (fread(ss, 1, 8, out.f) != 8) return false;
      out.dataOffset = pos + 16 + Be32(ss);
      dataBytes = size >= 8 + (uint64_t)Be32(ss) ? size - 8 - Be32(ss) : 0;
      haveData = true;
    }
    pos += 8 + size + (size & 1);
  }
  if (!haveFmt || !haveData || out.channels <= 0 || (out.bits != 16 && out.bits != 24 && out.bits != 32)) return false;
  out.frames = dataBytes / out.FrameBytes();
  return true;
}

// ---------------------------------------------------------------------------------------------

bool IsRenderedFadeName(const std::string& n)
{
  // Pro Tools' localized names of rendered fade clips
  static const char* const kNames[] = {"Fade ", "Fondu ", "Fundido ", "\xed\x8e\x98\xec\x9d\xb4\xeb\x93\x9c",
                                       "\xe6\xb7\xa1\xe5\x8f\x98 ", "\xe6\xb7\xa1\xe8\xae\x8a "};
  for (const char* k : kNames)
    if (n == k) return true;
  return false;
}

bool IsSampleAccurateEditName(const std::string& n)
{
  static const char* const kNames[] = {
      "Sample accurate edit",
      "Samplegenaue Bearbeitung",
      "Edici\xc3\xb3n con precisi\xc3\xb3n de muestra",
      "Modification \xc3\xa0 l'\xc3\xa9\x63hantillon pr\xc3\xa8s",
      "\xe3\x82\xb5\xe3\x83\xb3\xe3\x83\x97\xe3\x83\xab\xe7\xb2\xbe\xe5\xba\xa6\xe7\xb7\xa8\xe9\x9b\x86",
      "\xec\x83\x98\xed\x94\x8c \xeb\x8b\xa8\xec\x9c\x84 \xec\xa0\x95\xeb\xb0\x80 \xed\x8e\xb8\xec\xa7\x91",
      "\xe7\xb2\xbe\xe7\xa1\xae\xe9\x87\x87\xe6\xa0\xb7\xe7\xbc\x96\xe8\xbe\x91",
      "\xe7\xb2\xbe\xe7\xa2\xba\xe5\x8f\x96\xe6\xa8\xa3\xe7\xb7\xa8\xe8\xbc\xaf",
  };
  for (const char* k : kNames)
    if (n == k) return true;
  return false;
}

std::string PercentDecode(const std::string& s)
{
  std::string out;
  for (size_t i = 0; i < s.size(); i++)
  {
    if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2]))
    {
      out += (char)std::stoi(s.substr(i + 1, 2), nullptr, 16);
      i += 2;
    }
    else
      out += s[i];
  }
  return out;
}

// "file:///C:/a/b.wav", "file://localhost/C:/a", "file:///c%3a/a", "file://host/share/a", "/Volumes/x/a"
std::string UrlToPath(const std::string& url)
{
  std::string s = url;
  std::string lower = ToLower(s.substr(0, 7));
  if (lower == "file://")
  {
    s = s.substr(7);
    if (ToLower(s.substr(0, 10)) == "localhost/") s = s.substr(9);
    s = PercentDecode(s);
    if (!s.empty() && s[0] != '/') s = "//" + s;  // file://host/share -> UNC
    else if (s.size() >= 3 && s[0] == '/' && isalpha((unsigned char)s[1]) && s[2] == ':')
      s = s.substr(1);  // /C:/... -> C:/...
  }
  return NativeSeparators(s);
}

std::vector<std::string> PathParts(const std::string& p)
{
  std::vector<std::string> parts;
  std::string cur;
  for (char c : p)
  {
    if (c == '/' || c == '\\')
    {
      if (!cur.empty()) parts.push_back(cur);
      cur.clear();
    }
    else
      cur += c;
  }
  if (!cur.empty()) parts.push_back(cur);
  return parts;
}

std::string ColorFromRgb16(const std::vector<uint8_t>& d)
{
  if (d.size() < 6) return std::string();
  char buf[16];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", Le16(&d[0]) >> 8, Le16(&d[2]) >> 8, Le16(&d[4]) >> 8);
  return buf;
}

// ---------------------------------------------------------------------------------------------

class Importer
{
public:
  Importer(const std::string& path, const ConvertOptions& options, const AafImportOptions& aafOptions,
           ConvertReport& report)
      : path_(path), options_(options), aafOptions_(aafOptions), report_(report)
  {
  }

  bool Build(std::string& xml, std::string& error);

private:
  void Warn(const std::string& m) { warnings_[m]++; }

  // structure
  void IndexMobs();
  Obj FindMob(const MobId& id) const;
  Obj FindSlot(Obj mob, uint32_t slotId) const;
  AafRational SlotRate(Obj slot) const;
  int SlotKind(Obj slot) const;  // 1 sound, 2 picture, 3 timecode, 4 sound master, 0 other
  Obj ChooseComposition();
  void CollectReferences(Obj o, std::set<MobId>& refs, int depth);
  std::vector<Obj> TaggedValues(Obj o, const char* listProperty) const;

  // timeline walking
  void Walk(Obj seg, double start, AafRational rate, bool trackLevel, std::vector<Clip>& out, int depth);
  void WalkSequence(Obj seq, double start, AafRational rate, std::vector<Clip>& out, int depth);
  void WalkOperation(Obj og, double start, AafRational rate, bool trackLevel, std::vector<Clip>& out, int depth);
  void SourceClipLeaf(Obj sc, double start, AafRational rate, std::vector<Clip>& out, int depth);
  int TransitionShape(Obj transition);
  bool ParamPoints(Obj param, double start, double len, std::vector<EnvPt>& pts, double& constant);
  static void Trim(std::vector<Clip>& clips, double a, double b);

  // sources
  bool Resolve(Obj mob, uint32_t slotId, double t, Resolved& r, int depth);
  bool ResolveSegment(Obj seg, AafRational rate, double t, Resolved& r, int depth);
  int GetEssence(Obj sourceMob);
  int ChannelOf(Obj sourceMob, Obj slot, int essence);
  std::string LocateFile(const std::string& url);
  bool SearchFile(const std::string& dir, const std::string& lowerName, int depth, int& budget, std::string& found);
  std::string EssencePath(int e);
  bool EnsureMediaDir();
  // A free file name in the media folder; an existing file of the expected size is taken as an earlier extraction.
  std::string UniqueMediaPath(const std::string& stem, const std::string& ext, uint64_t size);
  std::string ExtractEmbedded(Essence& e);
  std::string MergeChannels(const std::vector<Source>& sources, const std::string& clipName);

  // post-processing
  void ProToolsCleanup(std::vector<Clip>& clips);
  bool RemoveSampleAccurateEdit(std::vector<Clip>& clips, size_t i);
  bool ReplaceRenderedFade(std::vector<Clip>& clips, size_t i);

  // output
  void ReadMarkers(Obj comp);
  std::string BuildXml(Obj comp);

  std::string path_;
  const ConvertOptions& options_;
  AafImportOptions aafOptions_;
  ConvertReport& report_;
  AafReader r_;
  std::map<std::string, int> warnings_;

  std::map<MobId, Obj> mobs_;
  std::map<MobId, Obj> essenceData_;
  std::vector<Obj> compositions_;
  std::string company_, product_, version_;
  bool proTools_ = false, resolve_ = false;
  double compStart_ = 0;

  Track* track_ = nullptr;  // track being walked (track-level gain/pan)
  std::vector<Track> tracks_;
  Track master_;
  std::vector<MarkerInfo> markers_;
  std::vector<std::unique_ptr<Essence>> essences_;
  std::map<Obj, int> essenceByMob_;
  std::map<std::string, int> essenceByPath_;
  std::map<std::vector<Source>, std::string> merged_;
  std::string mediaDir_;
  bool mediaDirFailed_ = false;
  std::set<std::string> usedMediaNames_;
  int videoSlots_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Structure

void Importer::IndexMobs()
{
  Obj content = r_.Ref(r_.Header(), kHeaderContent);
  for (Obj m : r_.Refs(content, kContentMobs))
  {
    mobs_[r_.MobIdValue(m, kMobID)] = m;
    if (r_.IsA(m, "CompositionMob")) compositions_.push_back(m);
  }
  for (Obj d : r_.Refs(content, kContentEssenceData)) essenceData_[r_.MobIdValue(d, kEssenceDataMobID)] = d;
}

Obj Importer::FindMob(const MobId& id) const
{
  auto it = mobs_.find(id);
  return it == mobs_.end() ? nullptr : it->second;
}

Obj Importer::FindSlot(Obj mob, uint32_t slotId) const
{
  for (Obj s : r_.Refs(mob, kMobSlots))
    if (r_.UInt(s, kSlotID) == slotId) return s;
  return nullptr;
}

AafRational Importer::SlotRate(Obj slot) const
{
  AafRational r = r_.Rational(slot, r_.Has(slot, kTimelineSlotEditRate) ? kTimelineSlotEditRate : kEventSlotEditRate);
  if (r.num <= 0 || r.den <= 0) r = {25, 1};
  return r;
}

int Importer::SlotKind(Obj slot) const
{
  Obj seg = r_.Ref(slot, kSlotSegment);
  Auid dd;
  if (!r_.WeakRefKey(seg, kComponentDataDef, dd)) return 0;
  if (dd == kDataDefSound || dd == kDataDefLegacySound) return 1;
  if (dd == kDataDefPicture || dd == kDataDefLegacyPicture) return 2;
  if (dd == kDataDefTimecode || dd == kDataDefLegacyTimecode) return 3;
  std::string name = ToLower(r_.DefinitionName(dd));
  if (name.find("soundmaster") != std::string::npos) return 4;  // Media Composer's master fader
  if (name.find("sound") != std::string::npos) return 1;
  if (name.find("picture") != std::string::npos) return 2;
  if (name.find("timecode") != std::string::npos) return 3;
  return 0;
}

void Importer::CollectReferences(Obj o, std::set<MobId>& refs, int depth)
{
  if (!o || depth > 200) return;
  if (r_.IsA(o, "SourceReference")) refs.insert(r_.MobIdValue(o, kSourceID));
  for (const auto& kv : o->props)
  {
    if (kv.second.form == 0x22) CollectReferences(r_.Ref(o, kv.first), refs, depth + 1);
    else if (kv.second.form == 0x32 || kv.second.form == 0x3A)
      for (Obj c : r_.Refs(o, kv.first)) CollectReferences(c, refs, depth + 1);
  }
}

Obj Importer::ChooseComposition()
{
  if (compositions_.empty()) return nullptr;
  std::vector<Obj> top;
  for (Obj c : compositions_)
    if (r_.AuidValue(c, kMobUsageCode) == kUsageTopLevel) top.push_back(c);
  if (top.empty())
  {
    // not referenced by another composition
    std::set<MobId> refs;
    for (Obj c : compositions_) CollectReferences(c, refs, 0);
    for (Obj c : compositions_)
      if (!refs.count(r_.MobIdValue(c, kMobID))) top.push_back(c);
  }
  if (top.empty()) top = compositions_;
  if (top.size() > 1)
  {
    Warn("The AAF file holds " + std::to_string(top.size()) + " top-level compositions; only the first was imported");
  }
  return top[0];
}

std::vector<Obj> Importer::TaggedValues(Obj o, const char* listProperty) const
{
  uint16_t pid = r_.PidByName(listProperty);
  return pid ? r_.Refs(o, pid) : std::vector<Obj>();
}

// ---------------------------------------------------------------------------------------------
// Timeline

void Importer::Walk(Obj seg, double start, AafRational rate, bool trackLevel, std::vector<Clip>& out, int depth)
{
  if (!seg) return;
  if (depth > 100)
  {
    Warn("Composition nesting too deep (circular references?)");
    return;
  }
  if (r_.IsA(seg, "Sequence")) WalkSequence(seg, start, rate, out, depth);
  else if (r_.IsA(seg, "SourceClip")) SourceClipLeaf(seg, start, rate, out, depth);
  else if (r_.IsA(seg, "OperationGroup")) WalkOperation(seg, start, rate, trackLevel, out, depth);
  else if (r_.IsA(seg, "Selector"))
  {
    Obj selected = r_.Ref(seg, kSelectorSelected);
    std::vector<Obj> alternates = r_.Refs(seg, kSelectorAlternates);
    if ((!selected || r_.IsA(selected, "Filler")) && !alternates.empty())
    {
      // Media Composer's muted clip: silence selected, the clip as the alternate
      size_t first = out.size();
      Walk(alternates[0], start, rate, false, out, depth + 1);
      for (size_t i = first; i < out.size(); i++) out[i].mute = true;
    }
    else
      Walk(selected, start, rate, trackLevel, out, depth + 1);
  }
  else if (r_.IsA(seg, "EssenceGroup"))
  {
    std::vector<Obj> choices = r_.Refs(seg, kEssenceGroupChoices);
    if (!choices.empty()) Walk(choices[0], start, rate, trackLevel, out, depth + 1);
  }
  else if (r_.IsA(seg, "NestedScope"))
  {
    std::vector<Obj> slots = r_.Refs(seg, kNestedScopeSlots);
    if (!slots.empty()) Walk(slots.back(), start, rate, trackLevel, out, depth + 1);
  }
  else if (r_.IsA(seg, "ScopeReference"))
    Warn("Scope references (shared segments) are not supported and were skipped");
}

void Importer::WalkSequence(Obj seq, double start, AafRational rate, std::vector<Clip>& out, int depth)
{
  int64_t pos = 0;
  std::vector<size_t> previous;  // clips of the previous segment
  bool pendingFade = false;
  double fadeLen = 0;
  int fadeShape = 0;
  for (Obj c : r_.Refs(seq, kSequenceComponents))
  {
    int64_t len = r_.Int(c, kComponentLength, 0);
    if (r_.IsA(c, "Transition"))
    {
      // overlaps the end of the previous and the start of the next segment
      double end = start + Sec(pos, rate);
      pos -= len;
      fadeLen = Sec(len, rate);
      fadeShape = TransitionShape(c);
      for (size_t i : previous)
        if (Near(out[i].End(), end))
        {
          out[i].fadeOut = fadeLen;
          out[i].fadeOutShape = fadeShape;
        }
      pendingFade = true;
      previous.clear();
      continue;
    }
    double segStart = start + Sec(pos, rate);
    size_t first = out.size();
    Walk(c, segStart, rate, false, out, depth + 1);
    if (pendingFade)
    {
      for (size_t i = first; i < out.size(); i++)
        if (Near(out[i].pos, segStart))
        {
          out[i].fadeIn = fadeLen;
          out[i].fadeInShape = fadeShape;
        }
      pendingFade = false;
    }
    previous.clear();
    for (size_t i = first; i < out.size(); i++) previous.push_back(i);
    pos += len;
  }
}

// REAPER fade shape for a transition: 0 = linear, 1 = equal power / logarithmic
int Importer::TransitionShape(Obj transition)
{
  Obj og = r_.Ref(transition, kTransitionOpGroup);
  for (Obj tv : TaggedValues(og, "ComponentAttributeList"))
    if (r_.String(tv, kTaggedName) == "_ATN_AUDIO_DISSOLVE_CURVETYPE")
    {
      double v;
      if (r_.IndirectNumber(tv, kTaggedValue, v)) return v == 1 ? 1 : 0;
    }
  for (Obj p : r_.Refs(og, kOpGroupParameters))
  {
    Auid interp;
    if (r_.IsA(p, "VaryingValue") && r_.WeakRefKey(p, kVaryingInterpolation, interp))
    {
      if (interp == kInterpLog || interp == kInterpPower) return 1;
      std::string n = ToLower(r_.DefinitionName(interp));
      if (n.find("log") != std::string::npos || n.find("power") != std::string::npos) return 1;
    }
  }
  return 0;
}

// Constant or varying parameter. Points get absolute times (control point times are 0..1 of the
// operation's length).
bool Importer::ParamPoints(Obj param, double start, double len, std::vector<EnvPt>& pts, double& constant)
{
  pts.clear();
  if (r_.IsA(param, "ConstantValue")) return r_.IndirectNumber(param, kConstantValue, constant);
  if (!r_.IsA(param, "VaryingValue")) return false;
  Auid interp;
  bool hold = r_.WeakRefKey(param, kVaryingInterpolation, interp) && interp == kInterpConstant;
  for (Obj cp : r_.Refs(param, kVaryingPoints))
  {
    double v;
    if (!r_.IndirectNumber(cp, kControlPointValue, v)) continue;
    pts.push_back({start + r_.Rational(cp, kControlPointTime).Value() * len, v, hold});
  }
  std::stable_sort(pts.begin(), pts.end(), [](const EnvPt& a, const EnvPt& b) { return a.time < b.time; });
  if (pts.empty()) return false;
  constant = pts[0].value;
  // a flat curve is a constant (Resolve and Pro Tools write 0 dB curves on every clip)
  if (std::all_of(pts.begin(), pts.end(), [&](const EnvPt& p) { return p.value == constant; })) pts.clear();
  return true;
}

void Importer::WalkOperation(Obj og, double start, AafRational rate, bool trackLevel, std::vector<Clip>& out, int depth)
{
  std::vector<Obj> inputs = r_.Refs(og, kOpGroupInputs);
  Auid opId{};
  r_.WeakRefKey(og, kOpGroupOperation, opId);
  std::string opName = r_.DefinitionName(opId);
  double len = Sec(r_.Int(og, kComponentLength, 0), rate);
  std::vector<Obj> amplitudes;  // Media Composer: a constant level plus a clip gain curve
  Obj pan = nullptr, speed = nullptr;
  for (Obj p : r_.Refs(og, kOpGroupParameters))
  {
    Auid def = r_.AuidValue(p, kParameterDefinition);
    if (def == kParamAmplitude || r_.DefinitionName(def) == "AvidControlClipRatio") amplitudes.push_back(p);
    else if (def == kParamPan) pan = p;
    else if (def == kParamSpeedRatio) speed = p;
  }
  if (inputs.empty()) return;

  if (opId == kOpChannelCombiner || opName == "Audio Channel Combiner")
  {
    // one input per channel; each produces the same clips
    std::vector<Clip> combined;
    for (size_t i = 0; i < inputs.size(); i++)
    {
      std::vector<Clip> ch;
      Walk(inputs[i], start, rate, false, ch, depth + 1);
      if (i == 0) combined = std::move(ch);
      else if (ch.size() == combined.size())
        for (size_t k = 0; k < ch.size(); k++)
          combined[k].sources.insert(combined[k].sources.end(), ch[k].sources.begin(), ch[k].sources.end());
      else
        Warn("A multichannel clip has channels with different edits; only its first channel was imported");
    }
    // the operation's length is what the clip occupies (DaVinci Resolve writes shorter inputs)
    if (combined.size() == 1 && Near(combined[0].pos, start) && len > kTimeEps) combined[0].len = len;
    out.insert(out.end(), combined.begin(), combined.end());
    return;
  }

  bool warp = speed || opName == "Audio Warp" || opName == "Motion Control";
  if (warp)
  {
    // Speed change (Motion Control) or a different frame rate inside (Avid "mix and match").
    Obj in = inputs[0];
    AafRational inner = rate;
    bool mixMatch = false;
    int num = 0, den = 0;
    for (Obj tv : TaggedValues(in, "ComponentAttributeList"))
    {
      double v;
      std::string n = r_.String(tv, kTaggedName);
      if (!r_.IndirectNumber(tv, kTaggedValue, v)) continue;
      if (n == "_MIXMATCH_RATE_NUM") num = (int)v;
      else if (n == "_MIXMATCH_RATE_DENOM") den = (int)v;
    }
    if (num > 0 && den > 0)
    {
      inner = {num, den};
      mixMatch = true;
    }
    double innerLen = Sec(r_.Int(in, kComponentLength, 0), inner);
    double ratio = len > kTimeEps && innerLen > kTimeEps ? innerLen / len : 1.0;
    if (mixMatch && std::fabs(ratio - 1.0) < 0.01) ratio = 1.0;  // whole frames at another rate
    std::vector<Clip> tmp;
    Walk(in, 0, inner, false, tmp, depth + 1);
    for (Clip& c : tmp)
    {
      c.pos = start + c.pos / ratio;
      c.len /= ratio;
      c.rate *= ratio;
      c.fadeIn /= ratio;
      c.fadeOut /= ratio;
      for (EnvPt& p : c.gainEnv) p.time /= ratio;
    }
    Trim(tmp, start, start + len);
    out.insert(out.end(), tmp.begin(), tmp.end());
    return;
  }

  size_t first = out.size();
  Walk(inputs[0], start, rate, trackLevel, out, depth + 1);
  if (amplitudes.empty() && !pan)
  {
    if (opName.find("Dissolve") == std::string::npos)
      Warn("Effect not converted: " + (opName.empty() ? AuidToString(opId) : opName));
    return;
  }

  std::vector<EnvPt> pts;
  double constant = 1;
  for (Obj amplitude : amplitudes)
  {
    if (!ParamPoints(amplitude, start, len, pts, constant)) continue;
    if (trackLevel && track_)
    {
      if (pts.empty()) track_->gain *= constant;
      else track_->volumeEnv.insert(track_->volumeEnv.end(), pts.begin(), pts.end());
    }
    else
    {
      for (size_t i = first; i < out.size(); i++)
      {
        Clip& c = out[i];
        if (pts.empty()) c.gain *= constant;
        else if (c.gainEnv.empty())
          for (const EnvPt& p : pts) c.gainEnv.push_back({p.time - c.pos, p.value, p.hold});
        else
          Warn("Nested clip gain automation was not converted");
      }
    }
  }
  double panValue = 0.5;
  if (pan && ParamPoints(pan, start, len, pts, panValue))
  {
    if (trackLevel && track_)
    {
      if (pts.empty()) track_->pan = panValue;
      else track_->panEnv.insert(track_->panEnv.end(), pts.begin(), pts.end());
    }
    else if (!pts.empty() || std::fabs(panValue - 0.5) > 1e-6)
      Warn("Clip pan is not converted (REAPER items have no pan automation)");
  }
}

void Importer::SourceClipLeaf(Obj sc, double start, AafRational rate, std::vector<Clip>& out, int depth)
{
  double len = Sec(r_.Int(sc, kComponentLength, 0), rate);
  if (len <= kTimeEps) return;
  MobId id = r_.MobIdValue(sc, kSourceID);
  uint32_t slotId = (uint32_t)r_.UInt(sc, kSourceMobSlotID);
  double startTime = Sec(r_.Int(sc, kSourceClipStartTime, 0), rate);
  Obj mob = FindMob(id);
  if (!mob)
  {
    if (id != MobId{}) Warn("Some clips refer to media that is not in the AAF file and were skipped");
    return;
  }
  if (r_.IsA(mob, "CompositionMob"))
  {
    // nested composition (subclip, adjusted clip, nested sequence)
    Obj slot = FindSlot(mob, slotId);
    if (!slot) return;
    AafRational nested = SlotRate(slot);
    std::vector<Clip> tmp;
    Track* saved = track_;
    track_ = nullptr;
    Walk(r_.Ref(slot, kSlotSegment), 0, nested, false, tmp, depth + 1);
    track_ = saved;
    Trim(tmp, startTime, startTime + len);
    std::string name = r_.String(mob, kMobName);
    // Media Composer: levels set on a (sub)clip in a bin are copied into the sequence when it is
    // edited in, so the bin clip's own gain must not be applied again.
    Auid usage = r_.AuidValue(mob, kMobUsageCode);
    bool binClip = usage == kUsageSubClip || usage == kUsageAdjustedClip;
    for (Clip& c : tmp)
    {
      c.pos += start - startTime;
      if (!name.empty()) c.name = name;
      if (binClip)
      {
        c.gain = 1;
        c.gainEnv.clear();
      }
    }
    out.insert(out.end(), tmp.begin(), tmp.end());
    return;
  }
  Resolved res;
  if (!Resolve(mob, slotId, startTime, res, depth + 1) || res.sources.empty())
  {
    Warn("Some clips have no audio file (only tape or import sources) and were skipped");
    return;
  }
  Clip c;
  c.pos = start;
  c.len = len;
  c.offset = res.offset;
  c.sources = res.sources;
  c.name = res.name;
  c.sourceName = res.name;
  c.sourceLength = res.length;
  out.push_back(c);
}

void Importer::Trim(std::vector<Clip>& clips, double a, double b)
{
  std::vector<Clip> kept;
  for (Clip c : clips)
  {
    if (c.pos < a)
    {
      double cut = a - c.pos;
      c.pos = a;
      c.len -= cut;
      c.offset += cut * c.rate;
      c.fadeIn = std::max(0.0, c.fadeIn - cut);
      for (EnvPt& p : c.gainEnv) p.time -= cut;
    }
    if (c.End() > b)
    {
      double cut = c.End() - b;
      c.len -= cut;
      c.fadeOut = std::max(0.0, c.fadeOut - cut);
    }
    if (c.len > kTimeEps) kept.push_back(c);
  }
  clips.swap(kept);
}

// ---------------------------------------------------------------------------------------------
// Sources

bool Importer::Resolve(Obj mob, uint32_t slotId, double t, Resolved& r, int depth)
{
  if (!mob || depth > 60) return false;
  if (r_.IsA(mob, "SourceMob"))
  {
    Obj desc = r_.Ref(mob, kSourceMobDescriptor);
    bool file = r_.IsA(desc, "FileDescriptor") || r_.IsA(desc, "MultipleDescriptor") ||
                essenceData_.count(r_.MobIdValue(mob, kMobID));
    if (!file) return false;  // tape, film or import source: the file is described elsewhere
    int e = GetEssence(mob);
    if (e < 0) return false;
    r.sources.push_back({e, ChannelOf(mob, FindSlot(mob, slotId), e)});
    r.offset = t;
    if (essences_[e]->name.empty()) essences_[e]->name = r.name;  // embedded essence of a named master clip
    if (r.name.empty()) r.name = essences_[e]->name;
    r.length = essences_[e]->length;
    return true;
  }
  Obj slot = FindSlot(mob, slotId);
  if (!slot) return false;
  if (r.name.empty() && r_.IsA(mob, "MasterMob")) r.name = r_.String(mob, kMobName);
  return ResolveSegment(r_.Ref(slot, kSlotSegment), SlotRate(slot), t, r, depth + 1);
}

bool Importer::ResolveSegment(Obj seg, AafRational rate, double t, Resolved& r, int depth)
{
  if (!seg || depth > 60) return false;
  if (r_.IsA(seg, "SourceClip"))
  {
    Obj target = FindMob(r_.MobIdValue(seg, kSourceID));
    if (!target) return false;
    return Resolve(target, (uint32_t)r_.UInt(seg, kSourceMobSlotID), t + Sec(r_.Int(seg, kSourceClipStartTime, 0), rate),
                   r, depth + 1);
  }
  if (r_.IsA(seg, "Sequence"))
  {
    std::vector<Obj> comps = r_.Refs(seg, kSequenceComponents);
    int64_t pos = 0;
    Obj last = nullptr;
    int64_t lastPos = 0;
    for (Obj c : comps)
    {
      int64_t len = r_.Int(c, kComponentLength, 0);
      if (r_.IsA(c, "Transition"))
      {
        pos -= len;
        continue;
      }
      if (!r_.IsA(c, "Filler"))
      {
        last = c;
        lastPos = pos;
        if (t < Sec(pos + len, rate) - kTimeEps) return ResolveSegment(c, rate, t - Sec(pos, rate), r, depth + 1);
      }
      pos += len;
    }
    return last && ResolveSegment(last, rate, t - Sec(lastPos, rate), r, depth + 1);
  }
  if (r_.IsA(seg, "EssenceGroup"))
  {
    std::vector<Obj> choices = r_.Refs(seg, kEssenceGroupChoices);
    return !choices.empty() && ResolveSegment(choices[0], rate, t, r, depth + 1);
  }
  if (r_.IsA(seg, "OperationGroup"))
  {
    std::vector<Obj> inputs = r_.Refs(seg, kOpGroupInputs);
    Auid opId{};
    r_.WeakRefKey(seg, kOpGroupOperation, opId);
    if (opId == kOpChannelCombiner || r_.DefinitionName(opId) == "Audio Channel Combiner")
    {
      bool any = false;
      for (Obj in : inputs)
      {
        Resolved sub;
        sub.name = r.name;
        if (!ResolveSegment(in, rate, t, sub, depth + 1)) continue;
        if (!any)
        {
          r.offset = sub.offset;
          r.name = sub.name;
          r.length = sub.length;
        }
        r.sources.insert(r.sources.end(), sub.sources.begin(), sub.sources.end());
        any = true;
      }
      return any;
    }
    return !inputs.empty() && ResolveSegment(inputs[0], rate, t, r, depth + 1);
  }
  return false;
}

int Importer::ChannelOf(Obj sourceMob, Obj slot, int essence)
{
  int n = essences_[essence]->channels;
  if (n <= 1 || !slot) return -1;
  std::vector<Obj> sound;
  for (Obj s : r_.Refs(sourceMob, kMobSlots))
    if (SlotKind(s) == 1) sound.push_back(s);
  if ((int)sound.size() == n)
    for (int i = 0; i < n; i++)
      if (sound[i] == slot) return i;
  int64_t track = r_.Int(slot, kSlotPhysicalTrack, 0);
  if (sound.size() == 1 && track >= 1 && track <= n)
  {
    // one source mob per channel of the same file (DaVinci Resolve)
    return (int)track - 1;
  }
  return -1;
}

int Importer::GetEssence(Obj mob)
{
  auto cached = essenceByMob_.find(mob);
  if (cached != essenceByMob_.end()) return cached->second;

  auto e = std::make_unique<Essence>();
  e->name = r_.String(mob, kMobName);
  Obj desc = r_.Ref(mob, kSourceMobDescriptor);
  Obj fileDesc = desc;
  if (r_.IsA(desc, "MultipleDescriptor"))
  {
    for (Obj sub : r_.Refs(desc, kMultipleDescriptors))
      if (r_.IsA(sub, "SoundDescriptor") || r_.IsA(sub, "WAVEDescriptor") || r_.IsA(sub, "AIFCDescriptor"))
      {
        fileDesc = sub;
        break;
      }
  }
  e->descriptor = fileDesc;
  AafRational sr = r_.Rational(fileDesc, kFileSampleRate);
  if (sr.num > 0 && sr.den > 0)
  {
    e->sampleRate = sr.Value();
    e->length = Sec(r_.Int(fileDesc, kFileLength, 0), sr);
  }
  e->channels = (int)r_.UInt(fileDesc, kSoundChannels, 0);
  // WAVE / AIFC descriptors keep the file header as "Summary"
  if (const AafReader::Prop* p = r_.Find(fileDesc, r_.IsA(fileDesc, "WAVEDescriptor") ? kWaveSummary : kAifcSummary))
  {
    const std::vector<uint8_t>& d = p->data;
    bool riff = d.size() >= 12 && memcmp(d.data(), "RIFF", 4) == 0;
    for (size_t pos = 12; pos + 8 <= d.size();)
    {
      uint32_t size = riff ? Le32(&d[pos + 4]) : Be32(&d[pos + 4]);
      if (riff && memcmp(&d[pos], "fmt ", 4) == 0 && pos + 16 <= d.size())
        e->channels = Le16(&d[pos + 10]);
      else if (!riff && memcmp(&d[pos], "COMM", 4) == 0 && pos + 10 <= d.size())
        e->channels = Be16(&d[pos + 8]);
      pos += 8 + size + (size & 1);
    }
  }

  MobId id = r_.MobIdValue(mob, kMobID);
  auto ed = essenceData_.find(id);
  if (ed != essenceData_.end())
  {
    e->embedded = true;
    e->essenceData = ed->second;
  }
  else
  {
    std::vector<Obj> locators = r_.Refs(desc, kDescriptorLocator);
    if (locators.empty() && fileDesc != desc) locators = r_.Refs(fileDesc, kDescriptorLocator);
    std::string url;
    for (Obj loc : locators)
    {
      url = r_.Has(loc, kNetworkLocatorUrl) ? r_.String(loc, kNetworkLocatorUrl) : r_.String(loc, kTextLocatorName);
      if (!url.empty()) break;
    }
    if (url.empty())
    {
      Warn("Some source files have no location and were skipped");
      essenceByMob_[mob] = -1;
      return -1;
    }
    e->path = LocateFile(url);
    if (e->path.empty())
    {
      e->path = UrlToPath(url);
      Warn("Media file not found (relink it in REAPER): " + e->path);
    }
    else
    {
      MediaInfo info;
      if (ProbeAudioFile(e->path, info) && info.channels > 0)
      {
        e->channels = info.channels;
        e->sampleRate = info.sampleRate;
        e->length = info.duration;
      }
    }
    std::string ext = LowerExtension(e->path);
    if (ext == "mxf") Warn("MXF media is referenced as is; REAPER may not be able to play it");
    std::string key = ToLower(NativeSeparators(e->path));
    auto same = essenceByPath_.find(key);
    if (same != essenceByPath_.end())
    {
      essenceByMob_[mob] = same->second;
      return same->second;
    }
    essenceByPath_[key] = (int)essences_.size();
  }
  if (e->name.empty()) e->name = FileStem(e->path);
  int index = (int)essences_.size();
  essences_.push_back(std::move(e));
  essenceByMob_[mob] = index;
  return index;
}

std::string Importer::LocateFile(const std::string& url)
{
  std::string p = UrlToPath(url);
  if (!p.empty() && FileExists(p)) return p;
  std::vector<std::string> parts = PathParts(p);
  if (parts.empty()) return std::string();
  // relative to the AAF file: its folder plus the last 1..4 components of the original path
  std::string dir = DirName(path_);
  for (size_t k = 1; k <= std::min<size_t>(4, parts.size()); k++)
  {
    std::string c = dir;
    for (size_t i = parts.size() - k; i < parts.size(); i++) c = JoinPath(c, parts[i]);
    c = NativeSeparators(c);
    if (FileExists(c)) return c;
  }
  // anywhere below the AAF file's folder (and the media folder)
  std::string found;
  int budget = 20000;
  if (SearchFile(dir, ToLower(parts.back()), 0, budget, found)) return found;
  return std::string();
}

bool Importer::SearchFile(const std::string& dir, const std::string& lowerName, int depth, int& budget,
                          std::string& found)
{
  std::vector<std::string> names;
  std::vector<bool> dirs;
  if (depth > 4 || budget <= 0 || !ListDirectory(dir, names, dirs)) return false;
  budget -= (int)names.size();
  for (size_t i = 0; i < names.size(); i++)
    if (!dirs[i] && ToLower(names[i]) == lowerName)
    {
      found = NativeSeparators(JoinPath(dir, names[i]));
      return true;
    }
  for (size_t i = 0; i < names.size(); i++)
    if (dirs[i] && SearchFile(JoinPath(dir, names[i]), lowerName, depth + 1, budget, found)) return true;
  return false;
}

bool Importer::EnsureMediaDir()
{
  if (!mediaDir_.empty()) return true;
  if (mediaDirFailed_) return false;
  std::string stem = FileStem(path_);
  std::vector<std::string> candidates;
  if (!options_.mediaDir.empty()) candidates.push_back(options_.mediaDir);
  candidates.push_back(JoinPath(DirName(path_), stem + "-media"));
  candidates.push_back(JoinPath(JoinPath(TempDirectory(), "aaf-import"), stem));
  for (const std::string& candidate : candidates)
  {
    std::string c = NativeSeparators(candidate);
    if (CreateDirectories(c))
    {
      mediaDir_ = c;
      report_.mediaDir = c;
      return true;
    }
  }
  mediaDirFailed_ = true;
  Warn("Could not create a folder for the extracted audio files");
  return false;
}

std::string Importer::UniqueMediaPath(const std::string& stem, const std::string& ext, uint64_t size)
{
  std::string safe;
  for (char c : stem) safe += (strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) ? '_' : c;
  while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) safe.pop_back();
  if (safe.empty()) safe = "audio";
  std::string name = safe + "." + ext;
  for (int n = 2;; n++)
  {
    int64_t existing = FileSize(JoinPath(mediaDir_, name));
    if (!usedMediaNames_.count(ToLower(name)) && (existing < 0 || existing == (int64_t)size)) break;
    name = safe + "_" + std::to_string(n) + "." + ext;
  }
  usedMediaNames_.insert(ToLower(name));
  return NativeSeparators(JoinPath(mediaDir_, name));
}

// Writes embedded essence to the media folder: complete WAV/AIFF files are copied, raw PCM gets a
// WAV header (from the descriptor).
std::string Importer::ExtractEmbedded(Essence& e)
{
  int entry = r_.StreamEntry(e.essenceData, kEssenceDataData);
  if (entry < 0)
  {
    Warn("Embedded audio data is missing");
    return std::string();
  }
  if (!EnsureMediaDir()) return std::string();
  const CfbReader& cfb = r_.Cfb();
  uint64_t size = cfb.Get(entry).size;
  uint8_t head[12] = {};
  if (size >= 12) cfb.ReadStreamRange(entry, 0, 12, head);
  std::vector<uint8_t> header;
  std::string ext;
  bool whole = false;
  if (memcmp(head, "RIFF", 4) == 0) ext = "wav", whole = true;
  else if (memcmp(head, "FORM", 4) == 0) ext = "aif", whole = true;
  else if (r_.IsA(e.descriptor, "WAVEDescriptor"))
  {
    // the Summary holds the RIFF header with the fmt chunk
    const AafReader::Prop* p = r_.Find(e.descriptor, kWaveSummary);
    if (!p) return std::string();
    const std::vector<uint8_t>& d = p->data;
    for (size_t pos = 12; pos + 8 <= d.size();)
    {
      uint32_t csize = Le32(&d[pos + 4]);
      if (memcmp(&d[pos], "fmt ", 4) == 0 && pos + 8 + csize <= d.size())
      {
        header = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
        header.insert(header.end(), d.begin() + pos, d.begin() + pos + 8 + csize);
        header.insert(header.end(), {'d', 'a', 't', 'a'});
        Put32(header, (uint32_t)size);
        uint32_t riffSize = (uint32_t)(header.size() - 8 + size);
        for (int b = 0; b < 4; b++) header[4 + b] = (uint8_t)(riffSize >> (8 * b));
        break;
      }
      pos += 8 + csize + (csize & 1);
    }
    ext = "wav";
  }
  else if (r_.IsA(e.descriptor, "AIFCDescriptor"))
  {
    const AafReader::Prop* p = r_.Find(e.descriptor, kAifcSummary);
    if (!p) return std::string();
    const std::vector<uint8_t>& d = p->data;
    for (size_t pos = 12; pos + 8 <= d.size();)
    {
      uint32_t csize = Be32(&d[pos + 4]);
      if (memcmp(&d[pos], "COMM", 4) == 0 && pos + 8 + csize <= d.size())
      {
        header = {'F', 'O', 'R', 'M', 0, 0, 0, 0, d[8], d[9], d[10], d[11]};
        header.insert(header.end(), d.begin() + pos, d.begin() + pos + 8 + csize);
        if (csize & 1) header.push_back(0);
        uint32_t ssndSize = (uint32_t)(size + 8);
        header.insert(header.end(), {'S', 'S', 'N', 'D', (uint8_t)(ssndSize >> 24), (uint8_t)(ssndSize >> 16),
                                     (uint8_t)(ssndSize >> 8), (uint8_t)ssndSize, 0, 0, 0, 0, 0, 0, 0, 0});
        uint32_t formSize = (uint32_t)(header.size() - 8 + size);
        header[4] = (uint8_t)(formSize >> 24);
        header[5] = (uint8_t)(formSize >> 16);
        header[6] = (uint8_t)(formSize >> 8);
        header[7] = (uint8_t)formSize;
        break;
      }
      pos += 8 + csize + (csize & 1);
    }
    ext = "aif";
  }
  else if (r_.IsA(e.descriptor, "SoundDescriptor"))
  {
    int channels = (int)r_.UInt(e.descriptor, kSoundChannels, 1);
    int bits = (int)r_.UInt(e.descriptor, kSoundBits, 16);
    AafRational sr = r_.Rational(e.descriptor, kSoundSamplingRate);
    if (sr.num <= 0) sr = r_.Rational(e.descriptor, kFileSampleRate);
    if (channels <= 0 || bits <= 0 || sr.num <= 0 || sr.den <= 0) return std::string();
    header = WavHeader(channels, (uint32_t)std::lround(sr.Value()), bits, false, size);
    ext = "wav";
  }
  if (!whole && header.empty())
  {
    Warn("Embedded audio in an unsupported format was skipped");
    return std::string();
  }

  uint64_t total = (whole ? 0 : header.size()) + size;
  std::string dest = UniqueMediaPath(e.name, ext, total);
  if (FileSize(dest) == (int64_t)total) return dest;  // extracted by an earlier import
  FILE* f = OpenFile(dest, "wb");
  if (!f)
  {
    Warn("Could not write extracted audio: " + dest);
    return std::string();
  }
  bool ok = header.empty() || fwrite(header.data(), 1, header.size(), f) == header.size();
  std::vector<uint8_t> buf(1 << 20);
  for (uint64_t pos = 0; ok && pos < size;)
  {
    size_t n = (size_t)std::min<uint64_t>(buf.size(), size - pos);
    ok = cfb.ReadStreamRange(entry, pos, n, buf.data()) && fwrite(buf.data(), 1, n, f) == n;
    pos += n;
  }
  ok = fclose(f) == 0 && ok;
  if (!ok)
  {
    RemoveFile(dest);
    Warn("Could not extract embedded audio: " + e.name);
    return std::string();
  }
  return dest;
}

std::string Importer::EssencePath(int index)
{
  Essence& e = *essences_[index];
  if (e.embedded && !e.extracted)
  {
    e.extracted = true;
    e.path = ExtractEmbedded(e);
    MediaInfo info;
    if (!e.path.empty() && ProbeAudioFile(e.path, info) && info.channels > 0)
    {
      e.channels = info.channels;
      e.sampleRate = info.sampleRate;
      e.length = info.duration;
    }
  }
  return e.path;
}

// Interleaves the given channels of several files into one WAV file (Pro Tools and Media
// Composer keep each channel of a stereo or surround clip in its own mono file).
std::string Importer::MergeChannels(const std::vector<Source>& sources, const std::string& clipName)
{
  auto cached = merged_.find(sources);
  if (cached != merged_.end()) return cached->second;
  merged_[sources] = std::string();

  std::vector<std::unique_ptr<PcmFile>> files;
  std::vector<int> fileOf, channelOf;
  std::map<int, int> byEssence;
  for (const Source& s : sources)
  {
    auto it = byEssence.find(s.essence);
    if (it == byEssence.end())
    {
      std::string p = EssencePath(s.essence);
      auto f = std::make_unique<PcmFile>();
      if (p.empty() || !FileExists(p) || !OpenPcm(p, *f)) return std::string();
      it = byEssence.emplace(s.essence, (int)files.size()).first;
      files.push_back(std::move(f));
    }
    const PcmFile& f = *files[it->second];
    int ch = s.channel < 0 ? 0 : s.channel;
    if (ch >= f.channels) return std::string();
    fileOf.push_back(it->second);
    channelOf.push_back(ch);
  }
  const PcmFile& ref = *files[0];
  uint64_t frames = 0;
  for (const auto& f : files)
  {
    if (f->sampleRate != ref.sampleRate || f->bits != ref.bits || f->isFloat != ref.isFloat) return std::string();
    frames = std::max(frames, f->frames);
  }
  int bytes = ref.bits / 8;
  int outChannels = (int)sources.size();
  uint64_t dataBytes = frames * outChannels * bytes;
  if (dataBytes > 0xFFFFFFF0ull)
  {
    Warn("A multichannel clip is too long to be merged into one WAV file");
    return std::string();
  }
  if (!EnsureMediaDir()) return std::string();

  std::string stem = clipName.empty() ? essences_[sources[0].essence]->name : clipName;
  size_t dot = stem.rfind('.');
  if (dot != std::string::npos && dot > 0 && stem.size() - dot <= 4) stem = stem.substr(0, dot);  // "name.L"
  std::vector<uint8_t> header = WavHeader(outChannels, ref.sampleRate, ref.bits, ref.isFloat, dataBytes);
  std::string dest = UniqueMediaPath(stem + " (" + std::to_string(outChannels) + "ch)", "wav", header.size() + dataBytes);
  if (FileSize(dest) == (int64_t)(header.size() + dataBytes))
  {
    merged_[sources] = dest;
    return dest;
  }
  FILE* out = OpenFile(dest, "wb");
  if (!out) return std::string();
  bool ok = fwrite(header.data(), 1, header.size(), out) == header.size();
  const uint64_t block = 16384;
  std::vector<std::vector<uint8_t>> in(files.size());
  std::vector<uint8_t> buf;
  for (uint64_t pos = 0; ok && pos < frames; pos += block)
  {
    uint64_t n = std::min(block, frames - pos);
    for (size_t i = 0; i < files.size(); i++)
    {
      PcmFile& f = *files[i];
      in[i].assign((size_t)(n * f.FrameBytes()), 0);
      if (pos < f.frames)
      {
        size_t want = (size_t)(std::min(n, f.frames - pos) * f.FrameBytes());
        if (!SeekFile(f.f, (int64_t)(f.dataOffset + pos * f.FrameBytes())) || fread(in[i].data(), 1, want, f.f) != want)
          ok = false;
      }
    }
    buf.resize((size_t)(n * outChannels * bytes));
    for (uint64_t fr = 0; fr < n; fr++)
      for (int c = 0; c < outChannels; c++)
      {
        const PcmFile& f = *files[fileOf[c]];
        const uint8_t* src = &in[fileOf[c]][(size_t)(fr * f.FrameBytes() + channelOf[c] * bytes)];
        uint8_t* dst = &buf[(size_t)((fr * outChannels + c) * bytes)];
        for (int b = 0; b < bytes; b++) dst[b] = f.bigEndian ? src[bytes - 1 - b] : src[b];
      }
    ok = ok && fwrite(buf.data(), 1, buf.size(), out) == buf.size();
  }
  ok = fclose(out) == 0 && ok;
  if (!ok)
  {
    RemoveFile(dest);
    return std::string();
  }
  merged_[sources] = dest;
  return dest;
}

// ---------------------------------------------------------------------------------------------
// Pro Tools

bool Importer::RemoveSampleAccurateEdit(std::vector<Clip>& clips, size_t i)
{
  const Clip& s = clips[i];
  if (i > 0)
  {
    Clip& l = clips[i - 1];
    if (Near(l.End(), s.pos) && l.sourceLength > 0 && l.sourceLength - l.offset - l.len * l.rate >= s.len * l.rate - kTimeEps)
    {
      l.len += s.len;
      clips.erase(clips.begin() + i);
      return true;
    }
  }
  if (i + 1 < clips.size())
  {
    Clip& r = clips[i + 1];
    if (Near(s.End(), r.pos) && r.offset >= s.len * r.rate - kTimeEps)
    {
      double l = s.len;
      r.pos -= l;
      r.len += l;
      r.offset = std::max(0.0, r.offset - l * r.rate);
      clips.erase(clips.begin() + i);
      return true;
    }
  }
  return false;
}

bool Importer::ReplaceRenderedFade(std::vector<Clip>& clips, size_t i)
{
  // Neighbours touching the fade clip; a "sample accurate edit" in between is merged away first.
  int prev = -1, next = -1;
  if (i > 0 && Near(clips[i - 1].End(), clips[i].pos))
  {
    if (IsSampleAccurateEditName(clips[i - 1].sourceName))
    {
      size_t before = clips.size();
      if (RemoveSampleAccurateEdit(clips, i - 1) && clips.size() < before)
      {
        i--;  // the fade clip moved down
        if (i > 0 && Near(clips[i - 1].End(), clips[i].pos)) prev = (int)i - 1;
      }
    }
    else
      prev = (int)i - 1;
  }
  if (i + 1 < clips.size() && Near(clips[i].End(), clips[i + 1].pos))
  {
    if (IsSampleAccurateEditName(clips[i + 1].sourceName))
    {
      size_t before = clips.size();
      if (RemoveSampleAccurateEdit(clips, i + 1) && clips.size() < before)
      {
        // merged into the fade clip (left) or into the clip after it (now at i + 1)
        if (i + 1 < clips.size() && Near(clips[i].End(), clips[i + 1].pos)) next = (int)i + 1;
      }
    }
    else
      next = (int)i + 1;
  }
  if (prev < 0 && next < 0) return false;
  double len = clips[i].len;
  if (prev >= 0)
  {
    const Clip& p = clips[prev];
    if (!(p.sourceLength > 0 && p.sourceLength - p.offset - p.len * p.rate >= len * p.rate - kTimeEps))
    {
      Warn("Some Pro Tools fades were kept as rendered clips (not enough handle to make a real fade)");
      return false;
    }
  }
  if (next >= 0 && clips[next].offset < len * clips[next].rate - kTimeEps)
  {
    Warn("Some Pro Tools fades were kept as rendered clips (not enough handle to make a real fade)");
    return false;
  }
  if (prev >= 0)
  {
    Clip& p = clips[prev];
    p.len += len;
    p.fadeOut = len;
    p.fadeOutShape = 0;
  }
  if (next >= 0)
  {
    Clip& n = clips[next];
    n.pos -= len;
    n.len += len;
    n.offset = std::max(0.0, n.offset - len * n.rate);
    n.fadeIn = len;
    n.fadeInShape = 0;
  }
  clips.erase(clips.begin() + i);
  return true;
}

void Importer::ProToolsCleanup(std::vector<Clip>& clips)
{
  std::stable_sort(clips.begin(), clips.end(), [](const Clip& a, const Clip& b) { return a.pos < b.pos; });
  for (size_t i = 0; i < clips.size();)
  {
    size_t before = clips.size();
    if (aafOptions_.proToolsTrueFades && IsRenderedFadeName(clips[i].sourceName)) ReplaceRenderedFade(clips, i);
    else if (aafOptions_.proToolsRemoveSampleAccurateEdits && IsSampleAccurateEditName(clips[i].sourceName))
      RemoveSampleAccurateEdit(clips, i);
    if (clips.size() != before) i = 0;  // start over, neighbours changed
    else i++;
  }
}

// ---------------------------------------------------------------------------------------------
// Output

void Importer::ReadMarkers(Obj comp)
{
  uint16_t userPid = r_.PidByName("CommentMarkerUser");
  if (!userPid) userPid = r_.PidByName("CommentMarkerUSer");
  uint16_t colorPid = r_.PidByName("CommentMarkerColor");
  for (Obj slot : r_.Refs(comp, kMobSlots))
  {
    // event slots, or timeline slots holding markers (Media Composer's marker on a timecode track)
    AafRational rate = SlotRate(slot);
    Obj seg = r_.Ref(slot, kSlotSegment);
    std::vector<Obj> events = r_.IsA(seg, "Sequence") ? r_.Refs(seg, kSequenceComponents) : std::vector<Obj>{seg};
    for (Obj ev : events)
    {
      if (!r_.IsA(ev, "Event")) continue;
      MarkerInfo m;
      m.pos = Sec(r_.Int(ev, kEventPosition, 0), rate);
      m.len = Sec(r_.Int(ev, kComponentLength, 0), rate);
      std::string comment = r_.String(ev, kEventComment);
      std::string label = userPid ? r_.String(ev, userPid) : std::string();
      // Resolve stores the marker name as "user"; Media Composer the user who made it.
      m.name = resolve_ ? (label.empty() ? comment : label) : (comment.empty() ? label : comment);
      if (const AafReader::Prop* p = colorPid ? r_.Find(ev, colorPid) : nullptr) m.color = ColorFromRgb16(p->data);
      markers_.push_back(m);
    }
  }
}

std::string Importer::BuildXml(Obj comp)
{
  pugi::xml_document doc;
  xml_node project = doc.append_child("Project");
  project.append_attribute("version") = "1.0";
  if (compStart_ != 0) project.append_attribute("xTimeOffset") = FormatDouble(compStart_).c_str();
  xml_node app = project.append_child("Application");
  app.append_attribute("name") = product_.c_str();
  app.append_attribute("version") = version_.c_str();

  xml_node meta = project.append_child("xMetaData");
  std::string title = r_.String(comp, kMobName);
  if (!title.empty()) meta.append_child("Title").text() = title.c_str();
  std::string comment;
  for (Obj tv : r_.Refs(comp, kMobUserComments))
  {
    std::string value;
    if (!r_.IndirectString(tv, kTaggedValue, value) || value.empty()) continue;
    if (!comment.empty()) comment += "\n";
    comment += r_.String(tv, kTaggedName) + ": " + value;
  }
  if (!comment.empty()) meta.append_child("Comment").text() = comment.c_str();

  xml_node structure = project.append_child("Structure");
  xml_node arrangement = project.append_child("Arrangement");
  xml_node lanes = arrangement.append_child("Lanes");
  lanes.append_attribute("timeUnit") = "seconds";

  auto points = [&](xml_node parent, const std::vector<EnvPt>& env, const std::string& param, const char* unit) {
    if (env.empty()) return;
    xml_node pts = parent.append_child("Points");
    pts.append_attribute("unit") = unit;
    pts.append_child("Target").append_attribute("parameter") = param.c_str();
    for (const EnvPt& p : env)
    {
      xml_node pt = pts.append_child("RealPoint");
      pt.append_attribute("time") = FormatDouble(std::max(0.0, p.time)).c_str();
      pt.append_attribute("value") = FormatDouble(p.value).c_str();
      pt.append_attribute("interpolation") = p.hold ? "hold" : "linear";
    }
  };

  if (master_.gain != 1 || !master_.volumeEnv.empty() || master_.pan != 0.5 || !master_.panEnv.empty())
  {
    xml_node track = structure.append_child("Track");
    track.append_attribute("contentType") = "audio";
    track.append_attribute("loaded") = "true";
    track.append_attribute("id") = "tm";
    track.append_attribute("name") = "Master";
    xml_node channel = track.append_child("Channel");
    channel.append_attribute("role") = "master";
    channel.append_attribute("audioChannels") = 2;
    channel.append_attribute("id") = "cm";
    xml_node pan = channel.append_child("Pan");
    pan.append_attribute("value") = FormatDouble(master_.pan).c_str();
    pan.append_attribute("unit") = "normalized";
    pan.append_attribute("id") = "pm";
    xml_node vol = channel.append_child("Volume");
    vol.append_attribute("value") = FormatDouble(master_.gain).c_str();
    vol.append_attribute("unit") = "linear";
    vol.append_attribute("id") = "vm";
    xml_node tl = lanes.append_child("Lanes");
    tl.append_attribute("track") = "tm";
    points(tl, master_.volumeEnv, "vm", "linear");
    points(tl, master_.panEnv, "pm", "normalized");
  }

  for (size_t ti = 0; ti < tracks_.size(); ti++)
  {
    Track& t = tracks_[ti];
    std::string n = std::to_string(ti + 1);
    xml_node track = structure.append_child("Track");
    track.append_attribute("contentType") = "audio";
    track.append_attribute("loaded") = "true";
    track.append_attribute("id") = ("t" + n).c_str();
    track.append_attribute("name") = t.name.c_str();
    xml_node channel = track.append_child("Channel");
    channel.append_attribute("role") = "regular";
    channel.append_attribute("audioChannels") = 1;
    channel.append_attribute("id") = ("c" + n).c_str();
    channel.append_attribute("solo") = t.solo ? "true" : "false";
    xml_node mute = channel.append_child("Mute");
    mute.append_attribute("value") = t.mute ? "true" : "false";
    mute.append_attribute("id") = ("m" + n).c_str();
    xml_node pan = channel.append_child("Pan");
    pan.append_attribute("value") = FormatDouble(t.pan).c_str();
    pan.append_attribute("unit") = "normalized";
    pan.append_attribute("id") = ("p" + n).c_str();
    xml_node vol = channel.append_child("Volume");
    vol.append_attribute("value") = FormatDouble(t.gain).c_str();
    vol.append_attribute("unit") = "linear";
    vol.append_attribute("id") = ("v" + n).c_str();

    xml_node tl = lanes.append_child("Lanes");
    tl.append_attribute("track") = ("t" + n).c_str();
    xml_node clipsNode = tl.append_child("Clips");
    int maxChannels = 1;
    for (const Clip& c : t.clips)
    {
      // what to play: one file (all channels or one of them), a merged file, or one item per channel
      struct Part
      {
        std::string file;
        int channel;
      };
      std::vector<Part> parts;
      int channels = 0;
      std::vector<Source> srcs = c.sources;
      bool sameFile = std::all_of(srcs.begin(), srcs.end(), [&](const Source& s) { return s.essence == srcs[0].essence; });
      std::string file0 = EssencePath(srcs[0].essence);
      int fileChannels = essences_[srcs[0].essence]->channels;
      bool inOrder = sameFile && (int)srcs.size() == fileChannels;
      for (size_t k = 0; inOrder && k < srcs.size(); k++) inOrder = srcs[k].channel == (int)k;
      if (srcs.size() == 1 && (srcs[0].channel < 0 || fileChannels <= 1))
      {
        parts.push_back({file0, -1});
        channels = std::max(1, fileChannels);
      }
      else if (srcs.size() == 1)
      {
        parts.push_back({file0, srcs[0].channel});
        channels = 1;
      }
      else if (inOrder)
      {
        parts.push_back({file0, -1});
        channels = fileChannels;
      }
      else
      {
        std::string merged = MergeChannels(srcs, c.sourceName);
        if (!merged.empty())
        {
          parts.push_back({merged, -1});
          channels = (int)srcs.size();
        }
        else
        {
          Warn("Some multichannel clips could not be merged into one file (media missing or in different "
               "formats); each channel became its own item");
          for (const Source& s : srcs) parts.push_back({EssencePath(s.essence), s.channel});
          channels = 1;
        }
      }
      maxChannels = std::max(maxChannels, channels);
      for (const Part& part : parts)
      {
        if (part.file.empty()) continue;
        xml_node clip = clipsNode.append_child("Clip");
        clip.append_attribute("time") = FormatDouble(c.pos).c_str();
        clip.append_attribute("duration") = FormatDouble(c.len).c_str();
        clip.append_attribute("name") = c.name.c_str();
        if (c.mute) clip.append_attribute("enable") = "false";
        if (c.fadeIn > 0) clip.append_attribute("fadeInTime") = FormatDouble(c.fadeIn).c_str();
        if (c.fadeOut > 0) clip.append_attribute("fadeOutTime") = FormatDouble(c.fadeOut).c_str();
        if (c.fadeInShape) clip.append_attribute("xFadeInShape") = c.fadeInShape;
        if (c.fadeOutShape) clip.append_attribute("xFadeOutShape") = c.fadeOutShape;
        if (c.gain != 1) clip.append_attribute("xGain") = FormatDouble(c.gain).c_str();
        if (!c.gainEnv.empty())
        {
          xml_node env = clip.append_child("xGainEnvelope");
          for (const EnvPt& p : c.gainEnv)
          {
            xml_node pt = env.append_child("Point");
            pt.append_attribute("time") = FormatDouble(p.time).c_str();
            pt.append_attribute("value") = FormatDouble(p.value).c_str();
            pt.append_attribute("shape") = p.hold ? 1 : 0;
          }
        }
        xml_node audio;
        if (std::fabs(c.rate - 1.0) > 1e-9)
        {
          // speed change: play the source range [offset, offset + len * rate] in len seconds
          clip.append_attribute("playStart") = "0";
          xml_node warps = clip.append_child("Warps");
          warps.append_attribute("contentTimeUnit") = "seconds";
          audio = warps.append_child("Audio");
          audio.append_attribute("algorithm") = "repitch";
          xml_node w0 = warps.append_child("Warp");
          w0.append_attribute("time") = "0";
          w0.append_attribute("contentTime") = FormatDouble(c.offset).c_str();
          xml_node w1 = warps.append_child("Warp");
          w1.append_attribute("time") = FormatDouble(c.len).c_str();
          w1.append_attribute("contentTime") = FormatDouble(c.offset + c.len * c.rate).c_str();
        }
        else
        {
          clip.append_attribute("playStart") = FormatDouble(c.offset).c_str();
          audio = clip.append_child("Audio");
        }
        if (part.channel >= 0) audio.append_attribute("xChannel") = part.channel;
        xml_node file = audio.append_child("File");
        file.append_attribute("path") = part.file.c_str();
        file.append_attribute("external") = "true";
      }
    }
    channel.attribute("audioChannels").set_value(maxChannels);

    points(tl, t.volumeEnv, "v" + n, "linear");
    points(tl, t.panEnv, "p" + n, "normalized");
  }

  if (!markers_.empty())
  {
    xml_node markers = arrangement.append_child("Markers");
    markers.append_attribute("timeUnit") = "seconds";
    for (const MarkerInfo& m : markers_)
    {
      xml_node mk = markers.append_child("Marker");
      mk.append_attribute("time") = FormatDouble(m.pos).c_str();
      mk.append_attribute("name") = m.name.c_str();
      if (!m.color.empty()) mk.append_attribute("color") = m.color.c_str();
      if (m.len > 0) mk.append_attribute("xDuration") = FormatDouble(m.len).c_str();
    }
  }

  struct Writer : pugi::xml_writer
  {
    std::string s;
    void write(const void* data, size_t size) override { s.append((const char*)data, size); }
  } w;
  doc.save(w, "  ");
  return w.s;
}

bool Importer::Build(std::string& xml, std::string& error)
{
  if (!r_.Open(path_, error)) return false;
  for (Obj id : r_.Refs(r_.Header(), kHeaderIdentificationList))
  {
    company_ = r_.String(id, kIdentCompany);
    product_ = r_.String(id, kIdentProduct);
    version_ = r_.String(id, kIdentVersionString);
  }
  proTools_ = company_ == "Digidesign, Inc." && product_ == "ProTools";
  resolve_ = product_.find("Resolve") != std::string::npos;

  IndexMobs();
  Obj comp = ChooseComposition();
  if (!comp)
  {
    error = "The AAF file contains no composition";
    return false;
  }

  // composition start: the (first) timecode track
  std::vector<Obj> slots = r_.Refs(comp, kMobSlots);
  Obj tcSlot = nullptr;
  for (Obj s : slots)
  {
    if (!r_.IsA(s, "TimelineMobSlot") || SlotKind(s) != 3) continue;
    Obj seg = r_.Ref(s, kSlotSegment);
    if (r_.IsA(seg, "Sequence"))
    {
      std::vector<Obj> c = r_.Refs(seg, kSequenceComponents);
      seg = c.empty() ? nullptr : c[0];
    }
    if (!r_.IsA(seg, "Timecode")) continue;
    if (!tcSlot || r_.Int(s, kSlotPhysicalTrack, 0) == 1)
    {
      compStart_ = Sec(r_.Int(seg, kTimecodeStart, 0), SlotRate(s));
      if (r_.Int(s, kSlotPhysicalTrack, 0) == 1 && tcSlot) break;
      tcSlot = s;
    }
  }

  int audioIndex = 0;
  for (Obj s : slots)
  {
    if (!r_.IsA(s, "TimelineMobSlot")) continue;
    int kind = SlotKind(s);
    if (kind == 2)
    {
      Obj seg = r_.Ref(s, kSlotSegment);
      if (!r_.IsA(seg, "Sequence") || !r_.Refs(seg, kSequenceComponents).empty()) videoSlots_++;
      continue;
    }
    if (kind == 4)
    {
      // master fader: only its level and automation matter
      std::vector<Clip> none;
      track_ = &master_;
      Walk(r_.Ref(s, kSlotSegment), 0, SlotRate(s), true, none, 0);
      track_ = nullptr;
      continue;
    }
    if (kind != 1) continue;
    audioIndex++;
    tracks_.emplace_back();
    Track& t = tracks_.back();
    t.name = r_.String(s, kSlotName);
    if (t.name.empty())
    {
      int64_t n = r_.Int(s, kSlotPhysicalTrack, 0);
      t.name = "A" + std::to_string(n > 0 ? n : audioIndex);
    }
    for (Obj tv : TaggedValues(s, "TimelineMobAttributeList"))
    {
      double v;
      std::string name = r_.String(tv, kTaggedName);
      if (!r_.IndirectNumber(tv, kTaggedValue, v)) continue;
      if (name == "AudioMixerCompSolo") t.solo = v != 0;
      else if (name == "AudioMixerCompMute") t.mute = v != 0;
    }
    track_ = &t;
    Walk(r_.Ref(s, kSlotSegment), 0, SlotRate(s), true, t.clips, 0);
    track_ = nullptr;
    std::stable_sort(t.clips.begin(), t.clips.end(), [](const Clip& a, const Clip& b) { return a.pos < b.pos; });
    if (proTools_) ProToolsCleanup(t.clips);
    t.clips.erase(std::remove_if(t.clips.begin(), t.clips.end(), [](const Clip& c) { return c.sources.empty(); }),
                  t.clips.end());
  }
  if (videoSlots_) Warn("Video tracks were not imported");
  ReadMarkers(comp);

  xml = BuildXml(comp);
  for (auto& w : warnings_)
    report_.warnings.push_back(w.second > 1 ? w.first + " (" + std::to_string(w.second) + "x)" : w.first);
  return true;
}

}  // namespace

bool AafToProjectXml(const std::string& path, const ConvertOptions& options, const AafImportOptions& aafOptions,
                     std::string& xml, ConvertReport& report, std::string& error)
{
  Importer imp(path, options, aafOptions, report);
  return imp.Build(xml, error);
}

bool ConvertAaf(const std::string& path, RppSink& sink, const ConvertOptions& options, ConvertReport& report,
                std::string& error, const AafImportOptions& aafOptions)
{
  std::string xml;
  ConvertReport importReport;
  if (!AafToProjectXml(path, options, aafOptions, xml, importReport, error)) return false;
  if (!ConvertProjectXml(xml, path, sink, options, report, error)) return false;
  report.warnings.insert(report.warnings.begin(), importReport.warnings.begin(), importReport.warnings.end());
  if (!importReport.mediaDir.empty()) report.mediaDir = importReport.mediaDir;
  return true;
}

}  // namespace dawp
