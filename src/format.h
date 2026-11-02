// Markdown formatting commands (toolbar, Format menu, shortcuts). Every command is one undo step.
#pragma once
#include "app.h"

struct FmtState {
    int inlineFlags = 0;   // 1 bold, 2 italic, 4 strikethrough, 8 inline code (at the caret)
    int heading = 0;       // 0 normal text, 1..6
    int list = 0;          // 0 none, 1 bulleted, 2 numbered
    bool task = false, taskDone = false, quote = false, inTable = false, inCode = false;
};

struct TableSize { int cols, rows; };   // rows = body rows (the header row comes on top)
const std::vector<TableSize>& Fmt_TableSizes();

FmtState Fmt_State(Sci& s);
// Runs a CMD_BOLD / CMD_ITALIC / CMD_CODE / CMD_LINK / CMD_FMT_* command; false if `cmd` isn't one.
// `docPath` is the document's file (may be empty) for relative image paths; `owner` parents dialogs.
bool Fmt_Command(Sci& s, int cmd, HWND owner, const std::wstring& docPath);
// Inserts Markdown image links for dropped image files at `pos`.
void Fmt_InsertImages(Sci& s, const std::vector<std::wstring>& files, const std::wstring& docPath, sptr_t pos);
// Replaces the selection with links to the images (pasting); one undo step.
void Fmt_PasteImages(Sci& s, const std::vector<std::wstring>& files, const std::wstring& docPath, const std::string& alt = "");
bool Fmt_IsImageFile(const std::wstring& path);

// When a document is saved under a new folder, its local images go with it: images inside the old
// folder are copied to the same relative place in the new one (images\x.png -> images\x.png);
// links to images elsewhere are rewritten to work from the new folder. Links are only changed
// when needed (one undo step). Web images and links that are already broken are left alone.
struct ImageMove { int copied = 0, rewritten = 0; std::wstring error; };
ImageMove Fmt_RelocateImages(Sci& s, const std::wstring& oldDoc, const std::wstring& newDoc);
