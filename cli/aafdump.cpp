// Prints the object tree of an AAF file (for diagnosing files from other applications).
//   aafdump <file.aaf>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "aaf_file.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace dawp;

static void Dump(const AafReader& r, AafReader::Obj o, int depth, std::set<const void*>& seen)
{
  std::string ind(depth * 2, ' ');
  printf("%s<%s>\n", ind.c_str(), r.ClassName(o).c_str());
  if (!seen.insert(o).second) return;
  for (const auto& kv : o->props)
  {
    uint16_t pid = kv.first;
    const AafReader::Prop& p = kv.second;
    if (pid == 0x3b04 || (depth == 0 && pid == 0x0001)) continue;  // dictionary / meta dictionary
    printf("%s  %04x ", ind.c_str(), pid);
    if (p.form == 0x22)
    {
      printf("ref\n");
      if (AafReader::Obj c = r.Ref(o, pid)) Dump(r, c, depth + 2, seen);
    }
    else if (p.form == 0x32 || p.form == 0x3A)
    {
      std::vector<AafReader::Obj> list = r.Refs(o, pid);
      printf("%s[%d]\n", p.form == 0x32 ? "vector" : "set", (int)list.size());
      for (AafReader::Obj c : list) Dump(r, c, depth + 2, seen);
    }
    else if (p.form == 0x02)
    {
      Auid key;
      r.WeakRefKey(o, pid, key);
      std::string name = r.DefinitionName(key);
      printf("weak %s %s\n", AuidToString(key).c_str(), name.c_str());
    }
    else if (p.form == 0x42)
      printf("stream %llu bytes\n", (unsigned long long)(r.StreamEntry(o, pid) >= 0 ? r.Cfb().Get(r.StreamEntry(o, pid)).size : 0));
    else
    {
      double num;
      std::string s;
      if (r.IndirectString(o, pid, s)) printf("\"%s\"\n", s.c_str());
      else if (r.IndirectNumber(o, pid, num)) printf("%g\n", num);
      else
      {
        bool text = p.data.size() > 4 && p.data.size() % 2 == 0 && p.data[p.data.size() - 1] == 0 &&
                    p.data[p.data.size() - 2] == 0;
        for (size_t i = 0; text && i + 2 < p.data.size(); i += 2)
          if (p.data[i] == 0 && p.data[i + 1] == 0) text = false;
        if (text) printf("\"%s\"", r.String(o, pid).c_str());
        else
          for (size_t i = 0; i < p.data.size() && i < 40; i++) printf("%02x", p.data[i]);
        if ((p.data.size() == 8 || p.data.size() == 4) && !text) printf(" (%lld)", (long long)r.Int(o, pid));
        printf("\n");
      }
    }
  }
}

static int Run(const std::vector<std::string>& args)
{
  if (args.size() < 2)
  {
    fprintf(stderr, "usage: aafdump <file.aaf>\n");
    return 2;
  }
  AafReader r;
  std::string error;
  if (!r.Open(args[1], error))
  {
    fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  std::set<const void*> seen;
  Dump(r, r.Header(), 0, seen);
  return 0;
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 0; i < argc; i++)
  {
    int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, &s[0], n, nullptr, nullptr);
    args.push_back(s);
  }
  return Run(args);
}
#else
int main(int argc, char** argv) { return Run(std::vector<std::string>(argv, argv + argc)); }
#endif
