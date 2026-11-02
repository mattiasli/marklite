# MarkLite

[![CI](https://github.com/mattiasli/marklite/actions/workflows/ci.yml/badge.svg)](https://github.com/mattiasli/marklite/actions/workflows/ci.yml)

A small, fast, native Markdown reader and editor for Windows 10 and 11. The whole app is a single
1.7 MB `MarkLite.exe` with no runtime and no web browser inside.

Website and download: [marklite-app.vercel.app](https://marklite-app.vercel.app)

- **Editor:** syntax-coloured Markdown editing, with tabs for several files.
- **Preview:** a live, native preview (Direct2D/DirectWrite) beside the editor or on its own. You can select and copy text in it.
- **Formatting:** every Markdown feature is available from the formatting toolbar, the **Format** menu and the right-click menu, as well as from keyboard shortcuts.
- **Spell checking:** in all of the enabled languages at the same time (for example English and Swedish together). It uses the spell checkers built into Windows.
- **Rewrite with Claude:** select text, optionally type an instruction, and review the suggestion before accepting it.
- **Other:** light and dark themes (or follow Windows), find and replace, UTF-8, UTF-8 with BOM and UTF-16 files, CRLF and LF line endings, and session restore.

## Installing

Download `MarkLite-Setup.exe` from the [latest release](https://github.com/mattiasli/marklite/releases/latest)
and run it. Setup asks whether to install for you alone or for everyone on the PC:

- **Just me** (the default) installs to `%LOCALAPPDATA%\Programs\MarkLite`. No administrator rights are needed.
- **All users** installs to `Program Files\MarkLite` and asks for administrator rights.

Setup adds MarkLite to the Start menu and, if you leave the boxes ticked, to the desktop and to the
"Open with" list for `.md` and `.markdown` files. MarkLite only becomes the default app for Markdown
files if no other app has claimed them; otherwise Windows lets you pick it in **Settings → Default apps**.

Uninstall it from **Settings → Apps** like any other app. Your settings are kept unless you tick
"Also remove my settings".

The release builds are not code-signed yet, so Windows SmartScreen may warn the first time you run
Setup. Choose **More info → Run anyway**.

Setup can also run without a window, for scripted installs:

```
MarkLite-Setup.exe --silent [--just-me | --all-users] [--dir=PATH]
                   [--no-desktop-shortcut] [--no-file-associations]
uninstall.exe --silent [--remove-settings]
```

Exit code 0 means success.

### Without installing

`MarkLite.exe` is also attached to each release. Copy it anywhere and run it. You can open `.md`
files with it from Explorer or drop them onto the window.

Settings are saved in `%APPDATA%\MarkLite\MarkLite.ini`. For a portable setup, put a `MarkLite.ini` file
(it can be empty) next to `MarkLite.exe`, and that file is used instead.

## Formatting toolbar

The toolbar under the tabs has these controls, from left to right:

- **Paragraph style:** normal text or heading 1–6. The label always shows the style of the line the cursor is on.
- **Text styles:** bold, italic, strikethrough and inline code.
- **Lists:** bulleted, numbered and task lists, plus decrease and increase indent.
- **Blocks:** quote, callout (note, tip, important, warning or caution) and code block.
- **Inserts:**
  - Link.
  - Image from a file. The path is stored relative to the document.
  - Table. Inside a table, this button adds or deletes rows and columns, sets a column's alignment and lines the columns up.
  - Horizontal rule.
- **More (…):**
  - Underline, keyboard key, inline math and math block.
  - Footnote and line break.
  - Image from a web address.
  - Mark a task as done.
  - Clear formatting.
- **Right-hand side:** "Rewrite" with Claude, and the three view modes.

How the controls behave:

- Buttons light up for the formatting at the cursor.
- Clicking a button again removes that formatting.
- With no selection, inline styles apply to the word at the cursor, and line styles apply to the current line.
- Inline styles over several lines are applied line by line. In headings, list items and quotes the
  `#`, `-` or `>` stays outside the markers (`# **Title**`). If part of the selection already has the
  style, the whole selection gets it (`**a** and **b**` becomes `**a and b**`).
- With the cursor on one word of a longer styled phrase, clicking the button again takes just that
  word out of it (`**one two three**` becomes `**one** two **three**`).
- Headings underlined with `===` or `---` count as headings. Changing their style turns them into
  `#` headings or plain text.
- Each action can be undone in one step.
- If the window is narrow, the buttons that don't fit go into a **»** menu.
- You can hide the toolbar under **View → Formatting toolbar**.

Image files dropped onto the editor are inserted as image links, not opened as documents.

## Pasting images

You can paste a screenshot (for example one taken with **Win+Shift+S**) or a picture copied from another program with **Ctrl+V** or **Alt+V**:

- The picture is saved as a PNG in an `images` folder next to the document, for example `images/image-20261102-142530.png`. A link such as `![image](images/image-20261102-142530.png)` is inserted at the cursor.
- If the document has never been saved, MarkLite asks you to save it first, so that it knows where to put the `images` folder.
- **Ctrl+V** pastes text if the clipboard holds any. Some programs, such as Excel, copy both text and a picture. Use **Alt+V** to paste the picture instead.
- Image files copied in File Explorer are linked where they are, the same as when you drop them onto the editor.
- Undo removes the link but keeps the PNG file.
- **Save As** into another folder takes the document's local images along:
  - Images inside the document's folder (such as `images/…`) are copied to the same place in the new folder. If a different file with that name is already there, the copy gets a new name (`logo-2.png`) and the link is updated.
  - Links to images elsewhere (such as `../shared/pic.png`) are rewritten so that they still work from the new folder.
    An image on another drive or network share gets a full path instead, such as `D:/pictures/pic.png`
    or `file://server/share/pic.png`.
  - If an image can't be copied, its link points to the original file instead.
  - Web images, links that were already broken, and examples inside code are left alone.

For safety, the preview doesn't load images from other computers (network paths such as
`\\server\share\pic.png`) unless the document itself is on that computer. Opening them would make
Windows send your login to that computer.

YAML front matter (between `---` lines at the very top of a document) is shown in the preview as a
code block.

**Edit → Paste image** and the right-click menu do the same as Alt+V. The **View** menu's access key is now **Alt+I**, because Alt+V pastes.

## Keyboard shortcuts

| Shortcut | Action |
|---|---|
| Ctrl+B / Ctrl+I / Ctrl+U | Bold / italic / underline |
| Ctrl+Shift+X | Strikethrough |
| Ctrl+Shift+C | Inline code |
| Ctrl+Space | Clear formatting |
| Ctrl+Shift+1 … 6 | Heading 1 … 6 |
| Ctrl+Shift+0 | Normal text |
| Ctrl+Shift+7 / 8 / 9 | Numbered / bulleted / task list |
| Tab / Shift+Tab | Nest a list item deeper / move it back out (otherwise, and always in code blocks, an ordinary tab) |
| Ctrl+Shift+Q | Quote |
| Ctrl+Shift+K | Code block |
| Ctrl+L | Link |
| Ctrl+Shift+I | Image from a file |
| Ctrl+V / Alt+V | Paste (a screenshot or copied picture is saved to `images/` and linked; Alt+V prefers the picture) |
| Ctrl+K | Rewrite the selection with Claude |
| Ctrl+Enter | Accept Claude's suggestion |
| Ctrl+1 / Ctrl+2 / Ctrl+3 | Editor only / editor + preview / preview only |
| Ctrl+E | Toggle the preview |
| Ctrl+F / Ctrl+H / F3 | Find / replace / find next |
| Ctrl+G | Go to line |
| F7 | Spell checking on or off |
| Ctrl+N / Ctrl+O / Ctrl+S / Ctrl+W | New tab / open / save / close tab |
| Ctrl+Tab / Ctrl+Shift+Tab | Next / previous tab |
| Ctrl++ / Ctrl+- / Ctrl+0 | Zoom in / out / reset |
| Alt+Z | Word wrap |

Headings use Ctrl+**Shift**+digit, not Ctrl+Alt+digit. On Swedish and many other keyboards,
Ctrl+Alt acts as AltGr, which types characters such as `@ £ $ { [ ] }`.

## Spell checking

MarkLite uses the spell checkers that come with Windows. In the **Spelling** menu, tick every language
you write in. A word counts as correct if any of the ticked languages knows it, so English and
Swedish can be mixed freely.

To get more languages, open **Windows Settings → Time & language → Language & region → Add a language**.
Restart MarkLite and the new language appears in the Spelling menu. Right-click a misspelled word
to see suggestions, add it to a dictionary or ignore it. Code, links and HTML are not spell-checked.

## Rewriting text with Claude

1. Open **Claude → Claude settings**, paste your Anthropic API key and press **Verify key**. This also
   fetches the list of models your key can use.
2. Select text and press **Ctrl+K**, or click **Rewrite** on the toolbar.
3. Optionally type an instruction, such as "shorter", "more formal" or "translate to Swedish".
   Then press **Enter** or **Generate**.
4. The suggestion appears next to the original. Press **Accept** (Ctrl+Enter) to replace the text.
   You can also **Retry**, or **Discard** to leave the document unchanged. Accepting can be undone
   with Ctrl+Z.

Which model is used:

- You choose the model for each request in the dropdown in the panel.
- The default model is set in Settings: Claude Opus 5.5, Sonnet 5.5, Haiku 5.5, Fable 5.1, or any
  other model your key has access to.
- For models that support it, requests ask the API to fall back to another model automatically if the chosen one is overloaded.

"Whole document as context" sends the rest of the document along with the request, so the rewrite
matches its style.

How your API key and text are handled:

- The API key is encrypted with Windows DPAPI, so only your Windows account can decrypt it.
- Only the selected text, plus the document when the context option is on, is sent, and only to `api.anthropic.com`.

## Building from source

Requirements: [MSYS2](https://www.msys2.org) with the UCRT64 toolchain.

```
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make
```

Then run `build.bat`, or `mingw32-make` from an MSYS2 UCRT64 shell. Both take the same targets:

| Command | Builds |
|---|---|
| `mingw32-make` | `MarkLite.exe`, statically linked, in the project folder |
| `mingw32-make installer` | `MarkLite-Setup.exe`, the installer with `MarkLite.exe` inside |
| `mingw32-make test` | the unit tests, and runs them |
| `mingw32-make clean` | removes all build output |

The version number lives in the `VERSION` file. The build compiles it into the app, the installer and
their version resources.

How the build is configured:

- Code that only runs when you click something is compiled for size (`-Os`).
- The editor styling, preview, spell checking and Markdown parser are compiled for speed (`-O2`).

The source is in `src/`:

| File | Contents |
|---|---|
| `main.cpp` | Window, menus, toolbar wiring, commands |
| `editor.cpp` | Markdown syntax colouring |
| `preview.cpp` | Preview layout, rendering and selection |
| `format.cpp` | Markdown formatting commands |
| `clipimage.cpp` | Saving pasted pictures as PNG files |
| `toolbar.cpp` | Formatting toolbar control |
| `spell.cpp` | Spell checking |
| `ai.cpp`, `aipanel.cpp` | Claude requests and the rewrite panel |
| `settings.cpp`, `dialogs.cpp` | Settings file and dialogs |
| `tabs.cpp`, `findbar.cpp`, `theme.cpp` | Tab strip, find bar and colours |

The rest of the repository:

| Folder | Contents |
|---|---|
| `installer/` | The installer, also written in C++ with Win32 and Direct2D. The same program is copied into the install folder as `uninstall.exe`. `installer/tools/pack.cpp` compresses `MarkLite.exe` (LZMS) before it is embedded. |
| `tests/` | Unit tests for the formatting commands, the editor's syntax colouring, the JSON code and image handling on Save As. They drive a real, hidden Scintilla control. `installer-smoke.ps1` installs and uninstalls the app and checks the files, shortcuts and registry entries. |
| `website/` | The website at [marklite-app.vercel.app](https://marklite-app.vercel.app), built with [Astro](https://astro.build). See `website/README.md`. |
| `.github/workflows/` | `ci.yml` builds everything and runs all tests on every push and pull request. `release.yml` publishes a release when a version tag is pushed. |

## Releasing a new version

1. Update `VERSION` and commit.
2. Tag the commit and push the tag: `git tag v1.2.0 && git push origin v1.2.0`.
3. The release workflow builds and tests the app and installer, then publishes a GitHub release
   with `MarkLite-Setup.exe`, `MarkLite.exe` and `SHA256SUMS.txt`. The website's download button
   always points at the newest release.

Code signing is prepared in `release.yml` but switched off. To use it, add the repository variable
`SIGN_RELEASES=true` and the secrets `SIGNING_CERT_PFX_BASE64` (the .pfx file, base64-encoded) and
`SIGNING_CERT_PASSWORD`.

## Third-party code

- [Scintilla](https://www.scintilla.org) 5.6.9: the editing component. Licence: `third_party/scintilla/License.txt`.
- [md4c](https://github.com/mity/md4c): the Markdown parser. Licence: `third_party/md4c/LICENSE.md`.

## Licence

MarkLite is released under the MIT licence. See `LICENSE`.
