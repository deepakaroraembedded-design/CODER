#include "cf/rag/chunker.hpp"

#include <algorithm>
#include <cctype>

namespace cf::rag {
namespace {

void push_token(std::vector<std::string>& tokens, std::string& current) {
  if (current.empty()) {
    return;
  }
  const bool has_alnum = std::any_of(current.begin(), current.end(), [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0;
  });
  if (has_alnum) {
    tokens.push_back(current);
  }
  current.clear();
}

std::vector<std::string> split_words(std::string_view text) {
  std::vector<std::string> words;
  std::string current;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      if (!current.empty()) {
        words.push_back(std::move(current));
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    words.push_back(std::move(current));
  }
  return words;
}

std::string join_words(const std::vector<std::string>& words, std::size_t begin,
                       std::size_t end) {
  std::string out;
  for (std::size_t i = begin; i < end; ++i) {
    if (!out.empty()) {
      out.push_back(' ');
    }
    out += words[i];
  }
  return out;
}

std::string slug(std::string_view heading) {
  std::string out;
  bool last_dash = false;
  for (const char c : heading) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0) {
      out.push_back(static_cast<char>(std::tolower(uc)));
      last_dash = false;
    } else if (!out.empty() && !last_dash) {
      out.push_back('-');
      last_dash = true;
    }
  }
  while (!out.empty() && out.back() == '-') {
    out.pop_back();
  }
  return out.empty() ? "section" : out;
}

std::vector<std::string> split_paragraphs(std::string_view text) {
  std::vector<std::string> paragraphs;
  std::string current;
  std::size_t newlines = 0;
  for (const char c : text) {
    if (c == '\n') {
      ++newlines;
      if (newlines >= 2) {
        if (!current.empty()) {
          paragraphs.push_back(std::move(current));
          current.clear();
        }
        newlines = 0;
        continue;
      }
    } else {
      if (newlines > 0 && !current.empty()) {
        current.push_back('\n');
      }
      newlines = 0;
    }
    if (newlines == 0 && c != '\r') {
      current.push_back(c);
    }
  }
  if (!current.empty()) {
    paragraphs.push_back(std::move(current));
  }
  return paragraphs;
}

std::vector<std::string> pack_paragraphs(const std::vector<std::string>& paragraphs,
                                         const ChunkOptions& options,
                                         bool respect_min) {
  std::vector<std::string> pieces;
  std::string current;
  std::size_t current_tokens = 0;
  const auto flush = [&pieces, &current, &current_tokens]() {
    if (!current.empty()) {
      pieces.push_back(current);
      current.clear();
      current_tokens = 0;
    }
  };
  for (const auto& paragraph : paragraphs) {
    const std::size_t paragraph_tokens = estimate_tokens(paragraph);
    if (paragraph_tokens > options.max_tokens) {
      flush();
      const std::vector<std::string> words = split_words(paragraph);
      for (std::size_t i = 0; i < words.size(); i += options.max_tokens) {
        const std::size_t end = std::min(i + options.max_tokens, words.size());
        pieces.push_back(join_words(words, i, end));
      }
      continue;
    }
    if (!current.empty() && current_tokens + paragraph_tokens > options.max_tokens) {
      flush();
    }
    if (!current.empty()) {
      current += "\n\n";
    }
    current += paragraph;
    current_tokens += paragraph_tokens;
    if (respect_min && current_tokens >= options.min_tokens) {
      flush();
    }
  }
  flush();
  return pieces;
}

std::vector<std::string> apply_overlap(const std::vector<std::string>& pieces,
                                       const ChunkOptions& options) {
  if (options.overlap <= 0.0 || pieces.size() < 2) {
    return pieces;
  }
  const auto overlap_words = static_cast<std::size_t>(
      options.overlap * static_cast<double>(options.max_tokens));
  if (overlap_words == 0) {
    return pieces;
  }
  std::vector<std::string> out;
  out.reserve(pieces.size());
  out.push_back(pieces.front());
  for (std::size_t i = 1; i < pieces.size(); ++i) {
    const std::vector<std::string> previous = split_words(pieces[i - 1]);
    const std::size_t take = std::min(overlap_words, previous.size());
    const std::string prefix = join_words(previous, previous.size() - take, previous.size());
    out.push_back(prefix.empty() ? pieces[i] : prefix + "\n" + pieces[i]);
  }
  return out;
}

bool is_heading(std::string_view line) {
  if (line.empty() || line.front() != '#') {
    return false;
  }
  std::size_t hashes = 0;
  while (hashes < line.size() && line[hashes] == '#') {
    ++hashes;
  }
  return hashes <= 6 && (hashes == line.size() || line[hashes] == ' ');
}

std::string heading_text(std::string_view line) {
  std::size_t pos = 0;
  while (pos < line.size() && line[pos] == '#') {
    ++pos;
  }
  while (pos < line.size() && line[pos] == ' ') {
    ++pos;
  }
  return std::string(line.substr(pos));
}

}  // namespace

std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char c : text) {
    const unsigned char uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) != 0 || c == '_' || c == '-') {
      current.push_back(static_cast<char>(std::tolower(uc)));
    } else {
      push_token(tokens, current);
    }
  }
  push_token(tokens, current);
  return tokens;
}

std::size_t estimate_tokens(std::string_view text) {
  std::size_t count = 0;
  bool in_word = false;
  for (const char c : text) {
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      in_word = false;
    } else if (!in_word) {
      in_word = true;
      ++count;
    }
  }
  return count;
}

std::vector<TextChunk> chunk_markdown(std::string_view text,
                                      const ChunkOptions& options) {
  struct Section {
    std::string anchor;
    std::string heading;
    std::string body;
  };
  std::vector<Section> sections;
  Section current{"intro", {}, {}};
  std::size_t pos = 0;
  while (pos <= text.size()) {
    const std::size_t line_end = text.find('\n', pos);
    const std::string_view line =
        line_end == std::string_view::npos ? text.substr(pos)
                                           : text.substr(pos, line_end - pos);
    if (is_heading(line)) {
      if (!current.body.empty() || !current.heading.empty()) {
        sections.push_back(std::move(current));
      }
      current = Section{slug(heading_text(line)), heading_text(line), {}};
    } else if (!line.empty()) {
      if (!current.body.empty()) {
        current.body.push_back('\n');
      }
      current.body.append(line);
    } else if (!current.body.empty()) {
      current.body.push_back('\n');
    }
    if (line_end == std::string_view::npos) {
      break;
    }
    pos = line_end + 1;
  }
  if (!current.body.empty() || !current.heading.empty()) {
    sections.push_back(std::move(current));
  }

  std::vector<TextChunk> chunks;
  for (const auto& section : sections) {
    std::vector<std::string> paragraphs = split_paragraphs(section.body);
    if (!section.heading.empty()) {
      // The heading is part of the chunk text so queries can match on it, and
      // it joins the first body paragraph so size-based splitting stays even.
      if (paragraphs.empty()) {
        paragraphs.push_back(section.heading);
      } else {
        paragraphs.front() = section.heading + "\n" + paragraphs.front();
      }
    }
    const std::vector<std::string> pieces =
        apply_overlap(pack_paragraphs(paragraphs, options, false), options);
    for (std::size_t i = 0; i < pieces.size(); ++i) {
      TextChunk chunk;
      chunk.text = pieces[i];
      chunk.anchor = pieces.size() == 1 ? section.anchor
                                        : section.anchor + "-part-" + std::to_string(i + 1);
      chunks.push_back(std::move(chunk));
    }
  }
  return chunks;
}

std::vector<TextChunk> chunk_text(std::string_view text, const ChunkOptions& options) {
  const std::vector<std::string> paragraphs = split_paragraphs(text);
  const std::vector<std::string> pieces =
      apply_overlap(pack_paragraphs(paragraphs, options, true), options);
  std::vector<TextChunk> chunks;
  for (std::size_t i = 0; i < pieces.size(); ++i) {
    TextChunk chunk;
    chunk.text = pieces[i];
    chunk.anchor = "chunk-" + std::to_string(i + 1);
    chunks.push_back(std::move(chunk));
  }
  return chunks;
}

}  // namespace cf::rag
