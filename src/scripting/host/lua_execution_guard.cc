#include "lua_execution_guard.hpp"

#include <algorithm>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace protoscope::scripting {
namespace {
    void checkExecution(lua_State* lua, lua_Debug*)
    {
        // 钩子通过 Lua 错误退出，跨越此处不得持有需要析构的 C++ 局部对象。
        auto* state = *static_cast<LuaExecutionState**>(lua_getextraspace(lua));
        if (state->stop->load(std::memory_order_relaxed)) {
            state->failure = "Lua execution stopped";
        } else if (std::chrono::steady_clock::now() >= state->deadline) {
            state->failure = "Lua execution budget exceeded";
        }
        if (state->failure != nullptr) {
            luaL_error(lua, "%s", state->failure);
        }
    }
}

LuaExecutionScope::LuaExecutionScope(lua_State* lua, LuaExecutionState& state,
                                     std::atomic_bool& stop, std::uint64_t timeoutMs)
    : lua_(lua)
{
    state.stop = &stop;
    state.deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(std::clamp<std::uint64_t>(timeoutMs, 1, 3600000));
    *static_cast<LuaExecutionState**>(lua_getextraspace(lua)) = &state;
    lua_sethook(lua, checkExecution, LUA_MASKCOUNT, 1000);
}

LuaExecutionScope::~LuaExecutionScope()
{
    lua_sethook(lua_, nullptr, 0, 0);
}

} // namespace protoscope::scripting
