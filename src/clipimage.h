// Pictures on the clipboard: screenshots (Win+Shift+S), images copied from other programs, and
// image files copied in File Explorer.
#pragma once
#include "util.h"

bool Clip_HasText();
bool Clip_HasImage();                          // picture data (PNG or bitmap)
std::vector<std::wstring> Clip_ImageFiles();   // image files copied in File Explorer
bool Clip_HasPicture();                        // either of the two above

// Saves the clipboard picture as a new PNG file in `folder` (created if missing), named
// image-YYYYMMDD-HHMMSS.png. Returns the file's full path, or "" with `error` set.
std::wstring Clip_SaveImage(HWND owner, const std::wstring& folder, std::wstring& error);
