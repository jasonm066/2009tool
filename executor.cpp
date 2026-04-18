#include "executor.h"
#include "roblox.h"
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <vector>

namespace executor {

static LuaAPI*               g_api = nullptr;
static std::mutex            g_qMtx;
static std::vector<std::string> g_queue;
static std::mutex            g_logMtx;
static std::deque<LogEntry>  g_log;
constexpr size_t MAX_LOG = 200;

void Init(LuaAPI* api) { g_api = api; }

void Log(const char* fmt, ...) {
    char buf[2048];
    va_list va; va_start(va, fmt); vsnprintf(buf, sizeof(buf), fmt, va); va_end(va);
    bool isErr = strstr(buf, "error") || strstr(buf, "FATAL") || strstr(buf, "fail");
    {
        std::lock_guard<std::mutex> lk(g_logMtx);
        g_log.push_back({buf, isErr});
        while (g_log.size() > MAX_LOG) g_log.pop_front();
    }
    OutputDebugStringA("[exec] "); OutputDebugStringA(buf); OutputDebugStringA("\n");
}

std::deque<LogEntry> SnapshotLog() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    return g_log;
}

void ClearLog() {
    std::lock_guard<std::mutex> lk(g_logMtx);
    g_log.clear();
}

void QueueScript(std::string src) {
    std::lock_guard<std::mutex> lk(g_qMtx);
    g_queue.push_back(std::move(src));
}

static void RunOne(const std::string& src) {
    if (!g_api) { Log("api not init"); return; }

    uintptr_t sc = GetScriptContext(g_api->baseAddr);
    if (!sc) { Log("ScriptContext not resolved"); return; }

    lua_State* L = SafeDeref<lua_State*>(sc + offsets::SC_LuaState, nullptr);
    if (!L) { Log("lua_State null"); return; }

    int top0 = -1;
    __try { top0 = g_api->gettop(L); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("FATAL: gettop crashed"); return; }

    int rc = -1;
    __try { rc = g_api->loadbuffer(L, src.data(), src.size(), "=script"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("FATAL: loadbuffer crashed"); return; }

    if (rc != 0) {
        const char* err = nullptr;
        __try { err = g_api->tolstring(L, -1, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("compile error: %s", err ? err : "<unreadable>");
        __try { g_api->settop(L, top0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    int cr = -1;
    __try { cr = g_api->pcall(L, 0, 0, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("FATAL: pcall crashed"); return; }

    if (cr != 0) {
        const char* err = nullptr;
        __try { err = g_api->tolstring(L, -1, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        Log("runtime error: %s", err ? err : "<unreadable>");
    } else {
        Log("script executed OK (%zu bytes)", src.size());
    }
    __try { g_api->settop(L, top0); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

void Tick() {
    std::vector<std::string> work;
    {
        std::lock_guard<std::mutex> lk(g_qMtx);
        if (g_queue.empty()) return;
        work.swap(g_queue);
    }
    for (auto& s : work) RunOne(s);
}

} // namespace executor
