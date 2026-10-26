#include "rpp_reader.h"

#include <cstring>

#include "util.h"

namespace dawp {

const std::string& RppNode::Token(size_t i) const
{
  static const std::string empty;
  return i < tokens.size() ? tokens[i] : empty;
}

double RppNode::Num(size_t i, double def) const
{
  double v;
  return i < tokens.size() && ParseDouble(tokens[i].c_str(), v) ? v : def;
}

int RppNode::Int(size_t i, int def) const
{
  if (i >= tokens.size()) return def;
  const char* s = tokens[i].c_str();
  char* end;
  long v = strtol(s, &end, 10);
  return end == s ? def : (int)v;
}

const RppNode* RppNode::Child(const char* name) const
{
  for (const RppNode& c : children)
    if (c.Name() == name) return &c;
  return nullptr;
}

std::vector<const RppNode*> RppNode::Children(const char* name) const
{
  std::vector<const RppNode*> out;
  for (const RppNode& c : children)
    if (c.Name() == name) out.push_back(&c);
  return out;
}

std::vector<std::string> RppTokenize(const std::string& line)
{
  std::vector<std::string> out;
  size_t i = 0, n = line.size();
  while (i < n)
  {
    char c = line[i];
    if (c == ' ' || c == '\t')
    {
      i++;
      continue;
    }
    if (c == '"' || c == '\'' || c == '`')
    {
      size_t j = line.find(c, i + 1);
      if (j == std::string::npos) j = n;
      out.push_back(line.substr(i + 1, j - i - 1));
      i = j + 1;
      continue;
    }
    size_t j = i;
    while (j < n && line[j] != ' ' && line[j] != '\t') j++;
    out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

bool ParseRpp(const std::string& text, RppNode& root, std::string& error)
{
  std::vector<RppNode*> stack;
  RppNode top;
  top.block = true;
  stack.push_back(&top);

  size_t pos = 0;
  if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) pos = 3;  // UTF-8 BOM
  while (pos < text.size())
  {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    size_t a = pos, b = end;
    pos = end + 1;
    while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
    while (b > a && (text[b - 1] == '\r' || text[b - 1] == ' ' || text[b - 1] == '\t')) b--;
    if (a == b) continue;
    std::string line = text.substr(a, b - a);

    if (line == ">")
    {
      if (stack.size() <= 1)
      {
        error = "Unbalanced '>' in the project";
        return false;
      }
      stack.pop_back();
      continue;
    }
    RppNode node;
    if (line[0] == '<')
    {
      node.block = true;
      node.line = line.substr(1);
    }
    else
      node.line = line;
    // Notes and similar free text start with '|'; keep them as a single token.
    if (node.line[0] == '|') node.tokens.push_back(node.line);
    else node.tokens = RppTokenize(node.line);
    stack.back()->children.push_back(std::move(node));
    if (stack.back()->children.back().block) stack.push_back(&stack.back()->children.back());
  }
  for (RppNode& c : top.children)
    if (c.block)
    {
      root = std::move(c);
      return true;
    }
  error = "No REAPER project data found";
  return false;
}

}  // namespace dawp
