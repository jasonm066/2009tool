#pragma once
#include <string>
#include <deque>
#include <mutex>
#include "lua_api.h"

namespace executor {

void Init(LuaAPI* api);
void QueueScript(std::string src);
// Called on the main (game) thread once per frame; drains queue.
void Tick();

// Console log buffer for the menu UI.
struct LogEntry { std::string text; bool isError; };
std::deque<LogEntry> SnapshotLog();
void Log(const char* fmt, ...);
void ClearLog();

} // namespace executor
