// A very small test framework. TEST(name) { ... } registers a test; CHECK and CHECK_EQ report a
// failure and carry on, so one run shows everything that is wrong.
#pragma once
#include "app.h"
#include <string>
#include <vector>
#include <sstream>

struct TestCase {
    const char* name;
    void (*fn)();
};
std::vector<TestCase>& TestRegistry();
void TestFail(const char* file, int line, const std::string& what);

struct TestRegistrar {
    TestRegistrar(const char* name, void (*fn)()) { TestRegistry().push_back({name, fn}); }
};

#define TEST(name)                                       \
    static void test_##name();                           \
    static TestRegistrar reg_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) TestFail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
    } while (0)

std::string TestShow(const std::string& v);
std::string TestShow(const char* v);
std::string TestShow(long long v);
std::string TestShow(double v);
inline std::string TestShow(int v) { return TestShow((long long)v); }
inline std::string TestShow(long v) { return TestShow((long long)v); }
inline std::string TestShow(unsigned v) { return TestShow((long long)v); }
inline std::string TestShow(unsigned long long v) { return TestShow((long long)v); }
inline std::string TestShow(bool v) { return v ? "true" : "false"; }

#define CHECK_EQ(actual, expected)                                                             \
    do {                                                                                       \
        const auto& a_ = (actual);                                                             \
        const auto& e_ = (expected);                                                           \
        if (!(a_ == e_))                                                                       \
            TestFail(__FILE__, __LINE__,                                                       \
                     std::string(#actual) + "\n      got:      " + TestShow(a_) +              \
                         "\n      expected: " + TestShow(e_));                                 \
    } while (0)

// The editor tests share one hidden Scintilla control, set up like MarkLite's editor. Documents are
// written with "@" for the caret, or "{" and "}" around the selection (Markdown needs | and [ ]).
Sci& TestEditor();
void SetDoc(const std::string& markedText);   // sets the text and the caret or selection, then styles it
std::string GetDoc();                          // the text with the caret or selection marked the same way
void Restyle();
bool Run(int cmd);                             // runs a formatting command, then restyles
