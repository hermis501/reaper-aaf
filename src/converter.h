// Renders DAWproject-style project XML (built in memory by the AAF importer) into REAPER project
// (RPP) lines. Derived from the DAWproject converter of reaper-dawproject.
#pragma once

#include <string>
#include <vector>

#include "rpp_sink.h"

namespace dawp {

struct ConvertOptions
{
  // Folder for media the AAF importer extracts or builds. Empty: "<file name>-media" next to the
  // AAF file, falling back to the temp folder if that is not writable.
  std::string mediaDir;
};

struct ConvertReport
{
  std::vector<std::string> warnings;  // things that could not be converted, deduplicated
  std::string mediaDir;               // where embedded media was extracted (if any)
  std::string application;            // exporting application, e.g. "Pro Tools 2023.6"
  int tracks = 0;
  int items = 0;
};

// Converts DAWproject-style project XML that another importer built in memory (all media
// external). It may use these extensions, which are not part of DAWproject:
//   <Project xTimeOffset="sec">, <Project><xMetaData>...</xMetaData> (as metadata.xml),
//   <Clip xGain="linear" xFadeInShape="n" xFadeOutShape="n"> (REAPER fade shapes),
//   <Clip><xGainEnvelope><Point time="sec from clip start" value="linear" shape="0|1"/>,
//   <Audio xChannel="n"> (play only channel n, 0-based, of a multichannel file),
//   <Marker xDuration="sec"> (a region).
bool ConvertProjectXml(const std::string& xml, const std::string& sourcePath, RppSink& sink,
                       const ConvertOptions& options, ConvertReport& report, std::string& error);

}  // namespace dawp
