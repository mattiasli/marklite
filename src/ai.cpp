#include "ai.h"
#include "json.h"
#include <winhttp.h>
#include <map>
#include <memory>
#include <mutex>
#include <atomic>

namespace {

const char* kSystem =
    "You are a writing assistant built into a Markdown editor. The user has selected a passage of their "
    "document and wants it rewritten.\n\n"
    "Rules:\n"
    "- Reply with the replacement text only. Your entire reply is inserted verbatim in place of the selection, "
    "so do not add any preamble, explanation, quotation marks or code fences around it.\n"
    "- Keep it valid Markdown and keep the passage's formatting conventions (headings, lists, emphasis, links, "
    "line breaks) unless the instruction asks you to change them.\n"
    "- Write in the same language as the selected passage unless the instruction asks for another language.\n"
    "- If no instruction is given, improve clarity, grammar, wording and flow while preserving the meaning, tone "
    "and approximate length.\n"
    "- Any surrounding text you are shown is context only. Rewrite just the selected passage so that it fits "
    "seamlessly with the text before and after it.";

struct Job {
    int id = 0;
    HWND notify = nullptr;
    AiRequest req;
    std::atomic<bool> cancelled{false};
    std::mutex m;
    HINTERNET hReq = nullptr;
};

std::mutex g_jobsMutex;
std::map<int, std::shared_ptr<Job>> g_jobs;
std::atomic<int> g_nextId{1};

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

bool IsModernModel(const std::string& m) {
    return StartsWith(m, "claude-opus-5") || StartsWith(m, "claude-sonnet-5") || StartsWith(m, "claude-haiku-5") ||
           StartsWith(m, "claude-fable") || StartsWith(m, "claude-mythos") || StartsWith(m, "claude-opus-4-6") ||
           StartsWith(m, "claude-opus-4-7") || StartsWith(m, "claude-opus-4-8") || StartsWith(m, "claude-sonnet-4-6");
}
bool SupportsEffort(const std::string& m) { return IsModernModel(m) || StartsWith(m, "claude-opus-4-5"); }
// The effort level to send: older models don't accept every level ("xhigh" arrived with Opus 4.7,
// "max" with Opus 4.6), and an unsupported one makes every request fail with 400, so step down to
// the nearest level the model has.
std::string EffortFor(const std::string& m, const std::string& e) {
    if (StartsWith(m, "claude-opus-4-5")) return (e == "xhigh" || e == "max") ? "high" : e;
    if (StartsWith(m, "claude-opus-4-6") || StartsWith(m, "claude-sonnet-4-6")) return e == "xhigh" ? "high" : e;
    return e;
}
bool SupportsFallback(const std::string& m) {
    return StartsWith(m, "claude-opus-5") || m == "claude-sonnet-5-5" || StartsWith(m, "claude-fable-5-1") ||
           StartsWith(m, "claude-mythos-5-1");
}
int MaxTokensFor(const std::string& m) {
    if (IsModernModel(m)) return 64000;
    if (StartsWith(m, "claude-opus-4") || StartsWith(m, "claude-sonnet-4") || StartsWith(m, "claude-haiku-4") ||
        StartsWith(m, "claude-3-7"))
        return 32000;
    return 4096;
}

std::string BuildBody(const AiRequest& r, bool& fallback) {
    std::string m = WToU8(r.model);
    std::string b = "{\"model\":";
    JQuote(b, m);
    b += ",\"max_tokens\":" + std::to_string(MaxTokensFor(m));
    b += ",\"stream\":true";
    if (SupportsEffort(m) && !r.effort.empty()) {
        b += ",\"output_config\":{\"effort\":";
        JQuote(b, EffortFor(m, WToU8(r.effort)));
        b += "}";
    }
    fallback = SupportsFallback(m);
    if (fallback) b += ",\"fallbacks\":\"default\"";
    b += ",\"system\":";
    JQuote(b, kSystem);
    b += ",\"messages\":[{\"role\":\"user\",\"content\":[";
    if (!r.document.empty()) {
        std::string doc = "The full document, for context only (do not rewrite it):\n<document>\n" + r.document + "\n</document>";
        b += "{\"type\":\"text\",\"text\":";
        JQuote(b, doc);
        b += ",\"cache_control\":{\"type\":\"ephemeral\"}},";
    }
    std::string task = "<selected_text>\n" + r.selection + "\n</selected_text>\n\n";
    if (!r.before.empty()) task += "Text immediately before the selection:\n<before>\n" + r.before + "\n</before>\n\n";
    if (!r.after.empty()) task += "Text immediately after the selection:\n<after>\n" + r.after + "\n</after>\n\n";
    if (r.instruction.empty())
        task += "No specific instruction was given: improve the selected text as described in your rules.\n\n";
    else
        task += "Instruction: " + r.instruction + "\n\n";
    task += "Reply with the replacement for the selected text only.";
    b += "{\"type\":\"text\",\"text\":";
    JQuote(b, task);
    b += "}]}]}";
    return b;
}

std::wstring WinHttpErrorText(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return L"Could not resolve api.anthropic.com - check your internet connection.";
    case ERROR_WINHTTP_CANNOT_CONNECT: return L"Could not connect to api.anthropic.com.";
    case ERROR_WINHTTP_TIMEOUT: return L"The request timed out.";
    case ERROR_WINHTTP_CONNECTION_ERROR: return L"The connection was reset.";
    case ERROR_WINHTTP_SECURE_FAILURE: return L"Secure connection failed (TLS).";
    default: return L"Network error " + std::to_wstring(e) + L".";
    }
}

std::wstring HttpErrorText(DWORD status, const std::string& body) {
    std::wstring msg;
    JVal j;
    if (JParse(body, j)) msg = U8ToW(j["error"]["message"].s);
    std::wstring head;
    switch (status) {
    case 400: head = L"Bad request"; break;
    case 401: head = L"Invalid API key"; break;
    case 403: head = L"Permission denied"; break;
    case 404: head = L"Not found (unknown model?)"; break;
    case 413: head = L"Request too large"; break;
    case 429: head = L"Rate limited"; break;
    case 500: head = L"Anthropic server error"; break;
    case 529: head = L"Anthropic API is overloaded"; break;
    default: head = L"HTTP " + std::to_wstring(status);
    }
    head += L" (" + std::to_wstring(status) + L")";
    return msg.empty() ? head : head + L": " + msg;
}

struct Conn {
    HINTERNET ses = nullptr, con = nullptr;
    ~Conn() {
        if (con) WinHttpCloseHandle(con);
        if (ses) WinHttpCloseHandle(ses);
    }
    bool Open() {
        ses = WinHttpOpen(L"MarkLite/1.0", 4 /*WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY*/, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) ses = WinHttpOpen(L"MarkLite/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        if (!ses) return false;
        WinHttpSetTimeouts(ses, 15000, 15000, 30000, 180000);
        // Never follow redirects: the API key header must only ever reach api.anthropic.com.
        DWORD never = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(ses, WINHTTP_OPTION_REDIRECT_POLICY, &never, sizeof(never));
        con = WinHttpConnect(ses, L"api.anthropic.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        return con != nullptr;
    }
};

std::wstring Headers(const std::wstring& key, bool json, bool fallback) {
    std::wstring h = L"x-api-key: " + key + L"\r\nanthropic-version: 2026-10-21\r\n";
    if (json) h += L"content-type: application/json\r\naccept: text/event-stream\r\n";
    if (fallback) h += L"anthropic-beta: server-side-fallback-2026-07-01\r\n";
    return h;
}

DWORD StatusOf(HINTERNET r) {
    DWORD status = 0, sz = sizeof(status);
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &sz, WINHTTP_NO_HEADER_INDEX);
    return status;
}

int RetryAfterMs(HINTERNET r, int attempt) {
    wchar_t buf[32];
    DWORD sz = sizeof(buf);
    if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CUSTOM, L"retry-after", buf, &sz, WINHTTP_NO_HEADER_INDEX)) {
        int s = _wtoi(buf);
        if (s > 0) return std::min(s, 10) * 1000;
    }
    return 1000 << attempt;
}

std::string ReadAll(HINTERNET r) {
    std::string out;
    char buf[8192];
    DWORD got = 0;
    while (WinHttpReadData(r, buf, sizeof buf, &got) && got) out.append(buf, got);
    return out;
}

void Post(Job* job, AiEvent* e) {
    if (job->cancelled || !PostMessageW(job->notify, WM_APP_AI, (WPARAM)job->id, (LPARAM)e)) delete e;
}
void PostError(Job* job, const std::wstring& msg) {
    auto* e = new AiEvent{AI_ERROR};
    e->info = msg;
    Post(job, e);
}

#ifdef MARKLITE_FAKE_AI
// Test builds only (-DMARKLITE_FAKE_AI): simulate a streamed answer without any network access.
void FakeJob(std::shared_ptr<Job> job) {
    auto* s = new AiEvent{AI_STARTED};
    s->info = job->req.model;
    Post(job.get(), s);
    Sleep(300);
    Post(job.get(), new AiEvent{AI_THINKING});
    Sleep(500);
    const std::string nl(1, '\n');
    std::string out = "```markdown" + nl + "A *rewritten* version (" + job->req.instruction + "): " +
                      job->req.selection + nl + "```";
    for (size_t i = 0; i < out.size() && !job->cancelled; i += 7) {
        auto* d = new AiEvent{AI_DELTA};
        d->text = out.substr(i, 7);
        Post(job.get(), d);
        Sleep(25);
    }
    auto* e = new AiEvent{AI_DONE};
    e->info = L"end_turn";
    e->inTokens = 1234;
    e->outTokens = 56;
    e->cacheRead = 1000;
    Post(job.get(), e);
}
#endif

void RunJob(std::shared_ptr<Job> job) {
#ifdef MARKLITE_FAKE_AI
    FakeJob(job);
    return;
#endif
    Conn c;
    if (!c.Open()) { PostError(job.get(), WinHttpErrorText(GetLastError())); return; }
    bool fallback = false;
    std::string body = BuildBody(job->req, fallback);
    std::wstring headers = Headers(job->req.apiKey, true, fallback);

    for (int attempt = 0; attempt < 3 && !job->cancelled; attempt++) {
        HINTERNET r = WinHttpOpenRequest(c.con, L"POST", L"/v1/messages", nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        if (!r) { PostError(job.get(), WinHttpErrorText(GetLastError())); return; }
        { std::lock_guard<std::mutex> lk(job->m); job->hReq = r; }
        auto closeReq = [&] {
            std::lock_guard<std::mutex> lk(job->m);
            if (job->hReq) { WinHttpCloseHandle(job->hReq); job->hReq = nullptr; }
        };

        BOOL ok = WinHttpSendRequest(r, headers.c_str(), (DWORD)-1, (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0) &&
                  WinHttpReceiveResponse(r, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            closeReq();
            if (job->cancelled) return;
            if (attempt < 2 && (err == ERROR_WINHTTP_CONNECTION_ERROR || err == ERROR_WINHTTP_TIMEOUT)) { Sleep(1000 << attempt); continue; }
            PostError(job.get(), WinHttpErrorText(err));
            return;
        }
        DWORD status = StatusOf(r);
        if (status != 200) {
            std::string err = ReadAll(r);
            bool retry = status == 429 || status == 500 || status == 502 || status == 503 || status == 504 || status == 529;
            int wait = RetryAfterMs(r, attempt);
            closeReq();
            if (job->cancelled) return;
            if (retry && attempt < 2) {
                for (int t = 0; t < wait && !job->cancelled; t += 100) Sleep(100);
                continue;
            }
            PostError(job.get(), HttpErrorText(status, err));
            return;
        }

        // ---- stream
        std::string buf, pending;
        char chunk[16384];
        DWORD got = 0;
        DWORD lastPost = GetTickCount();
        long long inTok = 0, outTok = 0, cacheRead = 0;
        std::string stopReason;
        bool finished = false, thinkingPosted = false;
        auto flush = [&] {
            if (pending.empty()) return;
            auto* e = new AiEvent{AI_DELTA};
            e->text.swap(pending);
            Post(job.get(), e);
            lastPost = GetTickCount();
        };
        while (!job->cancelled) {
            if (!WinHttpReadData(r, chunk, sizeof chunk, &got)) {
                DWORD err = GetLastError();
                closeReq();
                if (!job->cancelled) { flush(); PostError(job.get(), WinHttpErrorText(err)); }
                return;
            }
            if (got == 0) break;
            buf.append(chunk, got);
            size_t pos;
            while ((pos = buf.find("\n\n")) != std::string::npos || (pos = buf.find("\r\n\r\n")) != std::string::npos) {
                size_t sepLen = buf.compare(pos, 4, "\r\n\r\n") == 0 ? 4 : 2;
                std::string ev = buf.substr(0, pos);
                buf.erase(0, pos + sepLen);
                std::string data;
                size_t ls = 0;
                while (ls < ev.size()) {
                    size_t le = ev.find('\n', ls);
                    if (le == std::string::npos) le = ev.size();
                    std::string line = ev.substr(ls, le - ls);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.rfind("data:", 0) == 0) {
                        size_t s = 5;
                        if (s < line.size() && line[s] == ' ') s++;
                        if (!data.empty()) data += '\n';
                        data += line.substr(s);
                    }
                    ls = le + 1;
                }
                if (data.empty()) continue;
                JVal j;
                if (!JParse(data, j)) continue;
                const std::string& type = j["type"].s;
                if (type == "message_start") {
                    const JVal& msg = j["message"];
                    inTok = msg["usage"]["input_tokens"].Int();
                    cacheRead = msg["usage"]["cache_read_input_tokens"].Int();
                    auto* e = new AiEvent{AI_STARTED};
                    e->info = U8ToW(msg["model"].s);
                    Post(job.get(), e);
                } else if (type == "content_block_start") {
                    const JVal& cb = j["content_block"];
                    if ((cb["type"].s == "thinking" || cb["type"].s == "redacted_thinking") && !thinkingPosted) {
                        thinkingPosted = true;
                        Post(job.get(), new AiEvent{AI_THINKING});
                    } else if (cb["type"].s == "fallback") {
                        flush();
                        auto* e = new AiEvent{AI_FALLBACK};
                        e->info = U8ToW(cb["to"]["model"].s);
                        Post(job.get(), e);
                    }
                } else if (type == "content_block_delta") {
                    const JVal& d = j["delta"];
                    if (d["type"].s == "text_delta") {
                        pending += d["text"].s;
                        if (GetTickCount() - lastPost > 30) flush();
                    }
                } else if (type == "message_delta") {
                    if (!j["delta"]["stop_reason"].IsNull()) stopReason = j["delta"]["stop_reason"].s;
                    if (!j["usage"]["output_tokens"].IsNull()) outTok = j["usage"]["output_tokens"].Int();
                    if (!j["usage"]["input_tokens"].IsNull()) inTok = j["usage"]["input_tokens"].Int();
                } else if (type == "message_stop") {
                    finished = true;
                } else if (type == "error") {
                    flush();
                    closeReq();
                    std::wstring m = U8ToW(j["error"]["message"].s);
                    std::wstring t = U8ToW(j["error"]["type"].s);
                    PostError(job.get(), (t == L"overloaded_error" ? L"Anthropic API is overloaded: " : L"API error: ") + m);
                    return;
                }
            }
        }
        closeReq();
        if (job->cancelled) return;
        flush();
        if (!finished && stopReason.empty()) { PostError(job.get(), L"The stream ended unexpectedly."); return; }
        auto* e = new AiEvent{AI_DONE};
        e->info = U8ToW(stopReason);
        e->inTokens = inTok;
        e->outTokens = outTok;
        e->cacheRead = cacheRead;
        Post(job.get(), e);
        return;
    }
}

DWORD WINAPI JobThread(LPVOID p) {
    auto* sp = (std::shared_ptr<Job>*)p;
    std::shared_ptr<Job> job = *sp;
    delete sp;
    RunJob(job);
    std::lock_guard<std::mutex> lk(g_jobsMutex);
    g_jobs.erase(job->id);
    return 0;
}

struct ModelsArgs { HWND notify; std::wstring key; };

DWORD WINAPI ModelsThread(LPVOID p) {
    std::unique_ptr<ModelsArgs> a((ModelsArgs*)p);
    auto* res = new ModelList();
    Conn c;
    if (!c.Open()) res->error = WinHttpErrorText(GetLastError());
    else {
        HINTERNET r = WinHttpOpenRequest(c.con, L"GET", L"/v1/models?limit=100", nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
        std::wstring h = Headers(a->key, false, false);
        if (r && WinHttpSendRequest(r, h.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = StatusOf(r);
            std::string body = ReadAll(r);
            if (status == 200) {
                JVal j;
                if (JParse(body, j)) {
                    for (auto& m : j["data"].a) res->models.push_back({U8ToW(m["id"].s), U8ToW(m["display_name"].s)});
                    res->ok = true;
                } else res->error = L"Unexpected response from the API.";
            } else res->error = HttpErrorText(status, body);
        } else res->error = WinHttpErrorText(GetLastError());
        if (r) WinHttpCloseHandle(r);
    }
    if (!PostMessageW(a->notify, WM_APP_MODELS, 0, (LPARAM)res)) delete res;
    return 0;
}
}

int Ai_Start(HWND notify, const AiRequest& req) {
    auto job = std::make_shared<Job>();
    job->id = g_nextId++;
    job->notify = notify;
    job->req = req;
    {
        std::lock_guard<std::mutex> lk(g_jobsMutex);
        g_jobs[job->id] = job;
    }
    auto* arg = new std::shared_ptr<Job>(job);
    HANDLE t = CreateThread(nullptr, 0, JobThread, arg, 0, nullptr);
    if (t) {
        CloseHandle(t);
    } else {   // no thread: report it instead of leaving the panel waiting forever
        delete arg;
        {
            std::lock_guard<std::mutex> lk(g_jobsMutex);
            g_jobs.erase(job->id);
        }
        PostError(job.get(), L"Couldn't start the request (Windows is low on resources).");
    }
    return job->id;
}

void Ai_Cancel(int id) {
    std::shared_ptr<Job> job;
    {
        std::lock_guard<std::mutex> lk(g_jobsMutex);
        auto it = g_jobs.find(id);
        if (it == g_jobs.end()) return;
        job = it->second;
    }
    job->cancelled = true;
    std::lock_guard<std::mutex> lk(job->m);
    if (job->hReq) { WinHttpCloseHandle(job->hReq); job->hReq = nullptr; }
}

void Ai_CancelAll() {
    std::vector<int> ids;
    {
        std::lock_guard<std::mutex> lk(g_jobsMutex);
        for (auto& kv : g_jobs) ids.push_back(kv.first);
    }
    for (int id : ids) Ai_Cancel(id);
}

void Ai_FetchModels(HWND notify, const std::wstring& apiKey) {
    auto* args = new ModelsArgs{notify, apiKey};
    HANDLE t = CreateThread(nullptr, 0, ModelsThread, args, 0, nullptr);
    if (t) CloseHandle(t);
    else delete args;
}
