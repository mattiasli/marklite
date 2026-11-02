// Claude Messages API client (raw HTTPS via WinHTTP, server-sent-event streaming).
#pragma once
#include "app.h"

struct AiRequest {
    std::wstring apiKey;
    std::wstring model;
    std::wstring effort;           // low / medium / high / xhigh / max
    std::string selection;         // UTF-8, whitespace-trimmed
    std::string instruction;       // may be empty
    std::string document;          // full document (empty = don't send)
    std::string before, after;     // short snippets around the selection
};

enum AiEventType { AI_STARTED, AI_THINKING, AI_DELTA, AI_FALLBACK, AI_DONE, AI_ERROR };

struct AiEvent {
    AiEventType type;
    std::string text;              // delta text (UTF-8)
    std::wstring info;             // model id / error message / stop reason
    long long inTokens = 0, outTokens = 0, cacheRead = 0;
};

struct ModelList {
    bool ok = false;
    std::wstring error;
    std::vector<ModelInfo> models;
};

// Starts a streaming request on a worker thread. Events are posted to `notify`
// as WM_APP_AI (wParam = returned job id, lParam = AiEvent*).
int  Ai_Start(HWND notify, const AiRequest& req);
void Ai_Cancel(int jobId);
void Ai_CancelAll();

// Fetches GET /v1/models; posts WM_APP_MODELS (lParam = ModelList*) to `notify`.
void Ai_FetchModels(HWND notify, const std::wstring& apiKey);
