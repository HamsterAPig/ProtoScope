#include "script_file_system_facade.hpp"

#include "script_host_internal.hpp"
#include "script_host_lua_helpers.hpp"

#include <algorithm>
#include <filesystem>
#include <tuple>

namespace protoscope::scripting {

std::optional<ScriptHost::FsOpenRequest> ScriptHost::parseFsOpenRequest(const std::string& pathText,
                                                                        const sol::object& opts,
                                                                        std::string& error) const
{
    FsOpenRequest request;
    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            error = "opts 必须是 table";
            return std::nullopt;
        }
        const auto table = opts.as<sol::table>();
        request.mode = luaStringField(table, "mode").value_or(request.mode);
        request.createDirs = luaBoolField(table, "create_dirs").value_or(false);
        request.overwrite = luaBoolField(table, "overwrite").value_or(false);
    }

    request.writeMode = request.mode == "write" || request.mode == "append";
    request.readMode = request.mode == "read";
    if (!request.readMode && !request.writeMode) {
        error = "mode 必须是 read/write/append";
        return std::nullopt;
    }

    request.path = std::filesystem::path(pathText);
    if (request.path.is_relative() && !protocolDirectory_.empty()) {
        request.path = std::filesystem::path(protocolDirectory_) / request.path;
    }
    request.path = canonicalPath(request.path);
    return request;
}

bool ScriptHost::isFsPathAuthorized(const std::filesystem::path& path, bool writeAccess) const
{
    if (fileIoConfig_.allowProtocolDir && !protocolDirectory_.empty() &&
        isSameOrChildPath(canonicalPath(protocolDirectory_), path, true)) {
        return true;
    }
    for (const auto& root : fileIoConfig_.extraAllowedRoots) {
        if (!root.empty() && isSameOrChildPath(canonicalPath(root), path, true)) {
            return true;
        }
    }
    for (const auto& root : dialogAuthorizedPaths_) {
        if ((!writeAccess || root.writable) && (writeAccess || root.readable) &&
            isSameOrChildPath(root.path, path, root.recursive)) {
            return true;
        }
    }
    return false;
}

bool ScriptHost::validateFsOpenRequest(const FsOpenRequest& request, std::string& error) const
{
    if (!isFsPathAuthorized(request.path, request.writeMode)) {
        error = "路径未授权: " + request.path.generic_string();
        return false;
    }

    std::error_code errorCode;
    if (request.readMode) {
        if (!std::filesystem::is_regular_file(request.path, errorCode)) {
            error = "文件不存在或不是普通文件: " + request.path.generic_string();
            return false;
        }
        const auto size = std::filesystem::file_size(request.path, errorCode);
        if (errorCode) {
            error = "读取文件大小失败: " + errorCode.message();
            return false;
        }
        if (size > fileIoConfig_.maxFileSizeBytes) {
            error = "文件大小超过 max_file_size_bytes";
            return false;
        }
        return true;
    }

    if (request.createDirs) {
        std::filesystem::create_directories(request.path.parent_path(), errorCode);
        if (errorCode) {
            error = "创建目录失败: " + errorCode.message();
            return false;
        }
    }
    if (request.mode == "write" && std::filesystem::exists(request.path, errorCode) && !request.overwrite) {
        error = "文件已存在，需设置 overwrite=true";
        return false;
    }
    return true;
}

std::unique_ptr<ScriptHost::FileHandle> ScriptHost::createFsOpenHandle(const FsOpenRequest& request, std::string& error)
{
    auto handle = std::make_unique<FileHandle>();
    handle->id = nextFileHandleId();
    handle->path = request.path;
    handle->readable = request.readMode;
    handle->writable = request.writeMode;

    auto openMode = std::ios::binary;
    if (request.readMode) {
        openMode |= std::ios::in;
    } else {
        openMode |= std::ios::out;
        openMode |= request.mode == "append" ? std::ios::app : std::ios::trunc;
    }
    handle->stream.open(request.path, openMode);
    if (!handle->stream.is_open()) {
        error = "打开文件失败: " + request.path.generic_string();
        return nullptr;
    }
    return handle;
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsOpen(sol::state_view lua,
                                                             const std::string& pathText,
                                                             const sol::object& opts)
{
    auto fail = [lua](const std::string& error) { return script_host_lua::luaNilError(lua, error); };
    if (!fileIoConfig_.enabled) {
        return fail("scripting.file_io 已禁用");
    }
    if (fileHandles_.size() >= fileIoConfig_.maxOpenFiles) {
        return fail("打开文件数超过 max_open_files");
    }

    std::string error;
    auto request = parseFsOpenRequest(pathText, opts, error);
    if (!request.has_value()) {
        return fail(error);
    }
    if (!validateFsOpenRequest(*request, error)) {
        return fail(error);
    }
    auto handle = createFsOpenHandle(*request, error);
    if (handle == nullptr) {
        return fail(error);
    }

    const auto id = handle->id;
    fileHandles_[id] = std::move(handle);
    return script_host_lua::luaValueOk(lua, id);
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsRead(sol::state_view lua,
                                                             std::uint64_t handleId,
                                                             const sol::object& opts)
{
    auto fail = [lua](const std::string& error) { return script_host_lua::luaNilError(lua, error); };
    const auto iter = fileHandles_.find(handleId);
    if (iter == fileHandles_.end() || !iter->second->readable) {
        return fail("文件句柄不可读或已关闭");
    }

    std::size_t maxBytes = fileIoConfig_.defaultChunkBytes;
    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            return fail("opts 必须是 table");
        }
        const sol::object value = opts.as<sol::table>()["max_bytes"];
        if (value.valid() && value.is<int>()) {
            maxBytes = static_cast<std::size_t>(std::max(1, value.as<int>()));
        }
    }
    maxBytes = std::min(positiveSizeOrDefault(maxBytes, fileIoConfig_.defaultChunkBytes), fileIoConfig_.maxChunkBytes);

    ProtoBuffer buffer;
    buffer.bytes.resize(maxBytes);
    auto& stream = iter->second->stream;
    stream.read(reinterpret_cast<char*>(buffer.bytes.data()), static_cast<std::streamsize>(buffer.bytes.size()));
    const auto readCount = stream.gcount();
    if (readCount <= 0) {
        return fail("eof");
    }
    buffer.bytes.resize(static_cast<std::size_t>(readCount));
    return script_host_lua::luaValueOk(lua, std::move(buffer));
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsWrite(sol::state_view lua,
                                                              std::uint64_t handleId,
                                                              const sol::object& payload)
{
    auto fail = [lua](const std::string& error) { return script_host_lua::luaOkResult(lua, false, error); };
    const auto iter = fileHandles_.find(handleId);
    if (iter == fileHandles_.end() || !iter->second->writable) {
        return fail("文件句柄不可写或已关闭");
    }
    std::string error;
    const auto bytes = bytesFromLuaObject(payload, error);
    if (!bytes.has_value()) {
        return fail(error);
    }
    auto& handle = *iter->second;
    if (handle.bytesWritten + bytes->size() > fileIoConfig_.maxWriteFileSizeBytes) {
        return fail("写入大小超过 max_write_file_size_bytes");
    }
    handle.stream.write(reinterpret_cast<const char*>(bytes->data()), static_cast<std::streamsize>(bytes->size()));
    if (!handle.stream.good()) {
        return fail("写入文件失败");
    }
    handle.bytesWritten += bytes->size();
    return script_host_lua::luaOkResult(lua, true, std::string{});
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsClose(sol::state_view lua, std::uint64_t handleId)
{
    const auto iter = fileHandles_.find(handleId);
    if (iter == fileHandles_.end()) {
        return script_host_lua::luaOkResult(lua, false, "文件句柄已关闭");
    }
    fileHandles_.erase(iter);
    return script_host_lua::luaOkResult(lua, true, std::string{});
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsStat(sol::state_view lua, const std::string& pathText)
{
    auto fail = [lua](const std::string& error) { return script_host_lua::luaNilError(lua, error); };
    if (!fileIoConfig_.enabled) {
        return fail("scripting.file_io 已禁用");
    }
    auto path = std::filesystem::path(pathText);
    if (path.is_relative() && !protocolDirectory_.empty()) {
        path = std::filesystem::path(protocolDirectory_) / path;
    }
    path = canonicalPath(path);
    const bool authorized =
        (fileIoConfig_.allowProtocolDir && !protocolDirectory_.empty() &&
         isSameOrChildPath(canonicalPath(protocolDirectory_), path, true)) ||
        std::any_of(fileIoConfig_.extraAllowedRoots.begin(),
                    fileIoConfig_.extraAllowedRoots.end(),
                    [&](const std::string& root) {
                        return !root.empty() && isSameOrChildPath(canonicalPath(root), path, true);
                    }) ||
        std::any_of(dialogAuthorizedPaths_.begin(), dialogAuthorizedPaths_.end(), [&](const AuthorizedPath& root) {
            return root.readable && isSameOrChildPath(root.path, path, root.recursive);
        });
    if (!authorized) {
        return fail("路径未授权: " + path.generic_string());
    }

    std::error_code errorCode;
    const bool exists = std::filesystem::exists(path, errorCode);
    if (errorCode) {
        return fail("检测路径存在性失败: " + errorCode.message());
    }
    if (!exists) {
        return fail("路径不存在: " + path.generic_string());
    }
    const bool regularFile = std::filesystem::is_regular_file(path, errorCode);
    if (errorCode) {
        return fail("读取文件类型失败: " + errorCode.message());
    }
    std::uint64_t fileSize = 0;
    if (regularFile) {
        const auto size = std::filesystem::file_size(path, errorCode);
        if (errorCode) {
            return fail("读取文件大小失败: " + errorCode.message());
        }
        fileSize = static_cast<std::uint64_t>(size);
    }
    const bool directory = std::filesystem::is_directory(path, errorCode);
    if (errorCode) {
        return fail("读取目录类型失败: " + errorCode.message());
    }
    sol::table table = lua.create_table();
    table["size"] = fileSize;
    table["mtime_ms"] = 0;
    table["is_file"] = regularFile;
    table["is_dir"] = directory;
    return script_host_lua::luaValueOk(lua, table);
}

std::tuple<sol::object, sol::object> ScriptHost::protoFsSendFile(sol::state_view lua,
                                                                 const std::string& pathText,
                                                                 const sol::object& opts)
{
    auto fail = [lua](const std::string& error) { return script_host_lua::luaNilError(lua, error); };
    std::string kind = "send";
    std::string tag = "file";
    std::size_t chunkSize = fileIoConfig_.sendFile.defaultChunkBytes;
    if (opts.valid() && opts.get_type() != sol::type::lua_nil) {
        if (!opts.is<sol::table>()) {
            return fail("opts 必须是 table");
        }
        const auto table = opts.as<sol::table>();
        kind = luaStringField(table, "kind").value_or(kind);
        tag = luaStringField(table, "tag").value_or(tag);
        const sol::object chunk = table["chunk_size"];
        if (chunk.valid() && chunk.is<int>()) {
            chunkSize = static_cast<std::size_t>(std::max(1, chunk.as<int>()));
        }
    }
    if (kind != "send" && kind != "request") {
        return fail("kind 必须是 send/request");
    }
    chunkSize = std::min(chunkSize, fileIoConfig_.maxChunkBytes);
    const auto [handleObject, openError] = protoFsOpen(lua, pathText, sol::make_object(lua, sol::lua_nil));
    if (!handleObject.valid() || handleObject.get_type() == sol::type::lua_nil) {
        return std::make_tuple(sol::make_object(lua, sol::lua_nil), openError);
    }

    const auto handleId = handleObject.as<std::uint64_t>();
    std::error_code sizeError;
    const auto total = std::filesystem::file_size(fileHandles_[handleId]->path, sizeError);
    if (sizeError) {
        protoFsClose(lua, handleId);
        return fail("读取文件大小失败: " + sizeError.message());
    }
    const auto jobId = nextFileJobId();
    fileSendJobs_[jobId] = FileSendJob{
        .id = jobId,
        .handleId = handleId,
        .kind = kind == "request" ? TxRequestKind::Request : TxRequestKind::Send,
        .tag = tag,
        .chunkSize = chunkSize,
        .total = total,
    };
    pumpFileSendJob(jobId);
    return script_host_lua::luaValueOk(lua, jobId);
}

void ScriptHost::pumpFileSendJob(std::uint64_t jobId)
{
    auto jobIter = fileSendJobs_.find(jobId);
    if (jobIter == fileSendJobs_.end()) {
        return;
    }
    auto& job = jobIter->second;
    const std::size_t maxInflight = std::max<std::size_t>(1, fileIoConfig_.sendFile.maxInflightChunks);
    while (!job.eof && job.inflight < maxInflight) {
        sol::table readOpts = runtime_->lua.create_table();
        readOpts["max_bytes"] = static_cast<int>(job.chunkSize);
        const auto offset = job.nextOffset;
        const auto [bufferObject, readError] =
            protoFsRead(luaView(), job.handleId, sol::make_object(runtime_->lua, readOpts));
        if (!bufferObject.valid() || bufferObject.get_type() == sol::type::lua_nil) {
            job.eof = true;
            break;
        }

        std::string error;
        const auto nilObject = sol::make_object(runtime_->lua, sol::lua_nil);
        const auto request = protoSendLike(job.kind, bufferObject, nilObject, error);
        if (!request.has_value()) {
            protoLog("error", "proto.fs.send_file 发送分块失败: " + error);
            job.eof = true;
            break;
        }
        if (!job.tag.empty()) {
            txRequests_.back().tag = job.tag;
        }
        txRequests_.back().fileJobId = job.id;
        txRequests_.back().fileOffset = offset;
        txRequests_.back().fileTotal = job.total;
        job.nextOffset += txRequests_.back().payload.size();
        ++job.inflight;
    }

    if (job.eof && job.inflight == 0) {
        const auto handleId = job.handleId;
        fileSendJobs_.erase(jobIter);
        protoFsClose(luaView(), handleId);
    }
}

} // namespace protoscope::scripting
