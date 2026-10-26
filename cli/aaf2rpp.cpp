// Command-line AAF importer using the same code as the REAPER extension.
//   aaf2rpp <input.aaf> [output.rpp | -] [--media <folder>] [--xml <file>] [--pt-rendered-fades]
//   --xml writes the intermediate DAWproject-style XML; --pt-rendered-fades keeps Pro Tools'
//   rendered fade clips and sample accurate edits as they are.

#include <cstdio>
#include <string>
#include <vector>

#include "aaf_import.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

static int Run(const std::vector<std::string>& args)
{
  std::string input, output, media, xmlPath;
  dawp::AafImportOptions aafOptions;
  for (size_t i = 1; i < args.size(); i++)
  {
    if (args[i] == "--media" && i + 1 < args.size()) media = args[++i];
    else if (args[i] == "--xml" && i + 1 < args.size()) xmlPath = args[++i];
    else if (args[i] == "--pt-rendered-fades")
      aafOptions.proToolsTrueFades = aafOptions.proToolsRemoveSampleAccurateEdits = false;
    else if (input.empty()) input = args[i];
    else if (output.empty()) output = args[i];
  }
  if (input.empty())
  {
    fprintf(stderr,
            "usage: aaf2rpp <input.aaf> [output.rpp | -] [--media <folder>] [--xml <file>] [--pt-rendered-fades]\n");
    return 2;
  }
  if (output.empty()) output = dawp::JoinPath(dawp::DirName(input), dawp::FileStem(input) + ".rpp");

  dawp::ConvertOptions options;
  options.mediaDir = media;
  std::string error;
  if (!xmlPath.empty())
  {
    std::string xml;
    dawp::ConvertReport xmlReport;
    if (!dawp::AafToProjectXml(input, options, aafOptions, xml, xmlReport, error) ||
        !dawp::WriteFileBytes(xmlPath, xml.data(), xml.size()))
    {
      fprintf(stderr, "error: %s\n", error.empty() ? "cannot write the XML file" : error.c_str());
      return 1;
    }
  }

  FILE* f = output == "-" ? stdout : dawp::OpenFile(output, "wb");
  if (!f)
  {
    fprintf(stderr, "cannot write %s\n", output.c_str());
    return 1;
  }
  dawp::FileRppSink sink(f);
  dawp::ConvertReport report;
  bool ok = dawp::ConvertAaf(input, sink, options, report, error, aafOptions);
  if (f != stdout) fclose(f);
  if (!ok)
  {
    fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  fprintf(stderr, "%s: %d tracks, %d items%s%s\n", report.application.c_str(), report.tracks, report.items,
          report.mediaDir.empty() ? "" : ", media in ", report.mediaDir.c_str());
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
