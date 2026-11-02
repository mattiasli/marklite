// Markdown formatting commands. See format.h.
#include "format.h"
#include "resource.h"
#include <commdlg.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>

namespace {

// ------------------------------------------------------------------ basics
std::string Eol(Sci& s) {
    int m = (int)s(SCI_GETEOLMODE);
    return m == SC_EOL_CRLF ? "\r\n" : m == SC_EOL_CR ? "\r" : "\n";
}

struct UndoGroup {
    Sci& s;
    explicit UndoGroup(Sci& x) : s(x) { s(SCI_BEGINUNDOACTION); }
    ~UndoGroup() { s(SCI_ENDUNDOACTION); }
};

void Replace(Sci& s, sptr_t a, sptr_t b, const std::string& t) {
    s(SCI_SETTARGETRANGE, a, b);
    s(SCI_REPLACETARGET, (uptr_t)t.size(), (sptr_t)t.data());
}

bool IsBlank(const std::string& t) {
    for (char c : t)
        if (c != ' ' && c != '\t') return false;
    return true;
}
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool IsAlnum(char c) { return (unsigned char)c >= 0x80 || isalnum((unsigned char)c); }

std::string TrimA(const std::string& t) {
    size_t a = 0, b = t.size();
    while (a < b && IsSpace(t[a])) a++;
    while (b > a && IsSpace(t[b - 1])) b--;
    return t.substr(a, b - a);
}

sptr_t LineStart(Sci& s, sptr_t l) { return s(SCI_POSITIONFROMLINE, l); }
sptr_t LineEnd(Sci& s, sptr_t l) { return s(SCI_GETLINEENDPOSITION, l); }
sptr_t LineCount(Sci& s) { return s(SCI_GETLINECOUNT); }
sptr_t LineOf(Sci& s, sptr_t pos) { return s(SCI_LINEFROMPOSITION, pos); }

// Lines touched by the main selection (a selection that ends at column 0 doesn't include that line).
void SelLines(Sci& s, sptr_t& l0, sptr_t& l1) {
    sptr_t a = s(SCI_GETSELECTIONSTART), b = s(SCI_GETSELECTIONEND);
    l0 = LineOf(s, a);
    l1 = LineOf(s, b);
    if (l1 > l0 && b == LineStart(s, l1)) l1--;
}

// ------------------------------------------------------------------ line prefixes
// A line split into: indent, blockquote markers, indent inside the quote, list marker, task box,
// heading marker, rest.
struct Parts { std::string indent, quote, inner, list, task, heading, rest; };

Parts Split(const std::string& t) {
    Parts p;
    size_t i = 0, n = t.size();
    while (i < n && (t[i] == ' ' || t[i] == '\t')) i++;
    p.indent = t.substr(0, i);
    size_t q = i;
    while (q < n && t[q] == '>') {   // each marker with the one space after it ("> > x")
        q++;
        if (q < n && t[q] == ' ') q++;
        size_t sp = q;
        while (sp < n && (t[sp] == ' ' || t[sp] == '\t')) sp++;
        if (sp < n && t[sp] == '>') q = sp;
        else break;
    }
    p.quote = t.substr(i, q - i);
    size_t l = q;
    while (l < n && (t[l] == ' ' || t[l] == '\t')) l++;   // "  " in ">   - item": nesting inside the quote
    p.inner = t.substr(q, l - q);
    if (l < n && (t[l] == '-' || t[l] == '*' || t[l] == '+') && (l + 1 == n || t[l + 1] == ' ' || t[l + 1] == '\t')) {
        size_t e = l + 1;
        while (e < n && (t[e] == ' ' || t[e] == '\t')) e++;
        p.list = t.substr(l, e - l);
        l = e;
    } else {
        size_t d = l;
        while (d < n && IsDigit(t[d]) && d - l < 9) d++;
        if (d > l && d < n && (t[d] == '.' || t[d] == ')') && (d + 1 == n || t[d + 1] == ' ' || t[d + 1] == '\t')) {
            size_t e = d + 1;
            while (e < n && (t[e] == ' ' || t[e] == '\t')) e++;
            p.list = t.substr(l, e - l);
            l = e;
        }
    }
    if (!p.list.empty() && n - l >= 3 && t[l] == '[' && (t[l + 1] == ' ' || t[l + 1] == 'x' || t[l + 1] == 'X') && t[l + 2] == ']' &&
        (l + 3 == n || t[l + 3] == ' ')) {
        size_t e = std::min(n, l + 4);
        p.task = t.substr(l, e - l);
        l = e;
    }
    size_t h = l;
    while (h < n && t[h] == '#' && h - l < 7) h++;
    if (h > l && h - l <= 6 && (h == n || t[h] == ' ' || t[h] == '\t')) {
        size_t e = h;
        while (e < n && (t[e] == ' ' || t[e] == '\t')) e++;
        p.heading = t.substr(l, e - l);
        l = e;
    }
    p.rest = t.substr(l);
    return p;
}
std::string Join(const Parts& p) { return p.indent + p.quote + p.inner + p.list + p.task + p.heading + p.rest; }
// The indentation that nests list items: inside a quote, the spaces after its marker.
std::string& ListIndent(Parts& p) { return p.quote.empty() ? p.indent : p.inner; }
int QuoteDepth(const Parts& p) { return (int)std::count(p.quote.begin(), p.quote.end(), '>'); }
int HeadingLevel(const Parts& p) { return (int)std::count(p.heading.begin(), p.heading.end(), '#'); }
bool IsBullet(const Parts& p) { return !p.list.empty() && (p.list[0] == '-' || p.list[0] == '*' || p.list[0] == '+'); }
bool IsNumbered(const Parts& p) { return !p.list.empty() && IsDigit(p.list[0]); }
int IndentWidth(const std::string& ind) {
    int w = 0;
    for (char c : ind) w = c == '\t' ? (w / 4 + 1) * 4 : w + 1;
    return w;
}

// Rewrites lines l0..l1 through fn(parts, originalText, line) and keeps the selection on the same text.
template <class F> void EditLines(Sci& s, sptr_t l0, sptr_t l1, F fn) {
    sptr_t anchor = s(SCI_GETANCHOR), caret = s(SCI_GETCURRENTPOS);
    bool empty = anchor == caret;
    sptr_t selMin = std::min(anchor, caret), rangeEnd = LineEnd(s, l1);
    sptr_t na = anchor, nc = caret, delta = 0;
    bool ma = false, mc = false;
    for (sptr_t ln = l0; ln <= l1; ln++) {
        sptr_t ls = LineStart(s, ln), le = LineEnd(s, ln);
        std::string t = s.Range(ls, le);
        Parts p = Split(t);
        size_t oldPre = t.size() - p.rest.size();
        fn(p, t, ln);
        std::string nt = Join(p);
        size_t newPre = nt.size() - p.rest.size();
        sptr_t origLs = ls - delta, oldLen = le - ls;
        auto map = [&](sptr_t pos, sptr_t& out, bool& done) {
            if (done || pos < origLs || pos > origLs + oldLen) return;
            sptr_t col = pos - origLs;
            if (!empty && pos == selMin && col == 0) out = ls;   // a selection from the line start keeps the prefix
            else if (col >= (sptr_t)oldPre) out = ls + col - (sptr_t)oldPre + (sptr_t)newPre;
            else out = ls + (sptr_t)newPre;
            done = true;
        };
        map(anchor, na, ma);
        map(caret, nc, mc);
        if (nt != t) {
            Replace(s, ls, le, nt);
            delta += (sptr_t)nt.size() - oldLen;
        }
    }
    if (!ma) na = anchor > rangeEnd ? anchor + delta : anchor;
    if (!mc) nc = caret > rangeEnd ? caret + delta : caret;
    s(SCI_SETSEL, na, nc);
}

// Inserts a block element on its own line(s), separated from surrounding text by blank lines.
// caretOff: caret offset inside `block` (-1 = on the line after the block); selLen: characters to select there.
void InsertBlock(Sci& s, const std::string& block, sptr_t caretOff, sptr_t selLen = 0) {
    std::string eol = Eol(s);
    sptr_t first, line;
    SelLines(s, first, line);   // a selection ending at column 0 doesn't include that line
    sptr_t ls = LineStart(s, line), le = LineEnd(s, line);
    bool blank = IsBlank(s.Range(ls, le));
    std::string pre, post;
    sptr_t at = le;
    if (blank) {
        at = ls;
        if (line > 0 && !IsBlank(s.LineText(line - 1))) pre = eol;
    } else {
        pre = eol + eol;
    }
    bool last = line + 1 >= LineCount(s);
    if (last || !IsBlank(s.LineText(line + 1))) post = eol;
    Replace(s, at, le, pre + block + post);
    sptr_t base = at + (sptr_t)pre.size();
    if (caretOff < 0) {
        sptr_t p = base + (sptr_t)(block.size() + post.size() + (post.empty() ? eol.size() : 0));
        s(SCI_SETSEL, p, p);
    } else {
        s(SCI_SETSEL, base + caretOff, base + caretOff + selLen);
    }
}

// ------------------------------------------------------------------ inline formatting
// Shrinks [a, b) so it doesn't start or end with whitespace (emphasis markers must touch text).
void TrimRange(Sci& s, sptr_t& a, sptr_t& b) {
    while (a < b && IsSpace((char)s(SCI_GETCHARAT, a))) a++;
    while (b > a && IsSpace((char)s(SCI_GETCHARAT, b - 1))) b--;
}

// Commands that type at the caret work on the main selection only: SCI_REPLACESEL with several
// selections empties all of them but inserts at one.
void SingleSelection(Sci& s) {
    if (s(SCI_GETSELECTIONS) <= 1) return;
    int m = (int)s(SCI_GETMAINSELECTION);
    sptr_t c = s(SCI_GETSELECTIONNCARET, m), an = s(SCI_GETSELECTIONNANCHOR, m);
    s(SCI_SETSELECTION, c, an);
}

// Pairs of emphasis delimiters `m` in t as (opener start, closer start): each opener with the first
// closer after it.
std::vector<std::pair<size_t, size_t>> Pairs(const std::string& t, const std::string& m) {
    size_t n = t.size(), k = m.size();
    bool single = k == 1, under = m[0] == '_';
    auto openAt = [&](size_t i) {
        if (t.compare(i, k, m) != 0 || i + k >= n || IsSpace(t[i + k])) return false;
        if (i > 0 && t[i - 1] == '\\') return false;
        if (single && m[0] != '`' && (t[i + 1] == m[0] || (i > 0 && t[i - 1] == m[0]))) return false;
        if (under && i > 0 && IsAlnum(t[i - 1])) return false;
        return true;
    };
    auto closeAt = [&](size_t j) {
        if (t.compare(j, k, m) != 0 || IsSpace(t[j - 1]) || t[j - 1] == '\\') return false;
        if (single && m[0] != '`' && ((j + 1 < n && t[j + 1] == m[0]) || t[j - 1] == m[0])) return false;
        if (under && j + k < n && IsAlnum(t[j + k])) return false;
        return true;
    };
    std::vector<std::pair<size_t, size_t>> out;
    size_t i = 0;
    while (i < n) {
        if (openAt(i)) {
            size_t j = i + k + 1;
            while (j + k <= n && !closeAt(j)) j++;
            if (j + k > n) break;   // no closer after this opener, so none after a later one either
            out.push_back({i, j});
            i = j + k;
            continue;
        }
        i++;
    }
    return out;
}

// t is a single code span: a backtick run, text without a run of that length, the same run.
bool IsCodeSpan(const std::string& t) {
    size_t r = 0, e = 0, n = t.size();
    while (r < n && t[r] == '`') r++;
    while (e < n && t[n - 1 - e] == '`') e++;
    if (r == 0 || r != e || n < 2 * r + 1) return false;
    for (size_t i = r; i < n - r;) {
        if (t[i] != '`') { i++; continue; }
        size_t j = i;
        while (j < n - r && t[j] == '`') j++;
        if (j - i == r) return false;
        i = j;
    }
    return true;
}

// The part of line `ln` inside [a, b) that inline formatting may wrap: after the line's block prefix
// (indent, > , list marker, task box, # ) and before a heading's closing #s, without outer spaces.
bool LineContent(Sci& s, sptr_t ln, sptr_t a, sptr_t b, sptr_t& ca, sptr_t& cb) {
    sptr_t ls = LineStart(s, ln), le = LineEnd(s, ln);
    std::string t = s.Range(ls, le);
    Parts p = Split(t);
    size_t pre = t.size() - p.rest.size();
    sptr_t post = le;
    if (!p.heading.empty()) {   // "## Title ##"
        size_t e = t.find_last_not_of(" \t");
        if (e != std::string::npos && e >= pre && t[e] == '#') {
            size_t h = e;
            while (h > pre && t[h - 1] == '#') h--;
            if (h == pre || t[h - 1] == ' ' || t[h - 1] == '\t') post = ls + (sptr_t)h;
        }
    }
    ca = std::max(a, ls + (sptr_t)pre);
    cb = std::min(b, post);
    if (ca >= cb) return false;
    TrimRange(s, ca, cb);
    return ca < cb;
}

// Toggles inline formatting over [a, b), line by line (markers can't take in a line's block prefix,
// and a pair can't span paragraphs). Formatted lines are unformatted when all of them are;
// otherwise the rest get formatted too. Ops: adjust(x, y) - a line's part of a selection that starts or
// ends between a marker and its text ("one**" of "**one**") moves past that marker;
// wrapped(x, y) - its markers are right outside [x, y);
// outer(t) - t is exactly one formatted span; unwrap(x, y) - removes the markers outside; strip(t) -
// t without its outer markers; plain(t) - t without any of these markers; wrap(t, openLen).
template <class Ops> void ToggleRanges(Sci& s, sptr_t a, sptr_t b, Ops& ops) {
    std::vector<std::pair<sptr_t, sptr_t>> rs;
    for (sptr_t ln = LineOf(s, a), l1 = LineOf(s, b); ln <= l1; ln++) {
        sptr_t ca, cb;
        if (LineContent(s, ln, a, b, ca, cb)) {
            ops.adjust(ca, cb);
            rs.push_back({ca, cb});
        }
    }
    if (rs.empty()) return;
    auto formatted = [&](sptr_t x, sptr_t y) { return ops.wrapped(x, y) || ops.outer(s.Range(x, y)); };
    bool all = true;
    for (auto& r : rs)
        if (!formatted(r.first, r.second)) { all = false; break; }
    sptr_t selA = 0, selB = 0, shift = 0;
    for (size_t k = rs.size(); k-- > 0;) {   // from the last line up: earlier positions stay valid
        sptr_t x = rs[k].first, y = rs[k].second, nx = x, ny = y, before = s.Len();
        if (all) {
            if (ops.wrapped(x, y)) {
                sptr_t o = ops.unwrap(x, y);
                nx = x - o;
                ny = y - o;
            } else {
                std::string in = ops.strip(s.Range(x, y));
                Replace(s, x, y, in);
                ny = x + (sptr_t)in.size();
            }
        } else if (!formatted(x, y)) {
            std::string in = ops.plain(s.Range(x, y));
            sptr_t o = 0;
            std::string w = ops.wrap(in, o);
            Replace(s, x, y, w);
            nx = x + o;
            ny = nx + (sptr_t)in.size();
        }
        if (k + 1 == rs.size()) selB = ny;
        else shift += s.Len() - before;
        if (k == 0) selA = nx;
    }
    s(SCI_SETSEL, selA, selB + shift);
}

// The caret's word inside a longer span of `mark` ("**hello wo|rld**"): takes just that word out of
// the span ("**hello** world") instead of nesting another pair in it.
bool UnwrapWordFromSpan(Sci& s, const std::string& mark, sptr_t a, sptr_t b) {
    sptr_t ln = LineOf(s, a), ls = LineStart(s, ln);
    if (LineOf(s, b) != ln) return false;
    sptr_t n = (sptr_t)mark.size();
    for (auto& pr : Pairs(s.LineText(ln), mark)) {
        sptr_t o = ls + (sptr_t)pr.first, c = ls + (sptr_t)pr.second;   // opener and closer starts
        if (!(o + n <= a && b <= c)) continue;
        std::string left = s.Range(o + n, a), word = s.Range(a, b), right = s.Range(b, c);
        size_t le = left.find_last_not_of(" \t");   // the part before keeps its format, closed before the spaces
        std::string L = le == std::string::npos ? left : mark + left.substr(0, le + 1) + mark + left.substr(le + 1);
        size_t rb = right.find_first_not_of(" \t");
        std::string R = rb == std::string::npos ? right : right.substr(0, rb) + mark + right.substr(rb) + mark;
        Replace(s, o, c + n, L + word + R);
        sptr_t na = o + (sptr_t)L.size();
        s(SCI_SETSEL, na, na + (sptr_t)word.size());
        return true;
    }
    return false;
}

std::string StripPairs(const std::string& t, const std::string& m);

// The word at the caret when nothing is selected; false if the caret isn't on a word.
bool WordOrSelection(Sci& s, sptr_t& a, sptr_t& b) {
    a = s(SCI_GETSELECTIONSTART);
    b = s(SCI_GETSELECTIONEND);
    if (a == b) {
        sptr_t wa = s(SCI_WORDSTARTPOSITION, a, 1), wb = s(SCI_WORDENDPOSITION, a, 1);
        if (wa >= wb) return false;
        a = wa;
        b = wb;
    }
    TrimRange(s, a, b);
    return a < b;
}

// Toggles a symmetric Markdown marker (**, *, ~~, `, $) around the selection or the word at the caret.
void ToggleWrap(Sci& s, const std::string& mark) {
    if (s(SCI_GETSELECTIONS) > 1) return;
    sptr_t n = (sptr_t)mark.size(), a, b;
    UndoGroup u(s);
    auto run = [&](sptr_t p, int dir) {   // marker characters directly outside the range
        int k = 0;
        sptr_t len = s.Len();
        while (k < 4) {
            sptr_t q = dir < 0 ? p - k - 1 : p + k;
            if (q < 0 || q >= len || (char)s(SCI_GETCHARAT, q) != mark[0]) break;
            k++;
        }
        return k;
    };
    auto isWrapped = [&](sptr_t x, sptr_t y) {
        int before = run(x, -1), after = run(y, 1);
        return mark == "*" ? (before % 2 == 1 && after % 2 == 1) : (before >= n && after >= n);
    };
    bool caretOnly = s(SCI_GETSELECTIONEMPTY) != 0;
    if (!WordOrSelection(s, a, b)) {   // no word here (or only spaces selected)
        sptr_t c = s(SCI_GETCURRENTPOS);
        // caret inside an empty pair (**|**): pressing again removes it - but not between two
        // formatted words (**a**|**b**), which would join them
        // (word characters as Scintilla sees them: emoji and punctuation aren't, letters in any script are)
        bool between = c - n > 0 && c + n < s.Len() && s(SCI_WORDSTARTPOSITION, c - n, 1) < c - n &&
                       s(SCI_WORDENDPOSITION, c + n, 1) > c + n;
        if (isWrapped(c, c) && !between) {
            s(SCI_DELETERANGE, c, n);
            s(SCI_DELETERANGE, c - n, n);
            s(SCI_SETSEL, c - n, c - n);
            return;
        }
        std::string t = mark + mark;    // insert an empty pair at the caret, keeping any selected spaces
        s(SCI_INSERTTEXT, c, (sptr_t)t.c_str());
        s(SCI_SETSEL, c + n, c + n);
        return;
    }
    if (caretOnly && !isWrapped(a, b) && UnwrapWordFromSpan(s, mark, a, b)) return;
    bool code = mark == "`";
    struct Ops {
        Sci& s;
        const std::string& mark;
        sptr_t n;
        bool code;
        std::function<bool(sptr_t, sptr_t)> isWrapped;
        sptr_t spaced(sptr_t x, sptr_t y) {   // "`` a`b ``": the backtick run around the spaces, or 0
            if (!code || x < 2 || (char)s(SCI_GETCHARAT, x - 1) != ' ' || (char)s(SCI_GETCHARAT, y) != ' ') return 0;
            sptr_t r1 = 0, r2 = 0, len = s.Len();
            while (x - 2 - r1 >= 0 && (char)s(SCI_GETCHARAT, x - 2 - r1) == '`') r1++;
            while (y + 1 + r2 < len && (char)s(SCI_GETCHARAT, y + 1 + r2) == '`') r2++;
            return r1 && r1 == r2 ? r1 : 0;
        }
        bool wrapped(sptr_t x, sptr_t y) { return isWrapped(x, y) || spaced(x, y); }
        void adjust(sptr_t& x, sptr_t& y) {
            std::string t = s.Range(x, y);
            size_t k = mark.size();
            bool st = t.compare(0, k, mark) == 0, en = t.size() >= k && t.compare(t.size() - k, k, mark) == 0;
            if (en && !st && y - n > x && isWrapped(x, y - n)) y -= n;
            else if (st && !en && x + n < y && isWrapped(x + n, y)) x += n;
        }
        bool outer(const std::string& t) {
            if (code) return IsCodeSpan(t);
            auto ps = Pairs(t, mark);
            return !ps.empty() && ps[0].first == 0 && ps[0].second + mark.size() == t.size();
        }
        sptr_t unwrap(sptr_t x, sptr_t y) {
            sptr_t m = isWrapped(x, y) ? n : spaced(x, y) + 1;
            s(SCI_DELETERANGE, y, m);
            s(SCI_DELETERANGE, x - m, m);
            return m;
        }
        std::string strip(const std::string& t) {
            if (!code) return t.substr(mark.size(), t.size() - 2 * mark.size());
            size_t r = 0;
            while (r < t.size() && t[r] == '`') r++;
            std::string in = t.substr(r, t.size() - 2 * r);
            if (in.size() >= 2 && in.front() == ' ' && in.back() == ' ' && in.find_first_not_of(' ') != std::string::npos)
                in = in.substr(1, in.size() - 2);
            return in;
        }
        std::string plain(const std::string& t) { return code ? t : StripPairs(t, mark); }
        std::string wrap(const std::string& t, sptr_t& open) {
            if (code) {   // a code span around backticks uses a longer run (and spaces)
                size_t longest = 0, cur = 0;
                for (char c : t) { cur = c == '`' ? cur + 1 : 0; longest = std::max(longest, cur); }
                if (longest) {
                    std::string f(longest + 1, '`');
                    open = (sptr_t)f.size() + 1;
                    return f + " " + t + " " + f;
                }
            }
            open = n;
            return mark + t + mark;
        }
    } ops{s, mark, n, code, isWrapped};
    ToggleRanges(s, a, b, ops);
}

// Toggles an HTML element (<u>, <kbd>) around the selection or the word at the caret.
void ToggleTags(Sci& s, const std::string& open, const std::string& close) {
    if (s(SCI_GETSELECTIONS) > 1) return;
    sptr_t a, b, no = (sptr_t)open.size(), nc = (sptr_t)close.size();
    UndoGroup u(s);
    if (!WordOrSelection(s, a, b)) {   // no word here (or only spaces selected)
        sptr_t c = s(SCI_GETCURRENTPOS);
        if (c >= no && s.Range(c - no, c) == open && s.Range(c, c + nc) == close) {   // <u>|</u>: remove
            s(SCI_DELETERANGE, c, nc);
            s(SCI_DELETERANGE, c - no, no);
            s(SCI_SETSEL, c - no, c - no);
            return;
        }
        std::string t = open + close;
        s(SCI_INSERTTEXT, c, (sptr_t)t.c_str());
        s(SCI_SETSEL, c + no, c + no);
        return;
    }
    struct Ops {
        Sci& s;
        const std::string &open, &close;
        sptr_t no, nc;
        bool wrapped(sptr_t x, sptr_t y) { return x >= no && s.Range(x - no, x) == open && s.Range(y, y + nc) == close; }
        void adjust(sptr_t& x, sptr_t& y) {
            std::string t = s.Range(x, y);
            bool st = t.compare(0, open.size(), open) == 0,
                 en = t.size() >= close.size() && t.compare(t.size() - close.size(), close.size(), close) == 0;
            if (en && !st && y - nc > x && wrapped(x, y - nc)) y -= nc;
            else if (st && !en && x + no < y && wrapped(x + no, y)) x += no;
        }
        bool outer(const std::string& t) {   // <u>...</u> with no other </u> before the end
            return t.size() >= (size_t)(no + nc) && t.compare(0, open.size(), open) == 0 &&
                   t.find(close, open.size()) == t.size() - close.size();
        }
        sptr_t unwrap(sptr_t x, sptr_t y) {
            s(SCI_DELETERANGE, y, nc);
            s(SCI_DELETERANGE, x - no, no);
            return no;
        }
        std::string strip(const std::string& t) { return t.substr(open.size(), t.size() - open.size() - close.size()); }
        std::string plain(std::string t) {
            for (const std::string* x : {&open, &close})
                for (size_t q = 0; (q = t.find(*x, q)) != std::string::npos;) t.erase(q, x->size());
            return t;
        }
        std::string wrap(const std::string& t, sptr_t& o) { o = no; return open + t + close; }
    } ops{s, open, close, no, nc};
    ToggleRanges(s, a, b, ops);
}

// Removes paired emphasis delimiters `m` (keeping the text between them).
std::string StripPairs(const std::string& t, const std::string& m) {
    std::string out;
    size_t i = 0, k = m.size();
    for (auto& pr : Pairs(t, m)) {
        out.append(t, i, pr.first - i);
        out.append(t, pr.first + k, pr.second - pr.first - k);
        i = pr.second + k;
    }
    out.append(t, i, std::string::npos);
    return out;
}

std::string StripInline(const std::string& src) {
    // Code spans keep their text exactly as it is (only their backticks go): set them aside first.
    std::vector<std::string> code;
    std::string t;
    for (size_t i = 0; i < src.size();) {
        if (src[i] == '`' && !(i > 0 && src[i - 1] == '\\')) {
            size_t j = i;
            while (j < src.size() && src[j] == '`') j++;
            size_t run = j - i, close = std::string::npos;
            for (size_t k = j; k < src.size();) {
                if (src[k] != '`') { k++; continue; }
                size_t e = k;
                while (e < src.size() && src[e] == '`') e++;
                if (e - k == run) { close = k; break; }
                k = e;
            }
            if (close != std::string::npos) {
                std::string in = src.substr(j, close - j);
                if (in.size() >= 2 && in.front() == ' ' && in.back() == ' ' && in.find_first_not_of(' ') != std::string::npos)
                    in = in.substr(1, in.size() - 2);
                t += "\x01" + std::to_string(code.size()) + "\x02";
                code.push_back(in);
                i = close + run;
                continue;
            }
            t.append(src, i, run);
            i = j;
            continue;
        }
        t += src[i++];
    }
    static const char* tags[] = {"<u>", "</u>", "<ins>", "</ins>", "<kbd>", "</kbd>", "<b>", "</b>", "<strong>", "</strong>",
                                 "<i>", "</i>", "<em>", "</em>", "<mark>", "</mark>", "<s>", "</s>", "<del>", "</del>"};
    for (const char* tg : tags) {
        std::string x = tg;
        for (size_t p = 0; (p = t.find(x, p)) != std::string::npos;) t.erase(p, x.size());
    }
    for (const char* m : {"**", "__", "~~", "*", "_"}) t = StripPairs(t, m);
    std::string out;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] == '\x01') {
            size_t e = t.find('\x02', i);
            if (e != std::string::npos) {
                size_t idx = (size_t)atoi(t.substr(i + 1, e - i - 1).c_str());
                if (idx < code.size()) { out += code[idx]; i = e; continue; }
            }
        }
        out += t[i];
    }
    return out;
}

// Removes inline formatting (emphasis, strikethrough, code, HTML formatting tags) from the selection,
// or from the current line when nothing is selected.
void ClearFormatting(Sci& s) {
    sptr_t a = s(SCI_GETSELECTIONSTART), b = s(SCI_GETSELECTIONEND);
    if (a == b) {
        sptr_t ln = LineOf(s, a);
        std::string lt = s.LineText(ln);
        Parts p = Split(lt);
        a = LineStart(s, ln) + (sptr_t)(lt.size() - p.rest.size());
        b = LineEnd(s, ln);
    }
    std::string t = s.Range(a, b), c = StripInline(t);
    if (c == t) return;
    UndoGroup u(s);
    Replace(s, a, b, c);
    s(SCI_SETSEL, a, a + (sptr_t)c.size());
}

void InsertLink(Sci& s) {
    sptr_t a = s(SCI_GETSELECTIONSTART), b = s(SCI_GETSELECTIONEND);
    std::string sel = s.Range(a, b);
    bool isUrl = sel.rfind("http://", 0) == 0 || sel.rfind("https://", 0) == 0 || sel.rfind("www.", 0) == 0;
    UndoGroup u(s);
    if (isUrl) {
        // www.example.com alone would be a file name next to the document
        Replace(s, a, b, "[](" + std::string(sel.rfind("www.", 0) == 0 ? "https://" : "") + sel + ")");
        s(SCI_GOTOPOS, a + 1);
    } else if (sel.empty()) {
        Replace(s, a, b, "[text](https://)");
        s(SCI_SETSEL, a + 1, a + 5);
    } else {
        Replace(s, a, b, "[" + sel + "](https://)");
        sptr_t url = a + (sptr_t)sel.size() + 3;
        s(SCI_SETSEL, url, url + 8);
    }
}

// ------------------------------------------------------------------ paragraph formatting
std::string FenceOf(const std::string& t);

// Level of a setext heading whose text is line `ln` (text over a line of === or ---), or 0.
int SetextLevel(Sci& s, sptr_t ln) {
    if (ln + 1 >= LineCount(s)) return 0;
    std::string t = s.LineText(ln), u = s.LineText(ln + 1), tu = TrimA(u);
    if (IsBlank(t) || tu.empty() || IndentWidth(Split(u).indent) >= 4) return 0;
    if (TrimA(t).find_first_not_of("-=*_ \t") == std::string::npos) return 0;   // a rule, not text
    Parts p = Split(t);
    if (!p.quote.empty() || !p.list.empty() || !p.heading.empty() || IndentWidth(p.indent) >= 4 || !FenceOf(t).empty() ||
        TrimA(t)[0] == '|')
        return 0;
    if (tu.find_first_not_of('=') == std::string::npos) return 1;
    if (tu.find_first_not_of('-') == std::string::npos) return 2;
    return 0;
}

void SetHeading(Sci& s, int level) {
    sptr_t l0, l1;
    SelLines(s, l0, l1);
    bool multi = l1 > l0, all = level > 0;
    // setext headings count as headings; changing one turns it into a # heading or text and drops the underline
    std::vector<int> setext((size_t)(l1 - l0 + 1));
    for (sptr_t ln = l0; ln <= l1; ln++) setext[(size_t)(ln - l0)] = SetextLevel(s, ln);
    auto underline = [&](sptr_t ln) { return ln > l0 && setext[(size_t)(ln - 1 - l0)] > 0; };
    for (sptr_t ln = l0; ln <= l1 && all; ln++) {
        std::string t = s.LineText(ln);
        if (underline(ln) || (multi && IsBlank(t))) continue;
        int h = HeadingLevel(Split(t));
        if ((h ? h : setext[(size_t)(ln - l0)]) != level) all = false;
    }
    int lv = all ? 0 : level;   // applying the current level again turns it back into normal text
    UndoGroup u(s);
    EditLines(s, l0, l1, [&](Parts& p, const std::string& t, sptr_t ln) {
        if ((multi && IsBlank(t)) || underline(ln)) return;
        bool wasHeading = !p.heading.empty();
        p.heading = lv ? std::string(lv, '#') + " " : "";
        if (lv) { p.indent.clear(); p.inner.clear(); p.list.clear(); p.task.clear(); }
        if (wasHeading && !lv) {   // "## Title ##" -> "Title"
            std::string& r = p.rest;
            size_t e = r.find_last_not_of(" \t");
            if (e != std::string::npos && r[e] == '#') {
                size_t h = e;
                while (h > 0 && r[h - 1] == '#') h--;
                if (h > 0 && (r[h - 1] == ' ' || r[h - 1] == '\t')) {
                    while (h > 0 && (r[h - 1] == ' ' || r[h - 1] == '\t')) h--;
                    r.erase(h);
                }
            }
        }
    });
    for (sptr_t ln = l1; ln >= l0; ln--)   // bottom up, so line numbers above stay right
        if (setext[(size_t)(ln - l0)] && !underline(ln) && !(multi && IsBlank(s.LineText(ln))))
            s(SCI_DELETERANGE, LineEnd(s, ln), LineEnd(s, ln + 1) - LineEnd(s, ln));
}

// The number for a numbered item at `width` indentation on line `ln`: one more than the previous item of the
// same list, or 1.
int NextNumber(Sci& s, sptr_t ln, int width) {
    int depth = QuoteDepth(Split(s.LineText(ln)));
    for (sptr_t k = ln - 1, seen = 0; k >= 0 && seen < 500; k--, seen++) {
        std::string t = s.LineText(k);
        if (IsBlank(t)) continue;
        Parts p = Split(t);
        if (QuoteDepth(p) != depth) break;   // another quote level: another list
        if (!p.quote.empty() && IsBlank(p.inner + p.list + p.task + p.heading + p.rest)) continue;   // ">" alone
        int w = IndentWidth(ListIndent(p));
        if (p.list.empty()) { if (w <= width) break; continue; }
        if (w < width) break;
        if (w == width) return IsNumbered(p) ? atoi(p.list.c_str()) + 1 : 1;
    }
    return 1;
}

void ToggleList(Sci& s, int kind) {   // 1 bulleted, 2 numbered, 3 task
    sptr_t l0, l1;
    SelLines(s, l0, l1);
    bool multi = l1 > l0, all = true;
    for (sptr_t ln = l0; ln <= l1 && all; ln++) {
        std::string t = s.LineText(ln);
        if (multi && IsBlank(t)) continue;
        Parts p = Split(t);
        bool has = kind == 1 ? (IsBullet(p) && p.task.empty()) : kind == 2 ? (IsNumbered(p) && p.task.empty()) : !p.task.empty();
        if (!has) all = false;
    }
    UndoGroup u(s);
    EditLines(s, l0, l1, [&](Parts& p, const std::string& t, sptr_t ln) {
        if (multi && IsBlank(t)) return;
        if (all) { p.list.clear(); p.task.clear(); return; }
        p.heading.clear();
        if (kind == 1) {
            p.list = "- ";
            p.task.clear();
        } else if (kind == 2) {
            p.list = std::to_string(NextNumber(s, ln, IndentWidth(ListIndent(p)))) + ". ";
            p.task.clear();
        } else {
            if (p.list.empty()) p.list = "- ";
            if (p.task.empty()) p.task = "[ ] ";
        }
    });
}

void ToggleTaskDone(Sci& s) {
    sptr_t l0, l1;
    SelLines(s, l0, l1);
    bool anyOpen = false, anyTask = false;
    for (sptr_t ln = l0; ln <= l1; ln++) {
        Parts p = Split(s.LineText(ln));
        if (p.task.size() >= 3) { anyTask = true; if (p.task[1] == ' ') anyOpen = true; }
    }
    if (!anyTask) { ToggleList(s, 3); return; }   // not a task yet: make it one
    UndoGroup u(s);
    EditLines(s, l0, l1, [&](Parts& p, const std::string&, sptr_t) {
        if (p.task.size() >= 3) p.task[1] = anyOpen ? 'x' : ' ';
    });
}

void ToggleQuote(Sci& s) {
    sptr_t l0, l1;
    SelLines(s, l0, l1);
    bool multi = l1 > l0, all = true;
    for (sptr_t ln = l0; ln <= l1 && all; ln++) {
        std::string t = s.LineText(ln);
        if (!(multi && IsBlank(t)) && Split(t).quote.empty()) all = false;
    }
    UndoGroup u(s);
    EditLines(s, l0, l1, [&](Parts& p, const std::string& t, sptr_t) {
        if (all) {
            if (!p.quote.empty()) {
                p.quote.erase(0, 1);
                if (!p.quote.empty() && p.quote[0] == ' ') p.quote.erase(0, 1);
            }
        } else {
            p.quote = (IsBlank(t) ? ">" : "> ") + p.quote;
        }
    });
}

const char* kCallouts[] = {"NOTE", "TIP", "IMPORTANT", "WARNING", "CAUTION"};

void InsertCallout(Sci& s, int k) {
    std::string eol = Eol(s), head = std::string("> [!") + kCallouts[k] + "]";
    sptr_t l0, l1;
    SelLines(s, l0, l1);
    UndoGroup u(s);
    if (s(SCI_GETSELECTIONEMPTY) && l0 == l1 && IsBlank(s.LineText(l0))) {
        InsertBlock(s, head + eol + "> ", -1);
        sptr_t p = s(SCI_GETCURRENTPOS);   // InsertBlock put the caret after the block: go back into it
        sptr_t ln = LineOf(s, p);
        while (ln > 0 && IsBlank(s.LineText(ln))) ln--;
        s(SCI_GOTOPOS, LineEnd(s, ln));
        return;
    }
    bool quoted = true;
    for (sptr_t ln = l0; ln <= l1 && quoted; ln++) {
        std::string t = s.LineText(ln);
        if (!IsBlank(t) && Split(t).quote.empty()) quoted = false;
    }
    if (!quoted)
        EditLines(s, l0, l1, [&](Parts& p, const std::string& t, sptr_t) { p.quote = (IsBlank(t) ? ">" : "> ") + p.quote; });
    sptr_t an = s(SCI_GETANCHOR), ca = s(SCI_GETCURRENTPOS), at = LineStart(s, l0);
    std::string ins = head + eol;
    s(SCI_INSERTTEXT, at, (sptr_t)ins.c_str());
    sptr_t n = (sptr_t)ins.size();
    s(SCI_SETSEL, an >= at ? an + n : an, ca >= at ? ca + n : ca);
}

// The code fence (``` or ~~~, three or more) a line starts with, or "".
std::string FenceOf(const std::string& t) {
    size_t i = 0;
    while (i < t.size() && i < 3 && t[i] == ' ') i++;
    size_t j = i;
    if (j < t.size() && (t[j] == '`' || t[j] == '~')) {
        char c = t[j];
        while (j < t.size() && t[j] == c) j++;
        if (j - i >= 3) return t.substr(i, j - i);
    }
    return std::string();
}

// Fenced code block containing line `ln`: [open, close] fence lines; false if `ln` isn't inside one.
bool FencedBlockAt(Sci& s, sptr_t ln, sptr_t& open, sptr_t& close) {
    sptr_t lines = LineCount(s);
    std::string fence;
    open = -1;
    for (sptr_t k = 0; k < lines; k++) {
        std::string f = FenceOf(s.LineText(k));
        if (open < 0) {
            if (!f.empty()) {
                if (k > ln) return false;
                open = k;
                fence = f;
            }
        } else if (!f.empty() && f[0] == fence[0] && f.size() >= fence.size() && TrimA(s.LineText(k)).size() == f.size()) {
            if (ln >= open && ln <= k) { close = k; return true; }
            open = -1;
        }
    }
    if (open >= 0 && ln >= open) { close = -1; return true; }   // unterminated block runs to the end
    return false;
}

// $$ math block containing line `ln` (its $$ lines included): [open, close].
bool MathBlockAt(Sci& s, sptr_t ln, sptr_t& open, sptr_t& close) {
    sptr_t lines = LineCount(s);
    open = -1;
    for (sptr_t k = 0; k < lines; k++) {
        if (TrimA(s.LineText(k)) != "$$") continue;
        if (open < 0) {
            if (k > ln) return false;
            open = k;
        } else {
            if (ln <= k) { close = k; return true; }
            open = -1;
        }
    }
    return false;
}

// Wraps the selected lines (or the current line) in fence lines; inside an existing block, removes its fences.
void ToggleFenced(Sci& s, const std::string& fence) {
    std::string eol = Eol(s);
    sptr_t l0, l1;
    SelLines(s, l0, l1);
    UndoGroup u(s);
    sptr_t open, close;
    bool inBlock = fence == "```" && FencedBlockAt(s, l0, open, close);
    if (inBlock && (close < 0 || l1 <= close)) {
        // the closing fence line with the real line end before it (documents can mix CRLF and LF)
        if (close >= 0) s(SCI_DELETERANGE, LineEnd(s, close - 1), LineEnd(s, close) - LineEnd(s, close - 1));
        s(SCI_DELETERANGE, LineStart(s, open), LineStart(s, open + 1) - LineStart(s, open));
        return;
    }
    if (fence == "$$") {
        sptr_t mo, mc;
        if (MathBlockAt(s, l0, mo, mc) && l1 <= mc) {
            s(SCI_DELETERANGE, LineEnd(s, mc - 1), LineEnd(s, mc) - LineEnd(s, mc - 1));
            s(SCI_DELETERANGE, LineStart(s, mo), LineStart(s, mo + 1) - LineStart(s, mo));
            return;
        }
    }
    if (s(SCI_GETSELECTIONEMPTY) && IsBlank(s.LineText(l0))) {
        InsertBlock(s, fence + eol + eol + fence, (sptr_t)(fence.size() + eol.size()));
        return;
    }
    std::string f = fence;
    if (fence == "```") {
        // a selection that starts or ends inside a code block takes in that whole block
        if (inBlock) l0 = open;
        sptr_t o2, c2;
        if (FencedBlockAt(s, l1, o2, c2)) l1 = c2 < 0 ? LineCount(s) - 1 : c2;
        // fences inside the selection must not close the new block, so make it longer than all of them
        size_t longest = 0;
        for (sptr_t ln = l0; ln <= l1; ln++) {
            std::string g = FenceOf(s.LineText(ln));
            if (!g.empty() && g[0] == '`') longest = std::max(longest, g.size());
        }
        if (longest >= f.size()) f.assign(longest + 1, '`');
    }
    sptr_t an = s(SCI_GETANCHOR), ca = s(SCI_GETCURRENTPOS);
    sptr_t a = LineStart(s, l0), b = LineEnd(s, l1);
    std::string top = f + eol, bottom = eol + f;
    s(SCI_INSERTTEXT, b, (sptr_t)bottom.c_str());
    s(SCI_INSERTTEXT, a, (sptr_t)top.c_str());
    sptr_t n = (sptr_t)top.size();
    if (an == ca) s(SCI_SETSEL, ca + n, ca + n);
    else s(SCI_SETSEL, a + n, b + n);
}

// Increase / decrease the nesting of list items. Without a list item in the selection this is the
// ordinary Tab / Shift+Tab of the editor.
void Indent(Sci& s, bool in) {
    sptr_t l0, l1;
    SelLines(s, l0, l1);
    bool anyList = false;
    bool inCode = Md_IsCodeBlockStyle((int)s(SCI_GETSTYLEAT, LineStart(s, l0)));   // "1. step" in code is code
    if (s(SCI_GETSELECTIONS) == 1 && !inCode)
        for (sptr_t ln = l0; ln <= l1 && !anyList; ln++) anyList = !Split(s.LineText(ln)).list.empty();
    if (!anyList) {
        s(in ? SCI_TAB : SCI_BACKTAB);
        return;
    }
    // the change in indentation is decided by the first list line and applied to every selected line
    int delta = 0;
    for (sptr_t ln = l0; ln <= l1; ln++) {
        Parts p = Split(s.LineText(ln));
        if (p.list.empty()) continue;
        int w = IndentWidth(ListIndent(p));
        int target = in ? w + 2 : std::max(0, w - 2);
        for (sptr_t k = ln - 1, seen = 0; k >= 0 && seen < 500; k--, seen++) {
            std::string t = s.LineText(k);
            if (IsBlank(t)) continue;
            Parts q = Split(t);
            if (QuoteDepth(q) != QuoteDepth(p)) break;
            if (q.list.empty()) continue;
            int qw = IndentWidth(ListIndent(q));
            if (in && qw == w) { target = qw + (int)q.list.size(); break; }   // nest under the item above
            if (in && qw < w) break;
            if (!in && qw < w) { target = qw; break; }                        // become a sibling of the parent
        }
        delta = target - w;
        break;
    }
    if (delta == 0) return;
    UndoGroup u(s);
    EditLines(s, l0, l1, [&](Parts& p, const std::string& t, sptr_t ln) {
        if (IsBlank(t)) return;
        int w = std::max(0, IndentWidth(ListIndent(p)) + delta);
        ListIndent(p) = std::string((size_t)w, ' ');
        if (IsNumbered(p)) {
            size_t d = 0;
            while (d < p.list.size() && IsDigit(p.list[d])) d++;
            p.list = std::to_string(NextNumber(s, ln, w)) + p.list.substr(d);
        }
    });
}

// ------------------------------------------------------------------ tables
struct Table {
    sptr_t l0 = 0, l1 = 0;                       // header line .. last row line
    std::string indent;
    std::vector<std::vector<std::string>> rows;  // header + body rows (the delimiter row is `align`)
    std::vector<int> align;                      // 0 default, 1 left, 2 center, 3 right
    int row = 0, col = 0;                        // the caret's cell
};

std::vector<std::string> SplitCells(const std::string& line, int* caretCell = nullptr, size_t caretCol = 0) {
    std::vector<std::string> cells;
    size_t i = 0, n = line.size();
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    bool lead = i < n && line[i] == '|';
    if (lead) i++;
    size_t end = n;
    while (end > i && IsSpace(line[end - 1])) end--;
    bool trail = end > i && line[end - 1] == '|' && !(end >= 2 && line[end - 2] == '\\');
    if (trail) end--;
    if (caretCell) *caretCell = 0;
    std::string cur;
    for (; i < end; i++) {
        char c = line[i];
        if (c == '\\' && i + 1 < end) { cur += c; cur += line[++i]; continue; }
        if (c == '`') {   // a pipe inside a code span doesn't split the cell (as in the preview)
            size_t j = i;
            while (j < end && line[j] == '`') j++;
            size_t run = j - i, close = std::string::npos;
            for (size_t k = j; k < end;) {
                if (line[k] != '`') { k++; continue; }
                size_t e = k;
                while (e < end && line[e] == '`') e++;
                if (e - k == run) { close = e; break; }
                k = e;
            }
            size_t to = close != std::string::npos ? close : j;
            cur.append(line, i, to - i);
            i = to - 1;
            continue;
        }
        if (c == '|') {
            cells.push_back(TrimA(cur));
            cur.clear();
            if (caretCell && i < caretCol) (*caretCell)++;
            continue;
        }
        cur += c;
    }
    cells.push_back(TrimA(cur));
    return cells;
}

bool IsDelimRow(const std::vector<std::string>& cells) {
    if (cells.empty()) return false;
    for (auto& c : cells) {
        if (c.empty()) return false;
        size_t a = c[0] == ':' ? 1 : 0, b = c.size() - (c.size() > 1 && c.back() == ':' ? 1 : 0);
        if (b <= a) return false;
        for (size_t k = a; k < b; k++)
            if (c[k] != '-') return false;
    }
    return true;
}

bool TableAt(Sci& s, sptr_t line, Table& T) {
    auto piped = [&](sptr_t ln) {
        std::string t = s.LineText(ln);
        return !IsBlank(t) && t.find('|') != std::string::npos;
    };
    if (!piped(line)) return false;
    sptr_t a = line, b = line, lines = LineCount(s);
    while (a > 0 && piped(a - 1) && line - a < 2000) a--;
    while (b + 1 < lines && piped(b + 1) && b - line < 2000) b++;
    // the header is the line above the first delimiter row
    sptr_t header = -1;
    for (sptr_t k = a + 1; k <= b; k++)
        if (IsDelimRow(SplitCells(s.LineText(k)))) { header = k - 1; break; }
    if (header < 0 || line < header) return false;
    T = Table();
    T.l0 = header;
    T.l1 = b;
    std::string ht = s.LineText(header);
    T.indent = Split(ht).indent;
    for (sptr_t k = header; k <= b; k++) {
        std::string t = s.LineText(k);
        if (k == header + 1) {
            for (auto& c : SplitCells(t)) {
                bool l = c[0] == ':', r = c.size() > 1 && c.back() == ':';
                T.align.push_back(l && r ? 2 : r ? 3 : l ? 1 : 0);
            }
            continue;
        }
        int cc = 0;
        size_t col = (size_t)(s(SCI_GETCURRENTPOS) - LineStart(s, k));
        T.rows.push_back(SplitCells(t, k == line ? &cc : nullptr, col));
        if (k == line) T.col = cc;
    }
    T.row = line <= header + 1 ? 0 : (int)(line - header - 1);
    if (line == header + 1) {   // on the delimiter row: use the header cell above
        int cc = 0;
        SplitCells(s.LineText(line), &cc, (size_t)(s(SCI_GETCURRENTPOS) - LineStart(s, line)));
        T.col = cc;
    }
    return true;
}

// Columns a cell's text takes in a monospace font: East Asian wide characters and emoji count two,
// combining marks, variation selectors and joiners none.
size_t DisplayWidth(const std::string& t) {
    size_t w = 0;
    for (size_t i = 0; i < t.size();) {
        unsigned char c = (unsigned char)t[i];
        uint32_t cp = 0xFFFD;
        size_t len = 1;
        auto cont = [&](size_t k) { return (uint32_t)((unsigned char)t[i + k] & 0x3F); };
        if (c < 0x80) cp = c;
        else if ((c >> 5) == 6 && i + 1 < t.size()) { cp = ((c & 0x1Fu) << 6) | cont(1); len = 2; }
        else if ((c >> 4) == 14 && i + 2 < t.size()) { cp = ((c & 0x0Fu) << 12) | (cont(1) << 6) | cont(2); len = 3; }
        else if ((c >> 3) == 30 && i + 3 < t.size()) { cp = ((c & 0x07u) << 18) | (cont(1) << 12) | (cont(2) << 6) | cont(3); len = 4; }
        i += len;
        if ((cp >= 0x300 && cp <= 0x36F) || (cp >= 0x200B && cp <= 0x200F) || (cp >= 0xFE00 && cp <= 0xFE0F) ||
            (cp >= 0x1F3FB && cp <= 0x1F3FF) || (cp >= 0xE0100 && cp <= 0xE01EF))
            continue;
        bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF) ||
                    (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
                    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
                    (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
                    (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
        w += wide ? 2 : 1;
    }
    return w;
}

// Writes the table back with aligned columns; the caret goes to the start of cell (row, col).
void WriteTable(Sci& s, Table& T, int row, int col) {
    std::string eol = Eol(s);
    size_t ncols = T.align.size();
    for (auto& r : T.rows) ncols = std::max(ncols, r.size());
    if (!ncols) return;
    T.align.resize(ncols, 0);
    for (auto& r : T.rows) r.resize(ncols);
    std::vector<size_t> w(ncols, 3);
    for (auto& r : T.rows)
        for (size_t c = 0; c < ncols; c++) w[c] = std::max(w[c], DisplayWidth(r[c]));
    std::string out;
    sptr_t caret = -1;
    auto rowText = [&](const std::vector<std::string>& r, int ri) {
        out += T.indent + "|";
        for (size_t c = 0; c < ncols; c++) {
            size_t pad = w[c] - DisplayWidth(r[c]);
            size_t left = T.align[c] == 3 ? pad : T.align[c] == 2 ? pad / 2 : 0;
            out += " " + std::string(left, ' ');
            if (ri == row && (int)c == col) caret = (sptr_t)out.size();
            out += r[c] + std::string(pad - left, ' ') + " |";
        }
    };
    for (size_t ri = 0; ri < T.rows.size(); ri++) {
        if (ri) out += eol;
        rowText(T.rows[ri], (int)ri);
        if (ri == 0) {
            out += eol + T.indent + "|";
            for (size_t c = 0; c < ncols; c++) {
                std::string d;
                switch (T.align[c]) {
                case 1: d = ":" + std::string(w[c] - 1, '-'); break;
                case 2: d = ":" + std::string(w[c] - 2, '-') + ":"; break;
                case 3: d = std::string(w[c] - 1, '-') + ":"; break;
                default: d = std::string(w[c], '-');
                }
                out += " " + d + " |";
            }
        }
    }
    sptr_t a = LineStart(s, T.l0);
    UndoGroup u(s);
    Replace(s, a, LineEnd(s, T.l1), out);
    if (caret >= 0) s(SCI_SETSEL, a + caret, a + caret);
}

void InsertTable(Sci& s, int cols, int rows) {
    std::string eol = Eol(s), t;
    std::vector<std::string> head;
    for (int c = 0; c < cols; c++) head.push_back("Column " + std::to_string(c + 1));
    size_t w = 8;
    t += "|";
    for (auto& h : head) t += " " + h + " |";
    t += eol + "|";
    for (int c = 0; c < cols; c++) t += " " + std::string(w, '-') + " |";
    for (int r = 0; r < rows; r++) {
        t += eol + "|";
        for (int c = 0; c < cols; c++) t += " " + std::string(w, ' ') + " |";
    }
    UndoGroup u(s);
    InsertBlock(s, t, 2, 8);   // select "Column 1"
}

bool TableCommand(Sci& s, int cmd) {
    Table T;
    if (!TableAt(s, LineOf(s, s(SCI_GETCURRENTPOS)), T)) return false;
    size_t ncols = T.align.size();
    for (auto& r : T.rows) ncols = std::max(ncols, r.size());
    int row = T.row, col = std::min<int>(T.col, (int)ncols - 1);
    std::vector<std::string> empty(ncols);
    switch (cmd) {
    case CMD_FMT_ROWABOVE:
        row = std::max(1, row);   // nothing goes above the header row
        T.rows.insert(T.rows.begin() + row, empty);
        break;
    case CMD_FMT_ROWBELOW:
        row = row + 1;
        T.rows.insert(T.rows.begin() + row, empty);
        break;
    case CMD_FMT_COLLEFT:
    case CMD_FMT_COLRIGHT: {
        int at = cmd == CMD_FMT_COLLEFT ? col : col + 1;
        for (auto& r : T.rows) { r.resize(ncols); r.insert(r.begin() + at, std::string()); }
        T.align.resize(ncols, 0);
        T.align.insert(T.align.begin() + at, 0);
        col = at;
        break;
    }
    case CMD_FMT_DELROW:
        if (row == 0) return true;   // the header row can't be removed
        T.rows.erase(T.rows.begin() + row);
        row = std::min<int>(row, (int)T.rows.size() - 1);
        break;
    case CMD_FMT_DELCOL:
        if (ncols <= 1) return true;
        for (auto& r : T.rows) { r.resize(ncols); r.erase(r.begin() + col); }
        T.align.resize(ncols, 0);
        T.align.erase(T.align.begin() + col);
        col = std::min<int>(col, (int)ncols - 2);
        break;
    case CMD_FMT_ALIGNLEFT: T.align.resize(ncols, 0); T.align[col] = 1; break;
    case CMD_FMT_ALIGNCENTER: T.align.resize(ncols, 0); T.align[col] = 2; break;
    case CMD_FMT_ALIGNRIGHT: T.align.resize(ncols, 0); T.align[col] = 3; break;
    case CMD_FMT_ALIGNNONE: T.align.resize(ncols, 0); T.align[col] = 0; break;
    case CMD_FMT_TABLETIDY: break;
    default: return false;
    }
    WriteTable(s, T, row, col);
    return true;
}

// ------------------------------------------------------------------ images, footnotes, misc
std::string ImageTarget(const std::wstring& file, const std::wstring& docPath) {
    std::wstring p = file;
    if (!docPath.empty()) {
        wchar_t rel[MAX_PATH * 2] = L"";
        std::wstring dir = DirOf(docPath);
        if (dir.size() == 2 && dir[1] == L':') dir += L'\\';   // document at a drive root ("E:" is E's current folder)
        // only within one drive or share: ..\ can't climb from one network share into another
        if (PathIsSameRootW(dir.c_str(), file.c_str()) &&
            PathRelativePathToW(rel, dir.c_str(), FILE_ATTRIBUTE_DIRECTORY, file.c_str(), FILE_ATTRIBUTE_NORMAL)) {
            p = rel;
            if (p.rfind(L".\\", 0) == 0) p = p.substr(2);
        }
    }
    // a network path as a file URL (file://server/share/x.png): "//server/..." would be a web address
    if (p.rfind(L"\\\\", 0) == 0 && p.rfind(L"\\\\?\\", 0) != 0 && p.rfind(L"\\\\.\\", 0) != 0) p = L"file:" + p;
    for (auto& c : p)
        if (c == L'\\') c = L'/';
    std::string u = WToU8(p), out;
    for (char c : u) {
        switch (c) {
        case ' ': out += "%20"; break;
        case '(': out += "%28"; break;
        case ')': out += "%29"; break;
        case '%': out += "%25"; break;
        case '#': out += "%23"; break;
        case '?': out += "%3F"; break;
        case '<': out += "%3C"; break;
        case '>': out += "%3E"; break;
        default: out += c;
        }
    }
    return out;
}

std::string AltFor(const std::wstring& file) {
    std::wstring n = PathFindFileNameW(file.c_str());
    size_t dot = n.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) n = n.substr(0, dot);
    std::string a = WToU8(n), out;
    for (char c : a) {
        if (c == '[' || c == ']') out += '\\';
        out += c;
    }
    return out;
}

void InsertImageFromFile(Sci& s, HWND owner, const std::wstring& docPath) {
    wchar_t buf[MAX_PATH * 4] = L"";
    std::wstring dir = docPath.empty() ? L"" : DirOf(docPath);
    OPENFILENAMEW of{};
    of.lStructSize = sizeof(of);
    of.hwndOwner = owner;
    of.lpstrFilter = L"Images (*.png, *.jpg, *.gif, *.bmp, *.webp, *.svg, *.ico, *.tif)\0"
                     L"*.png;*.jpg;*.jpeg;*.gif;*.bmp;*.webp;*.svg;*.ico;*.tif;*.tiff\0All files (*.*)\0*.*\0";
    of.lpstrFile = buf;
    of.nMaxFile = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    of.lpstrInitialDir = dir.empty() ? nullptr : dir.c_str();
    of.lpstrTitle = L"Insert image";
    of.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_EXPLORER;
    if (!GetOpenFileNameW(&of)) return;
    sptr_t a = s(SCI_GETSELECTIONSTART), b = s(SCI_GETSELECTIONEND);
    std::string sel = s.Range(a, b);
    std::string alt = (!sel.empty() && sel.find('\n') == std::string::npos) ? sel : AltFor(buf);
    std::string link = "![" + alt + "](" + ImageTarget(buf, docPath) + ")";
    UndoGroup u(s);
    Replace(s, a, b, link);
    s(SCI_SETSEL, a + 2, a + 2 + (sptr_t)alt.size());   // the alt text, ready to be described
}

void InsertImageUrl(Sci& s) {
    sptr_t a = s(SCI_GETSELECTIONSTART), b = s(SCI_GETSELECTIONEND);
    std::string sel = s.Range(a, b);
    std::string alt = (!sel.empty() && sel.find('\n') == std::string::npos) ? sel : "alt text";
    UndoGroup u(s);
    Replace(s, a, b, "![" + alt + "](https://)");
    sptr_t url = a + 4 + (sptr_t)alt.size();
    s(SCI_SETSEL, url, url + 8);
}

void InsertFootnote(Sci& s) {
    std::string doc = s.Text(), eol = Eol(s);
    int maxN = 0;
    for (size_t p = doc.find("[^"); p != std::string::npos; p = doc.find("[^", p + 2)) {
        size_t q = p + 2;
        int v = 0;
        while (q < doc.size() && IsDigit(doc[q]) && v < 100000) v = v * 10 + (doc[q++] - '0');
        if (q > p + 2 && q < doc.size() && doc[q] == ']') maxN = std::max(maxN, v);
    }
    std::string ref = "[^" + std::to_string(maxN + 1) + "]";
    UndoGroup u(s);
    sptr_t b = s(SCI_GETSELECTIONEND);
    s(SCI_SETEMPTYSELECTION, b);
    s(SCI_REPLACESEL, 0, (sptr_t)ref.c_str());
    // the definition goes at the end of the document, after any existing definitions
    sptr_t len = s.Len(), lastLine = LineCount(s) - 1;
    while (lastLine > 0 && IsBlank(s.LineText(lastLine))) lastLine--;
    auto isDef = [](const std::string& t) {   // "[^label]: text" (not the reference just typed at a line start)
        size_t i = 0;
        while (i < t.size() && i < 3 && t[i] == ' ') i++;
        if (t.compare(i, 2, "[^") != 0) return false;
        size_t e = t.find(']', i + 2);
        return e != std::string::npos && e > i + 2 && e + 1 < t.size() && t[e + 1] == ':';
    };
    bool afterDefs = isDef(s.LineText(lastLine));
    std::string tail = s.Range(std::max<sptr_t>(0, len - (sptr_t)eol.size() * 2), len);
    bool endsEol = len > 0 && s.Range(len - (sptr_t)eol.size(), len) == eol;
    std::string def = afterDefs ? (endsEol ? "" : eol) : (tail == eol + eol ? "" : endsEol ? eol : eol + eol);
    def += ref + ": ";
    s(SCI_APPENDTEXT, def.size(), (sptr_t)def.c_str());
    sptr_t end = s.Len();
    s(SCI_SETSEL, end, end);
}

void InsertLineBreak(Sci& s) {
    std::string t = "\\" + Eol(s);
    SingleSelection(s);
    s(SCI_REPLACESEL, 0, (sptr_t)t.c_str());
}

void InsertRule(Sci& s) {
    UndoGroup u(s);
    InsertBlock(s, "---", -1);
}

// ------------------------------------------------------------------ images when saving to another folder
// Byte ranges of the destinations of inline images ![alt](destination), outside code.
std::vector<std::pair<size_t, size_t>> ImageDestinations(const std::string& t) {
    std::vector<std::pair<size_t, size_t>> out;
    char fence = 0;
    size_t fenceLen = 0;
    bool prevBlank = true, inIndented = false, inList = false;
    for (size_t ls = 0; ls < t.size();) {
        size_t le = t.find_first_of("\r\n", ls);   // LF, CRLF and CR documents
        if (le == std::string::npos) le = t.size();
        size_t next = le + (le + 1 < t.size() && t[le] == '\r' && t[le + 1] == '\n' ? 2 : 1);
        // indented code: 4+ columns after a blank line, outside list items (where that indent is item text)
        size_t k = ls;
        int width = 0;
        while (k < le && (t[k] == ' ' || t[k] == '\t')) width += t[k++] == '\t' ? 4 - width % 4 : 1;
        bool blank = k >= le, wasBlank = prevBlank;
        prevBlank = blank;
        if (!fence) {
            if (inIndented && (blank || width >= 4)) { ls = next; continue; }
            inIndented = false;
            if (!blank && width >= 4 && wasBlank && !inList) { inIndented = true; ls = next; continue; }
            if (!blank && width < 4) {
                size_t d = k;
                while (d < le && d - k < 9 && IsDigit(t[d])) d++;
                bool marker = (k < le && (t[k] == '-' || t[k] == '*' || t[k] == '+') && (k + 1 >= le || t[k + 1] == ' ' || t[k + 1] == '\t')) ||
                              (d > k && d < le && (t[d] == '.' || t[d] == ')') && (d + 1 >= le || t[d + 1] == ' ' || t[d + 1] == '\t'));
                if (marker) inList = true;
                else if (width == 0 && wasBlank) inList = false;   // a paragraph after the list ends it
            }
        }
        size_t i = ls, sp = 0;
        while (i < le && t[i] == ' ' && sp < 4) i++, sp++;
        if (sp < 4 && i < le && (t[i] == '`' || t[i] == '~')) {
            size_t n = 0;
            while (i + n < le && t[i + n] == t[i]) n++;
            bool rest = t.find_first_not_of(" \t", i + n) >= le;   // nothing after the fence
            bool opens = !fence && n >= 3 && !(t[i] == '`' && t.find('`', i + n) < le);
            bool closes = fence && t[i] == fence && n >= fenceLen && rest;   // "```js" inside doesn't close
            if (opens || closes) {
                if (fence) fence = 0;
                else fence = t[i], fenceLen = n;
                ls = next;
                continue;
            }
        }
        if (fence) { ls = next; continue; }
        for (size_t p = ls; p < le; p++) {
            if (t[p] == '\\') { p++; continue; }
            if (t[p] == '`') {   // skip an inline code span
                size_t n = 0;
                while (p + n < le && t[p + n] == '`') n++;
                size_t q = p + n, close = std::string::npos;
                while (q < le) {
                    if (t[q] != '`') { q++; continue; }
                    size_t m = 0;
                    while (q + m < le && t[q + m] == '`') m++;
                    if (m == n) { close = q + m; break; }
                    q += m;
                }
                p = (close != std::string::npos ? close : p + n) - 1;
                continue;
            }
            if (t[p] != '!' || p + 1 >= le || t[p + 1] != '[') continue;
            int depth = 0;
            size_t q = p + 1;
            for (; q < le; q++) {
                if (t[q] == '\\') { q++; continue; }
                if (t[q] == '[') depth++;
                else if (t[q] == ']' && --depth == 0) break;
            }
            if (q + 1 >= le || t[q + 1] != '(') continue;
            size_t a = q + 2;
            while (a < le && (t[a] == ' ' || t[a] == '\t')) a++;
            size_t b = a;
            if (a < le && t[a] == '<') {
                b = t.find('>', ++a);
                if (b == std::string::npos || b > le) continue;
            } else {
                int par = 0;
                while (b < le && t[b] != ' ' && t[b] != '\t') {
                    if (t[b] == '\\' && b + 1 < le) { b += 2; continue; }
                    if (t[b] == '(') par++;
                    else if (t[b] == ')' && par-- == 0) break;
                    b++;
                }
            }
            if (b > a) out.push_back({a, b});
            p = q;
        }
        ls = next;
    }
    return out;
}

// Web addresses, data: URIs, absolute paths and in-page anchors keep working wherever the document is.
bool LocationIndependent(const std::string& d) {
    if (d.empty() || d[0] == '#' || d[0] == '/' || d[0] == '\\') return true;
    size_t i = 0;
    while (i < d.size() && (isalnum((unsigned char)d[i]) || d[i] == '+' || d[i] == '.' || d[i] == '-')) i++;
    return i > 0 && i < d.size() && d[i] == ':';   // "https:", "data:", "C:"
}

std::string PercentDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
            o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else if (s[i] == '\\' && i + 1 < s.size() && ispunct((unsigned char)s[i + 1])) {
            o += s[++i];   // Markdown backslash escape
        } else {
            o += s[i];
        }
    }
    return o;
}

bool ReadAll(const std::wstring& path, std::string& out) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart < (1LL << 31);
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, &out[0], (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

bool SameFileContent(const std::wstring& a, const std::wstring& b) {
    WIN32_FILE_ATTRIBUTE_DATA fa, fb;
    if (!GetFileAttributesExW(a.c_str(), GetFileExInfoStandard, &fa) || !GetFileAttributesExW(b.c_str(), GetFileExInfoStandard, &fb)) return false;
    if (fa.nFileSizeLow != fb.nFileSizeLow || fa.nFileSizeHigh != fb.nFileSizeHigh) return false;
    std::string x, y;
    return ReadAll(a, x) && ReadAll(b, y) && x == y;
}

std::wstring FullPathOf(const std::wstring& p) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH * 2, buf, nullptr);
    return n && n < MAX_PATH * 2 ? std::wstring(buf) : p;
}

std::wstring ErrorText(DWORD e) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, e, 0,
                   (LPWSTR)&buf, 0, nullptr);
    std::wstring s = buf ? buf : L"Error " + std::to_wstring(e);
    if (buf) LocalFree(buf);
    while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
    return s;
}
}  // namespace

// ------------------------------------------------------------------ public
const std::vector<TableSize>& Fmt_TableSizes() {
    static const std::vector<TableSize> v = {{2, 2}, {3, 2}, {3, 3}, {4, 3}, {4, 5}, {5, 5}, {6, 8}};
    return v;
}

bool Fmt_IsImageFile(const std::wstring& path) {
    const wchar_t* e = PathFindExtensionW(path.c_str());
    static const wchar_t* exts[] = {L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".svg", L".ico", L".tif", L".tiff"};
    for (auto x : exts)
        if (!_wcsicmp(e, x)) return true;
    return false;
}

void Fmt_InsertImages(Sci& s, const std::vector<std::wstring>& files, const std::wstring& docPath, sptr_t pos) {
    if (files.empty()) return;
    std::string eol = Eol(s), t;
    for (size_t i = 0; i < files.size(); i++) {
        if (i) t += eol + eol;
        t += "![" + AltFor(files[i]) + "](" + ImageTarget(files[i], docPath) + ")";
    }
    UndoGroup u(s);
    s(SCI_INSERTTEXT, pos, (sptr_t)t.c_str());
    s(SCI_SETSEL, pos + (sptr_t)t.size(), pos + (sptr_t)t.size());
}

void Fmt_PasteImages(Sci& s, const std::vector<std::wstring>& files, const std::wstring& docPath, const std::string& alt) {
    if (files.empty()) return;
    std::string eol = Eol(s), t;
    for (size_t i = 0; i < files.size(); i++) {
        if (i) t += eol + eol;
        t += "![" + (alt.empty() ? AltFor(files[i]) : alt) + "](" + ImageTarget(files[i], docPath) + ")";
    }
    UndoGroup u(s);
    SingleSelection(s);
    s(SCI_REPLACESEL, 0, (sptr_t)t.c_str());
}

ImageMove Fmt_RelocateImages(Sci& s, const std::wstring& oldDoc, const std::wstring& newDoc) {
    ImageMove r;
    std::wstring oldDir = DirOf(oldDoc), newDir = DirOf(newDoc);
    if (oldDir.empty() || newDir.empty()) return r;
    // folders with their final backslash: "E:" alone would mean the current folder of drive E
    auto folder = [](const std::wstring& d) {
        std::wstring f = FullPathOf(d + L"\\");
        if (f.empty() || f.back() != L'\\') f += L'\\';
        return f;
    };
    const std::wstring inside = folder(oldDir), newFolder = folder(newDir);
    if (!_wcsicmp(inside.c_str(), newFolder.c_str())) return r;
    std::string text = s.Text();
    struct Change { size_t a, b; std::string with; };
    std::vector<Change> changes;
    for (auto& d : ImageDestinations(text)) {
        std::string dest = text.substr(d.first, d.second - d.first);
        if (LocationIndependent(dest) || dest.find_first_of("?#") != std::string::npos) continue;
        std::wstring rel = U8ToW(PercentDecode(dest));
        for (auto& c : rel) if (c == L'/') c = L'\\';
        std::wstring src = FullPathOf(inside + rel);
        DWORD at = GetFileAttributesW(src.c_str());
        if (at == INVALID_FILE_ATTRIBUTES || (at & FILE_ATTRIBUTE_DIRECTORY)) continue;   // already broken: leave it
        std::string link;   // new destination text; empty = unchanged
        if (src.size() > inside.size() && !_wcsnicmp(src.c_str(), inside.c_str(), inside.size())) {
            // inside the old folder (e.g. images\x.png): copy it to the same place under the new folder
            std::wstring want = newFolder + src.substr(inside.size()), dst = want;
            SHCreateDirectoryExW(nullptr, DirOf(dst).c_str(), nullptr);
            size_t dot = want.find_last_of(L'.'), slash = want.find_last_of(L'\\');
            std::wstring stem = (dot != std::wstring::npos && dot > slash) ? want.substr(0, dot) : want;
            std::wstring ext = (dot != std::wstring::npos && dot > slash) ? want.substr(dot) : L"";
            bool placed = false;
            for (int i = 2; i < 1000; i++) {
                if (GetFileAttributesW(dst.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    if (CopyFileW(src.c_str(), dst.c_str(), TRUE)) { r.copied++; placed = true; }
                    else if (r.error.empty()) r.error = src + L": " + ErrorText(GetLastError());
                    break;
                }
                if (SameFileContent(src, dst)) { placed = true; break; }   // already there
                dst = stem + L"-" + std::to_wstring(i) + ext;              // a different file has that name
            }
            if (!placed) link = ImageTarget(src, newDoc);   // no copy: keep showing the original
            else if (_wcsicmp(dst.c_str(), want.c_str())) link = ImageTarget(dst, newDoc);
        } else {
            // elsewhere (e.g. ..\shared\x.png): keep using it, with a path that works from the new folder
            link = ImageTarget(src, newDoc);
        }
        if (!link.empty() && link != dest) changes.push_back({d.first, d.second, link});
    }
    if (!changes.empty()) {
        UndoGroup u(s);
        for (auto it = changes.rbegin(); it != changes.rend(); ++it) {
            s(SCI_SETTARGETRANGE, (uptr_t)it->a, (sptr_t)it->b);
            s(SCI_REPLACETARGET, it->with.size(), (sptr_t)it->with.c_str());
        }
        r.rewritten = (int)changes.size();
    }
    return r;
}

FmtState Fmt_State(Sci& s) {
    FmtState st;
    sptr_t caret = s(SCI_GETCURRENTPOS), line = LineOf(s, caret), ls = LineStart(s, line);
    st.inCode = Md_IsCodeBlockStyle((int)s(SCI_GETSTYLEAT, ls));
    if (!st.inCode) {
        Parts p = Split(s.LineText(line));
        st.heading = HeadingLevel(p);
        if (!st.heading) st.heading = SetextLevel(s, line);
        st.list = IsBullet(p) ? 1 : IsNumbered(p) ? 2 : 0;
        st.task = !p.task.empty();
        st.taskDone = p.task.size() >= 3 && p.task[1] != ' ';
        st.quote = !p.quote.empty();
        // the character before the caret decides; right after an opening marker ("**|bold") look ahead instead
        sptr_t at = caret > ls ? caret - 1 : caret;
        if (s(SCI_GETSELECTIONEMPTY) == 0) at = s(SCI_GETSELECTIONSTART);
        int f = Md_InlineFlags((int)s(SCI_GETSTYLEAT, at));
        if (f < 0 && at + 1 < s.Len() && at < LineEnd(s, line)) f = Md_InlineFlags((int)s(SCI_GETSTYLEAT, at + 1));
        st.inlineFlags = std::max(0, f);
        Table T;
        st.inTable = TableAt(s, line, T);
    }
    return st;
}

bool Fmt_Command(Sci& s, int cmd, HWND owner, const std::wstring& docPath) {
    switch (cmd) {
    case CMD_BOLD: ToggleWrap(s, "**"); break;
    case CMD_ITALIC: ToggleWrap(s, "*"); break;
    case CMD_CODE: ToggleWrap(s, "`"); break;
    case CMD_FMT_STRIKE: ToggleWrap(s, "~~"); break;
    case CMD_FMT_MATH: ToggleWrap(s, "$"); break;
    case CMD_FMT_UNDERLINE: ToggleTags(s, "<u>", "</u>"); break;
    case CMD_FMT_KBD: ToggleTags(s, "<kbd>", "</kbd>"); break;
    case CMD_FMT_CLEAR: ClearFormatting(s); break;
    case CMD_LINK: InsertLink(s); break;
    case CMD_FMT_PARA: SetHeading(s, 0); break;
    case CMD_FMT_BULLET: ToggleList(s, 1); break;
    case CMD_FMT_NUMBER: ToggleList(s, 2); break;
    case CMD_FMT_TASK: ToggleList(s, 3); break;
    case CMD_FMT_TASKDONE: ToggleTaskDone(s); break;
    case CMD_FMT_INDENT: Indent(s, true); break;
    case CMD_FMT_OUTDENT: Indent(s, false); break;
    case CMD_FMT_QUOTE: ToggleQuote(s); break;
    case CMD_FMT_CODEBLOCK: ToggleFenced(s, "```"); break;
    case CMD_FMT_MATHBLOCK: ToggleFenced(s, "$$"); break;
    case CMD_FMT_IMAGE: InsertImageFromFile(s, owner, docPath); break;
    case CMD_FMT_IMAGEURL: InsertImageUrl(s); break;
    case CMD_FMT_HR: InsertRule(s); break;
    case CMD_FMT_FOOTNOTE: InsertFootnote(s); break;
    case CMD_FMT_LINEBREAK: InsertLineBreak(s); break;
    case CMD_FMT_ROWABOVE: case CMD_FMT_ROWBELOW: case CMD_FMT_COLLEFT: case CMD_FMT_COLRIGHT:
    case CMD_FMT_DELROW: case CMD_FMT_DELCOL: case CMD_FMT_ALIGNLEFT: case CMD_FMT_ALIGNCENTER:
    case CMD_FMT_ALIGNRIGHT: case CMD_FMT_ALIGNNONE: case CMD_FMT_TABLETIDY:
        if (!TableCommand(s, cmd)) App_Status(L"Put the cursor inside a table first.", true);
        break;
    default:
        if (cmd >= CMD_FMT_H1 && cmd < CMD_FMT_H1 + 6) SetHeading(s, cmd - CMD_FMT_H1 + 1);
        else if (cmd >= CMD_FMT_CALLOUT0 && cmd < CMD_FMT_CALLOUT0 + 5) InsertCallout(s, cmd - CMD_FMT_CALLOUT0);
        else if (cmd >= CMD_FMT_TABLE0 && cmd < CMD_FMT_TABLE0 + (int)Fmt_TableSizes().size()) {
            const TableSize& t = Fmt_TableSizes()[cmd - CMD_FMT_TABLE0];
            InsertTable(s, t.cols, t.rows);
        } else return false;
    }
    s(SCI_SCROLLCARET);
    return true;
}
