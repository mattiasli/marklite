// "Save as" into another folder takes the document's local images along (Fmt_RelocateImages).
#include "test.h"
#include "format.h"
#include <shlobj.h>
#include <shellapi.h>
#include <fstream>

namespace {
struct TempDir {
    std::wstring path;
    TempDir() {
        wchar_t base[MAX_PATH];
        GetTempPathW(MAX_PATH, base);
        path = std::wstring(base) + L"marklite-tests-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(GetTickCount64());
        SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    }
    ~TempDir() {
        std::wstring from = path + L'\0';
        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = from.c_str();
        op.fFlags = FOF_NO_UI;
        SHFileOperationW(&op);
    }
    std::wstring operator/(const std::wstring& rel) const { return path + L"\\" + rel; }
};

void WriteFile(const std::wstring& path, const std::string& data) {
    size_t k = path.find_last_of(L'\\');
    SHCreateDirectoryExW(nullptr, path.substr(0, k).c_str(), nullptr);
    std::ofstream(path.c_str(), std::ios::binary) << data;
}

std::string ReadFile(const std::wstring& path) {
    std::ifstream f(path.c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
}  // namespace

TEST(save_as_copies_images_from_the_document_folder) {
    TempDir t;
    WriteFile(t / L"old\\images\\cat.png", "cat pixels");
    SetDoc("# Notes\n\n![a cat](images/cat.png)\n");
    ImageMove m = Fmt_RelocateImages(TestEditor(), t / L"old\\notes.md", t / L"new\\notes.md");
    CHECK_EQ(m.copied, 1);
    CHECK_EQ(m.rewritten, 0);
    CHECK(m.error.empty());
    CHECK_EQ(ReadFile(t / L"new\\images\\cat.png"), std::string("cat pixels"));
    CHECK_EQ(TestEditor().Text(), std::string("# Notes\n\n![a cat](images/cat.png)\n"));
}

TEST(save_as_rewrites_links_to_images_elsewhere) {
    TempDir t;
    WriteFile(t / L"docs\\shared\\logo.png", "logo");
    SetDoc("![logo](../shared/logo.png)");
    ImageMove m = Fmt_RelocateImages(TestEditor(), t / L"docs\\a\\readme.md", t / L"readme.md");
    CHECK_EQ(m.copied, 0);
    CHECK_EQ(m.rewritten, 1);
    std::string text = TestEditor().Text();
    // Seen from the new folder the image is at docs\shared\logo.png; links use forward slashes.
    CHECK_EQ(text, std::string("![logo](docs/shared/logo.png)"));
}

TEST(save_as_keeps_a_different_file_with_the_same_name) {
    TempDir t;
    WriteFile(t / L"old\\pic.png", "new picture");
    WriteFile(t / L"new\\pic.png", "something else");
    SetDoc("![](pic.png)");
    ImageMove m = Fmt_RelocateImages(TestEditor(), t / L"old\\doc.md", t / L"new\\doc.md");
    CHECK_EQ(m.copied, 1);
    CHECK_EQ(ReadFile(t / L"new\\pic.png"), std::string("something else"));
    CHECK_EQ(ReadFile(t / L"new\\pic-2.png"), std::string("new picture"));
    CHECK_EQ(TestEditor().Text(), std::string("![](pic-2.png)"));
}

TEST(save_as_leaves_web_and_broken_images_alone) {
    TempDir t;
    const std::string doc = "![](https://example.com/a.png)\n![](missing.png)\n![](data:image/png;base64,AAAA)";
    SetDoc(doc);
    ImageMove m = Fmt_RelocateImages(TestEditor(), t / L"old\\doc.md", t / L"new\\doc.md");
    CHECK_EQ(m.copied, 0);
    CHECK_EQ(m.rewritten, 0);
    CHECK_EQ(TestEditor().Text(), doc);
    CHECK(!Exists(t / L"new\\missing.png"));
}

TEST(save_as_in_the_same_folder_changes_nothing) {
    TempDir t;
    WriteFile(t / L"same\\x.png", "x");
    SetDoc("![](x.png)");
    ImageMove m = Fmt_RelocateImages(TestEditor(), t / L"same\\a.md", t / L"same\\b.md");
    CHECK_EQ(m.copied + m.rewritten, 0);
}
