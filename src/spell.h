// Spell checking through the Windows Spell Checking API (ISpellChecker).
// A word is accepted if ANY of the enabled languages accepts it.
#pragma once
#include "app.h"

struct SpellLang { std::wstring tag, name; };

namespace Spell {
bool Init();
const std::vector<SpellLang>& Supported();
std::vector<std::wstring> DefaultLanguages();
void SetLanguages(const std::vector<std::wstring>& tags);
const std::vector<std::wstring>& Active();
std::wstring NameOf(const std::wstring& tag);

bool IsCorrect(const std::wstring& word);
std::vector<std::wstring> Suggest(const std::wstring& word, size_t max);
void Add(const std::wstring& word, size_t langIndex);
void Ignore(const std::wstring& word);

// Re-checks the visible part of the document and updates the squiggles.
// `typing` suppresses marking the word currently being typed at the caret.
void CheckVisible(Sci& s, bool typing);
void Clear(Sci& s);
bool MisspelledAt(Sci& s, sptr_t pos, sptr_t& a, sptr_t& b);
}
