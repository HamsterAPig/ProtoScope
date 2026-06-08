-- 核心流程：模板只声明宿主已支持的 Dock UI，文件授权和读取由 proto.fs.* 异步完成。
function ui()
    return {
        {
            id = "file_dialog_template",
            title = "文件对话框模板",
            anchor = "left_bottom",
            tab_group = "protocol_templates",
            controls = {
                { id = "open_file", type = "button", label = "选择文件" },
                { id = "open_dir", type = "button", label = "选择目录" },
                { id = "read_size", type = "input_int", label = "读取字节", default = 64 },
            },
            layout = {
                type = "flow",
                children = {
                    { type = "control", id = "open_file" },
                    { type = "control", id = "open_dir" },
                    { type = "control", id = "read_size" },
                },
            },
        },
    }
end

local selected_file = nil

function on_control(ctx, id, value)
    if id == "open_file" and value == true then
        proto.fs.open_file_dialog({
            mode = "open",
            title = "选择一个协议样本文件",
            filters = {
                { name = "Binary", patterns = { "*.bin", "*.dat" } },
                { name = "All", patterns = { "*.*" } },
            },
        })
        return
    end

    if id == "open_dir" and value == true then
        proto.fs.open_dir_dialog({ title = "选择协议样本目录" })
    end
end

function on_file_dialog(ctx, evt)
    if evt.state == "error" then
        proto.status.set("文件对话框失败: " .. tostring(evt.error), { level = "error" })
        return
    end

    if evt.state ~= "selected" or not evt.path then
        proto.status.set("文件对话框未选择路径", { level = "warn" })
        return
    end

    if evt.kind == "open_dir" then
        proto.status.set("已授权目录: " .. evt.path)
        return
    end

    selected_file = evt.path
    local stat, stat_err = proto.fs.stat(selected_file)
    if not stat then
        proto.status.set("读取文件状态失败: " .. tostring(stat_err), { level = "error" })
        return
    end

    local handle, open_err = proto.fs.open(selected_file, { mode = "read", binary = true })
    if not handle then
        proto.status.set("打开文件失败: " .. tostring(open_err), { level = "error" })
        return
    end

    local chunk, read_err = proto.fs.read(handle, { max_bytes = proto.get_control("read_size") or 64 })
    proto.fs.close(handle)
    if not chunk and read_err ~= "eof" then
        proto.status.set("读取文件失败: " .. tostring(read_err), { level = "error" })
        return
    end

    proto.status.set(string.format("已读取 %s，文件大小 %d 字节", selected_file, stat.size))
end
