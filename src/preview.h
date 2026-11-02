// Native Markdown preview: md4c parser -> DirectWrite layout -> Direct2D drawing.
#pragma once
#include "app.h"

HWND Preview_Create(HWND parent);
void Preview_SetMarkdown(HWND h, const char* text, size_t len, const std::wstring& baseDir);
void Preview_SyncToSource(HWND h, size_t byteOffset);
void Preview_ThemeChanged(HWND h);
void Preview_Clear(HWND h);
size_t Preview_TopSource(HWND h);
void Preview_SetZoom(HWND h, float zoom);
float Preview_GetZoom(HWND h);
void Preview_Copy(HWND h);
void Preview_SelectAll(HWND h);
