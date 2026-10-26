// Reads length, sample rate and channel count from audio file headers (WAV/RF64, AIFF, FLAC).
#pragma once

#include <string>

namespace dawp {

struct MediaInfo
{
  double duration = 0;  // seconds
  int sampleRate = 0;
  int channels = 0;
};

bool ProbeAudioFile(const std::string& path, MediaInfo& info);

}  // namespace dawp
