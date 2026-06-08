-- 核心流程：guarded 请求只负责业务包和完成条件，排队、重试与熔断由宿主管理。
function ui()
    return {
        {
            id = "request_guarded_template",
            title = "受保护请求模板",
            anchor = "left_bottom",
            tab_group = "protocol_templates",
            controls = {
                { id = "send_guarded", type = "button", label = "发送受保护请求" },
                { id = "reset_guard", type = "button", label = "解除熔断" },
                { id = "timeout_ms", type = "input_int", label = "超时 ms", default = 500 },
                { id = "max_attempts", type = "input_int", label = "最大尝试", default = 3 },
            },
            layout = {
                type = "flow",
                children = {
                    { type = "control", id = "send_guarded" },
                    { type = "control", id = "reset_guard" },
                    { type = "control", id = "timeout_ms" },
                    { type = "control", id = "max_attempts" },
                },
            },
        },
    }
end

local awaiting_guarded_response = false
local rx_buffer = {}

local function reset_rx_buffer()
    rx_buffer = {}
end

local function append_rx_bytes(bytes)
    local chunk = bytes:bytes(#bytes)
    for _, value in ipairs(chunk) do
        rx_buffer[#rx_buffer + 1] = value
    end
    if #rx_buffer > 256 then
        reset_rx_buffer()
        proto.status.set("guarded 响应缓存过长，已清空", { level = "warn" })
    end
end

local function has_complete_response_frame()
    -- 示例协议约定响应帧为 AA 55 ... 0D；真实协议应在这里替换为自己的完整帧解析。
    for start_index = 1, #rx_buffer - 3 do
        if rx_buffer[start_index] == 0xAA and rx_buffer[start_index + 1] == 0x55 then
            for end_index = start_index + 3, #rx_buffer do
                if rx_buffer[end_index] == 0x0D then
                    return true
                end
            end
        end
    end
    return false
end

function on_control(ctx, id, value)
    if id == "reset_guard" and value == true then
        proto.reset_request_guard()
        awaiting_guarded_response = false
        reset_rx_buffer()
        proto.status.set("guarded 请求熔断已解除")
        return
    end

    if id == "send_guarded" and value == true then
        awaiting_guarded_response = false
        reset_rx_buffer()
        local request_id, err = proto.request_guarded({ 0xAA, 0x55, 0x01, 0x0D }, {
            timeout_ms = proto.get_control("timeout_ms") or 500,
            max_attempts = proto.get_control("max_attempts") or 3,
            tag = "template_guarded_request",
        })
        if not request_id then
            proto.status.set("受保护请求未发送: " .. tostring(err), { level = "error" })
        else
            awaiting_guarded_response = true
        end
    end
end

function on_bytes(ctx, bytes)
    if not awaiting_guarded_response or #bytes == 0 then
        return
    end

    append_rx_bytes(bytes)
    if has_complete_response_frame() then
        awaiting_guarded_response = false
        reset_rx_buffer()
        proto.request_done({ ok = true, message = "收到完整响应帧" })
        proto.status.set("受保护请求收到完整响应帧")
    end
end

function on_tx(ctx, evt)
    if evt.guarded and
        (evt.state == "timeout" or evt.state == "rejected" or evt.state == "dropped" or evt.state == "canceled") then
        if evt.guard_state == "halted" or evt.state == "rejected" or evt.state == "dropped" or evt.state == "canceled" then
            awaiting_guarded_response = false
            reset_rx_buffer()
        end
        proto.status.set("guarded 请求失败: " .. tostring(evt.error), { level = "error" })
    elseif evt.guarded and evt.state == "completed" then
        awaiting_guarded_response = false
        reset_rx_buffer()
        proto.status.set("guarded 请求发送完成")
    end
end
