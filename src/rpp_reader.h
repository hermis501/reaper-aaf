// Parses REAPER project text (.rpp, or chunks from the API) into a tree of lines and blocks.
#pragma once

#include <string>
#include <vector>

namespace dawp {

struct RppNode
{
  std::vector<std::string> tokens;  // tokens[0] is the line's name, e.g. "VOLPAN" or "TRACK"
  std::string line;                 // the trimmed source line (without the leading '<' of a block)
  std::vector<RppNode> children;
  bool block = false;

  const std::string& Name() const { return Token(0); }
  const std::string& Token(size_t i) const;
  double Num(size_t i, double def = 0) const;
  int Int(size_t i, int def = 0) const;
  const RppNode* Child(const char* name) const;
  std::vector<const RppNode*> Children(const char* name) const;
};

// Splits a line into tokens the way REAPER writes them: whitespace separated, optionally quoted
// with ", ' or `.
std::vector<std::string> RppTokenize(const std::string& line);

// The root node is the first top-level block (normally <REAPER_PROJECT).
bool ParseRpp(const std::string& text, RppNode& root, std::string& error);

}  // namespace dawp
