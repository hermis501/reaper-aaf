#include "reaper_ext.h"

#include <cstring>

#include "util.h"

namespace rext {

void (*ShowConsoleMsg)(const char* msg);
const char* (*GetResourcePath)();
ReaProject* (*EnumProjects)(int idx, char* projfn, int projfn_sz);
MediaTrack* (*GetTrack)(ReaProject* proj, int idx);
MediaTrack* (*GetMasterTrack)(ReaProject* proj);
int (*CountTracks)(ReaProject* proj);
int (*TrackFX_GetCount)(MediaTrack* track);
int (*TrackFX_GetNumParams)(MediaTrack* track, int fx);
bool (*TrackFX_GetParamIdent)(MediaTrack* track, int fx, int param, char* buf, int buf_sz);
TrackEnvelope* (*GetFXEnvelope)(MediaTrack* track, int fx, int param, bool create);
bool (*InsertEnvelopePoint)(TrackEnvelope* env, double time, double value, int shape, double tension, bool selected,
                            bool* noSort);
bool (*DeleteEnvelopePointRange)(TrackEnvelope* env, double start, double end);
bool (*Envelope_SortPoints)(TrackEnvelope* env);
unsigned int (*GetSetTrackGroupMembership)(MediaTrack* tr, const char* group, unsigned int mask, unsigned int value);
bool (*GetSetProjectInfo_String)(ReaProject* proj, const char* desc, char* value, bool set);
void (*UpdateArrange)();
bool (*GetUserFileName)(int mode, const char* caption, const char* initial, const char* extensions, char* out, int out_sz);
void (*Main_SaveProjectEx)(ReaProject* proj, const char* filename, int options);

reaper_plugin_info_t* g_rec;

namespace {

PCM_source* (*PCM_Source_CreateFromFile)(const char* filename);
void (*PCM_Source_Destroy)(PCM_source* src);
double (*GetMediaSourceLength)(PCM_source* src, bool* isQN);
int (*GetMediaSourceSampleRate)(PCM_source* src);
int (*GetMediaSourceNumChannels)(PCM_source* src);

std::vector<ExportCommand>* g_commands;

template <class T>
void Load(T& fn, const char* name)
{
  *(void**)&fn = g_rec->GetFunc(name);
}

bool OnAction(KbdSectionInfo* sec, int command, int val, int val2, int relmode, HWND hwnd)
{
  (void)sec, (void)val, (void)val2, (void)relmode, (void)hwnd;
  if (!command || !g_commands) return false;
  for (const ExportCommand& c : *g_commands)
    if (c.command == command)
    {
      c.run();
      return true;
    }
  return false;
}

// REAPER's own File menu exports; ours go after the last of these that is present.
const int kFileMenuAnchors[] = {
    40849,  // File: Export project MIDI...
    40015,  // File: Render project to disk...
};

// Puts the export commands into the default File menu (REAPER calls this with flag 0 once, while
// it builds the menu; a user-customized File menu keeps the user's layout).
void MenuHook(const char* menuidstr, void* menu, int flag)
{
  if (flag != 0 || !menu || !g_commands || !menuidstr || strcmp(menuidstr, "Main file") != 0) return;
  HMENU m = (HMENU)menu;
  int n = GetMenuItemCount(m), pos = -1;
  for (int anchor : kFileMenuAnchors)
  {
    for (int i = 0; i < n && pos < 0; i++)
      if ((int)GetMenuItemID(m, i) == anchor) pos = i + 1;
    if (pos >= 0) break;
  }
  if (pos < 0) pos = n;
  // after entries another of these extensions already added, so the exports stay together
  char text[256];
  while (pos < n && GetMenuStringA(m, pos, text, sizeof(text), MF_BYPOSITION) > 0 &&
         strncmp(text, "Export project to ", 18) == 0)
    pos++;
  for (const ExportCommand& c : *g_commands)
  {
    if (!c.command) continue;
    MENUITEMINFOA mi = {};
    mi.cbSize = sizeof(mi);
    mi.fMask = MIIM_ID | MIIM_TYPE;
    mi.fType = MFT_STRING;
    mi.wID = (UINT)c.command;
    mi.dwTypeData = (char*)c.menuLabel;
    InsertMenuItemA(m, pos++, TRUE, &mi);
  }
}

}  // namespace

bool Init(reaper_plugin_info_t* rec)
{
  if (rec->caller_version != REAPER_PLUGIN_VERSION || !rec->GetFunc || !rec->Register) return false;
  g_rec = rec;
  Load(ShowConsoleMsg, "ShowConsoleMsg");
  Load(GetResourcePath, "GetResourcePath");
  Load(EnumProjects, "EnumProjects");
  Load(GetTrack, "GetTrack");
  Load(GetMasterTrack, "GetMasterTrack");
  Load(CountTracks, "CountTracks");
  Load(TrackFX_GetCount, "TrackFX_GetCount");
  Load(TrackFX_GetNumParams, "TrackFX_GetNumParams");
  Load(TrackFX_GetParamIdent, "TrackFX_GetParamIdent");
  Load(GetFXEnvelope, "GetFXEnvelope");
  Load(InsertEnvelopePoint, "InsertEnvelopePoint");
  Load(DeleteEnvelopePointRange, "DeleteEnvelopePointRange");
  Load(Envelope_SortPoints, "Envelope_SortPoints");
  Load(GetSetTrackGroupMembership, "GetSetTrackGroupMembership");
  Load(GetSetProjectInfo_String, "GetSetProjectInfo_String");
  Load(UpdateArrange, "UpdateArrange");
  Load(GetUserFileName, "GetUserFileName");
  Load(Main_SaveProjectEx, "Main_SaveProjectEx");
  Load(PCM_Source_CreateFromFile, "PCM_Source_CreateFromFile");
  Load(PCM_Source_Destroy, "PCM_Source_Destroy");
  Load(GetMediaSourceLength, "GetMediaSourceLength");
  Load(GetMediaSourceSampleRate, "GetMediaSourceSampleRate");
  Load(GetMediaSourceNumChannels, "GetMediaSourceNumChannels");
  return true;
}

void Report(const std::string& text)
{
  if (ShowConsoleMsg) ShowConsoleMsg(text.c_str());
}

bool ProbeWithReaper(const std::string& file, dawp::MediaInfo& info)
{
  if (!PCM_Source_CreateFromFile || !PCM_Source_Destroy || !GetMediaSourceLength) return false;
  PCM_source* src = PCM_Source_CreateFromFile(file.c_str());
  if (!src) return false;
  bool qn = false;
  info.duration = GetMediaSourceLength(src, &qn);
  info.sampleRate = GetMediaSourceSampleRate ? GetMediaSourceSampleRate(src) : 0;
  info.channels = GetMediaSourceNumChannels ? GetMediaSourceNumChannels(src) : 0;
  PCM_Source_Destroy(src);
  return !qn && info.duration > 0 && info.sampleRate > 0;
}

bool CanExport()
{
  return EnumProjects && GetUserFileName && Main_SaveProjectEx && GetTrack && GetMasterTrack && TrackFX_GetParamIdent;
}

bool PrepareExport(const char* title, const char* ext, const char* filter, ReaProject*& proj, std::string& projectFile,
                   std::string& target, std::string& rpp)
{
  proj = EnumProjects(-1, nullptr, 0);
  char buf[4096] = "";
  EnumProjects(-1, buf, sizeof(buf));
  projectFile = buf;

  std::string initial = !projectFile.empty() ? dawp::JoinPath(dawp::DirName(projectFile), dawp::FileStem(projectFile) + "." + ext)
                                             : "Untitled." + std::string(ext);
  std::vector<char> out(4096, '\0');
  if (!GetUserFileName(0, title, initial.c_str(), filter, out.data(), (int)out.size())) return false;
  target = out.data();
  if (dawp::LowerExtension(target) != ext) target += "." + std::string(ext);

  std::string temp = dawp::JoinPath(dawp::TempDirectory(), "reaper-" + std::string(ext) + "-export.rpp");
  Main_SaveProjectEx(proj, temp.c_str(), 0);
  std::vector<uint8_t> bytes;
  bool read = dawp::ReadFileBytes(temp, bytes);
  dawp::RemoveFile(temp);
  dawp::RemoveFile(temp + "-bak");
  if (!read)
  {
    Report(std::string(title) + " failed: REAPER did not save a copy of the project.\n");
    return false;
  }
  rpp.assign(bytes.begin(), bytes.end());
  return true;
}

void RegisterExportCommands(std::vector<ExportCommand>& commands)
{
  if (!CanExport()) return;
  bool any = false;
  for (ExportCommand& c : commands)
  {
    // the registration struct must stay alive while the extension is loaded
    auto* action = new custom_action_register_t{0, c.idStr, c.actionName, nullptr};
    c.command = g_rec->Register("custom_action", action);
    any = any || c.command;
  }
  if (!any) return;
  g_commands = &commands;
  g_rec->Register("hookcommand2", (void*)OnAction);
  g_rec->Register("hookcustommenu", (void*)MenuHook);
}

}  // namespace rext
