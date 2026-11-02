// Test runner. Creates one hidden Scintilla control for the editor tests and runs every TEST.
//   build/tests.exe            run everything
//   build/tests.exe table      run the tests whose name contains "table"
#include "test.h"
#include "format.h"
#include <cstdio>
#include <cstring>

extern "C" int Scintilla_RegisterClasses(void* hInstance);

namespace {
int g_failures = 0;
bool g_currentFailed = false;
HWND g_host = nullptr;
Sci g_sci;

std::string Escape(const std::string& v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c == '"') out += "\\\"";
        else out += c;
    }
    return out + "\"";
}
}  // namespace

std::vector<TestCase>& TestRegistry() {
    static std::vector<TestCase> r;
    return r;
}

void TestFail(const char* file, int line, const std::string& what) {
    const char* base = strrchr(file, '/');
    if (!base) base = strrchr(file, '\\');
    printf("    %s:%d: %s\n", base ? base + 1 : file, line, what.c_str());
    g_currentFailed = true;
}

std::string TestShow(const std::string& v) { return Escape(v); }
std::string TestShow(const char* v) { return v ? Escape(v) : "null"; }
std::string TestShow(long long v) { return std::to_string(v); }
std::string TestShow(double v) {
    std::ostringstream o;
    o.precision(17);
    o << v;
    return o.str();
}

Sci& TestEditor() { return g_sci; }

void Restyle() {
    // Clearing the styles also marks the whole document as styled, so restart from the top.
    g_sci(SCI_CLEARDOCUMENTSTYLE);
    g_sci(SCI_STARTSTYLING, 0);
    Md_StyleNeeded(g_sci, g_sci.Len());
}

void SetDoc(const std::string& marked) {
    std::string text;
    long long caret = -1, selA = -1, selB = -1;
    for (char c : marked) {
        if (c == '@') caret = (long long)text.size();
        else if (c == '{') selA = (long long)text.size();
        else if (c == '}') selB = (long long)text.size();
        else text += c;
    }
    g_sci(SCI_SETTEXT, 0, (sptr_t)text.c_str());
    g_sci(SCI_EMPTYUNDOBUFFER);
    Restyle();
    if (selA >= 0 && selB >= 0) g_sci(SCI_SETSEL, selA, selB);
    else g_sci(SCI_GOTOPOS, caret >= 0 ? caret : (long long)text.size());
}

std::string GetDoc() {
    std::string text = g_sci.Text();
    size_t a = (size_t)g_sci(SCI_GETSELECTIONSTART), b = (size_t)g_sci(SCI_GETSELECTIONEND);
    if (a == b) return text.substr(0, a) + "@" + text.substr(a);
    return text.substr(0, a) + "{" + text.substr(a, b - a) + "}" + text.substr(b);
}

bool Run(int cmd) {
    bool handled = Fmt_Command(g_sci, cmd, g_host, L"");
    Restyle();
    return handled;
}

int wmain(int argc, wchar_t** argv) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    Scintilla_RegisterClasses(inst);
    g_host = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 600, 400, nullptr, nullptr, inst, nullptr);
    HWND ed = g_host ? CreateWindowExW(0, L"Scintilla", L"", WS_CHILD | WS_VISIBLE, 0, 0, 600, 400, g_host, nullptr, inst, nullptr)
                     : nullptr;
    if (!ed) {
        printf("could not create the Scintilla control\n");
        return 2;
    }
    g_sci.Attach(ed);
    g_sci(SCI_SETCODEPAGE, SC_CP_UTF8);
    g_sci(SCI_SETEOLMODE, SC_EOL_LF);
    Md_ConfigureView(g_sci, true);

    const wchar_t* filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    for (const TestCase& t : TestRegistry()) {
        if (filter) {
            std::wstring name(t.name, t.name + strlen(t.name));
            if (name.find(filter) == std::wstring::npos) continue;
        }
        g_currentFailed = false;
        t.fn();
        run++;
        if (g_currentFailed) g_failures++;
        printf("%s  %s\n", g_currentFailed ? "FAIL" : "ok  ", t.name);
    }
    printf("\n%d tests, %d failed\n", run, g_failures);
    DestroyWindow(g_host);
    return g_failures ? 1 : 0;
}
