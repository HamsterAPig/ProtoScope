#include "protoscope/scripting/script_host.hpp"

#include "script_host_api_module.hpp"
#include "script_host_lua_helpers.hpp"

namespace protoscope::scripting {

class CodecScriptHostApiModule final : public IScriptHostApiModule {
public:
    explicit CodecScriptHostApiModule(ScriptHost& host) : host_(host) {}

    std::string_view id() const override { return "codec_api_module"; }

    void registerApi(ScriptHostContextInternal& ctx, sol::table& proto) override
    {
        sol::table bitsApi = ctx.lua.create_table();
        bitsApi.set_function("count", [](std::uint32_t value) { return std::popcount(value); });
        proto["bits"] = bitsApi;
        proto.set_function("crc16_modbus", [](const sol::object& payload) -> std::uint16_t {
            std::string error;
            const auto bytes = script_host_lua::bytesFromLuaObject(payload, error);
            return bytes.has_value() ? protocol_utils::crc16Modbus(*bytes) : 0U;
        });
        proto.set_function("crc16_ccitt_false", [](const sol::object& payload) -> std::uint16_t {
            std::string error;
            const auto bytes = script_host_lua::bytesFromLuaObject(payload, error);
            return bytes.has_value() ? protocol_utils::crc16CcittFalse(*bytes) : 0U;
        });
        proto.set_function("crc32_ieee", [](const sol::object& payload) -> std::uint32_t {
            std::string error;
            const auto bytes = script_host_lua::bytesFromLuaObject(payload, error);
            return bytes.has_value() ? protocol_utils::crc32Ieee(*bytes) : 0U;
        });
    }

private:
    ScriptHost& host_;
};

std::unique_ptr<IScriptHostApiModule> makeCodecApiModule(ScriptHost& host)
{
    return std::make_unique<CodecScriptHostApiModule>(host);
}

} // namespace protoscope::scripting
