// Find / replace bar docked above the editor.
#pragma once
#include "app.h"

HWND Find_Create(HWND parent, Sci* editor);
void Find_Show(bool replace);
void Find_Hide();
bool Find_Visible();
int  Find_Height();
void Find_Next(bool backwards);
void Find_Refresh();          // re-highlight matches (after edits / doc switch)
void Find_ThemeChanged();
bool Find_PreTranslate(MSG* msg);
