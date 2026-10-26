// Microsoft Compound File Binary format (structured storage), the container of AAF files.
// Reader for version 3 (512-byte sectors) and 4 (4096-byte sectors); writer for version 4.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace dawp {

using Clsid = std::array<uint8_t, 16>;

class CfbReader
{
public:
  struct Entry
  {
    std::u16string name;
    int type = 0;  // 1 storage, 2 stream, 5 root
    Clsid clsid{};
    uint32_t start = 0;
    uint64_t size = 0;
    std::vector<int> children;  // entry indices
  };

  CfbReader();
  ~CfbReader();
  CfbReader(const CfbReader&) = delete;
  CfbReader& operator=(const CfbReader&) = delete;

  bool Open(const std::string& path, std::string& error);
  const Entry& Get(int index) const { return entries_[index]; }
  int Root() const { return 0; }
  int Child(int storage, const std::u16string& name) const;  // -1 if missing
  int Child(int storage, const char* asciiName) const;

  bool ReadStream(int index, std::vector<uint8_t>& out) const;
  // Partial read of a (large) stream.
  bool ReadStreamRange(int index, uint64_t offset, size_t len, uint8_t* out) const;

private:
  bool ReadSector(uint32_t sector, uint8_t* buf) const;
  bool Chain(uint32_t start, std::vector<uint32_t>& out, const std::vector<uint32_t>& fat) const;
  void CollectChildren(int storage, uint32_t node, int depth);

  FILE* file_ = nullptr;
  uint32_t sectorSize_ = 512;
  uint32_t miniCutoff_ = 4096;
  std::vector<uint32_t> fat_, miniFat_;
  std::vector<uint32_t> miniStreamSectors_;
  std::vector<Entry> entries_;
  struct RawDir
  {
    uint32_t left, right, child;
  };
  std::vector<RawDir> raw_;
};

// Builds a compound file in memory (small streams) or with streams copied from files on write
// (embedded audio), then writes it in one pass.
class CfbWriter
{
public:
  CfbWriter();
  int Root() const { return 0; }
  int AddStorage(int parent, const std::u16string& name, const Clsid& clsid);
  int AddStream(int parent, const std::u16string& name, std::vector<uint8_t> data);
  // Copies 'size' bytes of 'path' starting at 'offset' when Write() runs.
  int AddFileStream(int parent, const std::u16string& name, const std::string& path, uint64_t offset, uint64_t size);
  void SetClsid(int entry, const Clsid& clsid) { nodes_[entry].clsid = clsid; }
  // The header's class id field (AAF stores its file signature there).
  void SetHeaderClsid(const Clsid& clsid) { headerClsid_ = clsid; }
  bool Write(const std::string& path, std::string& error);

private:
  struct Node
  {
    std::u16string name;
    int type;
    Clsid clsid{};
    int parent = -1;
    std::vector<int> children;
    std::vector<uint8_t> data;
    std::string srcPath;
    uint64_t srcOffset = 0, size = 0;
    // layout
    uint32_t start = 0xFFFFFFFE;
    bool mini = false;
    uint32_t left = 0xFFFFFFFF, right = 0xFFFFFFFF, child = 0xFFFFFFFF;
    bool red = false;
  };
  uint32_t BuildTree(std::vector<int>& sorted, int lo, int hi, int depth, int redDepth);
  std::vector<Node> nodes_;
  Clsid headerClsid_{};
};

}  // namespace dawp
