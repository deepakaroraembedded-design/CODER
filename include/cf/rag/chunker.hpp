// cf-rag: tokenizer and structure-aware chunking (design section 6.3).
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cf::rag {

// Lowercase alphanumeric tokens; '_' and '-' are kept inside identifiers.
std::vector<std::string> tokenize(std::string_view text);

// Whitespace-word approximation of the token count used for chunk sizing.
std::size_t estimate_tokens(std::string_view text);

struct ChunkOptions {
  std::size_t min_tokens = 300;  // design section 6.3: 300-800 tokens
  std::size_t max_tokens = 800;
  double overlap = 0.15;  // fraction of max_tokens repeated between pieces
};

struct TextChunk {
  std::string text;
  std::string anchor;  // heading slug or "chunk-<n>"
};

// Splits Markdown by heading, then by size with overlap. Preamble before the
// first heading becomes the "intro" chunk.
std::vector<TextChunk> chunk_markdown(std::string_view text,
                                      const ChunkOptions& options = {});

// Splits plain text by paragraph, then by size with overlap.
std::vector<TextChunk> chunk_text(std::string_view text,
                                  const ChunkOptions& options = {});

}  // namespace cf::rag
