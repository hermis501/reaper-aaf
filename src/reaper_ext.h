// REAPER glue: API lookup, console messages, the project import sink,
// the export dialog + project copy, media probing through REAPER and File menu entries.
#pragma once

#include <string>
#include <vector>

#include "media_info.h"
#include "reaper_plugin.h"
#include "rpp_sink.h"

namespace rext {

// REAPER API, looked up by Init()
extern void (*ShowConsoleMsg)(const char* msg);
extern const char* (*GetResourcePath)();
extern ReaProject* (*EnumProjects)(int idx, char* projfn, int projfn_sz);
extern MediaTrack* (*GetTrack)(ReaProject* proj, int idx);
extern MediaTrack* (*GetMasterTrack)(ReaProject* proj);
extern int (*CountTracks)(ReaProject* proj);
extern int (*TrackFX_GetCount)(MediaTrack* track);
extern int (*TrackFX_GetNumParams)(MediaTrack* track, int fx);
extern bool (*TrackFX_GetParamIdent)(MediaTrack* track, int fx, int param, char* buf, int buf_sz);
extern TrackEnvelope* (*GetFXEnvelope)(MediaTrack* track, int fx, int param, bool create);
extern bool (*InsertEnvelopePoint)(TrackEnvelope* env, double time, double value, int shape, double tension,
                                   bool selected, bool* noSort);
extern bool (*DeleteEnvelopePointRange)(TrackEnvelope* env, double start, double end);
extern bool (*Envelope_SortPoints)(TrackEnvelope* env);
extern unsigned int (*GetSetTrackGroupMembership)(MediaTrack* tr, const char* group, unsigned int mask,
                                                  unsigned int value);
extern bool (*GetSetProjectInfo_String)(ReaProject* proj, const char* desc, char* value, bool set);
extern void (*UpdateArrange)();
extern bool (*GetUserFileName)(int mode, const char* caption, const char* initial, const char* extensions, char* out,
                               int out_sz);
extern void (*Main_SaveProjectEx)(ReaProject* proj, const char* filename, int options);

extern reaper_plugin_info_t* g_rec;

// Stores 'rec' and looks up the API; false if REAPER's version does not match.
bool Init(reaper_plugin_info_t* rec);

void Report(const std::string& text);

// Project import output: lines go straight into REAPER's project state.
class ContextSink : public dawp::RppSink
{
public:
  explicit ContextSink(ProjectStateContext* ctx) : ctx_(ctx) {}
  void Line(const std::string& line) override { ctx_->AddLine("%s", line.c_str()); }

private:
  ProjectStateContext* ctx_;
};

// Length, sample rate and channels of a media file as REAPER reads it (any format REAPER opens).
bool ProbeWithReaper(const std::string& file, dawp::MediaInfo& info);

// True when the API needed for exporting is there.
bool CanExport();

// Asks for the target file (with 'ext') and saves a copy of the current project as REAPER writes
// it; the project's own file and name are not touched.
bool PrepareExport(const char* title, const char* ext, const char* filter, ReaProject*& proj, std::string& projectFile,
                   std::string& target, std::string& rpp);

// Export commands: registered as actions in the Main section (a menu item runs an action) and
// added to the File menu next to REAPER's own exports. 'run' is called when one is triggered.
struct ExportCommand
{
  const char* idStr;       // action command ID (without the leading underscore)
  const char* actionName;  // name in the Actions list
  const char* menuLabel;   // File menu entry
  void (*run)();
  int command = 0;
};
void RegisterExportCommands(std::vector<ExportCommand>& commands);

}  // namespace rext
