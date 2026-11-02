// The editor's Markdown highlighting (the container lexer in src/editor.cpp).
#include "test.h"

namespace {
int StyleAt(const std::string& doc, const std::string& needle, size_t offset = 0) {
    SetDoc(doc);
    size_t at = doc.find(needle);
    CHECK(at != std::string::npos);
    return (int)TestEditor()(SCI_GETSTYLEAT, (sptr_t)(at + offset));
}
bool InCodeBlock(const std::string& doc, const std::string& needle) { return Md_IsCodeBlockStyle(StyleAt(doc, needle)); }
}  // namespace

TEST(styles_inline_emphasis) {
    CHECK_EQ(Md_InlineFlags(StyleAt("some **bold** text", "bold")), 1);
    CHECK_EQ(Md_InlineFlags(StyleAt("some *italic* text", "italic")), 2);
    CHECK_EQ(Md_InlineFlags(StyleAt("some ~~gone~~ text", "gone")), 4);
    CHECK_EQ(Md_InlineFlags(StyleAt("some `code` text", "code")), 8);
    CHECK_EQ(Md_InlineFlags(StyleAt("some plain text", "plain")), 0);
}

TEST(emphasis_markers_are_markup) {
    CHECK_EQ(Md_InlineFlags(StyleAt("some **bold** text", "**")), -1);
}

TEST(snake_case_is_not_italic) {
    CHECK_EQ(Md_InlineFlags(StyleAt("call some_long_name now", "long")), 0);
}

TEST(fenced_code_blocks) {
    const std::string doc = "intro\n```js\nlet a = *b*;\n```\nafter *this*";
    CHECK(InCodeBlock(doc, "let a"));
    CHECK(InCodeBlock(doc, "*b*"));
    CHECK(!InCodeBlock(doc, "after"));
    CHECK_EQ(Md_InlineFlags(StyleAt(doc, "this")), 2);
}

TEST(tilde_fences_and_longer_closers) {
    CHECK(InCodeBlock("~~~\ninside\n~~~", "inside"));
    CHECK(InCodeBlock("````\n```\nstill inside\n````", "still inside"));
    CHECK(!InCodeBlock("````\nx\n````\noutside", "outside"));
}

TEST(unclosed_fence_runs_to_the_end) {
    CHECK(InCodeBlock("```\na\n\nb at the end", "b at the end"));
}

TEST(indented_code_needs_a_blank_line_before_it) {
    CHECK(InCodeBlock("para\n\n    code here", "code here"));
    CHECK(!InCodeBlock("para\n    lazy continuation", "lazy"));
}

TEST(code_spans_hide_emphasis) {
    CHECK_EQ(Md_InlineFlags(StyleAt("`a *not italic* b`", "not")), 8);
}

TEST(editing_restyles_the_following_lines) {
    SetDoc("text\nmore\nlast");
    Sci& s = TestEditor();
    CHECK(!Md_IsCodeBlockStyle((int)s(SCI_GETSTYLEAT, 10)));
    s(SCI_INSERTTEXT, 0, (sptr_t)"```\n");
    Md_StyleNeeded(s, s.Len());
    CHECK(Md_IsCodeBlockStyle((int)s(SCI_GETSTYLEAT, 14)));   // "last" is now inside the open fence
}
