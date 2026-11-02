// "Rewrite with Claude" panel docked below the editor.
#pragma once
#include "app.h"

HWND AiPanel_Create(HWND parent);
void AiPanel_Begin();               // take the editor selection and open the panel
bool AiPanel_Visible();
int  AiPanel_Height();
void AiPanel_OnDocSwitch();
void AiPanel_OnDocClosed(int docId);
void AiPanel_ThemeChanged();
void AiPanel_FontChanged();
bool AiPanel_PreTranslate(MSG* msg);
void AiPanel_Shutdown();
