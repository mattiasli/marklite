// Custom-drawn tab strip (light/dark aware). Sends WM_APP_TAB to its parent.
#pragma once
#include "app.h"

struct TabItem { std::wstring title, tooltip; bool modified = false; };

HWND Tabs_Create(HWND parent);
void Tabs_Set(HWND h, const std::vector<TabItem>& items, int active);
int  Tabs_Height(HWND h);
void Tabs_ThemeChanged(HWND h);
