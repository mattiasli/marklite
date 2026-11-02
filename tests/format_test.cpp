// Formatting commands (toolbar, Format menu and shortcuts), run on a real Scintilla control.
#include "test.h"
#include "format.h"
#include "resource.h"

namespace {
// Runs `cmd` on `before` and returns the document with the caret or selection marked.
std::string After(const std::string& before, int cmd) {
    SetDoc(before);
    CHECK(Run(cmd));
    return GetDoc();
}
}  // namespace

// ------------------------------------------------------------------ inline formatting
TEST(bold_wraps_the_selection) {
    CHECK_EQ(After("make {this} bold", CMD_BOLD), std::string("make **{this}** bold"));
}

TEST(bold_wraps_the_word_at_the_caret) {
    CHECK_EQ(After("make th@is bold", CMD_BOLD), std::string("make **{this}** bold"));
}

TEST(bold_toggles_off_again) {
    CHECK_EQ(After("make **{this}** bold", CMD_BOLD), std::string("make {this} bold"));
    CHECK_EQ(After("make **th@is** bold", CMD_BOLD), std::string("make {this} bold"));
}

TEST(bold_on_empty_space_inserts_an_empty_pair) {
    CHECK_EQ(After("a @ b", CMD_BOLD), std::string("a **@** b"));
    // and pressing it again removes the pair
    CHECK(Run(CMD_BOLD));
    CHECK_EQ(GetDoc(), std::string("a @ b"));
}

TEST(italic_inside_bold_keeps_both) {
    CHECK_EQ(After("**{word}**", CMD_ITALIC), std::string("***{word}***"));
    CHECK_EQ(After("***{word}***", CMD_ITALIC), std::string("**{word}**"));
}

TEST(inline_code_and_strikethrough) {
    CHECK_EQ(After("run {ls -la} now", CMD_CODE), std::string("run `{ls -la}` now"));
    CHECK_EQ(After("an {old} idea", CMD_FMT_STRIKE), std::string("an ~~{old}~~ idea"));
}

TEST(underline_and_keyboard_tags) {
    CHECK_EQ(After("press {Ctrl}", CMD_FMT_KBD), std::string("press <kbd>{Ctrl}</kbd>"));
    CHECK_EQ(After("press <kbd>{Ctrl}</kbd>", CMD_FMT_KBD), std::string("press {Ctrl}"));
    CHECK_EQ(After("{note}", CMD_FMT_UNDERLINE), std::string("<u>{note}</u>"));
}

TEST(clear_formatting_removes_inline_marks) {
    SetDoc("{**bold** and *italic* and `code`}");
    CHECK(Run(CMD_FMT_CLEAR));
    CHECK_EQ(TestEditor().Text(), std::string("bold and italic and code"));
}

TEST(each_command_is_one_undo_step) {
    SetDoc("make {this} bold");
    CHECK(Run(CMD_BOLD));
    CHECK_EQ(TestEditor().Text(), std::string("make **this** bold"));
    TestEditor()(SCI_UNDO);
    CHECK_EQ(TestEditor().Text(), std::string("make this bold"));
}

// ------------------------------------------------------------------ links and images
TEST(link_from_selected_text_selects_the_url) {
    CHECK_EQ(After("see {the docs} here", CMD_LINK), std::string("see [the docs]({https://}) here"));
}

TEST(link_from_a_selected_url) {
    SetDoc("{https://example.com}");
    CHECK(Run(CMD_LINK));
    CHECK_EQ(TestEditor().Text(), std::string("[](https://example.com)"));
    SetDoc("{www.example.com}");
    CHECK(Run(CMD_LINK));
    CHECK_EQ(TestEditor().Text(), std::string("[](https://www.example.com)"));
}

TEST(link_with_nothing_selected) {
    CHECK_EQ(After("@", CMD_LINK), std::string("[{text}](https://)"));
}

TEST(image_url_uses_the_selection_as_alt_text) {
    CHECK_EQ(After("{a cat}", CMD_FMT_IMAGEURL), std::string("![a cat]({https://})"));
    CHECK_EQ(After("@", CMD_FMT_IMAGEURL), std::string("![alt text]({https://})"));
}

// ------------------------------------------------------------------ headings
TEST(heading_levels) {
    CHECK_EQ(After("Ti@tle", CMD_FMT_H1), std::string("# Ti@tle"));
    CHECK_EQ(After("# Ti@tle", CMD_FMT_H1 + 2), std::string("### Ti@tle"));
    CHECK_EQ(After("### Ti@tle", CMD_FMT_PARA), std::string("Ti@tle"));
}

TEST(heading_state_is_reported) {
    SetDoc("## Ti@tle");
    CHECK_EQ(Fmt_State(TestEditor()).heading, 2);
    SetDoc("plain te@xt");
    CHECK_EQ(Fmt_State(TestEditor()).heading, 0);
}

// ------------------------------------------------------------------ lists and quotes
TEST(bullet_list_toggles) {
    SetDoc("{one\ntwo}");
    CHECK(Run(CMD_FMT_BULLET));
    CHECK_EQ(TestEditor().Text(), std::string("- one\n- two"));
    TestEditor()(SCI_SETSEL, 0, TestEditor().Len());
    CHECK(Run(CMD_FMT_BULLET));
    CHECK_EQ(TestEditor().Text(), std::string("one\ntwo"));
}

TEST(numbered_list_counts_up) {
    SetDoc("{one\ntwo\nthree}");
    CHECK(Run(CMD_FMT_NUMBER));
    CHECK_EQ(TestEditor().Text(), std::string("1. one\n2. two\n3. three"));
}

TEST(bullets_become_numbers) {
    SetDoc("{- one\n- two}");
    CHECK(Run(CMD_FMT_NUMBER));
    CHECK_EQ(TestEditor().Text(), std::string("1. one\n2. two"));
}

TEST(task_list_and_done) {
    SetDoc("bu@y milk");
    CHECK(Run(CMD_FMT_TASK));
    CHECK_EQ(TestEditor().Text(), std::string("- [ ] buy milk"));
    CHECK(Run(CMD_FMT_TASKDONE));
    CHECK_EQ(TestEditor().Text(), std::string("- [x] buy milk"));
    FmtState st = Fmt_State(TestEditor());
    CHECK(st.task);
    CHECK(st.taskDone);
    CHECK(Run(CMD_FMT_TASKDONE));
    CHECK_EQ(TestEditor().Text(), std::string("- [ ] buy milk"));
}

TEST(list_state_is_reported) {
    SetDoc("- it@em");
    CHECK_EQ(Fmt_State(TestEditor()).list, 1);
    SetDoc("1. it@em");
    CHECK_EQ(Fmt_State(TestEditor()).list, 2);
}

TEST(indent_and_outdent_a_list_item) {
    SetDoc("- one\n- t@wo");
    CHECK(Run(CMD_FMT_INDENT));
    std::string indented = TestEditor().Text();
    CHECK(indented == "- one\n  - two" || indented == "- one\n    - two" || indented == "- one\n\t- two");
    CHECK(Run(CMD_FMT_OUTDENT));
    CHECK_EQ(TestEditor().Text(), std::string("- one\n- two"));
}

TEST(quote_toggles) {
    SetDoc("{first\nsecond}");
    CHECK(Run(CMD_FMT_QUOTE));
    CHECK_EQ(TestEditor().Text(), std::string("> first\n> second"));
    CHECK(Fmt_State(TestEditor()).quote);
    TestEditor()(SCI_SETSEL, 0, TestEditor().Len());
    CHECK(Run(CMD_FMT_QUOTE));
    CHECK_EQ(TestEditor().Text(), std::string("first\nsecond"));
}

// ------------------------------------------------------------------ blocks
TEST(code_block_fences_the_selected_lines) {
    SetDoc("text\n{a = 1\nb = 2}\nmore");
    CHECK(Run(CMD_FMT_CODEBLOCK));
    CHECK_EQ(TestEditor().Text(), std::string("text\n```\na = 1\nb = 2\n```\nmore"));
    TestEditor()(SCI_GOTOPOS, 10);   // inside the block
    CHECK(Fmt_State(TestEditor()).inCode);
}

TEST(code_block_toggles_off) {
    SetDoc("```\na = @1\n```");
    CHECK(Run(CMD_FMT_CODEBLOCK));
    CHECK_EQ(TestEditor().Text(), std::string("a = 1"));
}

TEST(horizontal_rule) {
    SetDoc("above@");
    CHECK(Run(CMD_FMT_HR));
    std::string t = TestEditor().Text();
    CHECK(t.find("---") != std::string::npos);
    CHECK(t.rfind("above", 0) == 0);
}

TEST(footnotes_are_numbered) {
    SetDoc("A claim@.");
    CHECK(Run(CMD_FMT_FOOTNOTE));
    std::string t = TestEditor().Text();
    CHECK(t.find("A claim[^1].") != std::string::npos);
    CHECK(t.find("[^1]:") != std::string::npos);
    TestEditor()(SCI_GOTOPOS, 1);
    CHECK(Run(CMD_FMT_FOOTNOTE));
    CHECK(TestEditor().Text().find("[^2]:") != std::string::npos);
}

TEST(callouts) {
    SetDoc("@");
    CHECK(Run(CMD_FMT_CALLOUT0 + 3));   // warning
    CHECK(TestEditor().Text().find("> [!WARNING]") != std::string::npos);
}

// ------------------------------------------------------------------ tables
TEST(insert_table_of_each_size) {
    const auto& sizes = Fmt_TableSizes();
    CHECK(!sizes.empty());
    for (size_t i = 0; i < sizes.size(); i++) {
        SetDoc("@");
        CHECK(Run(CMD_FMT_TABLE0 + (int)i));
        Sci& s = TestEditor();
        int lines = 0, pipes = 0;
        for (sptr_t l = 0; l < s(SCI_GETLINECOUNT); l++) {
            std::string line = s.LineText(l);
            if (line.empty() || line[0] != '|') continue;
            lines++;
            if (lines == 1)
                for (char c : line) pipes += c == '|';
        }
        CHECK_EQ(lines, sizes[i].rows + 2);   // header, separator, body rows
        CHECK_EQ(pipes, sizes[i].cols + 1);
    }
}

TEST(table_row_and_column_commands) {
    const std::string table = "| a | b |\n| --- | --- |\n| 1 | 2 |";
    SetDoc(table);
    TestEditor()(SCI_GOTOPOS, table.size() - 3);   // in the "2" cell
    CHECK(Fmt_State(TestEditor()).inTable);
    CHECK(Run(CMD_FMT_ROWBELOW));
    CHECK_EQ(TestEditor()(SCI_GETLINECOUNT), (sptr_t)4);
    CHECK(Run(CMD_FMT_DELROW));
    CHECK_EQ(TestEditor()(SCI_GETLINECOUNT), (sptr_t)3);

    SetDoc(table);
    TestEditor()(SCI_GOTOPOS, 2);   // in the "a" cell
    CHECK(Run(CMD_FMT_COLRIGHT));
    std::string header = TestEditor().LineText(0);
    int pipes = 0;
    for (char c : header) pipes += c == '|';
    CHECK_EQ(pipes, 4);
    CHECK(Run(CMD_FMT_DELCOL));
    header = TestEditor().LineText(0);
    pipes = 0;
    for (char c : header) pipes += c == '|';
    CHECK_EQ(pipes, 3);
}

TEST(table_alignment) {
    SetDoc("| a | b |\n| --- | --- |\n| 1 | 2 |");
    TestEditor()(SCI_GOTOPOS, 2);
    CHECK(Run(CMD_FMT_ALIGNCENTER));
    std::string sep = TestEditor().LineText(1);
    CHECK(sep.find(":-") != std::string::npos && sep.find("-:") != std::string::npos);
    CHECK(Run(CMD_FMT_ALIGNRIGHT));
    sep = TestEditor().LineText(1);
    CHECK(sep.find(":-") == std::string::npos || sep.find(":-") > sep.find('|', 1));
}

TEST(table_tidy_lines_up_columns) {
    SetDoc("| a | longer header |\n|-|-|\n| 1234567 | 2 |");
    TestEditor()(SCI_GOTOPOS, 2);
    CHECK(Run(CMD_FMT_TABLETIDY));
    Sci& s = TestEditor();
    std::string l0 = s.LineText(0), l1 = s.LineText(1), l2 = s.LineText(2);
    CHECK_EQ(l0.size(), l1.size());
    CHECK_EQ(l0.size(), l2.size());
    CHECK_EQ(l0.find('|', 1), l2.find('|', 1));
}

TEST(table_commands_outside_a_table_say_so) {
    extern std::wstring g_lastStatus;
    g_lastStatus.clear();
    SetDoc("no table he@re");
    CHECK(Run(CMD_FMT_ROWBELOW));
    CHECK_EQ(TestEditor().Text(), std::string("no table here"));
    CHECK(!g_lastStatus.empty());
}

TEST(unknown_commands_are_not_handled) {
    SetDoc("text@");
    CHECK(!Fmt_Command(TestEditor(), 1, nullptr, L""));
}

// ------------------------------------------------------------------ images on disk
TEST(image_files_are_recognised) {
    CHECK(Fmt_IsImageFile(L"C:\\pics\\cat.png"));
    CHECK(Fmt_IsImageFile(L"photo.JPG"));
    CHECK(Fmt_IsImageFile(L"anim.gif"));
    CHECK(!Fmt_IsImageFile(L"notes.md"));
    CHECK(!Fmt_IsImageFile(L"png"));
}
