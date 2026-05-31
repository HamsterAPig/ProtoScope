// 本文件由 script_host_core.cpp 包含，承接对应 Scripting 业务组件实现。

#if !defined(PROTOSCOPE_SCRIPT_HOST_COMPONENT_INCLUDE)
#error "This scripting component implementation is included by script_host_core.cpp"
#endif


bool ScriptHost::loadScriptFile(const std::string& path) {
    resetRuntime();

    scriptPath_ = path;
    const std::filesystem::path filePath(path);
    protocolDirectory_ = filePath.parent_path().generic_string();

    if (path.empty()) {
        setLastError("脚本路径为空");
        protoLog("error", lastError_);
        return false;
    }

    if (!std::filesystem::exists(filePath)) {
        setLastError("未找到脚本文件: " + path);
        protoLog("error", lastError_);
        return false;
    }

    runtime_ = std::make_unique<Runtime>();
    runtime_->lua.open_libraries(
        sol::lib::base,
        sol::lib::math,
        sol::lib::package,
        sol::lib::string,
        sol::lib::table,
        sol::lib::utf8,
        sol::lib::os);

    auto& lua = runtime_->lua;
    lua.new_usertype<ProtoBuffer>("ProtoBuffer",
                                  "size",
                                  &ProtoBuffer::size,
                                  "slice",
                                  &ProtoBuffer::slice,
                                  "to_hex",
                                  &ProtoBuffer::toHex,
                                  "bytes",
                                  [this](const ProtoBuffer& buffer, const sol::object& maxBytes) {
                                      std::size_t limit = buffer.bytes.size();
                                      if (maxBytes.valid() && maxBytes.get_type() != sol::type::lua_nil && maxBytes.is<int>()) {
                                          limit = std::min<std::size_t>(limit, static_cast<std::size_t>(std::max(0, maxBytes.as<int>())));
                                      }
                                      limit = std::min(limit, fileIoConfig_.maxChunkBytes);
                                      sol::table table = runtime_->lua.create_table(static_cast<int>(limit), 0);
                                      for (std::size_t index = 0; index < limit; ++index) {
                                          table[index + 1] = buffer.bytes[index];
                                      }
                                      return table;
                                  });

    // 将协议脚本目录加入 Lua 模块搜索路径，使 main.lua 可 require 同目录模块。
    // 额外放开父目录，方便 protocols/<demo>/main.lua 共享 protocols/*.lua 公共脚本。
    // 使用 generic_string 格式，统一为 /，避免 Windows 反斜杠干扰 Lua package.path。
    const auto pkgPath = lua["package"]["path"].get<std::string>();
    const auto protocolParent = std::filesystem::path(protocolDirectory_).parent_path().generic_string();
    lua["package"]["path"] = protocolDirectory_ + "/?.lua;"
                           + protocolDirectory_ + "/?/init.lua;"
                           + (protocolParent.empty() ? std::string() : protocolParent + "/?.lua;")
                           + (protocolParent.empty() ? std::string() : protocolParent + "/?/init.lua;")
                           + pkgPath;
    auto proto = lua.create_named_table("proto");

    // 核心流程：所有脚本侧能力统一经由模块注册器挂到 proto.*，避免加载流程继续膨胀。
    registerLuaApi(proto);

    auto scriptResult = lua.safe_script_file(path, &sol::script_pass_on_error);
    if (!scriptResult.valid()) {
        setLastError("执行脚本失败: " + protectedCallError(scriptResult));
        protoLog("error", lastError_);
        return false;
    }

    std::string streamError;
    auto streamSchema = parseLoadedStreamSchema(lua, streamError);
    if (!streamError.empty()) {
        setLastError(streamError);
        protoLog("error", lastError_);
        return false;
    }
    runtime_->stream = std::move(streamSchema);

    std::string parseError;
    const auto parsedDocks = parseDockDescriptors(lua, parseError);
    if (!parsedDocks.has_value()) {
        setLastError(parseError);
        protoLog("error", lastError_);
        return false;
    }

    docks_ = *parsedDocks;
    controls_.clear();
    std::unordered_map<std::string, ControlValue> nextControlValues;
    for (const auto& dock : docks_) {
        for (const auto& control : dock.controls) {
            controls_.push_back(control);
            const auto existing = controlValues_.find(control.id);
            nextControlValues[control.id] = existing == controlValues_.end() ? defaultValueFor(control) : existing->second;
        }
    }
    controlValues_ = std::move(nextControlValues);
    lastError_.clear();
    scriptLoaded_ = true;
    return true;
}

bool ScriptHost::loadProtocolDirectory(const std::string& directory) {
    const auto path = std::filesystem::path(directory) / "main.lua";
    return loadScriptFile(path.generic_string());
}
