// Output target for generated RPP lines: REAPER's ProjectStateContext or a text file.
#pragma once

#include <cstdio>
#include <string>

namespace dawp {

class RppSink
{
public:
  virtual ~RppSink() {}
  // One RPP line without indentation. Lines starting with '<' open a block, ">" closes it.
  virtual void Line(const std::string& line) = 0;
};

// Writes an indented .rpp file.
class FileRppSink : public RppSink
{
public:
  explicit FileRppSink(FILE* f) : f_(f) {}
  void Line(const std::string& line) override;

private:
  FILE* f_;
  int depth_ = 0;
};

// Quotes a string the way REAPER does: "..." unless it contains '"', then '...' or `...`.
std::string RppQuote(const std::string& s);

}  // namespace dawp
