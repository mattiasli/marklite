// Minimal JSON reader/writer helpers (enough for the Anthropic API).
#pragma once
#include <string>
#include <vector>
#include <utility>

struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal& operator[](const char* key) const;
    const JVal& operator[](size_t i) const;
    bool IsNull() const { return t == Null; }
    const std::string& Str_() const { return s; }
    long long Int() const { return (long long)n; }
};

bool JParse(const char* p, size_t n, JVal& out);
inline bool JParse(const std::string& s, JVal& out) { return JParse(s.data(), s.size(), out); }

// Appends `in` as a quoted, escaped JSON string to `out`.
void JQuote(std::string& out, const std::string& in);
