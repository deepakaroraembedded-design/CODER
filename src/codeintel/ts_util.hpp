// cf-codeintel internal: text helpers shared by the grammar extractors.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <string>
#include <string_view>

#include <tree_sitter/api.h>

namespace cf::codeintel::detail {

inline std::string node_text(std::string_view source, TSNode node) {
  const std::uint32_t begin = ts_node_start_byte(node);
  const std::uint32_t end = ts_node_end_byte(node);
  return std::string(source.substr(begin, end - begin));
}

inline std::uint32_t node_line(TSNode node) {
  return ts_node_start_point(node).row + 1;
}

inline std::string rtrim(std::string text) {
  while (!text.empty() &&
         (text.back() == ' ' || text.back() == '\t' || text.back() == '\n' ||
          text.back() == '\r')) {
    text.pop_back();
  }
  return text;
}

// The named child with the given field name, or a null node.
inline TSNode field(TSNode node, const char* name) {
  return ts_node_child_by_field_name(node, name,
                                     static_cast<std::uint32_t>(std::strlen(name)));
}

// Strips the comment markers (/** ... */, /* ... */, // ...) from a doc
// comment, keeping the text.
inline std::string clean_comment(std::string text) {
  if (text.rfind("/**", 0) == 0 || text.rfind("/*!", 0) == 0) {
    text.erase(0, 3);
  } else if (text.rfind("/*", 0) == 0) {
    text.erase(0, 2);
  } else if (text.rfind("//", 0) == 0) {
    text.erase(0, 2);
  }
  if (text.size() >= 2 && text.compare(text.size() - 2, 2, "*/") == 0) {
    text.erase(text.size() - 2);
  }
  std::string cleaned;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    auto first = line.find_first_not_of(" \t");
    if (first == std::string::npos) {
      continue;
    }
    if (line[first] == '*') {
      ++first;  // leading '*' of a block-comment continuation line
      if (first < line.size() && line[first] == ' ') {
        ++first;
      }
    }
    if (!cleaned.empty()) {
      cleaned += '\n';
    }
    cleaned += rtrim(line.substr(first));
  }
  return cleaned;
}

// The doc comment immediately preceding a symbol node, when one exists
// (C block comments, Go // comments).
inline std::string doc_comment(std::string_view source, TSNode node) {
  const TSNode previous = ts_node_prev_named_sibling(node);
  if (ts_node_is_null(previous) ||
      std::strcmp(ts_node_type(previous), "comment") != 0) {
    return {};
  }
  const std::uint32_t gap_begin = ts_node_end_byte(previous);
  const std::uint32_t gap_end = ts_node_start_byte(node);
  const std::string_view gap = source.substr(gap_begin, gap_end - gap_begin);
  const bool only_whitespace = std::all_of(gap.begin(), gap.end(), [](char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  });
  if (!only_whitespace) {
    return {};
  }
  return clean_comment(node_text(source, previous));
}

}  // namespace cf::codeintel::detail
