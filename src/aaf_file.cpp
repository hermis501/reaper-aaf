#include "aaf_file.h"

#include <algorithm>
#include <deque>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <unordered_map>

#include "util.h"

namespace dawp {

#include "aaf_model_data.inc"

// ---------------------------------------------------------------------------------------------
// AUIDs and the model

Auid AuidFromString(const char* t)
{
  // text: Data1(8)-Data2(4)-Data3(4)-Data4[0..1](4)-Data4[2..7](12); stored little-endian per field
  unsigned v[16] = {};
  int n = 0;
  for (const char* p = t; *p && n < 32; p++)
  {
    int d;
    if (*p >= '0' && *p <= '9') d = *p - '0';
    else if (*p >= 'a' && *p <= 'f') d = *p - 'a' + 10;
    else if (*p >= 'A' && *p <= 'F') d = *p - 'A' + 10;
    else continue;
    v[n / 2] = (v[n / 2] << 4) | d;
    n++;
  }
  Auid a{};
  a[0] = (uint8_t)v[3], a[1] = (uint8_t)v[2], a[2] = (uint8_t)v[1], a[3] = (uint8_t)v[0];
  a[4] = (uint8_t)v[5], a[5] = (uint8_t)v[4];
  a[6] = (uint8_t)v[7], a[7] = (uint8_t)v[6];
  for (int i = 8; i < 16; i++) a[i] = (uint8_t)v[i];
  return a;
}

std::string AuidToString(const Auid& a)
{
  char buf[40];
  snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", a[3], a[2], a[1],
           a[0], a[5], a[4], a[7], a[6], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]);
  return buf;
}

bool IsNull(const Auid& a)
{
  for (uint8_t b : a)
    if (b) return false;
  return true;
}

namespace {

struct ModelIndex
{
  std::unordered_map<std::string, const AafClassModel*> classByName;
  std::map<Auid, const AafClassModel*> classByAuid;
  std::map<Auid, const AafTypeModel*> typeByAuid;
  std::map<Auid, uint16_t> pidByPropAuid;
  ModelIndex()
  {
    for (const AafClassModel& c : kAafClasses)
    {
      classByName[c.name] = &c;
      classByAuid[AuidFromString(c.auid)] = &c;
      for (int i = 0; i < c.numProps; i++) pidByPropAuid[AuidFromString(c.props[i].auid)] = c.props[i].pid;
    }
    for (const AafTypeModel& t : kAafTypes) typeByAuid[AuidFromString(t.auid)] = &t;
  }
};

const ModelIndex& Model()
{
  static ModelIndex m;
  return m;
}

uint16_t Rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

std::u16string Ascii16(const std::string& s)
{
  std::u16string r;
  for (char c : s) r += (char16_t)(uint8_t)c;
  return r;
}

std::u16string HexKey(uint32_t k)
{
  char buf[16];
  snprintf(buf, sizeof(buf), "{%x}", k);
  return Ascii16(buf);
}

std::u16string DecodeUtf16(const uint8_t* p, size_t n)
{
  std::u16string s;
  for (size_t i = 0; i + 1 < n; i += 2)
  {
    char16_t c = (char16_t)Rd16(p + i);
    if (!c) break;
    s += c;
  }
  return s;
}

}  // namespace

const AafClassModel* FindClassModel(const char* name)
{
  auto it = Model().classByName.find(name);
  return it == Model().classByName.end() ? nullptr : it->second;
}

const AafClassModel* FindClassModel(const Auid& auid)
{
  auto it = Model().classByAuid.find(auid);
  return it == Model().classByAuid.end() ? nullptr : it->second;
}

const AafTypeModel* FindTypeModel(const Auid& auid)
{
  auto it = Model().typeByAuid.find(auid);
  return it == Model().typeByAuid.end() ? nullptr : it->second;
}

// ---------------------------------------------------------------------------------------------
// Reader

bool AafReader::Open(const std::string& path, std::string& error)
{
  if (!cfb_.Open(path, error)) return false;
  Obj root = Load(cfb_.Root());
  if (!root)
  {
    error = "not an AAF file (no root properties)";
    return false;
  }
  header_ = Ref(root, 0x0002);
  metadict_ = Ref(root, 0x0001);
  if (!header_)
  {
    error = "not an AAF file (no header object)";
    return false;
  }
  for (const AafClassModel& c : kAafClasses)
    if (c.parent) parents_[AuidFromString(c.auid)] = AuidFromString(c.parent);
  if (metadict_)
  {
    for (Obj cd : Refs(metadict_, 0x0003))
    {
      Auid id = AuidValue(cd, 0x0005);
      Auid parent;
      if (WeakRefKey(cd, 0x0008, parent) && parent != id && !parents_.count(id)) parents_[id] = parent;
      for (Obj pd : Refs(cd, 0x0009))
      {
        std::string name = String(pd, 0x0006);
        uint16_t pid = (uint16_t)UInt(pd, 0x000d);
        if (pid >= 0x8000 || !pidsByName_.count(name)) pidsByName_[name] = pid;
      }
    }
    for (Obj td : Refs(metadict_, 0x0004)) typeNames_[AuidValue(td, 0x0005)] = String(td, 0x0006);
  }
  if (Obj dict = Ref(header_, 0x3b04))
  {
    for (uint16_t pid = 0x2603; pid <= 0x260b; pid++)
      for (Obj d : Refs(dict, pid)) defNames_[AuidValue(d, 0x1b01)] = String(d, 0x1b02);
  }
  return true;
}

AafReader::Obj AafReader::Load(int entry) const
{
  if (entry < 0) return nullptr;
  auto it = objects_.find(entry);
  if (it != objects_.end()) return it->second.get();
  auto obj = std::make_unique<Object>();
  obj->entry = entry;
  memcpy(obj->cls.data(), cfb_.Get(entry).clsid.data(), 16);
  int ps = cfb_.Child(entry, "properties");
  std::vector<uint8_t> d;
  if (ps < 0 || !cfb_.ReadStream(ps, d) || d.size() < 4) return nullptr;
  int count = Rd16(&d[2]);
  size_t pos = 4 + (size_t)count * 6;
  if (pos > d.size()) return nullptr;
  for (int i = 0; i < count; i++)
  {
    const uint8_t* e = &d[4 + i * 6];
    uint16_t pid = Rd16(e), form = Rd16(e + 2), len = Rd16(e + 4);
    if (pos + len > d.size()) break;
    Prop p;
    p.form = form;
    p.data.assign(d.begin() + pos, d.begin() + pos + len);
    obj->props[pid] = std::move(p);
    pos += len;
  }
  Obj raw = obj.get();
  objects_[entry] = std::move(obj);
  return raw;
}

const AafReader::Prop* AafReader::Find(Obj o, uint16_t pid) const
{
  if (!o || !pid) return nullptr;
  auto it = o->props.find(pid);
  return it == o->props.end() ? nullptr : &it->second;
}

uint64_t AafReader::UInt(Obj o, uint16_t pid, uint64_t def) const
{
  const Prop* p = Find(o, pid);
  if (!p || p->data.empty() || p->data.size() > 8) return def;
  uint64_t v = 0;
  for (size_t i = 0; i < p->data.size(); i++) v |= (uint64_t)p->data[i] << (8 * i);
  return v;
}

int64_t AafReader::Int(Obj o, uint16_t pid, int64_t def) const
{
  const Prop* p = Find(o, pid);
  if (!p || p->data.empty() || p->data.size() > 8) return def;
  uint64_t v = UInt(o, pid);
  if (p->data.size() == 4) return (int32_t)(uint32_t)v;
  return (int64_t)v;
}

bool AafReader::Bool(Obj o, uint16_t pid, bool def) const
{
  const Prop* p = Find(o, pid);
  if (!p || p->data.empty()) return def;
  return p->data[0] != 0;
}

std::string AafReader::String(Obj o, uint16_t pid) const
{
  const Prop* p = Find(o, pid);
  if (!p) return std::string();
  return Utf16ToUtf8(DecodeUtf16(p->data.data(), p->data.size()));
}

AafRational AafReader::Rational(Obj o, uint16_t pid) const
{
  AafRational r;
  const Prop* p = Find(o, pid);
  if (p && p->data.size() >= 8)
  {
    r.num = (int32_t)Rd32(&p->data[0]);
    r.den = (int32_t)Rd32(&p->data[4]);
  }
  return r;
}

Auid AafReader::AuidValue(Obj o, uint16_t pid) const
{
  Auid a{};
  const Prop* p = Find(o, pid);
  if (p && p->data.size() >= 16) memcpy(a.data(), p->data.data(), 16);
  return a;
}

MobId AafReader::MobIdValue(Obj o, uint16_t pid) const
{
  MobId m{};
  const Prop* p = Find(o, pid);
  if (p && p->data.size() >= 32) memcpy(m.data(), p->data.data(), 32);
  return m;
}

bool AafReader::Indirect(Obj o, uint16_t pid, Auid& type, std::vector<uint8_t>& value) const
{
  const Prop* p = Find(o, pid);
  if (!p || p->data.size() < 17) return false;
  memcpy(type.data(), &p->data[1], 16);
  value.assign(p->data.begin() + 17, p->data.end());
  return true;
}

bool AafReader::IndirectNumber(Obj o, uint16_t pid, double& out) const
{
  Auid t;
  std::vector<uint8_t> v;
  if (!Indirect(o, pid, t, v)) return false;
  static const Auid kRationalType = AuidFromString("03010100-0000-0000-060e-2b3401040101");
  if (t == kRationalType && v.size() >= 8)
  {
    int32_t den = (int32_t)Rd32(&v[4]);
    out = den ? (double)(int32_t)Rd32(&v[0]) / den : 0.0;
    return true;
  }
  const AafTypeModel* tm = FindTypeModel(t);
  while (tm && tm->kind == AafTypeModel::kRename) tm = tm->element ? FindTypeModel(AuidFromString(tm->element)) : nullptr;
  if (tm && tm->kind == AafTypeModel::kInt && (size_t)tm->size <= v.size())
  {
    uint64_t x = 0;
    for (int i = 0; i < tm->size; i++) x |= (uint64_t)v[i] << (8 * i);
    if (tm->isSigned && tm->size < 8 && (x >> (8 * tm->size - 1)) & 1) x |= ~0ull << (8 * tm->size);
    out = tm->isSigned ? (double)(int64_t)x : (double)x;
    return true;
  }
  return false;
}

bool AafReader::IndirectString(Obj o, uint16_t pid, std::string& out) const
{
  Auid t;
  std::vector<uint8_t> v;
  if (!Indirect(o, pid, t, v)) return false;
  static const Auid kStringType = AuidFromString("01100200-0000-0000-060e-2b3401040101");
  if (t != kStringType) return false;
  out = Utf16ToUtf8(DecodeUtf16(v.data(), v.size()));
  return true;
}

std::u16string AafReader::RefName(const Prop& p) const { return DecodeUtf16(p.data.data(), p.data.size()); }

AafReader::Obj AafReader::Ref(Obj o, uint16_t pid) const
{
  const Prop* p = Find(o, pid);
  if (!p || (p->form != 0x22)) return nullptr;
  return Load(cfb_.Child(o->entry, RefName(*p)));
}

std::vector<AafReader::Obj> AafReader::Refs(Obj o, uint16_t pid) const
{
  std::vector<Obj> out;
  const Prop* p = Find(o, pid);
  if (!p || (p->form != 0x32 && p->form != 0x3A)) return out;
  std::u16string name = RefName(*p);
  int idx = cfb_.Child(o->entry, name + u" index");
  std::vector<uint8_t> d;
  if (idx < 0 || !cfb_.ReadStream(idx, d) || d.size() < 12) return out;
  uint32_t count = Rd32(&d[0]);
  if (p->form == 0x32)
  {
    for (uint32_t i = 0; i < count && 12 + 4 * (size_t)i + 4 <= d.size(); i++)
      if (Obj c = Load(cfb_.Child(o->entry, name + HexKey(Rd32(&d[12 + 4 * i]))))) out.push_back(c);
  }
  else
  {
    if (d.size() < 15) return out;
    uint8_t keySize = d[14];
    size_t pos = 15;
    for (uint32_t i = 0; i < count && pos + 8 + keySize <= d.size(); i++, pos += 8 + keySize)
      if (Obj c = Load(cfb_.Child(o->entry, name + HexKey(Rd32(&d[pos]))))) out.push_back(c);
  }
  return out;
}

bool AafReader::WeakRefKey(Obj o, uint16_t pid, Auid& key) const
{
  const Prop* p = Find(o, pid);
  if (!p || p->form != 0x02 || p->data.size() < 5 + 16) return false;
  memcpy(key.data(), &p->data[5], 16);
  return true;
}

int AafReader::StreamEntry(Obj o, uint16_t pid) const
{
  const Prop* p = Find(o, pid);
  if (!p || p->form != 0x42 || p->data.size() < 3) return -1;
  return cfb_.Child(o->entry, DecodeUtf16(p->data.data() + 1, p->data.size() - 1));
}

bool AafReader::IsA(const Auid& cls, const Auid& base) const
{
  Auid c = cls;
  for (int depth = 0; depth < 32; depth++)
  {
    if (c == base) return true;
    auto it = parents_.find(c);
    if (it == parents_.end() || it->second == c) return false;
    c = it->second;
  }
  return false;
}

bool AafReader::IsA(Obj o, const char* className) const
{
  const AafClassModel* m = FindClassModel(className);
  return o && m && IsA(o->cls, AuidFromString(m->auid));
}

std::string AafReader::ClassName(Obj o) const
{
  if (!o) return std::string();
  // nearest known class (files may use subclasses defined only in their own meta dictionary)
  Auid c = o->cls;
  for (int depth = 0; depth < 32; depth++)
  {
    if (const AafClassModel* m = FindClassModel(c)) return m->name;
    auto it = parents_.find(c);
    if (it == parents_.end() || it->second == c) break;
    c = it->second;
  }
  return AuidToString(o->cls);
}

uint16_t AafReader::PidByName(const char* propertyName) const
{
  auto it = pidsByName_.find(propertyName);
  return it == pidsByName_.end() ? 0 : it->second;
}

std::string AafReader::DefinitionName(const Auid& id) const
{
  auto it = defNames_.find(id);
  return it == defNames_.end() ? std::string() : it->second;
}

std::string AafReader::TypeName(const Auid& id) const
{
  auto it = typeNames_.find(id);
  if (it != typeNames_.end()) return it->second;
  const AafTypeModel* t = FindTypeModel(id);
  return t ? t->name : std::string();
}

// ---------------------------------------------------------------------------------------------
// Writer

namespace {

void Put16(std::vector<uint8_t>& v, uint16_t x)
{
  v.push_back((uint8_t)x);
  v.push_back((uint8_t)(x >> 8));
}
void Put32(std::vector<uint8_t>& v, uint32_t x)
{
  for (int i = 0; i < 4; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
void Put64(std::vector<uint8_t>& v, uint64_t x)
{
  for (int i = 0; i < 8; i++) v.push_back((uint8_t)(x >> (8 * i)));
}
void PutUtf16(std::vector<uint8_t>& v, const std::u16string& s)
{
  for (char16_t c : s) Put16(v, (uint16_t)c);
  Put16(v, 0);
}

std::string Squeeze(const std::string& name, size_t size)
{
  if (name.size() <= size) return name;
  size_t half = size / 2;
  std::string r;
  for (size_t i = 0; i < size; i++)
  {
    if (i < half) r += name[i];
    else if (i == half) r += '-';
    else r += name[name.size() - (size - i)];
  }
  return r;
}

// Storage/stream names of references: "<property name>-<pid in hex>", shortened to fit.
std::string Mangle(const std::string& name, uint16_t pid, size_t size)
{
  char p[8];
  snprintf(p, sizeof(p), "%x", pid);
  return Squeeze(name, size - strlen(p) - 2) + "-" + p;
}

const AafTypeModel* Resolve(const AafTypeModel* t)
{
  while (t && t->kind == AafTypeModel::kRename) t = FindTypeModel(AuidFromString(t->element));
  return t;
}

}  // namespace

struct AafWriter::Impl
{
  struct WProp
  {
    const AafPropModel* def = nullptr;
    uint16_t form = 0x82;
    std::vector<uint8_t> data;
    int ref = -1;
    std::vector<int> refs;
    // weak reference
    Auid key{};
    // stream
    std::string path;
    uint64_t offset = 0, size = 0;
  };
  struct WObj
  {
    const AafClassModel* cls;
    std::vector<WProp> props;
  };
  std::vector<WObj> objs;
  std::vector<std::vector<uint16_t>> refTable;  // "referenced properties"
  // extension properties (dynamic local ids count down from 0xFFFF)
  std::deque<AafPropModel> extProps;
  std::deque<std::string> extStrings;
  std::map<const AafClassModel*, std::vector<const AafPropModel*>> extByClass;
  uint16_t nextPid = 0xFFFF;

  const char* Keep(const std::string& s)
  {
    extStrings.push_back(s);
    return extStrings.back().c_str();
  }

  const AafPropModel* PropDef(const AafClassModel* c, const char* name) const
  {
    while (c)
    {
      for (int i = 0; i < c->numProps; i++)
        if (strcmp(c->props[i].name, name) == 0) return &c->props[i];
      auto ext = extByClass.find(c);
      if (ext != extByClass.end())
        for (const AafPropModel* p : ext->second)
          if (strcmp(p->name, name) == 0) return p;
      c = c->parent ? FindClassModel(AuidFromString(c->parent)) : nullptr;
    }
    return nullptr;
  }

  WProp& Prop(int obj, const char* name)
  {
    WObj& o = objs[obj];
    const AafPropModel* def = PropDef(o.cls, name);
    if (!def || !def->pid)
    {
      fprintf(stderr, "AafWriter: class %s has no property %s\n", o.cls->name, name);
      abort();
    }
    for (WProp& p : o.props)
      if (p.def == def) return p;
    o.props.push_back(WProp());
    o.props.back().def = def;
    return o.props.back();
  }

  const AafTypeModel* PropType(const AafPropModel* def) const
  {
    return FindTypeModel(AuidFromString(def->type));
  }

  int RefIndex(const std::vector<uint16_t>& path)
  {
    for (size_t i = 0; i < refTable.size(); i++)
      if (refTable[i] == path) return (int)i;
    refTable.push_back(path);
    return (int)refTable.size() - 1;
  }

  // unique id property of a class (pid and key size)
  bool UniqueKey(const AafClassModel* c, uint16_t& pid, uint8_t& size) const
  {
    while (c)
    {
      for (int i = 0; i < c->numProps; i++)
        if (c->props[i].unique)
        {
          pid = c->props[i].pid;
          size = strcmp(c->props[i].type, "01030200-0000-0000-060e-2b3401040101") == 0 ? 32 : 16;
          return true;
        }
      c = c->parent ? FindClassModel(AuidFromString(c->parent)) : nullptr;
    }
    return false;
  }

  std::vector<uint8_t> WeakRefData(const AafTypeModel* t, const Auid& key)
  {
    t = Resolve(t);
    std::vector<uint16_t> path;
    for (int i = 0; i < t->count; i++) path.push_back(Model().pidByPropAuid.at(AuidFromString(t->names[i])));
    uint16_t keyPid = 0;
    uint8_t keySize = 16;
    UniqueKey(FindClassModel(AuidFromString(t->element)), keyPid, keySize);
    std::vector<uint8_t> d;
    Put16(d, (uint16_t)RefIndex(path));
    Put16(d, keyPid);
    d.push_back(keySize);
    d.insert(d.end(), key.begin(), key.end());
    return d;
  }

  bool KeyOf(int obj, std::vector<uint8_t>& key, uint16_t& pid, uint8_t& size) const
  {
    if (!UniqueKey(objs[obj].cls, pid, size)) return false;
    for (const WProp& p : objs[obj].props)
      if (p.def->pid == pid && p.data.size() == size)
      {
        key = p.data;
        return true;
      }
    return false;
  }

  // Serializes object 'obj' into storage 'entry'.
  bool WriteObject(CfbWriter& cfb, int obj, int entry, std::string& error)
  {
    WObj& o = objs[obj];
    std::vector<WProp*> props;
    for (WProp& p : o.props) props.push_back(&p);
    std::sort(props.begin(), props.end(), [](const WProp* a, const WProp* b) { return a->def->pid < b->def->pid; });
    std::vector<uint8_t> head, body;
    head.push_back(0x4c);
    head.push_back(32);
    Put16(head, (uint16_t)props.size());
    for (WProp* p : props)
    {
      std::vector<uint8_t> value;
      std::string pname = p->def->name;
      uint16_t pid = p->def->pid;
      switch (p->form)
      {
        case 0x22:
        {
          std::string n = Mangle(pname, pid, 32);
          PutUtf16(value, Ascii16(n));
          int child = cfb.AddStorage(entry, Ascii16(n), ClassClsid(p->ref));
          if (!WriteObject(cfb, p->ref, child, error)) return false;
          break;
        }
        case 0x32:
        case 0x3A:
        {
          std::string n = Mangle(pname, pid, 32 - 10);
          PutUtf16(value, Ascii16(n));
          std::vector<uint8_t> index;
          Put32(index, (uint32_t)p->refs.size());
          Put32(index, (uint32_t)p->refs.size());
          Put32(index, 0xFFFFFFFF);
          if (p->form == 0x3A)
          {
            uint16_t keyPid = 0;
            uint8_t keySize = 16;
            if (!p->refs.empty()) UniqueKey(objs[p->refs[0]].cls, keyPid, keySize);
            else if (const AafTypeModel* set = PropType(p->def))  // empty set: the element class's key
              if (const AafTypeModel* ref = FindTypeModel(AuidFromString(set->element)))
                UniqueKey(FindClassModel(AuidFromString(ref->element)), keyPid, keySize);
            Put16(index, keyPid);
            index.push_back(keySize);
          }
          for (size_t k = 0; k < p->refs.size(); k++)
          {
            Put32(index, (uint32_t)k);
            if (p->form == 0x3A)
            {
              std::vector<uint8_t> key;
              uint16_t kp;
              uint8_t ks;
              if (!KeyOf(p->refs[k], key, kp, ks))
              {
                error = std::string("set element of ") + pname + " has no unique id";
                return false;
              }
              Put32(index, 1);
              index.insert(index.end(), key.begin(), key.end());
            }
            int child = cfb.AddStorage(entry, Ascii16(n) + HexKey((uint32_t)k), ClassClsid(p->refs[k]));
            if (!WriteObject(cfb, p->refs[k], child, error)) return false;
          }
          cfb.AddStream(entry, Ascii16(n) + u" index", std::move(index));
          break;
        }
        case 0x12:
        {
          // weak reference vector (record member types): keys in the index stream
          std::string n = Mangle(pname, pid, 32);
          PutUtf16(value, Ascii16(n));
          cfb.AddStream(entry, Ascii16(n) + u" index", p->data);
          break;
        }
        case 0x42:
        {
          std::string n = Mangle(pname, pid, 32);
          value.push_back(0x55);
          PutUtf16(value, Ascii16(n));
          cfb.AddFileStream(entry, Ascii16(n), p->path, p->offset, p->size);
          break;
        }
        default:
          value = p->data;
          break;
      }
      Put16(head, pid);
      Put16(head, p->form);
      Put16(head, (uint16_t)value.size());
      body.insert(body.end(), value.begin(), value.end());
    }
    head.insert(head.end(), body.begin(), body.end());
    cfb.AddStream(entry, u"properties", std::move(head));
    return true;
  }

  Clsid ClassClsid(int obj) const
  {
    Auid a = AuidFromString(objs[obj].cls->auid);
    Clsid c;
    memcpy(c.data(), a.data(), 16);
    return c;
  }

  // ---- meta dictionary ----
  std::set<const AafClassModel*> usedClasses;
  std::set<const AafTypeModel*> usedTypes;

  void UseType(const AafTypeModel* t)
  {
    if (!t || usedTypes.count(t)) return;
    usedTypes.insert(t);
    auto type = [&](const char* a) {
      if (a) UseType(FindTypeModel(AuidFromString(a)));
    };
    switch (t->kind)
    {
      case AafTypeModel::kEnum:
      case AafTypeModel::kFixedArray:
      case AafTypeModel::kVarArray:
      case AafTypeModel::kRename:
      case AafTypeModel::kString:
      case AafTypeModel::kSet:
        type(t->element);
        break;
      case AafTypeModel::kRecord:
        for (int i = 0; i < t->count; i++) type(((const char* const*)t->values)[i]);
        break;
      case AafTypeModel::kStrongRef:
      case AafTypeModel::kWeakRef:
        UseClass(FindClassModel(AuidFromString(t->element)));
        break;
      default:
        break;
    }
  }

  void UseClass(const AafClassModel* c)
  {
    if (!c || usedClasses.count(c) || strcmp(c->name, "Root") == 0) return;
    usedClasses.insert(c);
    if (c->parent) UseClass(FindClassModel(AuidFromString(c->parent)));
    for (int i = 0; i < c->numProps; i++)
      if (c->props[i].pid) UseType(FindTypeModel(AuidFromString(c->props[i].type)));
    auto ext = extByClass.find(c);
    if (ext != extByClass.end())
      for (const AafPropModel* p : ext->second) UseType(FindTypeModel(AuidFromString(p->type)));
  }

  int NewObj(const char* cls)
  {
    const AafClassModel* c = FindClassModel(cls);
    objs.push_back({c, {}});
    return (int)objs.size() - 1;
  }
  void SetData(int obj, const char* prop, std::vector<uint8_t> v)
  {
    WProp& p = Prop(obj, prop);
    p.form = 0x82;
    p.data = std::move(v);
  }
  void SetAuidData(int obj, const char* prop, const Auid& a) { SetData(obj, prop, std::vector<uint8_t>(a.begin(), a.end())); }
  void SetWeak(int obj, const char* prop, const Auid& key)
  {
    WProp& p = Prop(obj, prop);
    p.form = 0x02;
    p.data = WeakRefData(PropType(p.def), key);
  }
  static std::vector<uint8_t> Utf16Array(const std::vector<std::string>& names)
  {
    std::vector<uint8_t> v;
    for (const std::string& n : names) PutUtf16(v, Utf8ToUtf16(n));
    return v;
  }

  int BuildMetaDictionary()
  {
    for (const char* n : {"MetaDictionary", "ClassDefinition", "PropertyDefinition"}) UseClass(FindClassModel(n));
    for (size_t i = 0; i < objs.size(); i++) UseClass(objs[i].cls);
    // every type kind present needs its TypeDefinition class
    static const char* kKindClass[] = {"TypeDefinitionInteger",   "TypeDefinitionEnumeration",
                                       "TypeDefinitionRecord",    "TypeDefinitionFixedArray",
                                       "TypeDefinitionVariableArray", "TypeDefinitionRename",
                                       "TypeDefinitionString",    "TypeDefinitionStream",
                                       "TypeDefinitionOpaque",    "TypeDefinitionExtendibleEnumeration",
                                       "TypeDefinitionCharacter", "TypeDefinitionIndirect",
                                       "TypeDefinitionSet",       "TypeDefinitionStrongObjectReference",
                                       "TypeDefinitionWeakObjectReference"};
    for (size_t pass = 0; pass < 4; pass++)
    {
      std::set<const AafTypeModel*> types = usedTypes;
      for (const AafTypeModel* t : types) UseClass(FindClassModel(kKindClass[t->kind]));
    }

    int md = NewObj("MetaDictionary");
    std::vector<const AafClassModel*> classes(usedClasses.begin(), usedClasses.end());
    std::sort(classes.begin(), classes.end(), [](auto a, auto b) { return strcmp(a->name, b->name) < 0; });
    for (const AafClassModel* c : classes)
    {
      int cd = NewObj("ClassDefinition");
      SetAuidData(cd, "Identification", AuidFromString(c->auid));
      {
        std::vector<uint8_t> v;
        PutUtf16(v, Ascii16(c->name));
        SetData(cd, "Name", v);
      }
      SetWeak(cd, "ParentClass", AuidFromString(c->parent ? c->parent : c->auid));
      SetData(cd, "IsConcrete", {(uint8_t)(c->concrete ? 1 : 0)});
      std::vector<const AafPropModel*> props;
      for (int i = 0; i < c->numProps; i++) props.push_back(&c->props[i]);
      auto ext = extByClass.find(c);
      if (ext != extByClass.end()) props.insert(props.end(), ext->second.begin(), ext->second.end());
      for (const AafPropModel* pp : props)
      {
        const AafPropModel& p = *pp;
        if (!p.pid) continue;
        int pd = NewObj("PropertyDefinition");
        SetAuidData(pd, "Identification", AuidFromString(p.auid));
        std::vector<uint8_t> v;
        PutUtf16(v, Ascii16(p.name));
        SetData(pd, "Name", v);
        SetAuidData(pd, "Type", AuidFromString(p.type));
        SetData(pd, "IsOptional", {(uint8_t)(p.optional ? 1 : 0)});
        std::vector<uint8_t> pidv;
        Put16(pidv, p.pid);
        SetData(pd, "LocalIdentification", pidv);
        if (p.unique) SetData(pd, "IsUniqueIdentifier", {1});
        WProp& set = Prop(cd, "Properties");
        set.form = 0x3A;
        set.refs.push_back(pd);
      }
      WProp& set = Prop(md, "ClassDefinitions");
      set.form = 0x3A;
      set.refs.push_back(cd);
    }
    std::vector<const AafTypeModel*> types(usedTypes.begin(), usedTypes.end());
    std::sort(types.begin(), types.end(), [](auto a, auto b) { return strcmp(a->name, b->name) < 0; });
    for (const AafTypeModel* t : types)
    {
      int td = NewObj(kKindClass[t->kind]);
      SetAuidData(td, "Identification", AuidFromString(t->auid));
      std::vector<uint8_t> v;
      PutUtf16(v, Ascii16(t->name));
      SetData(td, "Name", v);
      auto elementRef = [&](const char* prop) { SetWeak(td, prop, AuidFromString(t->element)); };
      switch (t->kind)
      {
        case AafTypeModel::kInt:
          SetData(td, "Size", {(uint8_t)t->size});
          SetData(td, "IsSigned", {(uint8_t)(t->isSigned ? 1 : 0)});
          break;
        case AafTypeModel::kEnum:
        {
          elementRef("ElementType");
          std::vector<std::string> names;
          std::vector<uint8_t> values;
          for (int i = 0; i < t->count; i++)
          {
            names.push_back(t->names[i]);
            Put64(values, (uint64_t)((const int64_t*)t->values)[i]);
          }
          SetData(td, "ElementNames", Utf16Array(names));
          SetData(td, "ElementValues", values);
          break;
        }
        case AafTypeModel::kRecord:
        {
          std::vector<std::string> names;
          for (int i = 0; i < t->count; i++) names.push_back(t->names[i]);
          SetData(td, "MemberNames", Utf16Array(names));
          // weak reference vector to the member types
          std::vector<uint8_t> index;
          Put32(index, (uint32_t)t->count);
          Put16(index, (uint16_t)RefIndex({0x0001, 0x0004}));
          Put16(index, 0x0005);
          index.push_back(16);
          for (int i = 0; i < t->count; i++)
          {
            Auid a = AuidFromString(((const char* const*)t->values)[i]);
            index.insert(index.end(), a.begin(), a.end());
          }
          WProp& p = Prop(td, "MemberTypes");
          p.form = 0x12;
          p.data = index;
          break;
        }
        case AafTypeModel::kFixedArray:
        {
          elementRef("ElementType");
          std::vector<uint8_t> c;
          Put32(c, (uint32_t)t->size);
          SetData(td, "ElementCount", c);
          break;
        }
        case AafTypeModel::kVarArray:
        case AafTypeModel::kSet:
        case AafTypeModel::kString:
          elementRef("ElementType");
          break;
        case AafTypeModel::kRename:
          elementRef("RenamedType");
          break;
        case AafTypeModel::kExtEnum:
        {
          std::vector<std::string> names;
          std::vector<uint8_t> values;
          for (int i = 0; i < t->count; i++)
          {
            names.push_back(t->names[i]);
            Auid a = AuidFromString(((const char* const*)t->values)[i]);
            values.insert(values.end(), a.begin(), a.end());
          }
          SetData(td, "ElementNames", Utf16Array(names));
          SetData(td, "ElementValues", values);
          break;
        }
        case AafTypeModel::kStrongRef:
          elementRef("ReferencedType");
          break;
        case AafTypeModel::kWeakRef:
        {
          elementRef("ReferencedType");
          std::vector<uint8_t> path;
          for (int i = 0; i < t->count; i++)
          {
            Auid a = AuidFromString(t->names[i]);
            path.insert(path.end(), a.begin(), a.end());
          }
          SetData(td, "TargetSet", path);
          break;
        }
        default:
          break;
      }
      WProp& set = Prop(md, "TypeDefinitions");
      set.form = 0x3A;
      set.refs.push_back(td);
    }
    return md;
  }
};

AafWriter::AafWriter() : impl_(new Impl) {}
AafWriter::~AafWriter() = default;

void AafWriter::DefineProperty(const char* className, const char* propertyName, const char* auid, const char* typeAuid)
{
  Impl& im = *impl_;
  const AafClassModel* c = FindClassModel(className);
  if (!c || im.PropDef(c, propertyName)) return;
  im.extProps.push_back({im.Keep(propertyName), im.Keep(auid), im.nextPid--, im.Keep(typeAuid), true, false});
  im.extByClass[c].push_back(&im.extProps.back());
}

int AafWriter::Create(const char* className)
{
  const AafClassModel* c = FindClassModel(className);
  if (!c)
  {
    fprintf(stderr, "AafWriter: unknown class %s\n", className);
    abort();
  }
  return impl_->NewObj(className);
}

void AafWriter::SetInt(int obj, const char* prop, int64_t v)
{
  Impl::WProp& p = impl_->Prop(obj, prop);
  const AafTypeModel* t = Resolve(impl_->PropType(p.def));
  if (t && t->kind == AafTypeModel::kEnum) t = Resolve(FindTypeModel(AuidFromString(t->element)));
  int size = t && t->kind == AafTypeModel::kInt ? t->size : 8;
  p.form = 0x82;
  p.data.clear();
  for (int i = 0; i < size; i++) p.data.push_back((uint8_t)((uint64_t)v >> (8 * i)));
}

void AafWriter::SetBool(int obj, const char* prop, bool v) { impl_->SetData(obj, prop, {(uint8_t)(v ? 1 : 0)}); }

void AafWriter::SetString(int obj, const char* prop, const std::string& utf8)
{
  std::vector<uint8_t> v;
  PutUtf16(v, Utf8ToUtf16(utf8));
  impl_->SetData(obj, prop, v);
}

void AafWriter::SetRational(int obj, const char* prop, AafRational r)
{
  std::vector<uint8_t> v;
  Put32(v, (uint32_t)r.num);
  Put32(v, (uint32_t)r.den);
  impl_->SetData(obj, prop, v);
}

void AafWriter::SetAuid(int obj, const char* prop, const Auid& a) { impl_->SetAuidData(obj, prop, a); }

void AafWriter::SetMobId(int obj, const char* prop, const MobId& m)
{
  impl_->SetData(obj, prop, std::vector<uint8_t>(m.begin(), m.end()));
}

void AafWriter::SetBytes(int obj, const char* prop, std::vector<uint8_t> v) { impl_->SetData(obj, prop, std::move(v)); }

void AafWriter::SetTimestamp(int obj, const char* prop, int year, int month, int day, int hour, int minute, int second)
{
  std::vector<uint8_t> v;
  Put16(v, (uint16_t)year);
  v.push_back((uint8_t)month);
  v.push_back((uint8_t)day);
  v.push_back((uint8_t)hour);
  v.push_back((uint8_t)minute);
  v.push_back((uint8_t)second);
  v.push_back(0);
  impl_->SetData(obj, prop, v);
}

void AafWriter::SetProductVersion(int obj, const char* prop, int major, int minor, int tertiary, int patch, int type)
{
  std::vector<uint8_t> v;
  Put16(v, (uint16_t)major);
  Put16(v, (uint16_t)minor);
  Put16(v, (uint16_t)tertiary);
  Put16(v, (uint16_t)patch);
  v.push_back((uint8_t)type);
  impl_->SetData(obj, prop, v);
}

void AafWriter::SetVersion(int obj, const char* prop, int major, int minor)
{
  impl_->SetData(obj, prop, {(uint8_t)major, (uint8_t)minor});
}

static std::vector<uint8_t> IndirectHeader(const char* type)
{
  std::vector<uint8_t> v{0x4c};
  Auid a = AuidFromString(type);
  v.insert(v.end(), a.begin(), a.end());
  return v;
}

void AafWriter::SetIndirectRational(int obj, const char* prop, AafRational r)
{
  std::vector<uint8_t> v = IndirectHeader("03010100-0000-0000-060e-2b3401040101");
  Put32(v, (uint32_t)r.num);
  Put32(v, (uint32_t)r.den);
  impl_->SetData(obj, prop, v);
}

void AafWriter::SetIndirectInt32(int obj, const char* prop, int32_t x)
{
  std::vector<uint8_t> v = IndirectHeader("01010700-0000-0000-060e-2b3401040101");
  Put32(v, (uint32_t)x);
  impl_->SetData(obj, prop, v);
}

void AafWriter::SetIndirectString(int obj, const char* prop, const std::string& utf8)
{
  std::vector<uint8_t> v = IndirectHeader("01100200-0000-0000-060e-2b3401040101");
  PutUtf16(v, Utf8ToUtf16(utf8));
  impl_->SetData(obj, prop, v);
}

void AafWriter::SetRef(int obj, const char* prop, int child)
{
  Impl::WProp& p = impl_->Prop(obj, prop);
  p.form = 0x22;
  p.ref = child;
}

void AafWriter::Append(int obj, const char* prop, int child)
{
  Impl::WProp& p = impl_->Prop(obj, prop);
  p.form = 0x32;
  p.refs.push_back(child);
}

void AafWriter::Insert(int obj, const char* prop, int child)
{
  Impl::WProp& p = impl_->Prop(obj, prop);
  p.form = 0x3A;
  if (child >= 0) p.refs.push_back(child);
}

void AafWriter::SetWeakRef(int obj, const char* prop, const Auid& key) { impl_->SetWeak(obj, prop, key); }

void AafWriter::SetStream(int obj, const char* prop, const std::string& path, uint64_t offset, uint64_t size)
{
  Impl::WProp& p = impl_->Prop(obj, prop);
  p.form = 0x42;
  p.path = path;
  p.offset = offset;
  p.size = size;
}

bool AafWriter::Write(const std::string& path, int header, std::string& error)
{
  Impl& im = *impl_;
  // object properties may register referenced-property paths; the meta dictionary adds more
  int md = im.BuildMetaDictionary();
  CfbWriter cfb;
  cfb.SetHeaderClsid([] {
    Auid a = AuidFromString("0d010201-0200-0000-060e-2b3403020101");  // AAF 4K signature
    Clsid c;
    memcpy(c.data(), a.data(), 16);
    return c;
  }());
  {
    Auid a = AuidFromString("b3b398a5-1c90-11d4-8053-080036210804");  // Root class
    Clsid c;
    memcpy(c.data(), a.data(), 16);
    cfb.SetClsid(cfb.Root(), c);
  }
  // root object: MetaDictionary (0x0001) and Header (0x0002)
  std::vector<uint8_t> head{0x4c, 32};
  Put16(head, 2);
  std::vector<uint8_t> v1, v2;
  PutUtf16(v1, u"MetaDictionary-1");
  PutUtf16(v2, u"Header-2");
  Put16(head, 0x0001);
  Put16(head, 0x22);
  Put16(head, (uint16_t)v1.size());
  Put16(head, 0x0002);
  Put16(head, 0x22);
  Put16(head, (uint16_t)v2.size());
  head.insert(head.end(), v1.begin(), v1.end());
  head.insert(head.end(), v2.begin(), v2.end());
  int mdEntry = cfb.AddStorage(cfb.Root(), u"MetaDictionary-1", im.ClassClsid(md));
  if (!im.WriteObject(cfb, md, mdEntry, error)) return false;
  int hdrEntry = cfb.AddStorage(cfb.Root(), u"Header-2", im.ClassClsid(header));
  if (!im.WriteObject(cfb, header, hdrEntry, error)) return false;
  cfb.AddStream(cfb.Root(), u"properties", head);

  std::vector<uint8_t> refs{0x4c};
  Put16(refs, (uint16_t)im.refTable.size());
  uint32_t pidCount = 0;
  for (const auto& p : im.refTable) pidCount += (uint32_t)p.size() + 1;
  Put32(refs, pidCount);
  for (const auto& p : im.refTable)
  {
    for (uint16_t pid : p) Put16(refs, pid);
    Put16(refs, 0);
  }
  cfb.AddStream(cfb.Root(), u"referenced properties", refs);
  return cfb.Write(path, error);
}

}  // namespace dawp
