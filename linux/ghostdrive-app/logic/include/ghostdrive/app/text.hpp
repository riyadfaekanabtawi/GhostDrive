#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ghostdrive::app {

// True for code points that must not reach a label: C0 and C1 controls, DEL, line and paragraph
// separators, bidirectional marks, embeddings, overrides and isolates, zero-width and other
// invisible format characters, tag characters, surrogates and private-use characters.
bool is_unsafe_code_point(char32_t c);

// Text from the daemon or the CLI made safe to show. Invalid UTF-8 bytes and unsafe code points
// become "?". Text longer than `max_chars` code points is cut and ends in "...".
std::string clean_text(std::string_view text, std::size_t max_chars = 64);

// Up to `max_lines` non-empty lines of CLI output, each trimmed and cleaned, with the
// "ghostdrive: " prefix of CLI errors and the "error: " prefix of daemon replies removed.
std::vector<std::string> clean_lines(std::string_view text, std::size_t max_lines = 8,
                                     std::size_t max_chars = 200);

// Leading and trailing ASCII whitespace removed.
std::string trim(std::string_view text);

// The lines joined with "\n".
std::string join_lines(const std::vector<std::string>& lines);

}  // namespace ghostdrive::app
