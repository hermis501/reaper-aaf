// Converts an AAF file (Avid Media Composer, Pro Tools, Premiere Pro, DaVinci Resolve, ...) into
// REAPER project (RPP) lines. The top-level composition becomes DAWproject-style project XML
// (with the converter's x-extensions), which the DAWproject converter turns into RPP.
#pragma once

#include <string>
#include <vector>

#include "converter.h"

namespace dawp {

struct AafImportOptions
{
  // Pro Tools renders fades into separate "Fade" clips and splits clips at sub-frame edits
  // ("Sample accurate edit"). Replace those with real fades / merge them into their neighbours.
  bool proToolsTrueFades = true;
  bool proToolsRemoveSampleAccurateEdits = true;
};

bool ConvertAaf(const std::string& path, RppSink& sink, const ConvertOptions& options, ConvertReport& report,
                std::string& error, const AafImportOptions& aafOptions = AafImportOptions());

// The intermediate project XML (for tests and diagnostics). Media extraction happens as for
// ConvertAaf; warnings are added to 'report'.
bool AafToProjectXml(const std::string& path, const ConvertOptions& options, const AafImportOptions& aafOptions,
                     std::string& xml, ConvertReport& report, std::string& error);

}  // namespace dawp
