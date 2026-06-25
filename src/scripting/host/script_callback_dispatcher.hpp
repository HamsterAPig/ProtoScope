#pragma once

#include "script_host_service.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace protoscope::scripting {

class IScriptCallbackDispatcher : public IScriptHostService {
public:
    virtual void onOpen(ScriptHostContextInternal& ctx, const transport::ConnectionContext& connection) = 0;
    virtual void onClose(ScriptHostContextInternal& ctx, const transport::ConnectionContext& connection) = 0;
    virtual void onError(ScriptHostContextInternal& ctx, const std::string& message) = 0;
    virtual void onBytes(ScriptHostContextInternal& ctx, const std::vector<std::uint8_t>& bytes) = 0;
    virtual void onControl(ScriptHostContextInternal& ctx, const std::string& id, const ControlValue& value) = 0;
    virtual bool onOscilloscopeToggle(ScriptHostContextInternal& ctx, bool currentRunning, bool targetRunning) = 0;
    virtual void onTx(ScriptHostContextInternal& ctx, const TxEvent& event) = 0;
    virtual void onDialog(ScriptHostContextInternal& ctx, const DialogEvent& event) = 0;
    virtual void onFileDialog(ScriptHostContextInternal& ctx, const FileDialogEvent& event) = 0;
};

} // namespace protoscope::scripting
