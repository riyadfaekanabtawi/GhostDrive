#include "ghostdrive/app/text.hpp"

#include <gtest/gtest.h>

namespace ghostdrive::app {
namespace {

TEST(CleanText, KeepsOrdinaryText) {
    EXPECT_EQ(clean_text("laptop"), "laptop");
    EXPECT_EQ(clean_text("caf\xc3\xa9 \xe6\x9d\xb1\xe4\xba\xac"),
              "caf\xc3\xa9 \xe6\x9d\xb1\xe4\xba\xac");
    EXPECT_EQ(clean_text(""), "");
}

TEST(CleanText, ReplacesControlCharacters) {
    EXPECT_EQ(clean_text("a\nb\tc\rd"), "a?b?c?d");
    EXPECT_EQ(clean_text(std::string_view("a\0b", 3)), "a?b");
    EXPECT_EQ(clean_text("del\x7f"), "del?");
    EXPECT_EQ(clean_text("c1\xc2\x85"), "c1?");  // U+0085 NEXT LINE
    EXPECT_EQ(clean_text("\x1b[31mred"), "?[31mred");
}

TEST(CleanText, ReplacesBidiAndInvisibleCharacters) {
    EXPECT_EQ(clean_text("evil\xe2\x80\xaetxt.exe"), "evil?txt.exe");  // U+202E RLO
    EXPECT_EQ(clean_text("\xe2\x81\xa6x\xe2\x81\xa9"), "?x?");         // U+2066, U+2069
    EXPECT_EQ(clean_text("a\xe2\x80\x8b"
                         "b"),
              "a?b");                                    // U+200B ZWSP
    EXPECT_EQ(clean_text("\xef\xbb\xbfname"), "?name");  // U+FEFF BOM
    EXPECT_EQ(clean_text("a\xe2\x80\xa8"
                         "b"),
              "a?b");                                // U+2028 LS
    EXPECT_EQ(clean_text("\xd8\x9cx"), "?x");        // U+061C ALM
    EXPECT_EQ(clean_text("\xee\x80\x80"), "?");      // U+E000 private use
    EXPECT_EQ(clean_text("\xf3\xa0\x80\x81"), "?");  // U+E0001 tag
}

TEST(CleanText, ReplacesInvalidUtf8) {
    EXPECT_EQ(clean_text("a\xff"
                         "b"),
              "a?b");
    EXPECT_EQ(clean_text("\xc0\xaf"), "??");            // overlong "/"
    EXPECT_EQ(clean_text("\xed\xa0\x80"), "???");       // encoded surrogate
    EXPECT_EQ(clean_text("\xe2\x82"), "??");            // truncated
    EXPECT_EQ(clean_text("\xf4\x90\x80\x80"), "????");  // above U+10FFFF
}

TEST(CleanText, CutsLongText) {
    EXPECT_EQ(clean_text("abcdefghij", 10), "abcdefghij");
    EXPECT_EQ(clean_text("abcdefghijk", 10), "abcdefg...");
    // Counted in code points, never splitting a multi-byte character.
    std::string wide;
    for (int i = 0; i < 6; ++i) wide += "\xc3\xa9";
    EXPECT_EQ(clean_text(wide, 5), "\xc3\xa9\xc3\xa9...");
}

TEST(CleanLines, StripsPrefixesAndEmptyLines) {
    auto lines = clean_lines("ghostdrive: could not join\n\n  error: bad\x1b code \nlast\r\n", 8);
    ASSERT_EQ(lines.size(), 3u);
    EXPECT_EQ(lines[0], "could not join");
    EXPECT_EQ(lines[1], "bad? code");
    EXPECT_EQ(lines[2], "last");
}

TEST(CleanLines, LimitsLineCount) {
    EXPECT_EQ(clean_lines("a\nb\nc\nd", 2).size(), 2u);
    EXPECT_TRUE(clean_lines("").empty());
    EXPECT_TRUE(clean_lines("\n \n").empty());
}

TEST(Trim, RemovesSurroundingWhitespace) {
    EXPECT_EQ(trim("  x y \n"), "x y");
    EXPECT_EQ(trim(" \t"), "");
}

}  // namespace
}  // namespace ghostdrive::app
