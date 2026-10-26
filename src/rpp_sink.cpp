#include "rpp_sink.h"

namespace dawp {

void FileRppSink::Line(const std::string& line)
{
  if (line == ">" && depth_ > 0) depth_--;
  for (int i = 0; i < depth_; i++) fputs("  ", f_);
  fputs(line.c_str(), f_);
  fputs("\n", f_);
  if (!line.empty() && line[0] == '<') depth_++;
}

std::string RppQuote(const std::string& in)
{
  std::string s;
  s.reserve(in.size());
  for (char c : in) s += (c == '\r' || c == '\n' || c == '\t') ? ' ' : c;

  const char* quotes = "\"'`";
  for (const char* q = quotes; *q; q++)
    if (s.find(*q) == std::string::npos) return *q + s + *q;

  // All three quote characters present: REAPER cannot represent that, so drop backticks.
  std::string t;
  for (char c : s)
    if (c != '`') t += c;
  return "`" + t + "`";
}

}  // namespace dawp
