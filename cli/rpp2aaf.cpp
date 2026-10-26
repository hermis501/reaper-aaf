// Command-line AAF export of a REAPER project, using the same code as the REAPER extension.
//   rpp2aaf <input.rpp> [output.aaf] [--embed]

#include <cstdio>
#include <string>
#include <vector>

#include "aaf_export.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int Run(const std::vector<std::string>& args)
{
  std::string input, output;
  dawp::AafExportOptions options;
  for (size_t i = 1; i < args.size(); i++)
  {
    if (args[i] == "--embed") options.embedMedia = true;
    else if (input.empty()) input = args[i];
    else if (output.empty()) output = args[i];
  }
  if (input.empty())
  {
    fprintf(stderr, "usage: rpp2aaf <input.rpp> [output.aaf] [--embed]\n");
    return 2;
  }
  if (output.empty()) output = dawp::JoinPath(dawp::DirName(input), dawp::FileStem(input) + ".aaf");

  std::vector<uint8_t> bytes;
  if (!dawp::ReadFileBytes(input, bytes))
  {
    fprintf(stderr, "cannot read %s\n", input.c_str());
    return 1;
  }
  std::string dir = dawp::DirName(input);
  options.mediaDirs.push_back(dir.empty() ? "." : dir);
  options.name = dawp::FileStem(input);
  dawp::AafExportReport report;
  std::string error;
  if (!dawp::ExportAaf(std::string(bytes.begin(), bytes.end()), output, options, report, error))
  {
    fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  fprintf(stderr, "%s: %d tracks, %d clips, %d media files (%d embedded)\n", output.c_str(), report.tracks,
          report.clips, report.mediaFiles, report.embedded);
  for (const std::string& w : report.warnings) fprintf(stderr, "warning: %s\n", w.c_str());
  return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++)
  {
    int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &s[0], n, nullptr, nullptr);
    args.push_back(s);
  }
  return Run(args);
}
#else
int main(int argc, char** argv) { return Run(std::vector<std::string>(argv, argv + argc)); }
#endif
