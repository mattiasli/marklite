// Scintilla configuration and a fast, incremental Markdown "container lexer".
// Block structure that spans lines (fenced code, front matter, HTML comments)
// is carried from line to line through Scintilla's per-line state.
#include "app.h"
#include <cstring>
#include <climits>
#include <vector>

namespace {
enum MdStyle {
    S_DEFAULT = 0, S_H1, S_H2, S_H3, S_H4, S_H5, S_H6, S_MARK, S_BOLD, S_ITALIC, S_BOLDITALIC,
    S_CODE, S_CODEBLOCK, S_FENCE, S_LINKTEXT, S_URL, S_QUOTE, S_LIST, S_HR, S_HTML, S_STRIKE,
    S_TABLE, S_FRONT, S_TASK, S_COUNT
};

// line state bits
enum : int {
    LS_FENCE = 1 << 0,       // inside fenced code block
    LS_TILDE = 1 << 1,       // fence uses '~'
    LS_FLEN_SHIFT = 2,       // 6 bits fence length
    LS_FRONT = 1 << 8,       // inside YAML front matter
    LS_COMMENT = 1 << 9,     // inside a <!-- comment --> block (started at the start of a line)
    LS_PARA = 1 << 10,       // line is plain paragraph text (for setext headings)
    LS_ICOMMENT = 1 << 11,   // inside a <!-- comment that started within paragraph text (ends with the paragraph)
    LS_INLIST = 1 << 12,     // inside a list item: blank lines and indented lines continue it
    LS_CIND_SHIFT = 13,      // 5 bits: content column of that list item
    LS_QFENCE = 1 << 18,     // the fenced code block is inside a block quote
    LS_LPARA = 1 << 19,      // line is paragraph text of a list item (a lazy continuation may follow)
};
const int kFenceBits = LS_FENCE | LS_TILDE | (63 << LS_FLEN_SHIFT) | LS_QFENCE;
const int kListBits = LS_INLIST | (31 << LS_CIND_SHIFT);

inline bool IsSpace(char c) { return c == ' ' || c == '\t'; }
inline bool IsAlnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
inline bool IsPunct(unsigned char c) { return c > 32 && c < 127 && !IsAlnum(c); }

struct Line {
    const char* t;   // text (no EOL)
    int n;           // length
    char* st;        // styles (n entries)
};

void Fill(Line& L, int a, int b, char s) {
    if (a < 0) a = 0;
    if (b > L.n) b = L.n;
    if (a < b) memset(L.st + a, s, (size_t)(b - a));
}

int SkipSpaces(const Line& L, int i) { while (i < L.n && IsSpace(L.t[i])) i++; return i; }

bool IsBlank(const char* t, int n) {
    for (int i = 0; i < n; i++) if (!IsSpace(t[i])) return false;
    return true;
}

// Matches: up to 3 spaces, then `ch` repeated >= minRun, then only spaces.
bool IsRunLine(const char* t, int n, char ch, int minRun, bool allowInnerSpaces) {
    int i = 0;
    while (i < n && i < 3 && t[i] == ' ') i++;
    int run = 0;
    for (; i < n; i++) {
        if (t[i] == ch) run++;
        else if (IsSpace(t[i])) { if (!allowInnerSpaces) { for (; i < n; i++) if (!IsSpace(t[i])) return false; break; } }
        else return false;
    }
    return run >= minRun;
}
bool IsHr(const char* t, int n) {
    return IsRunLine(t, n, '-', 3, true) || IsRunLine(t, n, '*', 3, true) || IsRunLine(t, n, '_', 3, true);
}
bool IsSetextUnderline(const char* t, int n) {
    return IsRunLine(t, n, '=', 1, false) || IsRunLine(t, n, '-', 1, false);
}

int EmphStyle(int flags, int base) {
    // flags: 1 bold, 2 italic, 4 strike
    if (flags & 1) return (flags & 2) ? S_BOLDITALIC : S_BOLD;
    if (flags & 2) return S_ITALIC;
    return base;
}

// Find the closing backtick run of exactly `run` length starting at i (plain scan).
int FindCodeCloseScan(const Line& L, int i, int end, int run) {
    while (i < end) {
        if (L.t[i] == '`') {
            int j = i;
            while (j < end && L.t[j] == '`') j++;
            if (j - i == run) return i;
            i = j;
        } else i++;
    }
    return -1;
}

// Closing delimiters for the line being styled, found in one pass. Scanning ahead from every opener
// instead is quadratic: a long line full of unmatched "*a " or "[a " froze the editor for seconds.
struct LineScan {
    const Line* line = nullptr;
    std::vector<int> codeClose;   // at the start of a backtick run: start of the next run of the same length
    std::vector<int> match;       // at '[' / '(': its partner, -1 = none, -2 = inside a code span
};
LineScan g_scan;

void BuildScan(const Line& L) {
    g_scan.line = &L;
    g_scan.codeClose.assign((size_t)L.n, -1);
    g_scan.match.assign((size_t)L.n, -2);
    std::vector<int> nextRun(65, -1);
    std::vector<std::pair<int, int>> runs;   // (start, length)
    for (int i = 0; i < L.n;) {
        if (L.t[i] != '`') { i++; continue; }
        int j = i; while (j < L.n && L.t[j] == '`') j++;
        runs.push_back({i, j - i});
        i = j;
    }
    std::vector<std::pair<int, int>> longRuns;   // runs longer than 64: few, searched linearly
    for (size_t k = runs.size(); k-- > 0;) {
        int at = runs[k].first, len = runs[k].second;
        if (len <= 64) { g_scan.codeClose[at] = nextRun[len]; nextRun[len] = at; }
        else {
            for (auto it = longRuns.rbegin(); it != longRuns.rend(); ++it) if (it->second == len) { g_scan.codeClose[at] = it->first; break; }
            longRuns.push_back({at, len});
        }
    }
    std::vector<int> sq, par;
    for (int i = 0; i < L.n; i++) {
        char c = L.t[i];
        if (c == '\\') { if (i + 1 < L.n && (L.t[i + 1] == '[' || L.t[i + 1] == '(')) g_scan.match[i + 1] = -1; i++; continue; }
        if (c == '`') {
            int j = i; while (j < L.n && L.t[j] == '`') j++;
            int cl = g_scan.codeClose[i];
            if (cl >= 0) { i = cl + (j - i) - 1; continue; }   // brackets inside stay -2
            i = j - 1; continue;
        }
        if (c == '[') { g_scan.match[i] = -1; sq.push_back(i); }
        else if (c == '(') { g_scan.match[i] = -1; par.push_back(i); }
        else if (c == ']' && !sq.empty()) { g_scan.match[sq.back()] = i; sq.pop_back(); }
        else if (c == ')' && !par.empty()) { g_scan.match[par.back()] = i; par.pop_back(); }
    }
}

// Is [p, i) a whole run of backticks (nothing before or after it)?
bool WholeRun(const Line& L, int p, int i) {
    if (p < 0 || (p > 0 && L.t[p - 1] == '`') || (i < L.n && L.t[i] == '`')) return false;
    for (int k = p; k < i; k++) if (L.t[k] != '`') return false;
    return true;
}

// Find the closing backtick run of exactly `run` length starting at i (i = just after the opening run).
int FindCodeClose(const Line& L, int i, int end, int run) {
    if (!WholeRun(L, i - run, i)) return FindCodeCloseScan(L, i, end, run);
    if (g_scan.line != &L) BuildScan(L);
    int cl = run <= 64 ? g_scan.codeClose[i - run] : FindCodeCloseScan(L, i, end, run);
    return cl >= 0 && cl + run <= end ? cl : -1;
}

int FindBracketCloseScan(const Line& L, int i, int end, char open, char close);

// Partner of the bracket at i - 1 (the opener), searching up to `end`.
int FindBracketClose(const Line& L, int i, int end, char open, char close) {
    if (i < 1 || L.t[i - 1] != open) return FindBracketCloseScan(L, i, end, open, close);
    if (g_scan.line != &L) BuildScan(L);
    int m = g_scan.match[i - 1];
    if (m == -2) return FindBracketCloseScan(L, i, end, open, close);   // the pass saw it in code: look closely
    return m >= 0 && m < end ? m : -1;
}

int FindBracketCloseScan(const Line& L, int i, int end, char open, char close) {
    int depth = 1;
    for (; i < end; i++) {
        char c = L.t[i];
        if (c == '\\') { i++; continue; }
        if (c == '`') {   // skip code spans inside link text
            int j = i; while (j < end && L.t[j] == '`') j++;
            int cl = FindCodeClose(L, j, end, j - i);
            if (cl >= 0) { i = cl + (j - i) - 1; continue; }
            i = j - 1; continue;
        }
        if (c == open) depth++;
        else if (c == close && --depth == 0) return i;
    }
    return -1;
}

// Find a closing emphasis delimiter run of `ch` with length `len` (or >= len when len==3).
int FindEmphClose(const Line& L, int i, int end, char ch, int len) {
    while (i < end) {
        char c = L.t[i];
        if (c == '\\') { i += 2; continue; }
        if (c == '`') {
            int j = i; while (j < end && L.t[j] == '`') j++;
            int cl = FindCodeClose(L, j, end, j - i);
            i = cl >= 0 ? cl + (j - i) : j;
            continue;
        }
        if (c == ch) {
            int j = i; while (j < end && L.t[j] == ch) j++;
            int run = j - i;
            bool rightFlank = i > 0 && !IsSpace(L.t[i - 1]);
            bool okAfter = ch != '_' || j >= L.n || !IsAlnum((unsigned char)L.t[j]);
            if (rightFlank && okAfter) {
                if (run == len || (len == 1 && run >= 3) || (len == 2 && run >= 3)) return i;
            }
            i = j;
            continue;
        }
        i++;
    }
    return -1;
}

bool StartsWithI(const char* t, int n, const char* pre) {
    int m = (int)strlen(pre);
    if (n < m) return false;
    for (int i = 0; i < m; i++) {
        char c = t[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != pre[i]) return false;
    }
    return true;
}

int& CommentState() { static int s; return s; }

void Inline(Line& L, int i, int end, int base, int flags);

int UrlEnd(const Line& L, int i, int end) {
    int j = i;
    while (j < end && !IsSpace(L.t[j]) && L.t[j] != '<' && L.t[j] != '>') j++;
    while (j > i && strchr(".,:;!?)'\"*_", L.t[j - 1])) j--;
    return j;
}

void Inline(Line& L, int i, int end, int base, int flags) {
    int cur = EmphStyle(flags, base);
    // A search for a closing *, _ or ~~ that failed from position p fails from any later position
    // too (same end): remember it, or every unmatched opener would scan the rest of the line again.
    int noClose[3][4] = {{INT_MAX, INT_MAX, INT_MAX, INT_MAX}, {INT_MAX, INT_MAX, INT_MAX, INT_MAX}, {INT_MAX, INT_MAX, INT_MAX, INT_MAX}};
    auto emphClose = [&](int from, char ch, int len) {
        int k = ch == '*' ? 0 : ch == '_' ? 1 : 2;
        if (from >= noClose[k][len]) return -1;
        int cl = FindEmphClose(L, from, end, ch, len);
        if (cl < 0) noClose[k][len] = from;
        return cl;
    };
    while (i < end) {
        char c = L.t[i];
        if (c == '\\' && i + 1 < end && IsPunct((unsigned char)L.t[i + 1])) {
            L.st[i] = S_MARK; L.st[i + 1] = (char)cur; i += 2; continue;
        }
        if (c == '`') {
            int j = i; while (j < end && L.t[j] == '`') j++;
            int run = j - i;
            int cl = FindCodeClose(L, j, end, run);
            if (cl >= 0) {
                Fill(L, i, j, S_MARK); Fill(L, j, cl, S_CODE); Fill(L, cl, cl + run, S_MARK);
                i = cl + run; continue;
            }
            Fill(L, i, j, (char)cur); i = j; continue;
        }
        if (c == '<') {
            if (i + 3 < end && !strncmp(L.t + i, "<!--", 4)) {
                const char* e = nullptr;
                for (int k = i + 4; k + 2 < L.n; k++) if (!strncmp(L.t + k, "-->", 3)) { e = L.t + k; break; }
                if (e) { int k = (int)(e - L.t) + 3; Fill(L, i, k, S_HTML); i = k; continue; }
                Fill(L, i, L.n, S_HTML); CommentState() = 1; return;
            }
            int j = i + 1;
            if (j < end && (StartsWithI(L.t + j, end - j, "http://") || StartsWithI(L.t + j, end - j, "https://") || StartsWithI(L.t + j, end - j, "mailto:"))) {
                int k = j; while (k < end && L.t[k] != '>' && !IsSpace(L.t[k])) k++;
                if (k < end && L.t[k] == '>') { L.st[i] = S_MARK; Fill(L, j, k, S_URL); L.st[k] = S_MARK; i = k + 1; continue; }
            }
            if (j < end && L.t[j] == '/') j++;
            if (j < end && ((L.t[j] >= 'a' && L.t[j] <= 'z') || (L.t[j] >= 'A' && L.t[j] <= 'Z'))) {
                int k = j; while (k < end && L.t[k] != '>' && L.t[k] != '<') k++;
                if (k < end && L.t[k] == '>') { Fill(L, i, k + 1, S_HTML); i = k + 1; continue; }
            }
            L.st[i] = (char)cur; i++; continue;
        }
        if (c == '[' || (c == '!' && i + 1 < end && L.t[i + 1] == '[')) {
            int ob = c == '!' ? i + 1 : i;
            int cb = FindBracketClose(L, ob + 1, end, '[', ']');
            if (cb > ob) {
                if (cb + 1 < end && L.t[cb + 1] == '(') {
                    int cp = FindBracketClose(L, cb + 2, end, '(', ')');
                    if (cp > cb) {
                        Fill(L, i, ob + 1, S_MARK);
                        Fill(L, ob + 1, cb, S_LINKTEXT);
                        Fill(L, cb, cb + 2, S_MARK);
                        Fill(L, cb + 2, cp, S_URL);
                        L.st[cp] = S_MARK;
                        i = cp + 1; continue;
                    }
                } else if (cb + 1 < end && L.t[cb + 1] == '[') {
                    int cr = FindBracketClose(L, cb + 2, end, '[', ']');
                    if (cr > cb) {
                        Fill(L, i, ob + 1, S_MARK); Fill(L, ob + 1, cb, S_LINKTEXT);
                        Fill(L, cb, cb + 2, S_MARK); Fill(L, cb + 2, cr, S_URL); L.st[cr] = S_MARK;
                        i = cr + 1; continue;
                    }
                }
            }
            L.st[i] = (char)cur; i++; continue;
        }
        if ((c == 'h' || c == 'w') && (i == 0 || !IsAlnum((unsigned char)L.t[i - 1])) &&
            (StartsWithI(L.t + i, end - i, "http://") || StartsWithI(L.t + i, end - i, "https://") || StartsWithI(L.t + i, end - i, "www."))) {
            int j = UrlEnd(L, i, end);
            if (j > i + 4) { Fill(L, i, j, S_URL); i = j; continue; }
        }
        if (c == '*' || c == '_' || c == '~') {
            int j = i; while (j < end && L.t[j] == c) j++;
            int run = j - i;
            bool leftFlank = j < end && !IsSpace(L.t[j]);
            bool okBefore = c != '_' || i == 0 || !IsAlnum((unsigned char)L.t[i - 1]);
            if (c == '~') {
                if (run == 2 && leftFlank) {
                    int cl = emphClose(j, '~', 2);
                    if (cl > j) {
                        Fill(L, i, j, S_MARK);
                        Fill(L, j, cl, S_STRIKE);
                        Inline(L, j, cl, S_STRIKE, flags | 4);
                        Fill(L, cl, cl + 2, S_MARK);
                        i = cl + 2; continue;
                    }
                }
                Fill(L, i, j, (char)cur); i = j; continue;
            }
            if (leftFlank && okBefore) {
                int len = run >= 3 ? 3 : run;
                int cl = emphClose(j, c, len);
                if (cl > j) {
                    int add = len == 3 ? 3 : (len == 2 ? 1 : 2);
                    int open = i + run - len;       // extra delimiters are literal
                    Fill(L, i, open, (char)cur);
                    Fill(L, open, j, S_MARK);
                    Inline(L, j, cl, base, flags | add);
                    Fill(L, cl, cl + len, S_MARK);
                    i = cl + len; continue;
                }
            }
            Fill(L, i, j, (char)cur); i = j; continue;
        }
        L.st[i] = (char)cur;
        i++;
    }
}

// Visual indentation (tabs to the next multiple of 4) of the text from `from`; `end` = first non-space.
int IndentAt(const Line& L, int from, int& end) {
    int col = 0, i = from;
    while (i < L.n && IsSpace(L.t[i])) { col = L.t[i] == '\t' ? (col / 4 + 1) * 4 : col + 1; i++; }
    end = i;
    return col;
}

// List item marker at t[0..n): "- ", "* ", "+ " or "1." / "1)" (followed by a space, a tab or the end
// of the line). Returns the marker length, 0 if there is none.
int ListMarker(const char* t, int n) {
    if (n > 1 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && IsSpace(t[1])) return 1;
    int d = 0; while (d < n && d < 9 && t[d] >= '0' && t[d] <= '9') d++;
    if (d > 0 && d < n && (t[d] == '.' || t[d] == ')') && (d + 1 == n || IsSpace(t[d + 1]))) return d + 1;
    return 0;
}

// A line that ends a paragraph even without a blank line before it (heading, quote, fence, rule, item).
bool InterruptsParagraph(const Line& L) {
    int e;
    if (IndentAt(L, 0, e) >= 4) return false;
    const char* t = L.t + e;
    int n = L.n - e;
    if (n == 0 || t[0] == '>') return true;
    if (t[0] == '#') { int h = 0; while (h < n && t[h] == '#') h++; if (h <= 6 && (h == n || IsSpace(t[h]))) return true; }
    if (n >= 3 && (!strncmp(t, "```", 3) || !strncmp(t, "~~~", 3))) return true;
    if (IsHr(L.t, L.n)) return true;
    return n > 1 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && IsSpace(t[1]);
}

// Opening code fence at t[0..n)? Returns its length (0 = none); `tilde` tells the kind.
int FenceOpen(const char* t, int n, bool& tilde) {
    if (n < 3 || (t[0] != '`' && t[0] != '~')) return 0;
    char fc = t[0];
    int j = 0; while (j < n && t[j] == fc) j++;
    if (j < 3 || (fc == '`' && memchr(t + j, '`', (size_t)(n - j)))) return 0;
    tilde = fc == '~';
    return j;
}

int FenceState(int keep, int run, bool tilde) {
    int ns = (keep & ~kFenceBits) | LS_FENCE | ((run > 63 ? 63 : run) << LS_FLEN_SHIFT);
    return tilde ? ns | LS_TILDE : ns;
}

// Styles one line. Returns the new line state.
int StyleLine(Line& L, int lineNo, int state, bool prevPara, const char* next, int nextLen) {
    memset(L.st, S_DEFAULT, (size_t)L.n);
    g_scan.line = nullptr;   // delimiter tables are per line
    int keep = state & (kFenceBits | LS_FRONT | LS_COMMENT | LS_ICOMMENT | kListBits);
    int listInd = (state & LS_INLIST) ? ((state >> LS_CIND_SHIFT) & 31) : -1;
    bool prevLPara = (state & LS_LPARA) != 0;
    bool blank = IsBlank(L.t, L.n);

    // YAML front matter
    if (state & LS_FRONT) {
        Fill(L, 0, L.n, S_FRONT);
        if (IsRunLine(L.t, L.n, '-', 3, false) || IsRunLine(L.t, L.n, '.', 3, false)) keep &= ~LS_FRONT;
        return keep;
    }
    if (lineNo == 0 && L.n >= 3 && !strncmp(L.t, "---", 3) && IsRunLine(L.t, L.n, '-', 3, false)) {
        Fill(L, 0, L.n, S_FRONT);
        return keep | LS_FRONT;
    }

    // Fenced code continuing from the previous line
    if (state & LS_FENCE) {
        bool ended = false;
        int p = 0;
        if (state & LS_QFENCE) {   // fence inside a quote: the line's quote markers come first
            int q = SkipSpaces(L, 0);
            bool any = false;
            while (q < L.n && L.t[q] == '>') { L.st[q] = S_MARK; any = true; q++; if (q < L.n && L.t[q] == ' ') q++; }
            if (!any && !blank) ended = true;   // the quote ended, and its code block with it
            else p = q;
        } else if (listInd >= 0 && !blank) {
            int e;
            if (IndentAt(L, 0, e) < listInd) ended = true;   // text left of the item: the item (and fence) ended
        }
        if (ended) {
            keep &= ~kFenceBits;
            memset(L.st, S_DEFAULT, (size_t)L.n);
        } else {
            char fc = (state & LS_TILDE) ? '~' : '`';
            int flen = (state >> LS_FLEN_SHIFT) & 63;
            int e;
            int ind = IndentAt(L, p, e);
            if (listInd >= 0 && !(state & LS_QFENCE)) ind -= listInd;
            if (ind < 4) {
                int j = e; while (j < L.n && L.t[j] == fc) j++;
                if (j - e >= flen && IsBlank(L.t + j, L.n - j)) {
                    Fill(L, p, L.n, S_FENCE);
                    return keep & ~kFenceBits;
                }
            }
            Fill(L, p, L.n, S_CODEBLOCK);
            return keep;
        }
    }

    // HTML comment continuing from the previous line
    int i = 0;
    if ((state & LS_ICOMMENT) && (blank || InterruptsParagraph(L))) keep &= ~LS_ICOMMENT;   // ended with its paragraph
    else if (state & (LS_COMMENT | LS_ICOMMENT)) {
        const char* e = nullptr;
        for (int k = 0; k + 2 < L.n; k++) if (!strncmp(L.t + k, "-->", 3)) { e = L.t + k; break; }
        if (!e) { Fill(L, 0, L.n, S_HTML); return keep; }
        i = (int)(e - L.t) + 3;
        Fill(L, 0, i, S_HTML);
        keep &= ~(LS_COMMENT | LS_ICOMMENT);
    }

    if (blank) return keep;   // lists (and block comments) go on across blank lines

    CommentState() = 0;
    int base = S_DEFAULT;
    bool para = false;

    // Where the line stands relative to the list item it may belong to
    int fs;
    int ind = IndentAt(L, i, fs);
    if (listInd >= 0 && ind < listInd && i == 0) {
        bool marker = ListMarker(L.t + fs, L.n - fs) > 0;
        bool lazy = prevLPara && !InterruptsParagraph(L);   // paragraph text going on without indentation
        if (!marker && !lazy) {
            keep &= ~kListBits;
            if (ind > 0) keep |= LS_INLIST | ((ind > 31 ? 31 : ind) << LS_CIND_SHIFT);   // text of an outer item
            listInd = ind > 0 ? ind : -1;
        }
    }
    bool inList = listInd >= 0;
    int rel = inList && ind >= listInd ? ind - listInd : ind;   // indentation within the item

    // Indented code block (4+ columns, not continuing a paragraph)
    if (i == 0 && rel >= 4 && !prevPara && !prevLPara) {
        Fill(L, 0, L.n, S_CODEBLOCK);
        return keep;
    }

    // Fenced code opening
    if (i == 0 && rel < 4) {
        bool tilde = false;
        int run = FenceOpen(L.t + fs, L.n - fs, tilde);
        if (run) {
            Fill(L, 0, L.n, S_FENCE);
            return FenceState(keep, run, tilde);
        }
    }

    // Block quotes (possibly nested)
    int q = SkipSpaces(L, i);
    bool inQuote = false;
    while (q < L.n && L.t[q] == '>' ) {
        L.st[q] = S_MARK; inQuote = true; q++;
        if (q < L.n && L.t[q] == ' ') q++;
        q = SkipSpaces(L, q);
    }
    if (inQuote) {
        base = S_QUOTE;
        i = q;
        bool tilde = false;
        int run = FenceOpen(L.t + q, L.n - q, tilde);   // > ```
        if (run) {
            Fill(L, q, L.n, S_FENCE);
            return FenceState(keep, run, tilde) | LS_QFENCE;
        }
    }

    int s = SkipSpaces(L, i);
    const char* t = L.t + s;
    int n = L.n - s;

    if (!inQuote && prevPara && IsSetextUnderline(L.t, L.n)) {
        Fill(L, 0, L.n, S_MARK);
        return keep;
    }

    // Setext heading text (next line is === or ---); not for list items or text inside them
    if (!inQuote && !inList && s - i < 4 && next && IsSetextUnderline(next, nextLen) && !IsBlank(L.t, L.n) &&
        !(n > 0 && (t[0] == '#' || t[0] == '>' || t[0] == '|')) && !IsHr(L.t, L.n) && !ListMarker(t, n)) {
        int lvl = IsRunLine(next, nextLen, '=', 1, false) ? S_H1 : S_H2;
        Fill(L, i, L.n, (char)lvl);
        return keep | LS_PARA;
    }

    // ATX heading
    if (n > 0 && t[0] == '#') {
        int h = 0; while (h < n && t[h] == '#') h++;
        if (h <= 6 && (h == n || IsSpace(t[h]))) {
            Fill(L, s, s + h, S_MARK);
            int e = L.n;
            while (e > s + h && IsSpace(L.t[e - 1])) e--;
            int te = e; while (te > s + h && L.t[te - 1] == '#') te--;
            if (te < e && (te == s + h || IsSpace(L.t[te - 1]))) { Fill(L, te, e, S_MARK); e = te; }
            Fill(L, s + h, e, (char)(S_H1 + h - 1));
            Inline(L, s + h, e, S_H1 + h - 1, 0);
            // keep heading colour on non-code inline text
            for (int k = s + h; k < e; k++)
                if (L.st[k] == S_DEFAULT || L.st[k] == S_BOLD || L.st[k] == S_ITALIC || L.st[k] == S_BOLDITALIC)
                    L.st[k] = (char)(S_H1 + h - 1);
            return keep;
        }
    }

    // Horizontal rule
    if (IsHr(L.t + i, L.n - i)) { Fill(L, i, L.n, S_HR); return keep; }

    // Link reference definition  [label]: url
    if (n > 3 && t[0] == '[') {
        int cb = FindBracketClose(L, s + 1, L.n, '[', ']');
        if (cb > s && cb + 1 < L.n && L.t[cb + 1] == ':') {
            L.st[s] = S_MARK; Fill(L, s + 1, cb, S_LINKTEXT); Fill(L, cb, cb + 2, S_MARK);
            Fill(L, cb + 2, L.n, S_URL);
            return keep;
        }
    }

    // Table rows / delimiter rows
    if (n > 0 && t[0] == '|') {
        bool delim = true;
        for (int k = 0; k < n; k++) if (!strchr("|:- \t", t[k])) { delim = false; break; }
        if (delim) { Fill(L, s, L.n, S_TABLE); return keep; }
        Inline(L, s, L.n, base, 0);
        for (int k = s; k < L.n; k++) if (L.t[k] == '|' && (k == 0 || L.t[k - 1] != '\\') && L.st[k] != S_CODE) L.st[k] = S_TABLE;
        return keep;
    }

    // List items (+ task boxes)
    int m = ListMarker(t, n);
    if (m) {
        Fill(L, s, s + m, S_LIST);
        int c = SkipSpaces(L, s + m);
        if (!inQuote) {
            // the item's content column: after the marker and 1-4 spaces (just 1 if more, or if empty)
            int e;
            int markerEnd = IndentAt(L, 0, e) + m, gap = 0;
            for (int k = s + m; k < c; k++) gap = L.t[k] == '\t' ? ((markerEnd + gap) / 4 + 1) * 4 - markerEnd : gap + 1;
            int col = markerEnd + (c >= L.n || gap < 1 || gap > 4 ? 1 : gap);
            keep = (keep & ~kListBits) | LS_INLIST | ((col > 31 ? 31 : col) << LS_CIND_SHIFT);
        }
        if (c + 2 < L.n && L.t[c] == '[' && (L.t[c + 1] == ' ' || L.t[c + 1] == 'x' || L.t[c + 1] == 'X') && L.t[c + 2] == ']') {
            Fill(L, c, c + 3, S_TASK);
            c += 3;
        }
        Inline(L, c, L.n, base, 0);
        return keep | (inQuote ? 0 : LS_LPARA);
    }

    // Plain paragraph text
    para = !inQuote && (s - i) < 4;
    Inline(L, i, L.n, base, 0);
    if (CommentState()) {
        // "<!--" opening the line starts a comment block (it goes on over blank lines);
        // one inside the text only lasts until the paragraph ends
        bool blockComment = !inQuote && rel < 4 && n >= 4 && !strncmp(t, "<!--", 4);
        keep |= blockComment ? LS_COMMENT : LS_ICOMMENT;
    }
    if (inList && !inQuote) return keep | LS_LPARA;
    return keep | (para ? LS_PARA : 0);
}

}

bool Md_IsCodeStyle(int st) { return st == S_CODE || st == S_CODEBLOCK || st == S_FENCE; }
bool Md_IsCodeBlockStyle(int st) { return st == S_CODEBLOCK || st == S_FENCE; }
int Md_InlineFlags(int st) {
    switch (st) {
    case S_BOLD: return 1;
    case S_ITALIC: return 2;
    case S_BOLDITALIC: return 3;
    case S_STRIKE: return 4;
    case S_CODE: return 8;
    case S_MARK: return -1;
    default: return 0;
    }
}
bool Md_IsSpellSkipStyle(int st) {
    return st == S_CODE || st == S_CODEBLOCK || st == S_FENCE || st == S_URL || st == S_HTML ||
           st == S_FRONT || st == S_MARK || st == S_TABLE || st == S_HR || st == S_TASK || st == S_LIST;
}

void Md_StyleNeeded(Sci& s, sptr_t endPos) {
    sptr_t startPos = s(SCI_GETENDSTYLED);
    sptr_t line = s(SCI_LINEFROMPOSITION, startPos);
    if (line > 0) line--;                       // re-evaluate previous line (setext lookahead)
    sptr_t lastLine = s(SCI_LINEFROMPOSITION, endPos);
    sptr_t lineCount = s(SCI_GETLINECOUNT);
    int state = line > 0 ? (int)s(SCI_GETLINESTATE, line - 1) : 0;
    bool prevPara = (state & LS_PARA) != 0;

    sptr_t rangeStart = s(SCI_POSITIONFROMLINE, line);
    sptr_t rangeEnd = s(SCI_POSITIONFROMLINE, lastLine + 2 < lineCount ? lastLine + 2 : lineCount);
    if (rangeEnd < 0 || rangeEnd > s.Len()) rangeEnd = s.Len();
    const char* text = (const char*)s(SCI_GETRANGEPOINTER, rangeStart, rangeEnd - rangeStart);
    if (!text && rangeEnd > rangeStart) return;

    std::string styles;
    std::vector<std::pair<sptr_t, sptr_t>> strikes;
    s(SCI_SETINDICATORCURRENT, IND_STRIKE);
    for (sptr_t l = line; l <= lastLine && l < lineCount; l++) {
        sptr_t ls = s(SCI_POSITIONFROMLINE, l);
        sptr_t le = s(SCI_GETLINEENDPOSITION, l);
        sptr_t lnext = s(SCI_POSITIONFROMLINE, l + 1);
        if (lnext < le) lnext = s.Len();
        const char* lt = text + (ls - rangeStart);
        int n = (int)(le - ls);
        const char* next = nullptr;
        int nextLen = 0;
        if (l + 1 < lineCount) {
            sptr_t ns = s(SCI_POSITIONFROMLINE, l + 1), ne = s(SCI_GETLINEENDPOSITION, l + 1);
            if (ne <= rangeEnd) { next = text + (ns - rangeStart); nextLen = (int)(ne - ns); }
            else {
                // next line outside the pointer range: fetch it separately
                next = (const char*)s(SCI_GETRANGEPOINTER, ns, ne - ns);
                nextLen = (int)(ne - ns);
                text = (const char*)s(SCI_GETRANGEPOINTER, rangeStart, rangeEnd - rangeStart);
                lt = text + (ls - rangeStart);
            }
        }
        styles.assign((size_t)(lnext - ls), (char)S_DEFAULT);
        Line L{lt, n, &styles[0]};
        state = StyleLine(L, (int)l, state, prevPara, next, nextLen);
        prevPara = (state & LS_PARA) != 0;
        if (lnext - ls > n) {
            char eol = n > 0 ? styles[n - 1] : (char)((state & LS_FENCE) ? S_CODEBLOCK : (state & LS_FRONT) ? S_FRONT : S_DEFAULT);
            if (eol != S_CODEBLOCK && eol != S_FENCE && eol != S_FRONT) eol = S_DEFAULT;
            memset(&styles[n], eol, (size_t)(lnext - ls - n));
        }
        s(SCI_STARTSTYLING, ls);
        s(SCI_SETSTYLINGEX, styles.size(), (sptr_t)styles.data());
        // strikethrough indicator
        s(SCI_INDICATORCLEARRANGE, ls, lnext - ls);
        for (int k = 0; k < n;) {
            if (styles[k] == S_STRIKE) {
                int e = k; while (e < n && styles[e] == S_STRIKE) e++;
                s(SCI_INDICATORFILLRANGE, ls + k, e - k);
                k = e;
            } else k++;
        }
        s(SCI_SETLINESTATE, l, state);
    }
}

void Md_ConfigureView(Sci& s, bool mainEditor) {
    const Palette& p = Pal();
    std::string font = WToU8(g_set.font), code = WToU8(g_set.codeFont);
    int size = g_set.fontSize * 100;

    s(SCI_STYLESETFONT, STYLE_DEFAULT, (sptr_t)font.c_str());
    s(SCI_STYLESETSIZEFRACTIONAL, STYLE_DEFAULT, size);
    s(SCI_STYLESETFORE, STYLE_DEFAULT, p.fg);
    s(SCI_STYLESETBACK, STYLE_DEFAULT, p.bg);
    s(SCI_STYLECLEARALL);

    auto st = [&](int id, COLORREF fg, int weight = 400, bool italic = false, int sz = 0, bool mono = false) {
        s(SCI_STYLESETFORE, id, fg);
        s(SCI_STYLESETWEIGHT, id, weight);
        s(SCI_STYLESETITALIC, id, italic);
        if (sz) s(SCI_STYLESETSIZEFRACTIONAL, id, sz);
        if (mono) s(SCI_STYLESETFONT, id, (sptr_t)code.c_str());
    };
    st(S_H1, p.heading, 700, false, size * 124 / 100);
    st(S_H2, p.heading, 700, false, size * 112 / 100);
    st(S_H3, p.heading, 700, false, size * 104 / 100);
    st(S_H4, p.heading, 700);
    st(S_H5, p.heading, 700);
    st(S_H6, p.dim, 700);
    st(S_MARK, p.mark);
    st(S_BOLD, p.fg, 700);
    st(S_ITALIC, p.fg, 400, true);
    st(S_BOLDITALIC, p.fg, 700, true);
    st(S_CODE, p.codeFg, 400, false, size * 94 / 100, true);
    s(SCI_STYLESETBACK, S_CODE, p.codeBg);
    st(S_CODEBLOCK, p.codeFg, 400, false, size * 94 / 100, true);
    s(SCI_STYLESETBACK, S_CODEBLOCK, p.codeBg);
    s(SCI_STYLESETEOLFILLED, S_CODEBLOCK, 1);
    st(S_FENCE, p.dim, 400, false, size * 94 / 100, true);
    s(SCI_STYLESETBACK, S_FENCE, p.codeBg);
    s(SCI_STYLESETEOLFILLED, S_FENCE, 1);
    st(S_LINKTEXT, p.link);
    s(SCI_STYLESETUNDERLINE, S_LINKTEXT, 1);
    st(S_URL, p.dim);
    st(S_QUOTE, p.quote, 400, true);
    st(S_LIST, p.listMark, 700);
    st(S_HR, p.dim);
    st(S_HTML, p.html, 400, false, size * 94 / 100, true);
    st(S_STRIKE, p.dim);
    st(S_TABLE, p.mark);
    st(S_FRONT, p.dim, 400, false, size * 94 / 100, true);
    st(S_TASK, p.listMark, 700, false, 0, true);

    s(SCI_STYLESETFORE, STYLE_LINENUMBER, p.dim);
    s(SCI_STYLESETBACK, STYLE_LINENUMBER, p.bg);
    s(SCI_SETMARGINBACKN, 0, p.bg);

    // Caret / selection / caret line (colours are 0xAABBGGRR)
    auto rgba = [](COLORREF c, int a) { return (sptr_t)(c | ((unsigned)a << 24)); };
    s(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET, rgba(p.fg, 0xFF));
    s(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_BACK, rgba(p.selBg, 0xFF));
    s(SCI_SETELEMENTCOLOUR, SC_ELEMENT_SELECTION_INACTIVE_BACK, rgba(p.selBg, 0x90));
    s(SCI_SETSELECTIONLAYER, SC_LAYER_UNDER_TEXT);
    s(SCI_SETELEMENTCOLOUR, SC_ELEMENT_CARET_LINE_BACK, rgba(p.caretLine, 0xFF));
    // The current-line highlight must be part of the line background (base layer). On the
    // under-text layer Scintilla paints it after the selection, as an opaque fill that hides any
    // selection on the caret's own line (so a drag within one line looked like it selected nothing).
    s(SCI_SETCARETLINELAYER, SC_LAYER_BASE);
    s(SCI_SETCARETLINEVISIBLEALWAYS, 0);
    s(SCI_SETCARETWIDTH, 2);
    s(SCI_SETELEMENTCOLOUR, SC_ELEMENT_WHITE_SPACE, rgba(p.mark, 0xFF));

    // Indicators
    s(SCI_INDICSETSTYLE, IND_SPELL, INDIC_SQUIGGLEPIXMAP);
    s(SCI_INDICSETFORE, IND_SPELL, p.spell);
    s(SCI_INDICSETUNDER, IND_SPELL, 0);
    s(SCI_INDICSETSTYLE, IND_AISEL, INDIC_FULLBOX);
    s(SCI_INDICSETFORE, IND_AISEL, p.aiSel);
    s(SCI_INDICSETALPHA, IND_AISEL, 48);
    s(SCI_INDICSETOUTLINEALPHA, IND_AISEL, 120);
    s(SCI_INDICSETUNDER, IND_AISEL, 1);
    s(SCI_INDICSETSTYLE, IND_FIND, INDIC_ROUNDBOX);
    s(SCI_INDICSETFORE, IND_FIND, p.findHit);
    s(SCI_INDICSETALPHA, IND_FIND, Theme_IsDark() ? 110 : 120);
    s(SCI_INDICSETOUTLINEALPHA, IND_FIND, 200);
    s(SCI_INDICSETUNDER, IND_FIND, 1);
    s(SCI_INDICSETSTYLE, IND_STRIKE, INDIC_STRIKE);
    s(SCI_INDICSETFORE, IND_STRIKE, p.dim);

    s(SCI_SETCODEPAGE, SC_CP_UTF8);
    s(SCI_SETTECHNOLOGY, SC_TECHNOLOGY_DIRECTWRITE);
    s(SCI_SETFONTQUALITY, SC_EFF_QUALITY_LCD_OPTIMIZED);
    s(SCI_SETBUFFEREDDRAW, 0);
    s(SCI_SETIMEINTERACTION, SC_IME_INLINE);
    s(SCI_USEPOPUP, SC_POPUP_NEVER);
    s(SCI_SETWRAPMODE, (mainEditor ? g_set.wrap : true) ? SC_WRAP_WORD : SC_WRAP_NONE);
    s(SCI_SETWRAPINDENTMODE, SC_WRAPINDENT_SAME);
    s(SCI_SETHSCROLLBAR, (mainEditor ? !g_set.wrap : false));
    s(SCI_SETSCROLLWIDTHTRACKING, 1);
    s(SCI_SETSCROLLWIDTH, 1);
    s(SCI_SETMARGINWIDTHN, 1, 0);
    s(SCI_SETMARGINWIDTHN, 2, 0);
    s(SCI_SETEXTRAASCENT, 1);
    s(SCI_SETEXTRADESCENT, 1);
    s(SCI_SETTABWIDTH, 4);
    s(SCI_SETUSETABS, 0);
    s(SCI_SETMULTIPLESELECTION, 1);
    s(SCI_SETADDITIONALSELECTIONTYPING, 1);
    s(SCI_SETMOUSESELECTIONRECTANGULARSWITCH, 1);
    s(SCI_SETLAYOUTTHREADS, 4);
    s(SCI_SETPASTECONVERTENDINGS, 1);
    if (mainEditor) {
        s(SCI_SETENDATLASTLINE, 0);
        s(SCI_SETZOOM, g_set.zoom);
    }
    Theme_Control(s.hwnd);
}
