#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

struct lua_State;

namespace protoscope::scripting {

struct LuaExecutionState {
    std::atomic_bool* stop{nullptr};
    std::chrono::steady_clock::time_point deadline;
    const char* failure{nullptr};
};

class LuaExecutionScope {
public:
    LuaExecutionScope(lua_State* lua, LuaExecutionState& state, std::atomic_bool& stop, std::uint64_t timeoutMs);
    ~LuaExecutionScope();
    LuaExecutionScope(const LuaExecutionScope&) = delete;
    LuaExecutionScope& operator=(const LuaExecutionScope&) = delete;

private:
    lua_State* lua_;
};

} // namespace protoscope::scripting
