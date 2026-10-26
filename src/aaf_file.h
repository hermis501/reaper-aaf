// AAF object layer on top of the compound file: reads objects/properties of an existing AAF file
// and builds new ones (including the meta dictionary describing the classes and types used).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "cfb.h"

namespace dawp {

// 16 bytes in AAF's stored ("little-endian GUID") byte order.
using Auid = std::array<uint8_t, 16>;
// SMPTE UMID as stored in MobID properties.
using MobId = std::array<uint8_t, 32>;

Auid AuidFromString(const char* text);  // "0d010101-0101-0100-060e-2b3402060101"
std::string AuidToString(const Auid& a);
bool IsNull(const Auid& a);

struct AafRational
{
  int32_t num = 0, den = 1;
  double Value() const { return den ? (double)num / den : 0.0; }
};

// The baseline object model (generated from public specification tables).
struct AafPropModel
{
  const char* name;
  const char* auid;
  uint16_t pid;  // 0 = assigned per file
  const char* type;
  bool optional, unique;
};

struct AafClassModel
{
  const char* name;
  const char* auid;
  const char* parent;
  bool concrete;
  const AafPropModel* props;
  int numProps;
};

struct AafTypeModel
{
  enum Kind
  {
    kInt,
    kEnum,
    kRecord,
    kFixedArray,
    kVarArray,
    kRename,
    kString,
    kStream,
    kOpaque,
    kExtEnum,
    kCharacter,
    kIndirect,
    kSet,
    kStrongRef,
    kWeakRef
  };
  const char* name;
  const char* auid;
  Kind kind;
  const char* element;  // element / renamed / referenced class
  int size;             // int size, fixed array count
  bool isSigned;
  const char* const* names;  // enum/record/extenum names, weak reference target path
  const void* values;        // enum: int64_t[], record: member type auids, extenum: value auids
  int count;
};

const AafClassModel* FindClassModel(const char* name);
const AafClassModel* FindClassModel(const Auid& auid);
const AafTypeModel* FindTypeModel(const Auid& auid);

// ---------------------------------------------------------------------------------------------
// Reading

class AafReader
{
public:
  struct Prop
  {
    uint16_t form = 0;
    std::vector<uint8_t> data;
  };
  struct Object
  {
    int entry = -1;
    Auid cls{};
    std::map<uint16_t, Prop> props;
  };
  using Obj = const Object*;

  bool Open(const std::string& path, std::string& error);

  Obj Header() const { return header_; }
  Obj MetaDictionary() const { return metadict_; }

  // property access
  const Prop* Find(Obj o, uint16_t pid) const;
  bool Has(Obj o, uint16_t pid) const { return Find(o, pid) != nullptr; }
  int64_t Int(Obj o, uint16_t pid, int64_t def = 0) const;  // any stored integer size, signed if 8 bytes
  uint64_t UInt(Obj o, uint16_t pid, uint64_t def = 0) const;
  bool Bool(Obj o, uint16_t pid, bool def = false) const;
  std::string String(Obj o, uint16_t pid) const;
  AafRational Rational(Obj o, uint16_t pid) const;
  Auid AuidValue(Obj o, uint16_t pid) const;
  MobId MobIdValue(Obj o, uint16_t pid) const;
  // Indirect values (TaggedValue::Value, ConstantValue::Value, ControlPoint::Value).
  bool Indirect(Obj o, uint16_t pid, Auid& type, std::vector<uint8_t>& value) const;
  bool IndirectNumber(Obj o, uint16_t pid, double& out) const;  // Rational / integer / float types
  bool IndirectString(Obj o, uint16_t pid, std::string& out) const;

  Obj Ref(Obj o, uint16_t pid) const;                      // strong reference
  std::vector<Obj> Refs(Obj o, uint16_t pid) const;        // strong reference vector or set
  bool WeakRefKey(Obj o, uint16_t pid, Auid& key) const;   // key of a weak reference to a definition
  int StreamEntry(Obj o, uint16_t pid) const;              // data stream property -> CFB entry, -1
  const CfbReader& Cfb() const { return cfb_; }

  // class identification
  bool IsA(Obj o, const char* className) const;
  bool IsA(const Auid& cls, const Auid& base) const;
  std::string ClassName(Obj o) const;
  // Per-file property ids of extension properties, by name (e.g. "ComponentAttributeList").
  uint16_t PidByName(const char* propertyName) const;

  // Definitions in the Dictionary (DataDefinition, OperationDefinition, ...): name by AUID.
  std::string DefinitionName(const Auid& id) const;
  // Indirect type names (for TaggedValues of unknown types)
  std::string TypeName(const Auid& id) const;

private:
  Obj Load(int entry) const;
  std::u16string RefName(const Prop& p) const;

  CfbReader cfb_;
  mutable std::map<int, std::unique_ptr<Object>> objects_;
  Obj header_ = nullptr;
  Obj metadict_ = nullptr;
  std::map<Auid, Auid> parents_;  // class -> parent class (file metadict + model)
  std::map<std::string, uint16_t> pidsByName_;
  std::map<Auid, std::string> defNames_;
  std::map<Auid, std::string> typeNames_;
};

// ---------------------------------------------------------------------------------------------
// Writing

class AafWriter
{
public:
  AafWriter();
  ~AafWriter();

  int Create(const char* className);  // returns the object handle; aborts on unknown names
  // An extension property outside the baseline model (e.g. Avid's "TimelineMobAttributeList");
  // it gets a dynamic local id and is described in the meta dictionary.
  void DefineProperty(const char* className, const char* propertyName, const char* auid, const char* typeAuid);

  void SetInt(int obj, const char* prop, int64_t v);  // size taken from the property's type
  void SetBool(int obj, const char* prop, bool v);
  void SetString(int obj, const char* prop, const std::string& utf8);
  void SetRational(int obj, const char* prop, AafRational v);
  void SetAuid(int obj, const char* prop, const Auid& v);
  void SetMobId(int obj, const char* prop, const MobId& v);
  void SetBytes(int obj, const char* prop, std::vector<uint8_t> v);  // already encoded value
  void SetTimestamp(int obj, const char* prop, int year, int month, int day, int hour, int minute, int second);
  void SetProductVersion(int obj, const char* prop, int major, int minor, int tertiary, int patch, int type);
  void SetVersion(int obj, const char* prop, int major, int minor);
  // Indirect values: a rational, an Int32 or a string.
  void SetIndirectRational(int obj, const char* prop, AafRational v);
  void SetIndirectInt32(int obj, const char* prop, int32_t v);
  void SetIndirectString(int obj, const char* prop, const std::string& utf8);

  void SetRef(int obj, const char* prop, int child);
  void Append(int obj, const char* prop, int child);  // strong reference vector
  // Strong reference set (keyed by the child's unique id); child -1 just creates the (empty) set.
  void Insert(int obj, const char* prop, int child);
  // Weak reference to a definition object or meta definition with this identification.
  void SetWeakRef(int obj, const char* prop, const Auid& key);
  // Data stream copied from a file at write time.
  void SetStream(int obj, const char* prop, const std::string& path, uint64_t offset, uint64_t size);

  // 'root' must be the Header object.
  bool Write(const std::string& path, int header, std::string& error);

  struct Impl;

private:
  std::unique_ptr<Impl> impl_;
};

}  // namespace dawp
