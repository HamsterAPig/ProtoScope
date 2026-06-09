-- 核心流程：系统文件对话框先授权路径，确认后由宿主分块读取并进入发送队列。
function ui()
    return {
        {
            id = "send_file_template",
            title = "文件发送模板",
            anchor = "left_bottom",
            tab_group = "protocol_templates",
            controls = {
                { id = "choose_file", type = "button", label = "选择发送文件" },
                { id = "send_selected", type = "button", label = "发送文件" },
                { id = "chunk_size", type = "input_int", label = "分块字节", default = 256 },
            },
            layout = {
                type = "flow",
                children = {
                    { type = "control", id = "choose_file" },
                    { type = "control", id = "send_selected" },
                    { type = "control", id = "chunk_size" },
                },
            },
        },
    }
end

local selected_file = nil

function on_control(ctx, id, value)
    if id == "choose_file" and value == true then
        proto.fs.open_file_dialog({
            mode = "open",
            title = "选择要发送的文件",
            filters = {
                { name = "All", patterns = { "*.*" } },
            },
        })
        return
    end

    if id == "send_selected" and value == true then
        if not selected_file then
            proto.status.set("请先选择文件", { level = "warn" })
            return
        end
        local job_id, err = proto.fs.send_file(selected_file, {
            kind = "send",
            chunk_size = proto.get_control("chunk_size") or 256,
            tag = "template_send_file",
        })
        if not job_id then
            proto.status.set("文件发送启动失败: " .. tostring(err), { level = "error" })
        end
    end
end

function on_file_dialog(ctx, evt)
    if evt.state == "selected" and evt.path then
        selected_file = evt.path
        proto.status.set("已选择文件: " .. selected_file)
    end
end

function on_tx(ctx, evt)
    if evt.file_job_id and evt.progress then
        proto.status.set(string.format("文件发送进度 %.0f%%", evt.progress * 100.0))
    end
    if evt.state == "timeout" or evt.state == "rejected" or evt.state == "dropped" or evt.state == "canceled" then
        proto.status.set("文件发送失败: " .. tostring(evt.error), { level = "error" })
    elseif evt.state == "completed" then
        proto.status.set("文件发送完成")
    end
end
