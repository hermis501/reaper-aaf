// Converts a REAPER project (RPP text) into an AAF file (AAF Edit Protocol: a composition with
// one audio track per REAPER track, master clips and file source mobs for the audio files).
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "media_info.h"

namespace dawp {

struct AafExportOptions
{
  // Copy WAV/AIFF files into the AAF. Otherwise (and for other formats) files are referenced.
  bool embedMedia = false;
  // Folders that relative media paths in the project are relative to, tried in order.
  std::vector<std::string> mediaDirs;
  // Audio file information for formats the built-in reader does not know (MP3, OGG, ...).
  std::function<bool(const std::string& file, MediaInfo& info)> probeMedia;
  // Composition name; empty: the project title or "REAPER project".
  std::string name;
};

struct AafExportReport
{
  std::vector<std::string> warnings;  // what could not be exported, deduplicated
  int tracks = 0;
  int clips = 0;
  int mediaFiles = 0;
  int embedded = 0;
};

bool ExportAaf(const std::string& rppText, const std::string& outPath, const AafExportOptions& options,
               AafExportReport& report, std::string& error);

}  // namespace dawp
