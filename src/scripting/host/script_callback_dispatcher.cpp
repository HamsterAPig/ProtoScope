#include "script_callback_dispatcher.hpp"

namespace protoscope::scripting {

class LuaScriptCallbackDispatcher final : public IScriptCallbackDispatcher {
public:
    std::string_view id() const override { return "lua_script_callback_dispatcher"; }
    void onOpen(ScriptHostContextInternal&, const transport::ConnectionContext&) override {}
    void onClose(ScriptHostContextInternal&, const transport::ConnectionContext&) override {}
    void onError(ScriptHostContextInternal&, const std::string&) override {}
    void onBytes(ScriptHostContextInternal&, const std::vector<std::uint8_t>&) override {}
    void onControl(ScriptHostContextInternal&, const std::string&, const ControlValue&) override {}
    void onTx(ScriptHostContextInternal&, const TxEvent&) override {}
    void onDialog(ScriptHostContextInternal&, const DialogEvent&) override {}
    void onFileDialog(ScriptHostContextInternal&, const FileDialogEvent&) override {}
};

} // namespace protoscope::scripting
