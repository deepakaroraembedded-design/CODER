#include "cf/rag/html.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace cf::rag {
namespace {

std::string to_lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

bool is_space(char c) {
  return std::isspace(static_cast<unsigned char>(c)) != 0;
}

void trim(std::string& text) {
  while (!text.empty() && is_space(text.front())) {
    text.erase(text.begin());
  }
  while (!text.empty() && is_space(text.back())) {
    text.pop_back();
  }
}

// Removes `<tag ...> ... </tag>` blocks (case-insensitive), including the
// tag itself and everything up to the first closing tag.
void remove_block(std::string& text, const std::string& tag) {
  const std::string open = "<" + tag;
  const std::string close = "</" + tag;
  std::size_t pos = 0;
  while ((pos = to_lower(text).find(open, pos)) != std::string::npos) {
    const std::size_t close_pos = to_lower(text).find(close, pos + open.size());
    if (close_pos == std::string::npos) {
      text.erase(pos);
      return;
    }
    const std::size_t end = text.find('>', close_pos + close.size());
    text.erase(pos, (end == std::string::npos ? text.size() : end + 1) - pos);
  }
}

void remove_comments(std::string& text) {
  std::size_t pos = 0;
  while ((pos = text.find("<!--", pos)) != std::string::npos) {
    const std::size_t end = text.find("-->", pos + 4);
    text.erase(pos, (end == std::string::npos ? text.size() : end + 3) - pos);
  }
}

void decode_entities(std::string& text) {
  const std::vector<std::pair<std::string, std::string>> entities = {
      {"&amp;", "&"},  {"&lt;", "<"},   {"&gt;", ">"},   {"&quot;", "\""},
      {"&#39;", "'"},  {"&apos;", "'"}, {"&nbsp;", " "}, {"&mdash;", "-"},
      {"&ndash;", "-"}, {"&hellip;", "..."}};
  for (const auto& [entity, replacement] : entities) {
    std::size_t pos = 0;
    while ((pos = text.find(entity, pos)) != std::string::npos) {
      text.replace(pos, entity.size(), replacement);
      pos += replacement.size();
    }
  }
  // Numeric entities (&#NNN; and &#xHH;).
  std::size_t pos = 0;
  while ((pos = text.find("&#", pos)) != std::string::npos) {
    const std::size_t end = text.find(';', pos + 2);
    if (end == std::string::npos || end - pos > 8) {
      pos += 2;
      continue;
    }
    const std::string digits = text.substr(pos + 2, end - pos - 2);
    try {
      const long code = digits.size() > 1 && (digits[0] == 'x' || digits[0] == 'X')
                            ? std::stol(digits.substr(1), nullptr, 16)
                            : std::stol(digits);
      if (code > 0 && code < 128) {
        text.replace(pos, end - pos + 1, 1, static_cast<char>(code));
        ++pos;
      } else {
        pos = end + 1;
      }
    } catch (const std::exception&) {
      pos = end + 1;
    }
  }
}

void collapse_whitespace(std::string& text) {
  std::string out;
  out.reserve(text.size());
  int consecutive_newlines = 0;
  bool in_space = false;
  for (const char c : text) {
    if (c == '\n') {
      while (!out.empty() && out.back() == ' ') {
        out.pop_back();
      }
      if (consecutive_newlines < 2) {
        out.push_back('\n');
      }
      ++consecutive_newlines;
      in_space = true;
    } else if (is_space(c)) {
      if (!in_space) {
        out.push_back(' ');
        in_space = true;
      }
    } else {
      out.push_back(c);
      consecutive_newlines = 0;
      in_space = false;
    }
  }
  trim(out);
  text = std::move(out);
}

}  // namespace

std::string clean_html_to_markdown(std::string_view html) {
  std::string text(html);
  remove_comments(text);
  for (const char* tag : {"script", "style", "nav", "header", "footer", "aside"}) {
    remove_block(text, tag);
  }

  std::string out;
  out.reserve(text.size());
  std::size_t pos = 0;
  while (pos < text.size()) {
    const char c = text[pos];
    if (c != '<') {
      out.push_back(c);
      ++pos;
      continue;
    }
    const std::size_t end = text.find('>', pos);
    if (end == std::string::npos) {
      break;  // unterminated tag: drop the remainder
    }
    std::string tag = to_lower(text.substr(pos + 1, end - pos - 1));
    trim(tag);
    const bool closing = !tag.empty() && tag[0] == '/';
    if (closing) {
      tag.erase(tag.begin());
    }
    // Strip attributes.
    const std::size_t space = tag.find_first_of(" \t\r\n/");
    if (space != std::string::npos) {
      tag.erase(space);
    }

    if (!closing && tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
      out += "\n\n";
      out.append(static_cast<std::size_t>(tag[1] - '0'), '#');
      out.push_back(' ');
    } else if (closing && tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' &&
               tag[1] <= '6') {
      out += "\n\n";
    } else if (tag == "p" || tag == "div" || tag == "section" || tag == "tr") {
      out += "\n\n";
    } else if (tag == "br") {
      out.push_back('\n');
    } else if (tag == "li") {
      out += "\n- ";
    } else if (tag == "td" || tag == "th") {
      out.push_back(' ');
    }
    pos = end + 1;
  }

  decode_entities(out);
  collapse_whitespace(out);
  return out;
}

}  // namespace cf::rag
