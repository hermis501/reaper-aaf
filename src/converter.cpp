// Derived from the DAWproject converter of reaper-dawproject: renders
// the DAWproject-style XML that aaf_import.cpp builds into RPP. Plug-ins, built-in devices, the
// .dawproject container and the post-load steps are left out; AAF carries none of them.

#include "converter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>

#include "pugixml.hpp"
#include "tempo_map.h"
#include "util.h"

namespace dawp {
namespace {

using pugi::xml_node;

const double kInf = std::numeric_limits<double>::infinity();
const double kEps = 1e-9;
const int kNoTrack = -2;
const int kMasterTrack = -1;
const int kTicksPerQN = 960;

// ---------------------------------------------------------------------------------------------
// XML helpers

bool Is(xml_node n, const char* name) { return strcmp(n.name(), name) == 0; }

bool GetDouble(xml_node n, const char* name, double& out)
{
  pugi::xml_attribute a = n.attribute(name);
  return a && ParseDouble(a.value(), out);
}

double AttrDouble(xml_node n, const char* name, double def)
{
  double v;
  return GetDouble(n, name, v) ? v : def;
}

bool AttrBool(xml_node n, const char* name, bool def)
{
  pugi::xml_attribute a = n.attribute(name);
  if (!a) return def;
  std::string v = ToLower(a.value());
  return v == "true" || v == "1";
}

// -1 = not set, 0 = seconds, 1 = beats
int AttrUnit(xml_node n, const char* name)
{
  pugi::xml_attribute a = n.attribute(name);
  if (!a) return -1;
  return strcmp(a.value(), "seconds") == 0 ? 0 : 1;
}

bool IsTimelineElement(xml_node n)
{
  static const char* names[] = {"Lanes", "Clips", "Notes", "Points", "Audio", "Video", "Warps", "Markers", "markers", "ClipSlot", "Timeline"};
  for (const char* s : names)
    if (Is(n, s)) return true;
  return false;
}

xml_node FirstTimelineChild(xml_node n)
{
  for (xml_node c : n.children())
    if (IsTimelineElement(c)) return c;
  return xml_node();
}

bool HasContentType(xml_node track, const char* type)
{
  std::string types = track.attribute("contentType").value();
  size_t pos = 0;
  while (pos < types.size())
  {
    size_t end = types.find_first_of(" ,", pos);
    if (end == std::string::npos) end = types.size();
    if (types.compare(pos, end - pos, type) == 0) return true;
    pos = end + 1;
  }
  return false;
}

uint32_t ReaperColor(const char* hex)
{
  if (!hex || !*hex) return 0;
  if (*hex == '#') hex++;
  if (strlen(hex) < 6) return 0;
  unsigned v[3];
  for (int i = 0; i < 3; i++)
  {
    char buf[3] = {hex[2 * i], hex[2 * i + 1], 0};
    char* end;
    v[i] = (unsigned)strtoul(buf, &end, 16);
    if (*end) return 0;
  }
  return v[0] | (v[1] << 8) | (v[2] << 16) | 0x1000000;
}

std::string Fmt(double v) { return FormatDouble(v); }

// ---------------------------------------------------------------------------------------------
// REAPER-side model

struct EnvPoint
{
  double sec, value;
  int shape;  // 0 = linear, 1 = square
};

struct Envelope
{
  std::vector<EnvPoint> points;
};

struct Receive
{
  int source = 0;
  int mode = 0;  // 0 = post-fader, 3 = pre-fader (post-FX)
  double volume = 1, pan = 0;
  bool mute = false;
  Envelope volumeEnv, panEnv;
};

struct Item
{
  double position = 0, length = 0, offset = 0, rate = 1;
  bool preservePitch = true, mute = false;
  double fadeIn = 0, fadeOut = 0;
  int fadeInShape = 0, fadeOutShape = 0;  // REAPER fade shapes
  double volume = 1;
  int chanMode = 0;                  // REAPER take channel mode (3 + n: mono from channel n)
  std::vector<EnvPoint> takeVolume;  // take volume envelope, seconds from the item start
  std::string name, sourceType, file;
  std::vector<std::string> midi;  // MIDI source lines when sourceType == "MIDI"
};

struct Track
{
  std::string name;
  uint32_t color = 0;
  double volume = 1, pan = 0;
  bool mute = false, solo = false, offline = false;
  int channels = 2;
  int depth = 0;
  int parent = kNoTrack;  // enclosing folder track
  bool mainSend = true;
  xml_node channel;
  std::vector<Receive> receives;
  Envelope volumeEnv, panEnv, muteEnv;
  std::vector<Item> items;
};

struct Marker
{
  double start, end;  // end < 0: plain marker
  std::string name;
  uint32_t color;
};

struct ParamTarget
{
  enum Kind { Volume, Pan, Mute, SendVolume, SendPan, Tempo, TimeSignature, Device } kind;
  int track = kNoTrack;
  int receive = -1;
  xml_node param;
  double scale = 1;
};

// A window onto content: maps the content's local time to the absolute timeline.
struct View
{
  bool beats = true;      // domain of 'offset' and local time
  double offset = 0;      // absolute time (in that domain) of local time 0
  double ws = -kInf;      // visible window, absolute seconds
  double we = kInf;
  double fadeIn = 0, fadeOut = 0;  // seconds, applied at ws / we
  bool muted = false;
  std::string name;
  // Extensions used by in-memory conversions from other formats (AAF): clip gain, fade shapes
  // and a clip gain envelope (points in seconds from gainEnvStart).
  double gain = 1;
  int fadeInShape = 0, fadeOutShape = 0;
  xml_node gainEnv;
  double gainEnvStart = 0;
};

struct MidiEvent
{
  double beat;
  int priority;  // ordering at equal times: note-off, controller, note-on
  uint8_t status, data1, data2;
};

struct MidiCollector
{
  std::vector<MidiEvent> events;
  double minSec = kInf, maxSec = -kInf;
  void Add(double beat, double sec, int priority, uint8_t s, uint8_t d1, uint8_t d2)
  {
    events.push_back({beat, priority, s, d1, d2});
    minSec = std::min(minSec, sec);
    maxSec = std::max(maxSec, sec);
  }
};

// ---------------------------------------------------------------------------------------------

class Converter
{
public:
  Converter(const ConvertOptions& options, ConvertReport& report) : options_(options), report_(report) {}
  // Project XML built in memory by another importer; media files must be external.
  bool RunXml(const std::string& xml, const std::string& sourcePath, RppSink& sink, std::string& error);

private:
  void Warn(const std::string& message) { warnings_[message]++; }

  // time
  double ToSec(bool beats, double t) const { return beats ? tempo_.ToSeconds(t) : t; }
  double ToBeats(bool beats, double t) const { return beats ? t : tempo_.ToBeats(t); }
  double Convert(bool fromBeats, double t, bool toBeats) const
  {
    if (fromBeats == toBeats) return t;
    return toBeats ? tempo_.ToBeats(t) : tempo_.ToSeconds(t);
  }
  void Rebase(View& v, bool beats) const
  {
    if (v.beats == beats) return;
    v.offset = Convert(v.beats, v.offset, beats);
    v.beats = beats;
  }
  bool InheritedUnitIsBeats(xml_node n) const;

  // setup
  void IndexIds(xml_node n);
  void ReadTransport(xml_node project);
  void BuildTempoMap(xml_node arrangement);
  void AddStructure(xml_node parent, int depth, int parentTrack, xml_node skip);
  int NewTrack(xml_node trackOrChannel, int depth, int parentTrack);
  void SetupMaster(xml_node channel, xml_node track);
  void ApplyChannel(int index, xml_node channel, bool loaded);
  void BuildRouting();
  Track& TrackAt(int index) { return index == kMasterTrack ? master_ : tracks_[index]; }
  int TrackFromId(const char* id) const;

  // values
  double VolumeValue(xml_node param, double v, const std::string& pointsUnit) const;
  double PanValue(xml_node param, double v, const std::string& pointsUnit) const;

  // media
  std::string ResolveMedia(xml_node fileRef);

  // arrangement
  void Walk(xml_node timeline, int track, View v, MidiCollector* midi, int depth);
  void ProcessClip(xml_node clip, int track, const View& parent, int depth, double durationOverride);
  void NotesLeaf(xml_node notes, const View& v, MidiCollector& midi);
  void PointsLeaf(xml_node points, int track, const View& v, MidiCollector* midi);
  void ExpressionLeaf(xml_node points, const std::string& expr, int ch, int controller, int key, const View& v,
                      MidiCollector& midi, int priority);
  void NoteExpressions(xml_node content, int ch, int key, double time, double startSec, double endSec, const View& v,
                       MidiCollector& midi);
  void AudioLeaf(xml_node media, int track, View v, double rate, bool preservePitch);
  void WarpsLeaf(xml_node warps, int track, const View& v, MidiCollector* midi, int depth);
  void WarpedContent(xml_node content, int track, const View& v, bool contentBeats,
                     std::vector<std::pair<double, double>> warps, MidiCollector* midi, int depth);
  void MarkersLeaf(xml_node markers, const View& v);
  void EmitMidi(int track, const View& v, MidiCollector& midi);
  bool AddItem(int track, Item item);
  void ConvertScenes(xml_node scenes);

  // output
  void Emit(RppSink& sink, xml_node metadata);
  void EmitEnvelope(RppSink& sink, const std::string& tag, const Envelope& env, int defaultShape);
  void EmitItem(RppSink& sink, const Item& item);
  void EmitTempo(RppSink& sink);

  const ConvertOptions& options_;
  ConvertReport& report_;
  std::map<std::string, int> warnings_;

  bool Convert(const std::vector<uint8_t>& xml, const std::vector<uint8_t>& meta, RppSink& sink, std::string& error);

  std::string sourcePath_;  // the file being converted
  double timeOffset_ = 0;   // project start time (PROJOFFS)
  pugi::xml_document doc_;
  std::unordered_map<std::string, xml_node> ids_;
  std::string application_;
  bool isBitwig_ = false;

  TempoMap tempo_;
  double baseTempo_ = 120;
  int numerator_ = 4, denominator_ = 4;
  struct SigPoint
  {
    double beat;
    int num, den;
  };
  std::vector<SigPoint> signatures_;
  std::string tempoParamId_, signatureParamId_;
  bool arrangementBeats_ = true;

  Track master_;
  bool haveMaster_ = false;
  std::vector<Track> tracks_;
  std::unordered_map<std::string, int> trackIds_, channelIds_;
  std::unordered_map<std::string, ParamTarget> params_;
  std::vector<Marker> markers_;

  int itemCount_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Setup

void Converter::IndexIds(xml_node n)
{
  for (xml_node c : n.children())
  {
    if (c.type() != pugi::node_element) continue;
    pugi::xml_attribute id = c.attribute("id");
    if (id) ids_[id.value()] = c;
    IndexIds(c);
  }
}

bool Converter::InheritedUnitIsBeats(xml_node n) const
{
  for (; n && !Is(n, "Arrangement") && !Is(n, "Project"); n = n.parent())
  {
    int u = AttrUnit(n, "timeUnit");
    if (u >= 0) return u == 1;
  }
  return arrangementBeats_;
}

void Converter::ReadTransport(xml_node project)
{
  xml_node transport = project.child("Transport");
  xml_node tempo = transport.child("Tempo");
  baseTempo_ = AttrDouble(tempo, "value", 120.0);
  if (!(baseTempo_ > 0)) baseTempo_ = 120.0;
  tempoParamId_ = tempo.attribute("id").value();
  xml_node sig = transport.child("TimeSignature");
  numerator_ = (int)AttrDouble(sig, "numerator", 4.0);
  denominator_ = (int)AttrDouble(sig, "denominator", 4.0);
  if (numerator_ < 1 || numerator_ > 255) numerator_ = 4;
  if (denominator_ < 1 || denominator_ > 255) denominator_ = 4;
  signatureParamId_ = sig.attribute("id").value();
  tempo_ = TempoMap(baseTempo_);
}

void Converter::BuildTempoMap(xml_node arrangement)
{
  std::vector<xml_node> tempoLanes, sigLanes;
  if (xml_node t = arrangement.child("TempoAutomation")) tempoLanes.push_back(t);
  if (xml_node s = arrangement.child("TimeSignatureAutomation")) sigLanes.push_back(s);

  // Some exporters put tempo automation into the regular lanes, targeting the transport tempo.
  std::function<void(xml_node)> scan = [&](xml_node n) {
    for (xml_node c : n.children())
    {
      if (Is(c, "Clip")) continue;
      if (Is(c, "Points"))
      {
        const char* p = c.child("Target").attribute("parameter").value();
        if (*p && p == tempoParamId_) tempoLanes.push_back(c);
        else if (*p && p == signatureParamId_) sigLanes.push_back(c);
      }
      scan(c);
    }
  };
  scan(arrangement.child("Lanes"));

  xml_node tempoParam = doc_.child("Project").child("Transport").child("Tempo");
  std::vector<TempoMap::Point> points;
  bool inSeconds = false;
  for (xml_node lane : tempoLanes)
  {
    inSeconds = !InheritedUnitIsBeats(lane);
    std::string unit = lane.attribute("unit").value();
    for (xml_node p : lane.children())
    {
      double t, v;
      if (!GetDouble(p, "time", t) || !GetDouble(p, "value", v)) continue;
      if (unit == "normalized")
      {
        double mn = AttrDouble(tempoParam, "min", 20.0), mx = AttrDouble(tempoParam, "max", 666.0);
        v = mn + v * (mx - mn);
      }
      TempoMap::Point tp;
      tp.time = t;
      tp.bpm = v;
      tp.linear = strcmp(p.attribute("interpolation").value(), "hold") != 0;
      points.push_back(tp);
    }
  }
  if (!points.empty()) tempo_.Build(points, inSeconds);

  for (xml_node lane : sigLanes)
  {
    bool beats = InheritedUnitIsBeats(lane);
    for (xml_node p : lane.children("TimeSignaturePoint"))
    {
      double t;
      if (!GetDouble(p, "time", t)) continue;
      int num = (int)AttrDouble(p, "numerator", 4.0), den = (int)AttrDouble(p, "denominator", 4.0);
      if (num < 1 || den < 1) continue;
      signatures_.push_back({ToBeats(beats, t), num, den});
    }
  }
  std::stable_sort(signatures_.begin(), signatures_.end(), [](const SigPoint& a, const SigPoint& b) { return a.beat < b.beat; });
  if (!signatures_.empty() && signatures_[0].beat < kEps)
  {
    numerator_ = signatures_[0].num;
    denominator_ = signatures_[0].den;
  }
}

int Converter::NewTrack(xml_node node, int depth, int parentTrack)
{
  Track t;
  t.name = node.attribute("name").value();
  t.color = ReaperColor(node.attribute("color").value());
  t.depth = depth;
  t.parent = parentTrack;
  tracks_.push_back(t);
  return (int)tracks_.size() - 1;
}

void Converter::SetupMaster(xml_node channel, xml_node track)
{
  haveMaster_ = true;
  xml_node named = track ? track : channel;
  master_.name = named.attribute("name").value();
  master_.color = ReaperColor(named.attribute("color").value());
  if (track) trackIds_[track.attribute("id").value()] = kMasterTrack;
  channelIds_[channel.attribute("id").value()] = kMasterTrack;
  master_.channel = channel;
}

void Converter::AddStructure(xml_node parent, int depth, int parentTrack, xml_node skip)
{
  bool topLevel = Is(parent, "Structure");
  for (xml_node n : parent.children())
  {
    if (n == skip) continue;
    if (Is(n, "Channel"))
    {
      if (!topLevel) continue;
      if (strcmp(n.attribute("role").value(), "master") == 0 && !haveMaster_)
      {
        SetupMaster(n, xml_node());
        continue;
      }
      int idx = NewTrack(n, depth, parentTrack);
      tracks_[idx].channel = n;
      channelIds_[n.attribute("id").value()] = idx;
      continue;
    }
    if (!Is(n, "Track")) continue;

    xml_node channel = n.child("Channel");
    bool folder = HasContentType(n, "tracks") || n.child("Track");
    if (!folder && channel && strcmp(channel.attribute("role").value(), "master") == 0 && topLevel && !haveMaster_)
    {
      SetupMaster(channel, n);
      continue;
    }

    int idx = NewTrack(n, depth, parentTrack);
    trackIds_[n.attribute("id").value()] = idx;
    if (!AttrBool(n, "loaded", true)) tracks_[idx].offline = true;

    xml_node childToSkip;
    if (folder && !channel)
    {
      // A group may carry its mix bus as a child track with the master role.
      for (xml_node c : n.children("Track"))
      {
        xml_node cc = c.child("Channel");
        if (!c.child("Track") && !HasContentType(c, "tracks") && cc && strcmp(cc.attribute("role").value(), "master") == 0)
        {
          channel = cc;
          childToSkip = c;
          trackIds_[c.attribute("id").value()] = idx;
          break;
        }
      }
    }
    if (channel)
    {
      tracks_[idx].channel = channel;
      channelIds_[channel.attribute("id").value()] = idx;
    }
    if (folder) AddStructure(n, depth + 1, idx, childToSkip);
  }
}

double Converter::VolumeValue(xml_node param, double v, const std::string& pointsUnit) const
{
  std::string paramUnit = param.attribute("unit").value();
  std::string unit = pointsUnit.empty() ? paramUnit : pointsUnit;
  if (unit == "decibel") return std::pow(10.0, v / 20.0);
  if (unit == "normalized" && (paramUnit == "linear" || paramUnit == "decibel"))
  {
    double mn = AttrDouble(param, "min", 0.0), mx = AttrDouble(param, "max", paramUnit == "linear" ? 2.0 : 6.0);
    double x = mn + v * (mx - mn);
    return paramUnit == "decibel" ? std::pow(10.0, x / 20.0) : x;
  }
  return std::max(0.0, v);
}

double Converter::PanValue(xml_node param, double v, const std::string& pointsUnit) const
{
  std::string unit = pointsUnit.empty() ? std::string(param.attribute("unit").value()) : pointsUnit;
  double p;
  double mn, mx;
  if (unit == "normalized") p = v * 2.0 - 1.0;
  else if (unit == "percent") p = v / 100.0;
  else if (GetDouble(param, "min", mn) && GetDouble(param, "max", mx) && mx > mn) p = (v - mn) / (mx - mn) * 2.0 - 1.0;
  else p = v;
  return std::max(-1.0, std::min(1.0, p));
}

void Converter::ApplyChannel(int index, xml_node channel, bool loaded)
{
  Track& t = TrackAt(index);
  int ch = (int)AttrDouble(channel, "audioChannels", 2.0);
  t.channels = std::max(2, ch + (ch & 1));

  if (xml_node vol = channel.child("Volume"))
  {
    t.volume = VolumeValue(vol, AttrDouble(vol, "value", 1.0), std::string());
    params_[vol.attribute("id").value()] = {ParamTarget::Volume, index, -1, vol};
  }
  if (xml_node pan = channel.child("Pan"))
  {
    t.pan = PanValue(pan, AttrDouble(pan, "value", 0.5), std::string());
    params_[pan.attribute("id").value()] = {ParamTarget::Pan, index, -1, pan};
  }
  if (xml_node mute = channel.child("Mute"))
  {
    t.mute = AttrBool(mute, "value", false);
    params_[mute.attribute("id").value()] = {ParamTarget::Mute, index, -1, mute};
  }
  t.solo = AttrBool(channel, "solo", false);
  if (!loaded) t.offline = true;
  if (channel.child("Devices").first_child()) Warn("Devices are not converted");
}

int Converter::TrackFromId(const char* id) const
{
  auto it = trackIds_.find(id);
  if (it != trackIds_.end()) return it->second;
  it = channelIds_.find(id);
  if (it != channelIds_.end()) return it->second;
  return kNoTrack;
}

void Converter::BuildRouting()
{
  std::vector<int> all;
  if (haveMaster_) all.push_back(kMasterTrack);
  for (int i = 0; i < (int)tracks_.size(); i++) all.push_back(i);

  for (int idx : all)
  {
    Track& t = TrackAt(idx);
    if (!t.channel) continue;

    if (idx != kMasterTrack)
    {
      const char* dest = t.channel.attribute("destination").value();
      if (*dest)
      {
        auto it = channelIds_.find(dest);
        if (it == channelIds_.end()) Warn("Output routing to an unknown channel was replaced by the master");
        else if (it->second != kMasterTrack && it->second != t.parent && it->second != idx)
        {
          // Route to a bus that is not the enclosing folder: disable the parent send, use a send.
          tracks_[idx].mainSend = false;
          Receive r;
          r.source = idx;
          tracks_[it->second].receives.push_back(r);
        }
      }
    }

    for (xml_node send : t.channel.child("Sends").children("Send"))
    {
      const char* dest = send.attribute("destination").value();
      auto it = channelIds_.find(dest);
      if (it == channelIds_.end() || it->second == idx)
      {
        Warn("A send to an unknown channel was skipped");
        continue;
      }
      if (it->second == kMasterTrack || idx == kMasterTrack)
      {
        Warn("Sends from or to the master channel are not possible in REAPER and were skipped");
        continue;
      }
      xml_node vol = send.child("Volume");
      xml_node pan = send.child("Pan");
      double scale = 1.0;
      // Bitwig writes send levels as 0..1 where 1 is +12 dB (same handling as ProjectConverter).
      if (isBitwig_ && strcmp(vol.attribute("unit").value(), "linear") == 0 && AttrDouble(vol, "max", 2.0) <= 1.0001)
        scale = std::pow(10.0, 12.0 / 20.0);

      Receive r;
      r.source = idx;
      r.mode = strcmp(send.attribute("type").value(), "pre") == 0 ? 3 : 0;
      r.volume = vol ? VolumeValue(vol, AttrDouble(vol, "value", 1.0), std::string()) * scale : 1.0;
      r.pan = pan ? PanValue(pan, AttrDouble(pan, "value", 0.5), std::string()) : 0.0;
      r.mute = !AttrBool(send.child("Enable"), "value", true);
      Track& d = tracks_[it->second];
      int ri = (int)d.receives.size();
      d.receives.push_back(r);
      if (vol) params_[vol.attribute("id").value()] = {ParamTarget::SendVolume, it->second, ri, vol, scale};
      if (pan) params_[pan.attribute("id").value()] = {ParamTarget::SendPan, it->second, ri, pan};
    }
  }
}

// ---------------------------------------------------------------------------------------------
// Media

std::string Converter::ResolveMedia(xml_node fileRef)
{
  std::string path = fileRef.attribute("path").value();
  if (path.empty()) return std::string();
  if (AttrBool(fileRef, "external", false))
  {
    std::string full = IsAbsolutePath(path) ? path : JoinPath(DirName(sourcePath_), path);
    full = NativeSeparators(full);
    if (!FileExists(full)) Warn("External media file not found: " + full);
    return full;
  }

  Warn("Media embedded in the project is not supported: " + path);
  return std::string();
}

// ---------------------------------------------------------------------------------------------
// Arrangement

bool Converter::AddItem(int track, Item item)
{
  if (track == kNoTrack)
  {
    Warn("Some clips are not assigned to a track and were skipped");
    return false;
  }
  if (track == kMasterTrack)
  {
    Warn("Clips on the master track were skipped (REAPER's master track cannot hold items)");
    return false;
  }
  if (!(item.length > kEps)) return false;
  tracks_[track].items.push_back(std::move(item));
  itemCount_++;
  return true;
}

void Converter::Walk(xml_node tl, int track, View v, MidiCollector* midi, int depth)
{
  if (depth > 64)
  {
    Warn("Timeline nesting too deep (circular clip references?)");
    return;
  }
  int unit = AttrUnit(tl, "timeUnit");
  if (unit >= 0) Rebase(v, unit == 1);
  if (pugi::xml_attribute a = tl.attribute("track"))
  {
    int t = TrackFromId(a.value());
    if (t == kNoTrack) Warn("Lanes refer to an unknown track");
    else track = t;
  }

  if (Is(tl, "Lanes"))
  {
    for (xml_node c : tl.children())
      if (IsTimelineElement(c)) Walk(c, track, v, midi, depth + 1);
  }
  else if (Is(tl, "Clips"))
  {
    for (xml_node clip : tl.children("Clip")) ProcessClip(clip, track, v, depth + 1, std::nan(""));
  }
  else if (Is(tl, "Notes") || Is(tl, "Points"))
  {
    MidiCollector local;
    MidiCollector* m = midi ? midi : &local;
    if (Is(tl, "Notes")) NotesLeaf(tl, v, *m);
    else PointsLeaf(tl, track, v, m);
    if (!midi && !local.events.empty()) EmitMidi(track, v, local);
  }
  else if (Is(tl, "Audio") || Is(tl, "Video"))
  {
    Rebase(v, false);
    AudioLeaf(tl, track, v, 1.0, strcmp(tl.attribute("algorithm").value(), "repitch") != 0);
  }
  else if (Is(tl, "Warps"))
    WarpsLeaf(tl, track, v, midi, depth);
  else if (Is(tl, "Markers") || Is(tl, "markers"))
    MarkersLeaf(tl, v);
  else if (Is(tl, "ClipSlot"))
    Warn("Clip launcher slots inside the arrangement were skipped");
}

void Converter::ProcessClip(xml_node clip, int track, const View& pv, int depth, double durationOverride)
{
  bool contentBeats = AttrUnit(clip, "contentTimeUnit") >= 0 ? AttrUnit(clip, "contentTimeUnit") == 1 : pv.beats;
  bool fadeBeats = AttrUnit(clip, "fadeTimeUnit") >= 0 ? AttrUnit(clip, "fadeTimeUnit") == 1 : pv.beats;
  double time = AttrDouble(clip, "time", 0.0);
  double playStart = AttrDouble(clip, "playStart", 0.0);
  double playStop = 0, duration = 0, loopStart = 0, loopEnd = 0;
  bool hasStop = GetDouble(clip, "playStop", playStop);
  bool hasDuration = GetDouble(clip, "duration", duration);
  if (!std::isnan(durationOverride))
  {
    hasDuration = true;
    duration = durationOverride;
  }
  bool loop = GetDouble(clip, "loopStart", loopStart) && GetDouble(clip, "loopEnd", loopEnd) && loopEnd > loopStart + kEps &&
              playStart < loopEnd;

  double startAbs = pv.offset + time;
  double startContent = Convert(pv.beats, startAbs, contentBeats);
  double s0 = ToSec(pv.beats, startAbs);
  double s1 = kInf;
  if (hasDuration) s1 = ToSec(pv.beats, startAbs + duration);
  else if (hasStop) s1 = ToSec(contentBeats, startContent + (playStop - playStart));

  double fadeIn = AttrDouble(clip, "fadeInTime", 0.0), fadeOut = AttrDouble(clip, "fadeOutTime", 0.0);
  double fadeInSec, fadeOutSec = 0;
  if (fadeIn < 0)
  {
    // Negative fade-in: cross-fade, the clip starts earlier by that amount.
    double earlier = fadeBeats ? tempo_.ToSeconds(tempo_.ToBeats(s0) + fadeIn) : s0 + fadeIn;
    fadeInSec = s0 - earlier;
    s0 = earlier;
  }
  else
    fadeInSec = fadeBeats ? tempo_.ToSeconds(tempo_.ToBeats(s0) + fadeIn) - s0 : fadeIn;
  if (std::isfinite(s1) && fadeOut > 0) fadeOutSec = fadeBeats ? s1 - tempo_.ToSeconds(tempo_.ToBeats(s1) - fadeOut) : fadeOut;

  View cv;
  cv.beats = contentBeats;
  cv.ws = std::max(pv.ws, s0);
  cv.we = std::min(pv.we, s1);
  if (!(cv.we - cv.ws > kEps)) return;
  cv.fadeIn = s0 >= pv.ws - kEps ? fadeInSec : pv.fadeIn;
  cv.fadeOut = s1 <= pv.we + kEps ? fadeOutSec : pv.fadeOut;
  cv.muted = pv.muted || !AttrBool(clip, "enable", true);
  cv.name = clip.attribute("name") ? clip.attribute("name").value() : pv.name;
  cv.gain = pv.gain * AttrDouble(clip, "xGain", 1.0);
  if (s0 >= pv.ws - kEps) cv.fadeInShape = clip.attribute("xFadeInShape").as_int(0);
  if (s1 <= pv.we + kEps) cv.fadeOutShape = clip.attribute("xFadeOutShape").as_int(0);
  if (xml_node env = clip.child("xGainEnvelope"))
  {
    cv.gainEnv = env;
    cv.gainEnvStart = ToSec(pv.beats, startAbs);
  }

  xml_node content = FirstTimelineChild(clip);
  if (!content && clip.attribute("reference"))
  {
    auto it = ids_.find(clip.attribute("reference").value());
    if (it != ids_.end()) content = it->second;
    else Warn("A linked clip refers to missing content");
  }
  if (!content) return;

  MidiCollector midi;
  if (loop)
  {
    double segStart = startContent;  // absolute content-domain time where the segment starts
    double localLo = playStart;
    for (int guard = 0; guard < 100000; guard++)
    {
      View sv = cv;
      sv.offset = segStart - localLo;
      double a = ToSec(contentBeats, segStart), b = ToSec(contentBeats, segStart + (loopEnd - localLo));
      sv.ws = std::max(cv.ws, a);
      sv.we = std::min(cv.we, b);
      if (sv.we - sv.ws > kEps)
      {
        sv.fadeIn = sv.ws <= cv.ws + kEps ? cv.fadeIn : 0;
        sv.fadeOut = sv.we >= cv.we - kEps ? cv.fadeOut : 0;
        Walk(content, track, sv, &midi, depth + 1);
      }
      if (b >= cv.we - kEps) break;
      if (!std::isfinite(cv.we))
      {
        Warn("A looped clip without length was played only once");
        break;
      }
      segStart += loopEnd - localLo;
      localLo = loopStart;
    }
  }
  else
  {
    View sv = cv;
    sv.offset = startContent - playStart;
    if (hasStop)
    {
      double stopSec = ToSec(contentBeats, startContent + (playStop - playStart));
      if (stopSec < sv.we)
      {
        sv.we = stopSec;
        sv.fadeOut = 0;
      }
    }
    if (sv.we - sv.ws > kEps) Walk(content, track, sv, &midi, depth + 1);
  }
  if (!midi.events.empty()) EmitMidi(track, cv, midi);
}

void Converter::NotesLeaf(xml_node notes, const View& v, MidiCollector& midi)
{
  for (xml_node n : notes.children("Note"))
  {
    double t, d;
    if (!GetDouble(n, "time", t) || !GetDouble(n, "duration", d)) continue;
    double startSec = ToSec(v.beats, v.offset + t);
    if (startSec < v.ws - kEps || startSec >= v.we - kEps) continue;
    double endSec = std::min(ToSec(v.beats, v.offset + t + d), v.we);
    double startBeat = v.beats ? v.offset + t : tempo_.ToBeats(startSec);
    double endBeat = tempo_.ToBeats(endSec);
    if (v.beats && endSec >= ToSec(true, v.offset + t + d) - kEps) endBeat = v.offset + t + d;

    int ch = std::max(0, std::min(15, (int)AttrDouble(n, "channel", 0.0)));
    int key = (int)AttrDouble(n, "key", -1.0);
    if (key < 0 || key > 127) continue;
    int vel = (int)std::lround(AttrDouble(n, "vel", 0.8) * 127.0);
    int rel = (int)std::lround(AttrDouble(n, "rel", 0.5) * 127.0);
    vel = std::max(1, std::min(127, vel));
    rel = std::max(0, std::min(127, rel));

    midi.Add(startBeat, startSec, 2, (uint8_t)(0x90 | ch), (uint8_t)key, (uint8_t)vel);
    midi.Add(std::max(endBeat, startBeat), endSec, 0, (uint8_t)(0x80 | ch), (uint8_t)key, (uint8_t)rel);
    if (xml_node content = FirstTimelineChild(n)) NoteExpressions(content, ch, key, t, startSec, endSec, v, midi);
  }
}

// Per-note expressions live inside the note, timed from the note start. REAPER has no per-note
// expressions; pressure maps exactly to polyphonic aftertouch, the others have no equivalent.
void Converter::NoteExpressions(xml_node content, int ch, int key, double time, double startSec, double endSec,
                                const View& v, MidiCollector& midi)
{
  if (Is(content, "Lanes"))
  {
    for (xml_node c : content.children())
      if (IsTimelineElement(c)) NoteExpressions(c, ch, key, time, startSec, endSec, v, midi);
    return;
  }
  if (!Is(content, "Points")) return;
  std::string expr = content.child("Target").attribute("expression").value();
  if (expr != "pressure" && expr != "polyPressure")
  {
    Warn("Per-note " + expr + " expressions are not converted (REAPER has no per-note expressions; " +
         "per-note pressure becomes polyphonic aftertouch)");
    return;
  }
  View nv = v;
  nv.offset = v.offset + time;
  int unit = AttrUnit(content, "timeUnit");
  if (unit >= 0) Rebase(nv, unit == 1);
  nv.ws = std::max(v.ws, startSec);
  nv.we = std::min(v.we, endSec);
  ExpressionLeaf(content, "polyPressure", ch, 0, key, nv, midi, 3);
}

void Converter::ExpressionLeaf(xml_node points, const std::string& expr, int ch, int controller, int key, const View& v,
                               MidiCollector& midi, int priority)
{
  ch = std::max(0, std::min(15, ch));
  uint8_t status;
  int maxValue = 127;
  if (expr == "channelController") status = 0xB0;
  else if (expr == "pitchBend") status = 0xE0, maxValue = 16383;
  else if (expr == "channelPressure") status = 0xD0;
  else if (expr == "polyPressure") status = 0xA0;
  else if (expr == "programChange") status = 0xC0;
  else
  {
    Warn("Note expression automation (" + expr + ") is not converted");
    return;
  }
  if (controller < 0 || controller > 127 || key < 0 || key > 127) return;

  struct P
  {
    double beat, sec, value;
    bool linear;
  };
  std::vector<P> pts;
  double maxRaw = 0, minRaw = 0;
  for (xml_node p : points.children())
  {
    double t, val;
    if (!GetDouble(p, "time", t)) continue;
    if (Is(p, "BoolPoint")) val = AttrBool(p, "value", false) ? 1.0 : 0.0;
    else if (!GetDouble(p, "value", val)) continue;
    bool linear = Is(p, "RealPoint") && strcmp(p.attribute("interpolation").value(), "hold") != 0;
    double abs = v.offset + t;
    pts.push_back({ToBeats(v.beats, abs), ToSec(v.beats, abs), val, linear});
    maxRaw = std::max(maxRaw, val);
    minRaw = std::min(minRaw, val);
  }
  if (pts.empty()) return;
  std::stable_sort(pts.begin(), pts.end(), [](const P& a, const P& b) { return a.beat < b.beat; });

  // Values are normalized (0..1); pitch bend may be bipolar, integer points may be raw MIDI values.
  std::string unit = points.attribute("unit").value();
  bool bipolar = expr == "pitchBend" && minRaw < -kEps;
  bool raw = !bipolar && maxRaw > 1.0 + kEps && unit != "percent";
  auto quantize = [&](double val) {
    if (unit == "percent") val /= 100.0;
    if (bipolar) val = (val + 1.0) * 0.5;
    int q = raw ? (int)std::lround(val) : (int)std::lround(val * maxValue);
    return std::max(0, std::min(maxValue, q));
  };
  auto valueAt = [&](size_t i, double beat) {
    if (!pts[i].linear || i + 1 >= pts.size() || pts[i + 1].beat <= pts[i].beat) return pts[i].value;
    double f = (beat - pts[i].beat) / (pts[i + 1].beat - pts[i].beat);
    return pts[i].value + (pts[i + 1].value - pts[i].value) * f;
  };

  int last = -1;
  auto emit = [&](double beat, double sec, double val) {
    if (sec < v.ws - kEps || sec >= v.we - kEps) return;
    int q = quantize(val);
    if (q == last) return;
    last = q;
    uint8_t s = (uint8_t)(status | ch);
    if (status == 0xE0) midi.Add(beat, sec, priority, s, (uint8_t)(q & 0x7F), (uint8_t)(q >> 7));
    else if (status == 0xB0) midi.Add(beat, sec, priority, s, (uint8_t)controller, (uint8_t)q);
    else if (status == 0xA0) midi.Add(beat, sec, priority, s, (uint8_t)key, (uint8_t)q);
    else midi.Add(beat, sec, priority, s, (uint8_t)q, 0);
  };

  // value at the window start when the curve starts earlier
  if (std::isfinite(v.ws) && pts[0].sec < v.ws - kEps)
  {
    double wb = tempo_.ToBeats(v.ws);
    size_t i = 0;
    while (i + 1 < pts.size() && pts[i + 1].beat <= wb) i++;
    emit(wb, v.ws, valueAt(i, wb));
  }
  for (size_t i = 0; i < pts.size(); i++)
  {
    emit(pts[i].beat, pts[i].sec, pts[i].value);
    if (status != 0xC0 && pts[i].linear && i + 1 < pts.size())
    {
      // densify ramps at 1/32 note resolution
      double b0 = pts[i].beat, b1 = pts[i + 1].beat;
      for (double b = b0 + 1.0 / 32.0; b < b1 - kEps; b += 1.0 / 32.0) emit(b, tempo_.ToSeconds(b), valueAt(i, b));
    }
  }
}

void Converter::PointsLeaf(xml_node points, int track, const View& v, MidiCollector* midi)
{
  xml_node target = points.child("Target");
  if (target.attribute("expression"))
  {
    ExpressionLeaf(points, target.attribute("expression").value(), (int)AttrDouble(target, "channel", 0.0),
                   (int)AttrDouble(target, "controller", 0.0), (int)AttrDouble(target, "key", 0.0), v, *midi, 1);
    return;
  }
  auto it = params_.find(target.attribute("parameter").value());
  if (it == params_.end())
  {
    const char* p = target.attribute("parameter").value();
    if (*p && (p == tempoParamId_ || p == signatureParamId_)) return;  // handled by the tempo map
    Warn("Automation of an unknown parameter was skipped");
    return;
  }
  const ParamTarget& pt = it->second;
  if (pt.kind == ParamTarget::Tempo || pt.kind == ParamTarget::TimeSignature) return;
  if (pt.kind == ParamTarget::Device)
  {
    Warn("Automation of device on/off switches and of devices that were not converted was skipped");
    return;
  }
  std::string unit = points.attribute("unit").value();
  Envelope* env = nullptr;
  Track& t = TrackAt(pt.track);
  switch (pt.kind)
  {
    case ParamTarget::Volume: env = &t.volumeEnv; break;
    case ParamTarget::Pan: env = &t.panEnv; break;
    case ParamTarget::Mute:
      if (pt.track == kMasterTrack)
      {
        Warn("Master mute automation is not converted");
        return;
      }
      env = &t.muteEnv;
      break;
    case ParamTarget::SendVolume: env = &t.receives[pt.receive].volumeEnv; break;
    case ParamTarget::SendPan: env = &t.receives[pt.receive].panEnv; break;
    default: return;
  }

  for (xml_node p : points.children())
  {
    double time, val;
    if (!GetDouble(p, "time", time)) continue;
    bool isBool = Is(p, "BoolPoint");
    if (isBool) val = AttrBool(p, "value", false) ? 1.0 : 0.0;
    else if (!GetDouble(p, "value", val)) continue;
    double sec = ToSec(v.beats, v.offset + time);
    if (sec < v.ws - kEps || sec > v.we + kEps) continue;
    int shape = Is(p, "RealPoint") && strcmp(p.attribute("interpolation").value(), "hold") != 0 ? 0 : 1;

    double out;
    switch (pt.kind)
    {
      case ParamTarget::Volume: out = VolumeValue(pt.param, val, unit); break;
      case ParamTarget::SendVolume: out = VolumeValue(pt.param, val, unit) * pt.scale; break;
      case ParamTarget::Pan:
      case ParamTarget::SendPan: out = -PanValue(pt.param, val, unit); break;  // REAPER pan envelopes: +1 = left
      case ParamTarget::Mute: out = val > 0.5 ? 0.0 : 1.0; shape = 1; break;  // REAPER mute envelope: 0 = muted
      default: continue;
    }
    env->points.push_back({std::max(0.0, sec), out, shape});
  }
}

void Converter::AudioLeaf(xml_node media, int track, View v, double rate, bool preservePitch)
{
  std::string file = ResolveMedia(media.child("File"));
  if (file.empty()) return;
  double duration = AttrDouble(media, "duration", 0.0);

  double ws = std::isfinite(v.ws) ? v.ws : v.offset;
  double we = std::isfinite(v.we) ? v.we : v.offset + duration / rate;
  if (duration > 0) we = std::min(we, v.offset + duration / rate);  // no silent tail after the file ends
  bool keepFadeOut = !(std::isfinite(v.we) && we < v.we - kEps);
  double offset = (ws - v.offset) * rate;
  bool keepFadeIn = true;
  if (offset < 0)
  {
    // the window starts before the file: start the item at the file start
    ws -= offset / rate;
    offset = 0;
    keepFadeIn = false;
  }
  Item item;
  item.position = ws;
  item.length = we - ws;
  item.offset = offset;
  item.rate = rate;
  item.preservePitch = preservePitch;
  item.mute = v.muted;
  item.fadeIn = keepFadeIn ? std::min(v.fadeIn, item.length) : 0;
  item.fadeOut = keepFadeOut ? std::min(v.fadeOut, item.length) : 0;
  item.fadeInShape = v.fadeInShape;
  item.fadeOutShape = v.fadeOutShape;
  item.volume = v.gain;
  if (pugi::xml_attribute ch = media.attribute("xChannel")) item.chanMode = 3 + ch.as_int(0);
  for (xml_node p : v.gainEnv.children("Point"))
  {
    double t = AttrDouble(p, "time", 0.0), val = AttrDouble(p, "value", 1.0);
    // take envelopes run in source time when the play rate is not 1
    item.takeVolume.push_back({(v.gainEnvStart + t - ws) * rate, val, p.attribute("shape").as_int(0)});
  }
  item.name = v.name.empty() ? FileName(file) : v.name;
  item.file = file;
  std::string ext = LowerExtension(file);
  if (Is(media, "Video")) item.sourceType = "VIDEO";
  else if (ext == "flac") item.sourceType = "FLAC";
  else if (ext == "mp3") item.sourceType = "MP3";
  else if (ext == "ogg") item.sourceType = "VORBIS";
  else if (ext == "opus") item.sourceType = "OPUS";
  else item.sourceType = "WAVE";
  AddItem(track, std::move(item));
}

void Converter::WarpsLeaf(xml_node warps, int track, const View& v, MidiCollector* midi, int depth)
{
  bool contentBeats = AttrUnit(warps, "contentTimeUnit") == 1;
  xml_node media, content;
  for (xml_node c : warps.children())
  {
    if (Is(c, "Audio") || Is(c, "Video")) media = c;
    else if (!content && IsTimelineElement(c)) content = c;
  }

  struct W
  {
    double sec, content;
  };
  std::vector<W> w;
  for (xml_node p : warps.children("Warp"))
  {
    double t, c;
    if (GetDouble(p, "time", t) && GetDouble(p, "contentTime", c)) w.push_back({ToSec(v.beats, v.offset + t), c});
  }
  std::stable_sort(w.begin(), w.end(), [](const W& a, const W& b) { return a.sec < b.sec; });

  if (!media || contentBeats)
  {
    if (!content)
    {
      Warn("Audio warped in beats is not converted");
      return;
    }
    std::vector<std::pair<double, double>> pairs;
    for (const W& x : w) pairs.push_back({x.content, x.sec});
    WarpedContent(content, track, v, contentBeats, pairs, midi, depth);
    return;
  }
  bool preservePitch = strcmp(media.attribute("algorithm").value(), "repitch") != 0;

  // Constant-rate segments (piecewise linear warping becomes one item per segment).
  struct Seg
  {
    double a0, a1, refSec, refContent, rate;
  };
  std::vector<Seg> segs;
  for (size_t i = 0; i + 1 < w.size(); i++)
  {
    double ds = w[i + 1].sec - w[i].sec, dc = w[i + 1].content - w[i].content;
    if (ds <= kEps || dc <= kEps) continue;
    double rate = dc / ds;
    if (!segs.empty() && std::fabs(segs.back().rate - rate) < 1e-6 * rate) segs.back().a1 = w[i + 1].sec;
    else segs.push_back({w[i].sec, w[i + 1].sec, w[i].sec, w[i].content, rate});
  }
  if (segs.empty())
  {
    // zero or one usable warp: plain playback anchored at the first warp
    View av = v;
    Rebase(av, false);
    if (!w.empty()) av.offset = w[0].sec - w[0].content;
    AudioLeaf(media, track, av, 1.0, preservePitch);
    return;
  }
  segs.front().a0 = -kInf;
  segs.back().a1 = kInf;
  for (size_t i = 0; i < segs.size(); i++)
  {
    const Seg& s = segs[i];
    View sv;
    sv.beats = false;
    sv.offset = s.refSec - s.refContent / s.rate;  // where content time 0 would be at this rate
    sv.ws = std::max(v.ws, s.a0);
    sv.we = std::min(v.we, s.a1);
    if (!(sv.we - sv.ws > kEps)) continue;
    sv.fadeIn = sv.ws <= v.ws + kEps ? v.fadeIn : 0;
    sv.fadeOut = sv.we >= v.we - kEps ? v.fadeOut : 0;
    sv.muted = v.muted;
    sv.name = v.name;
    AudioLeaf(media, track, sv, s.rate, preservePitch);
  }
}

// Warped MIDI (or other non-audio content): the content is converted unwarped, then each event is
// moved through the warp map (content time -> timeline seconds, piecewise linear).
void Converter::WarpedContent(xml_node content, int track, const View& v, bool contentBeats,
                              std::vector<std::pair<double, double>> warps, MidiCollector* midi, int depth)
{
  if (warps.empty())
  {
    Warn("Warped content without warp markers was skipped");
    return;
  }
  std::stable_sort(warps.begin(), warps.end());
  auto map = [&](double c) {
    if (warps.size() == 1)
    {
      if (!contentBeats) return warps[0].second + (c - warps[0].first);
      return tempo_.ToSeconds(tempo_.ToBeats(warps[0].second) + (c - warps[0].first));
    }
    size_t i = 0;
    while (i + 2 < warps.size() && c >= warps[i + 1].first) i++;
    const auto& a = warps[i];
    const auto& b = warps[i + 1];
    if (b.first - a.first < kEps) return a.second;
    return a.second + (c - a.first) * (b.second - a.second) / (b.first - a.first);
  };

  MidiCollector unwarped;
  View cv;
  cv.beats = contentBeats;
  cv.muted = v.muted;
  cv.name = v.name;
  Walk(content, track, cv, &unwarped, depth + 1);

  MidiCollector local;
  MidiCollector& out = midi ? *midi : local;
  for (const MidiEvent& e : unwarped.events)
  {
    double c = contentBeats ? e.beat : tempo_.ToSeconds(e.beat);
    double sec = map(c);
    bool noteOff = (e.status & 0xF0) == 0x80;
    if (sec < v.ws - kEps) continue;
    if (sec >= v.we - kEps)
    {
      if (!noteOff) continue;
      sec = v.we;  // notes still sounding at the clip end stop there
    }
    out.Add(tempo_.ToBeats(sec), sec, e.priority, e.status, e.data1, e.data2);
  }
  if (!midi && !local.events.empty()) EmitMidi(track, v, local);
}

void Converter::MarkersLeaf(xml_node markers, const View& v)
{
  for (xml_node m : markers.children("Marker"))
  {
    double t;
    if (!GetDouble(m, "time", t)) continue;
    double sec = ToSec(v.beats, v.offset + t);
    if (sec < v.ws - kEps || sec > v.we + kEps) continue;
    double end = -1, duration;
    if (GetDouble(m, "xDuration", duration) && duration > 0) end = sec + duration;  // region (AAF)
    markers_.push_back({std::max(0.0, sec), end, m.attribute("name").value(), ReaperColor(m.attribute("color").value())});
  }
}

void Converter::EmitMidi(int track, const View& v, MidiCollector& midi)
{
  double ws = std::isfinite(v.ws) ? v.ws : midi.minSec;
  double we = std::isfinite(v.we) ? v.we : midi.maxSec;
  if (!std::isfinite(ws)) return;
  double qs = tempo_.ToBeats(ws);
  if (!(we > ws + kEps)) we = tempo_.ToSeconds(qs + 1.0);
  double qe = tempo_.ToBeats(we);

  struct E
  {
    long long tick;
    int priority;
    uint8_t s, d1, d2;
  };
  std::vector<E> evs;
  for (const MidiEvent& e : midi.events)
  {
    if (e.beat > qe + kEps) continue;
    long long tick = std::max(0LL, std::llround((e.beat - qs) * kTicksPerQN));
    evs.push_back({tick, e.priority, e.status, e.data1, e.data2});
  }
  std::stable_sort(evs.begin(), evs.end(), [](const E& a, const E& b) {
    return a.tick != b.tick ? a.tick < b.tick : a.priority < b.priority;
  });

  Item item;
  item.position = ws;
  item.length = we - ws;
  item.mute = v.muted;
  item.name = v.name;
  item.sourceType = "MIDI";
  item.midi.push_back("HASDATA 1 960 QN");
  long long last = 0;
  char buf[64];
  for (const E& e : evs)
  {
    snprintf(buf, sizeof(buf), "E %lld %02x %02x %02x", e.tick - last, e.s, e.d1, e.d2);
    item.midi.push_back(buf);
    last = e.tick;
  }
  long long end = std::max(last, std::llround((qe - qs) * kTicksPerQN));
  snprintf(buf, sizeof(buf), "E %lld b0 7b 00", end - last);
  item.midi.push_back(buf);
  AddItem(track, std::move(item));
}

void Converter::ConvertScenes(xml_node scenes)
{
  double cursor = 0;  // beats
  int sceneNumber = 0;
  for (xml_node scene : scenes.children("Scene"))
  {
    sceneNumber++;
    std::vector<std::pair<xml_node, int>> slots;
    std::function<void(xml_node, int)> collect = [&](xml_node n, int track) {
      if (pugi::xml_attribute a = n.attribute("track"))
      {
        int t = TrackFromId(a.value());
        if (t != kNoTrack) track = t;
      }
      if (Is(n, "ClipSlot"))
      {
        if (xml_node clip = n.child("Clip")) slots.push_back({clip, track});
        return;
      }
      for (xml_node c : n.children())
        if (IsTimelineElement(c)) collect(c, track);
    };
    xml_node content = FirstTimelineChild(scene);
    if (!content) continue;
    collect(content, kNoTrack);
    if (slots.empty()) continue;

    bool beats = InheritedUnitIsBeats(content);
    double length = 0;
    for (auto& s : slots)
    {
      double d = AttrDouble(s.first, "duration", 0.0);
      bool clipBeats = beats;
      if (d <= 0)
      {
        d = AttrDouble(s.first, "playStop", 0.0) - AttrDouble(s.first, "playStart", 0.0);
        if (AttrUnit(s.first, "contentTimeUnit") >= 0) clipBeats = AttrUnit(s.first, "contentTimeUnit") == 1;
      }
      if (!clipBeats) d = d * tempo_.BpmAtBeat(cursor) / 60.0;
      length = std::max(length, d);
    }
    if (length <= kEps) continue;

    for (auto& s : slots)
    {
      View pv;
      pv.beats = true;
      pv.offset = cursor;
      bool looped = s.first.attribute("loopStart") && s.first.attribute("loopEnd");
      ProcessClip(s.first, s.second, pv, 0, looped ? length : std::nan(""));
    }
    std::string name = scene.attribute("name").value();
    if (name.empty()) name = "Scene " + std::to_string(sceneNumber);
    markers_.push_back({tempo_.ToSeconds(cursor), tempo_.ToSeconds(cursor + length), name, ReaperColor(scene.attribute("color").value())});
    cursor += length;
  }
}

// ---------------------------------------------------------------------------------------------
// Output

void Converter::EmitEnvelope(RppSink& sink, const std::string& tag, const Envelope& env, int defaultShape)
{
  if (env.points.empty()) return;
  std::vector<EnvPoint> pts = env.points;
  std::stable_sort(pts.begin(), pts.end(), [](const EnvPoint& a, const EnvPoint& b) { return a.sec < b.sec; });
  sink.Line("<" + tag);
  sink.Line("ACT 1 -1");
  sink.Line("VIS 1 1 1");
  sink.Line("LANEHEIGHT 0 0");
  sink.Line("ARM 0");
  sink.Line("DEFSHAPE " + std::to_string(defaultShape) + " -1 -1");
  for (const EnvPoint& p : pts) sink.Line("PT " + Fmt(p.sec) + " " + Fmt(p.value) + " " + std::to_string(p.shape));
  sink.Line(">");
}

void Converter::EmitItem(RppSink& sink, const Item& item)
{
  sink.Line("<ITEM");
  sink.Line("POSITION " + Fmt(item.position));
  sink.Line("LENGTH " + Fmt(item.length));
  sink.Line("LOOP 0");
  sink.Line(std::string("MUTE ") + (item.mute ? "1" : "0") + " 0");
  sink.Line("FADEIN " + std::to_string(item.fadeInShape) + " " + Fmt(item.fadeIn) + " 0 " +
            std::to_string(item.fadeInShape) + " 0 0 0");
  sink.Line("FADEOUT " + std::to_string(item.fadeOutShape) + " " + Fmt(item.fadeOut) + " 0 " +
            std::to_string(item.fadeOutShape) + " 0 0 0");
  if (item.volume != 1) sink.Line("VOLPAN " + Fmt(item.volume) + " 0 1 -1");
  if (!item.name.empty()) sink.Line("NAME " + RppQuote(item.name));
  if (item.sourceType == "MIDI")
  {
    sink.Line("<SOURCE MIDI");
    for (const std::string& l : item.midi) sink.Line(l);
    sink.Line(">");
  }
  else
  {
    sink.Line("SOFFS " + Fmt(item.offset));
    sink.Line("PLAYRATE " + Fmt(item.rate) + (item.preservePitch ? " 1" : " 0") + " 0 -1 0 0.0025");
    if (item.chanMode) sink.Line("CHANMODE " + std::to_string(item.chanMode));
    sink.Line("<SOURCE " + item.sourceType);
    sink.Line("FILE " + RppQuote(item.file));
    sink.Line(">");
    if (!item.takeVolume.empty())
    {
      Envelope env;
      env.points = item.takeVolume;
      EmitEnvelope(sink, "VOLENV", env, 0);
    }
  }
  sink.Line(">");
}

void Converter::EmitTempo(RppSink& sink)
{
  struct TP
  {
    double beat, bpm;
    int shape;
    int sig;  // 0 = none, else (den << 16) | num
  };
  std::vector<TP> pts;
  for (const TempoMap::Segment& s : tempo_.Segments()) pts.push_back({s.beat, s.bpm, s.linear ? 0 : 1, 0});
  for (const SigPoint& sp : signatures_)
  {
    int sig = (sp.den << 16) | sp.num;
    auto it = std::find_if(pts.begin(), pts.end(), [&](const TP& p) { return std::fabs(p.beat - sp.beat) < 1e-6; });
    if (it != pts.end()) it->sig = sig;
    else pts.push_back({sp.beat, tempo_.BpmAtBeat(sp.beat), tempo_.IsLinearAtBeat(sp.beat) ? 0 : 1, sig});
  }
  std::stable_sort(pts.begin(), pts.end(), [](const TP& a, const TP& b) { return a.beat < b.beat; });
  bool trivial = pts.size() == 1 && (pts[0].sig == 0 || pts[0].sig == ((denominator_ << 16) | numerator_));
  if (trivial) return;

  sink.Line("<TEMPOENVEX");
  sink.Line("ACT 1 -1");
  sink.Line("VIS 1 0 1");
  sink.Line("LANEHEIGHT 0 0");
  sink.Line("ARM 0");
  sink.Line("DEFSHAPE 1 -1 -1");
  for (const TP& p : pts)
  {
    std::string line = "PT " + Fmt(tempo_.ToSeconds(p.beat)) + " " + Fmt(p.bpm) + " " + std::to_string(p.shape);
    if (p.sig) line += " " + std::to_string(p.sig);
    sink.Line(line);
  }
  sink.Line(">");
}

void Converter::Emit(RppSink& sink, xml_node metadata)
{
  sink.Line("<REAPER_PROJECT 0.1 \"7.0/win64\" 0");

  std::string comment = metadata.child_value("Comment");
  if (!comment.empty())
  {
    sink.Line("<NOTES 0 2");
    size_t pos = 0;
    while (pos <= comment.size())
    {
      size_t end = comment.find('\n', pos);
      if (end == std::string::npos) end = comment.size();
      std::string line = comment.substr(pos, end - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      sink.Line("|" + line);
      pos = end + 1;
    }
    sink.Line(">");
  }

  if (timeOffset_ != 0) sink.Line("PROJOFFS " + Fmt(timeOffset_) + " 0 0");
  std::string lock = arrangementBeats_ ? "1" : "0";
  sink.Line("TIMELOCKMODE " + lock);
  sink.Line("TEMPOENVLOCKMODE " + lock);
  sink.Line("TEMPO " + Fmt(tempo_.Segments()[0].bpm) + " " + std::to_string(numerator_) + " " + std::to_string(denominator_));

  std::string author = metadata.child_value("Artist");
  if (!author.empty()) sink.Line("AUTHOR " + RppQuote(author));
  static const char* tags[][2] = {{"Title", "ID3:TIT2"},  {"Artist", "ID3:TPE1"},    {"Album", "ID3:TALB"},
                                  {"Composer", "ID3:TCOM"}, {"Genre", "ID3:TCON"},   {"Year", "ID3:TYER"},
                                  {"Copyright", "ID3:TCOP"}, {"Comment", "ID3:COMM"}, {"OriginalArtist", "ID3:TOPE"}};
  std::vector<std::string> tagLines;
  for (auto& t : tags)
  {
    std::string v = metadata.child_value(t[0]);
    if (!v.empty()) tagLines.push_back(std::string("TAG ") + t[1] + " " + RppQuote(v));
  }
  if (!tagLines.empty())
  {
    sink.Line("<RENDER_METADATA");
    for (const std::string& l : tagLines) sink.Line(l);
    sink.Line(">");
  }

  if (master_.color) sink.Line("MASTERPEAKCOL " + std::to_string(master_.color));
  sink.Line(std::string("MASTERMUTESOLO ") + std::to_string((master_.mute ? 1 : 0) | (master_.solo ? 2 : 0)));
  sink.Line("MASTER_NCH " + std::to_string(master_.channels) + " 2");
  sink.Line("MASTER_VOLUME " + Fmt(master_.volume) + " " + Fmt(master_.pan) + " -1 -1 1");
  EmitEnvelope(sink, "MASTERVOLENV2", master_.volumeEnv, 0);
  EmitEnvelope(sink, "MASTERPANENV2", master_.panEnv, 0);
  EmitTempo(sink);

  std::stable_sort(markers_.begin(), markers_.end(), [](const Marker& a, const Marker& b) { return a.start < b.start; });
  int markerIndex = 1, regionIndex = 1;
  for (const Marker& m : markers_)
  {
    bool region = m.end >= 0;
    int idx = region ? regionIndex++ : markerIndex++;
    sink.Line("MARKER " + std::to_string(idx) + " " + Fmt(m.start) + " " + RppQuote(m.name) + (region ? " 1 " : " 0 ") +
              std::to_string(m.color));
    if (region) sink.Line("MARKER " + std::to_string(idx) + " " + Fmt(m.end) + " \"\" 1");
  }

  for (size_t i = 0; i < tracks_.size(); i++)
  {
    const Track& t = tracks_[i];
    int nextDepth = i + 1 < tracks_.size() ? tracks_[i + 1].depth : 0;
    int delta = nextDepth - t.depth;

    sink.Line("<TRACK");
    sink.Line("NAME " + RppQuote(t.name));
    if (t.color) sink.Line("PEAKCOL " + std::to_string(t.color));
    sink.Line("VOLPAN " + Fmt(t.volume) + " " + Fmt(t.pan) + " -1 -1 1");
    sink.Line(std::string("MUTESOLO ") + (t.mute ? "1" : "0") + (t.solo ? " 2" : " 0") + " 0");
    if (delta > 0) sink.Line("ISBUS 1 1");
    else if (delta < 0) sink.Line("ISBUS 2 " + std::to_string(delta));
    else sink.Line("ISBUS 0 0");
    sink.Line("NCHAN " + std::to_string(t.channels));
    sink.Line(std::string("MAINSEND ") + (t.mainSend ? "1" : "0") + " 0");
    for (const Receive& r : t.receives)
    {
      sink.Line("AUXRECV " + std::to_string(r.source) + " " + std::to_string(r.mode) + " " + Fmt(r.volume) + " " + Fmt(r.pan) +
                (r.mute ? " 1" : " 0") + " 0 0 0 0 -1:U 0 -1 ''");
      EmitEnvelope(sink, "AUXVOLENV", r.volumeEnv, 0);
      EmitEnvelope(sink, "AUXPANENV", r.panEnv, 0);
    }
    EmitEnvelope(sink, "VOLENV2", t.volumeEnv, 0);
    EmitEnvelope(sink, "PANENV2", t.panEnv, 0);
    EmitEnvelope(sink, "MUTEENV", t.muteEnv, 1);
    for (const Item& item : t.items) EmitItem(sink, item);
    sink.Line(">");
  }
  sink.Line(">");
}

bool Converter::RunXml(const std::string& xml, const std::string& sourcePath, RppSink& sink, std::string& error)
{
  sourcePath_ = sourcePath;
  return Convert(std::vector<uint8_t>(xml.begin(), xml.end()), {}, sink, error);
}

bool Converter::Convert(const std::vector<uint8_t>& xml, const std::vector<uint8_t>& meta, RppSink& sink,
                        std::string& error)
{
  pugi::xml_parse_result res = doc_.load_buffer(xml.data(), xml.size());
  if (!res)
  {
    error = std::string("project.xml could not be parsed: ") + res.description();
    return false;
  }
  xml_node project = doc_.child("Project");
  if (!project)
  {
    error = "project.xml has no <Project> element";
    return false;
  }

  pugi::xml_document metaDoc;
  if (!meta.empty()) metaDoc.load_buffer(meta.data(), meta.size());
  else if (xml_node m = project.child("xMetaData")) metaDoc.append_copy(m).set_name("MetaData");
  timeOffset_ = AttrDouble(project, "xTimeOffset", 0.0);

  xml_node app = project.child("Application");
  application_ = app.attribute("name").value();
  isBitwig_ = application_.find("Bitwig") != std::string::npos;
  report_.application = application_;
  if (app.attribute("version")) report_.application += std::string(" ") + app.attribute("version").value();

  IndexIds(project);
  xml_node arrangement = project.child("Arrangement");
  int u = AttrUnit(arrangement.child("Lanes"), "timeUnit");
  arrangementBeats_ = u != 0;

  ReadTransport(project);
  BuildTempoMap(arrangement);
  if (!tempoParamId_.empty()) params_[tempoParamId_] = {ParamTarget::Tempo};
  if (!signatureParamId_.empty()) params_[signatureParamId_] = {ParamTarget::TimeSignature};

  AddStructure(project.child("Structure"), 0, kNoTrack, xml_node());
  if (haveMaster_) ApplyChannel(kMasterTrack, master_.channel, true);
  for (int i = 0; i < (int)tracks_.size(); i++)
    if (tracks_[i].channel) ApplyChannel(i, tracks_[i].channel, !tracks_[i].offline);
  BuildRouting();

  View root;
  root.beats = arrangementBeats_;
  if (xml_node lanes = arrangement.child("Lanes")) Walk(lanes, kNoTrack, root, nullptr, 0);
  if (xml_node markers = arrangement.child("Markers"))
  {
    View mv = root;
    int mu = AttrUnit(markers, "timeUnit");
    if (mu >= 0) Rebase(mv, mu == 1);
    MarkersLeaf(markers, mv);
  }
  if (itemCount_ == 0 && project.child("Scenes").child("Scene"))
  {
    ConvertScenes(project.child("Scenes"));
    if (itemCount_ > 0) Warn("The arrangement was empty, so clip launcher scenes were laid out one after another");
  }

  Emit(sink, metaDoc.child("MetaData"));

  report_.tracks = (int)tracks_.size();
  report_.items = itemCount_;
  for (auto& w : warnings_)
    report_.warnings.push_back(w.second > 1 ? w.first + " (" + std::to_string(w.second) + "x)" : w.first);
  return true;
}

}  // namespace

bool ConvertProjectXml(const std::string& xml, const std::string& sourcePath, RppSink& sink,
                       const ConvertOptions& options, ConvertReport& report, std::string& error)
{
  Converter c(options, report);
  return c.RunXml(xml, sourcePath, sink, error);
}

}  // namespace dawp
