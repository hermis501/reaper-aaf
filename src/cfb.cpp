#include "cfb.h"

#include <algorithm>
#include <cstring>

#include "util.h"

namespace dawp {

namespace {

const uint32_t kFree = 0xFFFFFFFF;
const uint32_t kEndOfChain = 0xFFFFFFFE;
const uint32_t kFatSect = 0xFFFFFFFD;
const uint32_t kDifSect = 0xFFFFFFFC;
const uint32_t kNoStream = 0xFFFFFFFF;
const uint8_t kSignature[8] = {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint64_t Rd64(const uint8_t* p) { return Rd32(p) | ((uint64_t)Rd32(p + 4) << 32); }
void Wr16(uint8_t* p, uint16_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
void Wr32(uint8_t* p, uint32_t v)
{
  for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
void Wr64(uint8_t* p, uint64_t v)
{
  Wr32(p, (uint32_t)v);
  Wr32(p + 4, (uint32_t)(v >> 32));
}

char16_t Upper(char16_t c)
{
  if (c >= 'a' && c <= 'z') return (char16_t)(c - 32);
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return (char16_t)(c - 32);
  return c;
}

// Directory order: shorter names first, then a case-insensitive comparison.
bool NameLess(const std::u16string& a, const std::u16string& b)
{
  if (a.size() != b.size()) return a.size() < b.size();
  for (size_t i = 0; i < a.size(); i++)
  {
    char16_t x = Upper(a[i]), y = Upper(b[i]);
    if (x != y) return x < y;
  }
  return false;
}

bool NameEqual(const std::u16string& a, const std::u16string& b)
{
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); i++)
    if (Upper(a[i]) != Upper(b[i])) return false;
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Reader

CfbReader::CfbReader() = default;

CfbReader::~CfbReader()
{
  if (file_) fclose(file_);
}

bool CfbReader::ReadSector(uint32_t sector, uint8_t* buf) const
{
  if (!SeekFile(file_, (int64_t)(sector + 1) * sectorSize_)) return false;
  return fread(buf, 1, sectorSize_, file_) == sectorSize_;
}

bool CfbReader::Chain(uint32_t start, std::vector<uint32_t>& out, const std::vector<uint32_t>& fat) const
{
  out.clear();
  uint32_t s = start;
  while (s != kEndOfChain && s != kFree)
  {
    if (s >= fat.size() || out.size() > fat.size()) return false;  // corrupt or cyclic
    out.push_back(s);
    s = fat[s];
  }
  return true;
}

bool CfbReader::Open(const std::string& path, std::string& error)
{
  file_ = OpenFile(path, "rb");
  if (!file_)
  {
    error = "cannot open " + path;
    return false;
  }
  uint8_t h[512];
  if (fread(h, 1, 512, file_) != 512 || memcmp(h, kSignature, 8) != 0)
  {
    error = "not a compound file (AAF files are Microsoft structured storage)";
    return false;
  }
  uint16_t shift = Rd16(h + 30);
  if (shift != 9 && shift != 12)
  {
    error = "unsupported sector size";
    return false;
  }
  sectorSize_ = 1u << shift;
  uint32_t numFat = Rd32(h + 44), dirStart = Rd32(h + 48);
  miniCutoff_ = Rd32(h + 56);
  uint32_t miniFatStart = Rd32(h + 60), difatStart = Rd32(h + 68), numDifat = Rd32(h + 72);

  std::vector<uint32_t> fatSectors;
  for (int i = 0; i < 109 && fatSectors.size() < numFat; i++) fatSectors.push_back(Rd32(h + 76 + 4 * i));
  std::vector<uint8_t> buf(sectorSize_);
  uint32_t per = sectorSize_ / 4;
  uint32_t d = difatStart;
  for (uint32_t n = 0; n < numDifat && d != kEndOfChain && d != kFree && fatSectors.size() < numFat; n++)
  {
    if (!ReadSector(d, buf.data())) break;
    for (uint32_t i = 0; i < per - 1 && fatSectors.size() < numFat; i++) fatSectors.push_back(Rd32(&buf[4 * i]));
    d = Rd32(&buf[4 * (per - 1)]);
  }
  fat_.reserve((size_t)fatSectors.size() * per);
  for (uint32_t s : fatSectors)
  {
    if (!ReadSector(s, buf.data()))
    {
      error = "truncated file (FAT)";
      return false;
    }
    for (uint32_t i = 0; i < per; i++) fat_.push_back(Rd32(&buf[4 * i]));
  }

  std::vector<uint32_t> chain;
  if (!Chain(miniFatStart, chain, fat_))
  {
    error = "corrupt mini FAT";
    return false;
  }
  for (uint32_t s : chain)
  {
    if (!ReadSector(s, buf.data())) return error = "truncated file (mini FAT)", false;
    for (uint32_t i = 0; i < per; i++) miniFat_.push_back(Rd32(&buf[4 * i]));
  }

  if (!Chain(dirStart, chain, fat_) || chain.empty())
  {
    error = "corrupt directory";
    return false;
  }
  for (uint32_t s : chain)
  {
    if (!ReadSector(s, buf.data())) return error = "truncated file (directory)", false;
    for (uint32_t off = 0; off + 128 <= sectorSize_; off += 128)
    {
      const uint8_t* e = &buf[off];
      Entry en;
      uint16_t nameBytes = std::min<uint16_t>(Rd16(e + 64), 64);
      for (int i = 0; i + 2 <= nameBytes - 2; i += 2) en.name += (char16_t)Rd16(e + i);
      en.type = e[66];
      memcpy(en.clsid.data(), e + 80, 16);
      en.start = Rd32(e + 116);
      en.size = sectorSize_ == 512 ? Rd32(e + 120) : Rd64(e + 120);
      entries_.push_back(en);
      raw_.push_back({Rd32(e + 68), Rd32(e + 72), Rd32(e + 76)});
    }
  }
  if (entries_.empty() || entries_[0].type != 5)
  {
    error = "missing root entry";
    return false;
  }
  if (!Chain(entries_[0].start, miniStreamSectors_, fat_))
  {
    error = "corrupt mini stream";
    return false;
  }
  for (size_t i = 0; i < entries_.size(); i++)
    if (entries_[i].type == 1 || entries_[i].type == 5) CollectChildren((int)i, raw_[i].child, 0);
  return true;
}

void CfbReader::CollectChildren(int storage, uint32_t node, int depth)
{
  if (node == kNoStream || node >= entries_.size() || depth > 64) return;
  CollectChildren(storage, raw_[node].left, depth + 1);
  entries_[storage].children.push_back((int)node);
  CollectChildren(storage, raw_[node].right, depth + 1);
}

int CfbReader::Child(int storage, const std::u16string& name) const
{
  if (storage < 0) return -1;
  for (int c : entries_[storage].children)
    if (NameEqual(entries_[c].name, name)) return c;
  return -1;
}

int CfbReader::Child(int storage, const char* asciiName) const
{
  std::u16string n;
  for (const char* p = asciiName; *p; p++) n += (char16_t)(uint8_t)*p;
  return Child(storage, n);
}

bool CfbReader::ReadStreamRange(int index, uint64_t offset, size_t len, uint8_t* out) const
{
  const Entry& e = entries_[index];
  if (offset + len > e.size) return false;
  if (len == 0) return true;
  if (e.size < miniCutoff_)
  {
    // mini stream: 64-byte sectors inside the root's chain
    std::vector<uint32_t> chain;
    if (!Chain(e.start, chain, miniFat_)) return false;
    std::vector<uint8_t> sec(sectorSize_);
    size_t done = 0;
    while (done < len)
    {
      uint64_t pos = offset + done;
      uint32_t mini = chain[(size_t)(pos / 64)];
      uint64_t miniPos = (uint64_t)mini * 64 + pos % 64;
      uint32_t big = (uint32_t)(miniPos / sectorSize_);
      if (big >= miniStreamSectors_.size()) return false;
      size_t inSec = (size_t)(miniPos % sectorSize_);
      size_t n = std::min<size_t>({len - done, (size_t)(64 - pos % 64), (size_t)sectorSize_ - inSec});
      if (!SeekFile(file_, (int64_t)(miniStreamSectors_[big] + 1) * sectorSize_ + inSec)) return false;
      if (fread(out + done, 1, n, file_) != n) return false;
      done += n;
    }
    return true;
  }
  // regular sectors: walk the chain to the first sector, then read runs of consecutive sectors
  uint32_t s = e.start;
  uint64_t skip = offset / sectorSize_;
  for (uint64_t i = 0; i < skip; i++)
  {
    if (s >= fat_.size()) return false;
    s = fat_[s];
  }
  size_t done = 0;
  size_t inSec = (size_t)(offset % sectorSize_);
  while (done < len)
  {
    if (s >= fat_.size()) return false;
    uint32_t runStart = s;
    size_t runSectors = 1;
    size_t want = len - done + inSec;
    while ((uint64_t)runSectors * sectorSize_ < want && fat_[s] == s + 1)
    {
      s++;
      runSectors++;
    }
    size_t n = std::min<size_t>(len - done, runSectors * sectorSize_ - inSec);
    if (!SeekFile(file_, (int64_t)(runStart + 1) * sectorSize_ + inSec)) return false;
    if (fread(out + done, 1, n, file_) != n) return false;
    done += n;
    inSec = 0;
    s = fat_[s];
  }
  return true;
}

bool CfbReader::ReadStream(int index, std::vector<uint8_t>& out) const
{
  out.resize((size_t)entries_[index].size);
  return ReadStreamRange(index, 0, out.size(), out.data());
}

// ---------------------------------------------------------------------------------------------
// Writer

CfbWriter::CfbWriter()
{
  Node root;
  root.name = u"Root Entry";
  root.type = 5;
  nodes_.push_back(root);
}

int CfbWriter::AddStorage(int parent, const std::u16string& name, const Clsid& clsid)
{
  Node n;
  n.name = name.substr(0, 31);
  n.type = 1;
  n.clsid = clsid;
  n.parent = parent;
  nodes_.push_back(n);
  nodes_[parent].children.push_back((int)nodes_.size() - 1);
  return (int)nodes_.size() - 1;
}

int CfbWriter::AddStream(int parent, const std::u16string& name, std::vector<uint8_t> data)
{
  Node n;
  n.name = name.substr(0, 31);
  n.type = 2;
  n.parent = parent;
  n.size = data.size();
  n.data = std::move(data);
  nodes_.push_back(std::move(n));
  nodes_[parent].children.push_back((int)nodes_.size() - 1);
  return (int)nodes_.size() - 1;
}

int CfbWriter::AddFileStream(int parent, const std::u16string& name, const std::string& path, uint64_t offset,
                             uint64_t size)
{
  int i = AddStream(parent, name, {});
  nodes_[i].srcPath = path;
  nodes_[i].srcOffset = offset;
  nodes_[i].size = size;
  return i;
}

// Balanced binary tree over the sorted siblings. All leaves of a size-balanced tree are on the
// last two levels, so colouring only an incomplete last level red gives a valid red-black tree.
uint32_t CfbWriter::BuildTree(std::vector<int>& sorted, int lo, int hi, int depth, int redDepth)
{
  if (lo > hi) return kNoStream;
  int mid = (lo + hi + 1) / 2;
  Node& n = nodes_[sorted[mid]];
  n.red = depth == redDepth;
  n.left = BuildTree(sorted, lo, mid - 1, depth + 1, redDepth);
  nodes_[sorted[mid]].right = BuildTree(sorted, mid + 1, hi, depth + 1, redDepth);
  return (uint32_t)sorted[mid];
}

bool CfbWriter::Write(const std::string& path, std::string& error)
{
  const uint32_t secSize = 4096, per = secSize / 4, cutoff = 4096;

  // small file-backed streams go into the mini stream, which is written from memory
  for (Node& n : nodes_)
  {
    if (n.type != 2 || n.srcPath.empty() || n.size >= cutoff) continue;
    n.data.assign((size_t)n.size, 0);
    FILE* src = OpenFile(n.srcPath, "rb");
    bool got = src && SeekFile(src, (int64_t)n.srcOffset) && fread(n.data.data(), 1, n.data.size(), src) == n.data.size();
    if (src) fclose(src);
    if (!got)
    {
      error = "cannot read " + n.srcPath;
      return false;
    }
    n.srcPath.clear();
  }

  // directory trees
  for (Node& n : nodes_)
  {
    if (n.children.empty()) continue;
    std::vector<int> sorted = n.children;
    std::sort(sorted.begin(), sorted.end(), [&](int a, int b) { return NameLess(nodes_[a].name, nodes_[b].name); });
    int count = (int)sorted.size();
    int fullDepth = 0;  // levels of a perfect tree that fit
    while ((1 << (fullDepth + 1)) - 1 <= count) fullDepth++;
    int redDepth = ((1 << fullDepth) - 1 == count) ? -1 : fullDepth;
    n.child = BuildTree(sorted, 0, count - 1, 0, redDepth);
  }

  // mini stream
  std::vector<uint32_t> miniFat;
  uint64_t miniSize = 0;
  for (size_t i = 1; i < nodes_.size(); i++)
  {
    Node& n = nodes_[i];
    if (n.type != 2) continue;
    if (n.size == 0)
    {
      n.start = kEndOfChain;
      continue;
    }
    if (n.size < cutoff)
    {
      n.mini = true;
      uint32_t count = (uint32_t)((n.size + 63) / 64);
      n.start = (uint32_t)miniFat.size();
      for (uint32_t k = 0; k < count; k++) miniFat.push_back(k + 1 == count ? kEndOfChain : n.start + k + 1);
      miniSize += (uint64_t)count * 64;
    }
  }

  // sector layout: streams, mini stream, mini FAT, directory, FAT, DIFAT
  std::vector<uint32_t> fat;
  auto allocChain = [&](uint64_t bytes) -> uint32_t {
    uint32_t count = (uint32_t)((bytes + secSize - 1) / secSize);
    if (count == 0) return kEndOfChain;
    uint32_t start = (uint32_t)fat.size();
    for (uint32_t k = 0; k < count; k++) fat.push_back(k + 1 == count ? kEndOfChain : start + k + 1);
    return start;
  };
  for (size_t i = 1; i < nodes_.size(); i++)
  {
    Node& n = nodes_[i];
    if (n.type == 2 && !n.mini && n.size > 0) n.start = allocChain(n.size);
  }
  nodes_[0].start = allocChain(miniSize);
  nodes_[0].size = miniSize;
  uint32_t miniFatStart = allocChain((uint64_t)miniFat.size() * 4);
  uint32_t miniFatSectors = (uint32_t)((miniFat.size() * 4 + secSize - 1) / secSize);
  uint32_t dirSectors = (uint32_t)((nodes_.size() + 31) / 32);
  uint32_t dirStart = allocChain((uint64_t)dirSectors * secSize);

  uint32_t used = (uint32_t)fat.size();
  uint32_t fatSectors = 0, difatSectors = 0;
  for (;;)
  {
    uint32_t f = (used + fatSectors + difatSectors + per - 1) / per;
    uint32_t d = f > 109 ? (f - 109 + per - 2) / (per - 1) : 0;
    if (f == fatSectors && d == difatSectors) break;
    fatSectors = f;
    difatSectors = d;
  }
  uint32_t fatStart = used;
  for (uint32_t k = 0; k < fatSectors; k++) fat.push_back(kFatSect);
  uint32_t difatStart = (uint32_t)fat.size();
  for (uint32_t k = 0; k < difatSectors; k++) fat.push_back(kDifSect);
  uint32_t totalSectors = (uint32_t)fat.size();
  fat.resize((size_t)fatSectors * per, kFree);

  FILE* f = OpenFile(path, "wb");
  if (!f)
  {
    error = "cannot write " + path;
    return false;
  }
  bool ok = true;
  auto put = [&](const void* data, size_t len) {
    if (ok && len && fwrite(data, 1, len, f) != len) ok = false;
  };
  auto pad = [&](uint64_t written) {
    static const uint8_t zeros[4096] = {};
    uint64_t rem = written % secSize;
    if (rem) put(zeros, (size_t)(secSize - rem));
  };

  // header (one full 4096-byte sector in version 4)
  std::vector<uint8_t> h(secSize, 0);
  memcpy(h.data(), kSignature, 8);
  memcpy(&h[8], headerClsid_.data(), 16);
  Wr16(&h[24], 0x003E);
  Wr16(&h[26], 4);
  Wr16(&h[28], 0xFFFE);
  Wr16(&h[30], 12);
  Wr16(&h[32], 6);
  Wr32(&h[40], dirSectors);
  Wr32(&h[44], fatSectors);
  Wr32(&h[48], dirStart);
  Wr32(&h[56], cutoff);
  Wr32(&h[60], miniFat.empty() ? kEndOfChain : miniFatStart);
  Wr32(&h[64], miniFatSectors);
  Wr32(&h[68], difatSectors ? difatStart : kEndOfChain);
  Wr32(&h[72], difatSectors);
  for (uint32_t i = 0; i < 109; i++) Wr32(&h[76 + 4 * i], i < fatSectors ? fatStart + i : kFree);
  put(h.data(), h.size());

  // regular streams, in allocation order
  std::vector<uint8_t> chunk(1 << 20);
  for (size_t i = 1; i < nodes_.size() && ok; i++)
  {
    const Node& n = nodes_[i];
    if (n.type != 2 || n.mini || n.size == 0) continue;
    if (n.srcPath.empty())
      put(n.data.data(), n.data.size());
    else
    {
      FILE* src = OpenFile(n.srcPath, "rb");
      if (!src || !SeekFile(src, (int64_t)n.srcOffset))
      {
        if (src) fclose(src);
        error = "cannot read " + n.srcPath;
        fclose(f);
        return false;
      }
      uint64_t left = n.size;
      while (left > 0 && ok)
      {
        size_t want = (size_t)std::min<uint64_t>(left, chunk.size());
        size_t got = fread(chunk.data(), 1, want, src);
        if (got != want)
        {
          // the file shrank since it was measured: keep the declared size
          memset(chunk.data() + got, 0, want - got);
        }
        put(chunk.data(), want);
        left -= want;
      }
      fclose(src);
    }
    pad(n.size);
  }

  // mini stream
  for (size_t i = 1; i < nodes_.size(); i++)
  {
    const Node& n = nodes_[i];
    if (!n.mini) continue;
    put(n.data.data(), n.data.size());
    static const uint8_t zeros[64] = {};
    if (n.size % 64) put(zeros, (size_t)(64 - n.size % 64));
  }
  pad(miniSize);

  // mini FAT
  {
    std::vector<uint8_t> b(miniFat.size() * 4);
    for (size_t i = 0; i < miniFat.size(); i++) Wr32(&b[4 * i], miniFat[i]);
    put(b.data(), b.size());
    // unused mini FAT entries must read as free
    uint64_t rem = b.size() % secSize;
    if (rem)
    {
      std::vector<uint8_t> ff((size_t)(secSize - rem), 0xFF);
      put(ff.data(), ff.size());
    }
  }

  // directory
  {
    std::vector<uint8_t> dir((size_t)dirSectors * secSize, 0);
    for (size_t i = 0; i < (size_t)dirSectors * 32; i++)
    {
      uint8_t* e = &dir[i * 128];
      Wr32(e + 68, kNoStream);
      Wr32(e + 72, kNoStream);
      Wr32(e + 76, kNoStream);
      if (i >= nodes_.size()) continue;
      const Node& n = nodes_[i];
      for (size_t c = 0; c < n.name.size(); c++) Wr16(e + 2 * c, (uint16_t)n.name[c]);
      Wr16(e + 64, (uint16_t)((n.name.size() + 1) * 2));
      e[66] = (uint8_t)n.type;
      e[67] = n.red ? 0 : 1;
      Wr32(e + 68, n.left);
      Wr32(e + 72, n.right);
      Wr32(e + 76, n.child);
      memcpy(e + 80, n.clsid.data(), 16);
      if (n.type == 2 || n.type == 5)
      {
        Wr32(e + 116, n.type == 5 && n.size == 0 ? kEndOfChain : n.start);
        Wr64(e + 120, n.size);
      }
    }
    put(dir.data(), dir.size());
  }

  // FAT
  {
    std::vector<uint8_t> b(fat.size() * 4);
    for (size_t i = 0; i < fat.size(); i++) Wr32(&b[4 * i], fat[i]);
    put(b.data(), b.size());
  }

  // DIFAT
  for (uint32_t k = 0; k < difatSectors; k++)
  {
    std::vector<uint8_t> b(secSize, 0xFF);
    for (uint32_t i = 0; i < per - 1; i++)
    {
      uint32_t idx = 109 + k * (per - 1) + i;
      Wr32(&b[4 * i], idx < fatSectors ? fatStart + idx : kFree);
    }
    Wr32(&b[4 * (per - 1)], k + 1 < difatSectors ? difatStart + k + 1 : kEndOfChain);
    put(b.data(), b.size());
  }
  (void)totalSectors;

  ok = fclose(f) == 0 && ok;
  if (!ok) error = "write error on " + path;
  return ok;
}

}  // namespace dawp
