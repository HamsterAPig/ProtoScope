#include "script_host_internal.hpp"

namespace protoscope::scripting {
void ScriptHost::setStorageRoot(std::filesystem::path root)
{
    storageRoot_ = std::move(root);
}

void ScriptHost::pollStorageCompletions()
{
    if (!runtime_ || !runtime_->data) return;
    for (const auto& event : runtime_->data->poll()) {
        const bool kv = event.operation == "set" || event.operation == "delete" || event.operation == "flush";
        const auto callback = resolveGlobalCallback(kv ? "on_kv" : "on_record");
        if (!callback) continue;
        CallbackScope execution(*this);
        try {
            sol::state_view lua(runtime_->lua);
            auto context = activeConnection_.value_or(transport::ConnectionContext{});
            auto result = (*callback)(makeContextTable(lua, context), runtime_->data->completionTable(lua, event));
            if (!result.valid()) protoLog("error", protectedCallError(result));
        } catch (const std::exception& error) {
            protoLog("error", error.what());
        }
        if (executionFaulted()) break;
    }
}
} // namespace protoscope::scripting
