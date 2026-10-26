// reaper_aaf: opens .aaf files via File > Open project (and drag & drop) like native projects and
// adds "Export project to AAF..." (referenced or embedded media) to the File menu (also actions).

#include <exception>
#include <string>
#include <vector>

#include "aaf_export.h"
#include "aaf_import.h"
#include "reaper_ext.h"
#include "util.h"

using namespace rext;

namespace {

// ---------------------------------------------------------------------------------------------
// Import

bool WantProjectFile(const char* fn)
{
  return fn && dawp::LowerExtension(fn) == "aaf";
}

const char* EnumFileExtensions(int i, char** descptr)
{
  if (i == 0)
  {
    if (descptr) *descptr = (char*)"AAF files";
    return "AAF";
  }
  if (descptr) *descptr = nullptr;
  return nullptr;
}

int LoadProject(const char* fn, ProjectStateContext* ctx)
{
  dawp::ConvertOptions options;
  dawp::ConvertReport report;
  std::string error;
  ContextSink sink(ctx);
  bool ok = false;
  try
  {
    ok = dawp::ConvertAaf(fn, sink, options, report, error);
  }
  catch (const std::exception& e)
  {
    error = e.what();
  }
  catch (...)
  {
    error = "unexpected error";
  }

  if (!ok)
  {
    Report("AAF import failed: " + dawp::FileName(fn) + "\n" + error + "\n");
    return -1;
  }
  if (!report.warnings.empty())
  {
    std::string msg = "AAF import: " + dawp::FileName(fn) + "\n";
    if (!report.application.empty()) msg += "Exported by " + report.application + ". ";
    msg += std::to_string(report.tracks) + " tracks, " + std::to_string(report.items) + " items.\n";
    if (!report.mediaDir.empty()) msg += "Embedded media extracted to: " + report.mediaDir + "\n";
    msg += "Not converted:\n";
    for (const std::string& w : report.warnings) msg += "- " + w + "\n";
    Report(msg);
  }
  return 0;
}

project_import_register_t g_import = {WantProjectFile, EnumFileExtensions, LoadProject};

// ---------------------------------------------------------------------------------------------
// Export

void ExportProjectAaf(bool embed)
{
  ReaProject* proj;
  std::string projectFile, target, rpp;
  if (!PrepareExport(embed ? "Export project to AAF (media embedded)" : "Export project to AAF (media referenced)", "aaf",
                     "AAF files (*.aaf)|*.aaf", proj, projectFile, target, rpp))
    return;

  dawp::AafExportOptions options;
  options.embedMedia = embed;
  options.mediaDirs.push_back(dawp::TempDirectory());  // REAPER may write paths relative to the copy
  if (!projectFile.empty()) options.mediaDirs.push_back(dawp::DirName(projectFile));
  options.probeMedia = ProbeWithReaper;
  options.name = !projectFile.empty() ? dawp::FileStem(projectFile) : dawp::FileStem(target);
  dawp::AafExportReport report;
  std::string error;
  bool ok = false;
  try
  {
    ok = dawp::ExportAaf(rpp, target, options, report, error);
  }
  catch (const std::exception& e)
  {
    error = e.what();
  }
  if (!ok)
  {
    Report("AAF export failed: " + error + "\n");
    return;
  }
  std::string msg = "AAF export: saved " + target + "\n" + std::to_string(report.tracks) + " tracks, " +
                    std::to_string(report.clips) + " clips, " + std::to_string(report.mediaFiles) + " media files (" +
                    std::to_string(report.embedded) + " embedded).\n";
  if (!report.warnings.empty())
  {
    msg += "Not exported or approximated:\n";
    for (const std::string& w : report.warnings) msg += "- " + w + "\n";
  }
  Report(msg);
}

std::vector<ExportCommand> g_commands = {
    {"DAWPROJECT_EXPORT_AAF", "AAF: Export project to AAF (media referenced)...",
     "Export project to AAF (media referenced)...", [] { ExportProjectAaf(false); }},
    {"DAWPROJECT_EXPORT_AAF_EMBED", "AAF: Export project to AAF (media embedded)...",
     "Export project to AAF (media embedded)...", [] { ExportProjectAaf(true); }},
};

}  // namespace

extern "C" REAPER_PLUGIN_DLL_EXPORT int REAPER_PLUGIN_ENTRYPOINT(REAPER_PLUGIN_HINSTANCE hInstance,
                                                                 reaper_plugin_info_t* rec)
{
  (void)hInstance;
  if (!rec) return 0;  // unloading
  if (!Init(rec)) return 0;
  if (!rec->Register("projectimport", &g_import)) return 0;
  RegisterExportCommands(g_commands);
  return 1;
}
