#include "json.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>

static const JVal kNull;

const JVal& JVal::operator[](const char* key) const {
    if (t != Obj) return kNull;
    for (auto& kv : o)
        if (kv.first == key) return kv.second;
    return kNull;
}
const JVal& JVal::operator[](size_t i) const {
    if (t != Arr || i >= a.size()) return kNull;
    return a[i];
}

namespace {
struct Parser {
    const char* p;
    const char* e;
    int depth = 0;

    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++; }

    static void putUtf8(std::string& s, unsigned cp) {
        if (cp < 0x80) s += (char)cp;
        else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) {
            s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
        } else {
            s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F));
            s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (e - p < 4) return false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool str(std::string& out) {
        if (p >= e || *p != '"') return false;
        p++;
        while (p < e) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (p >= e) return false;
            c = *p++;
            switch (c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    auto save = p;
                    p += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else p = save;   // not a pair: that escape is a character of its own
                }
                if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD;   // half a pair can't be written as UTF-8
                putUtf8(out, cp);
                break;
            }
            default: return false;
            }
        }
        return false;
    }
    bool val(JVal& v) {
        if (++depth > 200) return false;
        ws();
        if (p >= e) return false;
        bool ok = true;
        char c = *p;
        if (c == '{') {
            v.t = JVal::Obj; p++; ws();
            if (p < e && *p == '}') { p++; }
            else for (;;) {
                ws();
                std::string k;
                if (!str(k)) { ok = false; break; }
                ws();
                if (p >= e || *p != ':') { ok = false; break; }
                p++;
                v.o.emplace_back(std::move(k), JVal());
                if (!val(v.o.back().second)) { ok = false; break; }
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == '}') { p++; break; }
                ok = false; break;
            }
        } else if (c == '[') {
            v.t = JVal::Arr; p++; ws();
            if (p < e && *p == ']') { p++; }
            else for (;;) {
                v.a.emplace_back();
                if (!val(v.a.back())) { ok = false; break; }
                ws();
                if (p < e && *p == ',') { p++; continue; }
                if (p < e && *p == ']') { p++; break; }
                ok = false; break;
            }
        } else if (c == '"') {
            v.t = JVal::Str; ok = str(v.s);
        } else if (c == 't' && e - p >= 4 && !memcmp(p, "true", 4)) { v.t = JVal::Bool; v.b = true; p += 4; }
        else if (c == 'f' && e - p >= 5 && !memcmp(p, "false", 5)) { v.t = JVal::Bool; v.b = false; p += 5; }
        else if (c == 'n' && e - p >= 4 && !memcmp(p, "null", 4)) { v.t = JVal::Null; p += 4; }
        else if (c == '-' || (c >= '0' && c <= '9')) {
            char buf[64]; size_t i = 0;
            while (p < e && i < 63 && (strchr("+-0123456789.eE", *p))) buf[i++] = *p++;
            buf[i] = 0;
            v.t = JVal::Num; v.n = strtod(buf, nullptr);
        } else ok = false;
        depth--;
        return ok;
    }
};
}

bool JParse(const char* p, size_t n, JVal& out) {
    Parser ps{p, p + n};
    out = JVal();
    return ps.val(out);
}

void JQuote(std::string& out, const std::string& in) {
    out += '"';
    for (unsigned char c : in) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof b, "\\u%04x", c);
                out += b;
            } else out += (char)c;
        }
    }
    out += '"';
}
