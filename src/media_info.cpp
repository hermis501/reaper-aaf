#include "media_info.h"

#include <cmath>
#include <cstring>
#include <vector>

#include "util.h"

namespace dawp {
namespace {

uint32_t LE32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t LE16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint64_t LE64(const uint8_t* p) { return LE32(p) | ((uint64_t)LE32(p + 4) << 32); }
uint32_t BE32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
uint16_t BE16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }

bool ReadAt(FILE* f, int64_t pos, uint8_t* buf, size_t n)
{
#ifdef _WIN32
  if (_fseeki64(f, pos, SEEK_SET) != 0) return false;
#else
  if (fseeko(f, pos, SEEK_SET) != 0) return false;
#endif
  return fread(buf, 1, n, f) == n;
}

bool ProbeWav(FILE* f, int64_t fileSize, const uint8_t* head, MediaInfo& info)
{
  bool rf64 = memcmp(head, "RF64", 4) == 0;
  uint64_t dataSize = 0, ds64Data = 0;
  int blockAlign = 0;
  bool haveFmt = false, haveData = false;
  int64_t pos = 12;
  uint8_t ch[8];
  while (pos + 8 <= fileSize && ReadAt(f, pos, ch, 8))
  {
    uint32_t size = LE32(ch + 4);
    if (memcmp(ch, "fmt ", 4) == 0 && size >= 16)
    {
      uint8_t fmt[16];
      if (!ReadAt(f, pos + 8, fmt, 16)) return false;
      info.channels = LE16(fmt + 2);
      info.sampleRate = (int)LE32(fmt + 4);
      blockAlign = LE16(fmt + 12);
      haveFmt = true;
    }
    else if (memcmp(ch, "ds64", 4) == 0 && size >= 16)
    {
      uint8_t d[16];
      if (!ReadAt(f, pos + 8, d, 16)) return false;
      ds64Data = LE64(d + 8);
    }
    else if (memcmp(ch, "data", 4) == 0)
    {
      dataSize = (rf64 && size == 0xFFFFFFFF) ? ds64Data : size;
      if ((int64_t)dataSize > fileSize - pos - 8) dataSize = fileSize - pos - 8;  // truncated / still recording
      haveData = true;
      if (haveFmt) break;
    }
    pos += 8 + (int64_t)size + (size & 1);
    if (rf64 && size == 0xFFFFFFFF) break;
  }
  if (!haveFmt || !haveData || blockAlign <= 0 || info.sampleRate <= 0) return false;
  info.duration = (double)(dataSize / blockAlign) / info.sampleRate;
  return true;
}

// 80-bit IEEE extended (AIFF sample rate)
double Extended(const uint8_t* p)
{
  int exponent = ((p[0] & 0x7F) << 8) | p[1];
  uint64_t mantissa = 0;
  for (int i = 0; i < 8; i++) mantissa = (mantissa << 8) | p[2 + i];
  if (exponent == 0 && mantissa == 0) return 0;
  double v = std::ldexp((double)mantissa, exponent - 16383 - 63);
  return (p[0] & 0x80) ? -v : v;
}

bool ProbeAiff(FILE* f, int64_t fileSize, MediaInfo& info)
{
  int64_t pos = 12;
  uint8_t ch[8];
  while (pos + 8 <= fileSize && ReadAt(f, pos, ch, 8))
  {
    uint32_t size = BE32(ch + 4);
    if (memcmp(ch, "COMM", 4) == 0 && size >= 18)
    {
      uint8_t c[18];
      if (!ReadAt(f, pos + 8, c, 18)) return false;
      info.channels = BE16(c);
      uint32_t frames = BE32(c + 2);
      info.sampleRate = (int)std::lround(Extended(c + 8));
      if (info.sampleRate <= 0) return false;
      info.duration = (double)frames / info.sampleRate;
      return true;
    }
    pos += 8 + (int64_t)size + (size & 1);
  }
  return false;
}

bool ProbeFlac(FILE* f, MediaInfo& info)
{
  uint8_t b[4 + 4 + 18];
  if (!ReadAt(f, 0, b, sizeof(b)) || (b[4] & 0x7F) != 0) return false;  // STREAMINFO comes first
  const uint8_t* s = b + 8;
  info.sampleRate = (s[10] << 12) | (s[11] << 4) | (s[12] >> 4);
  info.channels = ((s[12] >> 1) & 7) + 1;
  uint64_t total = ((uint64_t)(s[13] & 0x0F) << 32) | BE32(s + 14);
  if (info.sampleRate <= 0) return false;
  info.duration = (double)total / info.sampleRate;
  return total > 0;
}

}  // namespace

bool ProbeAudioFile(const std::string& path, MediaInfo& info)
{
  int64_t size = FileSize(path);
  if (size < 12) return false;
  FILE* f = OpenFile(path, "rb");
  if (!f) return false;
  uint8_t head[12];
  bool ok = false;
  if (ReadAt(f, 0, head, 12))
  {
    if ((memcmp(head, "RIFF", 4) == 0 || memcmp(head, "RF64", 4) == 0) && memcmp(head + 8, "WAVE", 4) == 0)
      ok = ProbeWav(f, size, head, info);
    else if (memcmp(head, "FORM", 4) == 0 && (memcmp(head + 8, "AIFF", 4) == 0 || memcmp(head + 8, "AIFC", 4) == 0))
      ok = ProbeAiff(f, size, info);
    else if (memcmp(head, "fLaC", 4) == 0)
      ok = ProbeFlac(f, info);
  }
  fclose(f);
  return ok;
}

}  // namespace dawp
