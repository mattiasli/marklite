// The formatting and editor code calls a few services of MarkLite's main window. The tests link
// that code without main.cpp, so these stand in for them.
#include "test.h"

std::wstring g_lastStatus;

HINSTANCE App_Instance() { return GetModuleHandleW(nullptr); }
int App_ActiveDocId() { return 1; }
void App_Relayout() {}
void App_Status(const std::wstring& msg, bool) { g_lastStatus = msg; }
bool App_EnsureApiKey() { return false; }
void App_OpenSettings() {}
