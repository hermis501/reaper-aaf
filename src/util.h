// Small platform helpers: UTF-8 paths, file I/O, locale-independent numbers, base64.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace dawp {

// Opens a file whose name is UTF-8 (wide API on Windows).
FILE* OpenFile(const std::string& utf8Path, const char* mode);
bool SeekFile(FILE* f, int64_t offset);  // from the start; works past 2 GB

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out);
bool WriteFileBytes(const std::string& path, const void* data, size_t len);
bool FileExists(const std::string& path);
int64_t FileSize(const std::string& path);  // -1 if missing
bool CreateDirectories(const std::string& path);
bool RemoveFile(const std::string& path);
bool RenameFile(const std::string& from, const std::string& to);  // replaces 'to'
std::string TempDirectory();
// Names of the entries in a folder (without "." and ".."); 'dirs' gets the subfolder flags.
bool ListDirectory(const std::string& path, std::vector<std::string>& names, std::vector<bool>& dirs);

std::string DirName(const std::string& path);
std::string FileName(const std::string& path);
std::string FileStem(const std::string& path);
std::string LowerExtension(const std::string& path);  // without dot
std::string JoinPath(const std::string& a, const std::string& b);
bool IsAbsolutePath(const std::string& path);
std::string NativeSeparators(std::string path);

// Locale-independent number conversion (REAPER and XML both use '.' as decimal point).
std::string FormatDouble(double v);
bool ParseDouble(const char* s, double& out);

std::string Base64Encode(const uint8_t* data, size_t len);
// Decodes and appends to 'out'; stops at the first '=' or invalid character. Returns bytes added.
size_t Base64Decode(const std::string& text, std::vector<uint8_t>& out);
std::string ToLower(std::string s);

// UTF-16 (as stored in AAF/CFB files) <-> UTF-8
std::string Utf16ToUtf8(const std::u16string& s);
std::u16string Utf8ToUtf16(const std::string& s);

}  // namespace dawp
