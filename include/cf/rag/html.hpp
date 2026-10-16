// cf-rag: minimal HTML-to-Markdown cleaning for the reference mirror
// (design section 6.3.1 step 3).
//
// This is not a full HTML parser: it removes clearly non-content elements
// (script, style, nav, header, footer, aside, comments), converts headings,
// paragraphs and list items to Markdown, decodes common entities and
// collapses whitespace. Enough to index API documentation pages.
#pragma once

#include <string>
#include <string_view>

namespace cf::rag {

std::string clean_html_to_markdown(std::string_view html);

}  // namespace cf::rag
