#include "util.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#endif
#ifdef __APPLE__
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <xlocale.h>
#endif

namespace dawp {

#ifdef _WIN32
static std::wstring Widen(const std::string& s)
{
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

static std::string Narrow(const std::wstring& w)
{
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
#endif

FILE* OpenFile(const std::string& utf8Path, const char* mode)
{
#ifdef _WIN32
  return _wfopen(Widen(utf8Path).c_str(), Widen(mode).c_str());
#else
  return fopen(utf8Path.c_str(), mode);
#endif
}

bool SeekFile(FILE* f, int64_t offset)
{
#ifdef _WIN32
  return _fseeki64(f, offset, SEEK_SET) == 0;
#else
  return fseeko(f, (off_t)offset, SEEK_SET) == 0;
#endif
}

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out)
{
  FILE* f = OpenFile(path, "rb");
  if (!f) return false;
  out.clear();
  uint8_t buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.insert(out.end(), buf, buf + n);
  fclose(f);
  return true;
}

bool WriteFileBytes(const std::string& path, const void* data, size_t len)
{
  FILE* f = OpenFile(path, "wb");
  if (!f) return false;
  bool ok = len == 0 || fwrite(data, 1, len, f) == len;
  ok = fclose(f) == 0 && ok;
  return ok;
}

int64_t FileSize(const std::string& path)
{
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExW(Widen(path).c_str(), GetFileExInfoStandard, &fad)) return -1;
  if (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
  return ((int64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
#else
  struct stat st;
  if (stat(path.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) return -1;
  return (int64_t)st.st_size;
#endif
}

bool FileExists(const std::string& path) { return FileSize(path) >= 0; }

static bool DirectoryExists(const std::string& path)
{
#ifdef _WIN32
  DWORD a = GetFileAttributesW(Widen(path).c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool CreateDirectories(const std::string& path)
{
  if (path.empty() || DirectoryExists(path)) return true;
  std::string parent = DirName(path);
  if (!parent.empty() && parent != path && !CreateDirectories(parent)) return false;
#ifdef _WIN32
  return CreateDirectoryW(Widen(path).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
#else
  return mkdir(path.c_str(), 0777) == 0 || DirectoryExists(path);
#endif
}

bool ListDirectory(const std::string& path, std::vector<std::string>& names, std::vector<bool>& dirs)
{
  names.clear();
  dirs.clear();
#ifdef _WIN32
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW(Widen(JoinPath(path, "*")).c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return false;
  do
  {
    std::string n = Narrow(fd.cFileName);
    if (n == "." || n == "..") continue;
    names.push_back(n);
    dirs.push_back((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  return true;
#else
  DIR* d = opendir(path.c_str());
  if (!d) return false;
  while (struct dirent* e = readdir(d))
  {
    std::string n = e->d_name;
    if (n == "." || n == "..") continue;
    names.push_back(n);
    dirs.push_back(DirectoryExists(JoinPath(path, n)));
  }
  closedir(d);
  return true;
#endif
}

bool RemoveFile(const std::string& path)
{
#ifdef _WIN32
  return DeleteFileW(Widen(path).c_str()) != 0;
#else
  return remove(path.c_str()) == 0;
#endif
}

bool RenameFile(const std::string& from, const std::string& to)
{
#ifdef _WIN32
  return MoveFileExW(Widen(from).c_str(), Widen(to).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
#else
  return rename(from.c_str(), to.c_str()) == 0;
#endif
}

std::string TempDirectory()
{
#ifdef _WIN32
  wchar_t buf[MAX_PATH + 1];
  DWORD n = GetTempPathW(MAX_PATH + 1, buf);
  if (n == 0) return ".";
  std::string s = Narrow(std::wstring(buf, n));
  while (!s.empty() && (s.back() == '\\' || s.back() == '/')) s.pop_back();
  return s;
#else
  const char* t = getenv("TMPDIR");
  return t && *t ? t : "/tmp";
#endif
}

static bool IsSep(char c) { return c == '/' || c == '\\'; }

std::string DirName(const std::string& path)
{
  size_t i = path.size();
  while (i > 0 && !IsSep(path[i - 1])) i--;
  if (i == 0) return std::string();
  // keep the root separator ("/" or "C:\")
  size_t end = i - 1;
  if (end == 0 || (end == 2 && path[1] == ':')) return path.substr(0, end + 1);
  return path.substr(0, end);
}

std::string FileName(const std::string& path)
{
  size_t i = path.size();
  while (i > 0 && !IsSep(path[i - 1])) i--;
  return path.substr(i);
}

std::string FileStem(const std::string& path)
{
  std::string name = FileName(path);
  size_t dot = name.rfind('.');
  return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string LowerExtension(const std::string& path)
{
  std::string name = FileName(path);
  size_t dot = name.rfind('.');
  return dot == std::string::npos ? std::string() : ToLower(name.substr(dot + 1));
}

std::string JoinPath(const std::string& a, const std::string& b)
{
  if (a.empty()) return b;
  if (IsSep(a.back())) return a + b;
#ifdef _WIN32
  return a + "\\" + b;
#else
  return a + "/" + b;
#endif
}

bool IsAbsolutePath(const std::string& path)
{
  if (path.empty()) return false;
  if (IsSep(path[0])) return true;
  return path.size() >= 2 && path[1] == ':' && ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'));
}

std::string NativeSeparators(std::string path)
{
#ifdef _WIN32
  std::replace(path.begin(), path.end(), '/', '\\');
#else
  std::replace(path.begin(), path.end(), '\\', '/');
#endif
  return path;
}

std::string FormatDouble(double v)
{
  if (!std::isfinite(v)) v = 0.0;
  if (v == 0.0) return "0";  // also turns -0 into 0
  char buf[64];
#ifdef __APPLE__
  // Apple's libc++ has floating point to_chars only from macOS 13.3: the shortest "%g" form that
  // reads back exactly, in the C locale (integers in plain notation, as to_chars writes them)
  static locale_t c = newlocale(LC_ALL_MASK, "C", nullptr);
  if (std::fabs(v) < 1e15 && v == std::floor(v)) snprintf_l(buf, sizeof(buf), c, "%.0f", v);
  else
    for (int prec = 1; prec <= 17; prec++)
    {
      snprintf_l(buf, sizeof(buf), c, "%.*g", prec, v);
      if (strtod_l(buf, nullptr, c) == v) break;
    }
  return buf;
#else
  auto res = std::to_chars(buf, buf + sizeof(buf), v);
  return std::string(buf, res.ptr);
#endif
}

bool ParseDouble(const char* s, double& out)
{
  if (!s) return false;
  while (*s == ' ' || *s == '\t' || *s == '+') s++;
  if (!*s) return false;
#ifdef __APPLE__
  static locale_t c = newlocale(LC_ALL_MASK, "C", nullptr);
  if (!(isdigit((unsigned char)*s) || *s == '-' || *s == '.')) return false;
  char* end = nullptr;
  errno = 0;
  double v = strtod_l(s, &end, c);
  if (end == s || errno == ERANGE) return false;
  out = v;
  return true;
#else
  const char* end = s + strlen(s);
  auto res = std::from_chars(s, end, out);
  return res.ec == std::errc();
#endif
}

std::string Base64Encode(const uint8_t* data, size_t len)
{
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < len; i += 3)
  {
    uint32_t n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += tbl[(n >> 6) & 63];
    out += tbl[n & 63];
  }
  if (i < len)
  {
    uint32_t n = data[i] << 16;
    if (i + 1 < len) n |= data[i + 1] << 8;
    out += tbl[(n >> 18) & 63];
    out += tbl[(n >> 12) & 63];
    out += i + 1 < len ? tbl[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

size_t Base64Decode(const std::string& text, std::vector<uint8_t>& out)
{
  size_t before = out.size();
  uint32_t acc = 0;
  int bits = 0;
  for (char c : text)
  {
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else break;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      out.push_back((uint8_t)(acc >> bits));
    }
  }
  return out.size() - before;
}

std::string ToLower(std::string s)
{
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

std::string Utf16ToUtf8(const std::u16string& s)
{
  std::string out;
  for (size_t i = 0; i < s.size(); i++)
  {
    uint32_t c = s[i];
    if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000)
      c = 0x10000 + ((c - 0xD800) << 10) + (s[++i] - 0xDC00);
    if (c < 0x80) out += (char)c;
    else if (c < 0x800) out += {(char)(0xC0 | (c >> 6)), (char)(0x80 | (c & 0x3F))};
    else if (c < 0x10000) out += {(char)(0xE0 | (c >> 12)), (char)(0x80 | ((c >> 6) & 0x3F)), (char)(0x80 | (c & 0x3F))};
    else
      out += {(char)(0xF0 | (c >> 18)), (char)(0x80 | ((c >> 12) & 0x3F)), (char)(0x80 | ((c >> 6) & 0x3F)),
              (char)(0x80 | (c & 0x3F))};
  }
  return out;
}

std::u16string Utf8ToUtf16(const std::string& s)
{
  std::u16string out;
  for (size_t i = 0; i < s.size();)
  {
    uint8_t b = (uint8_t)s[i];
    uint32_t c;
    int extra;
    if (b < 0x80) c = b, extra = 0;
    else if ((b & 0xE0) == 0xC0) c = b & 0x1F, extra = 1;
    else if ((b & 0xF0) == 0xE0) c = b & 0x0F, extra = 2;
    else if ((b & 0xF8) == 0xF0) c = b & 0x07, extra = 3;
    else c = 0xFFFD, extra = 0;
    i++;
    for (int k = 0; k < extra && i < s.size(); k++, i++) c = (c << 6) | ((uint8_t)s[i] & 0x3F);
    if (c >= 0x10000)
    {
      c -= 0x10000;
      out += (char16_t)(0xD800 + (c >> 10));
      out += (char16_t)(0xDC00 + (c & 0x3FF));
    }
    else
      out += (char16_t)c;
  }
  return out;
}

}  // namespace dawp
