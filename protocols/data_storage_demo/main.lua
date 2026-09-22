-- 数据发布、记录、固定快照查询和 KV 的最小示例；尚不包含文件交换和新工业控件。
local sequence = 0
local snapshot

function ui()
    return {
        id = "data_storage", title = "Data Storage",
        controls = {
            {"btn", "record_start", "Start"},
            {"btn", "record_stop", "Stop"},
            {"btn", "record_query", "Query"}
        }
    }
end

function on_control(ctx, id, value)
    if id == "record_start" then
        proto.record.start()
    elseif id == "record_stop" then
        proto.cancel_timer("sample")
        proto.record.stop()
    elseif id == "record_query" then
        proto.record.query({dataset = "telemetry", limit = 200})
    end
end

function data()
    return {{id = "telemetry", fields = {
        {name = "sequence", type = "int64", nullable = false},
        {name = "temperature", type = "double", nullable = false}
    }}}
end

function on_open(ctx)
    sequence = proto.kv.get("device-a/sequence") or 0
    proto.record.start()
end

function on_record(ctx, evt)
    if not evt.ok then
        proto.log("error", evt.error)
        return
    end
    if evt.operation == "start" then
        proto.set_timer("sample", 100)
    elseif evt.operation == "stop" then
        proto.record.query({dataset = "telemetry", limit = 200})
    elseif evt.operation == "query" then
        snapshot = evt.snapshot
        proto.log("info", "query rows=" .. #evt.records .. " snapshot=" .. snapshot)
    end
end

function on_timer(ctx, name)
    if name ~= "sample" then return end
    sequence = sequence + 1
    proto.data.publish({dataset = "telemetry", device = "device-a",
        values = {sequence = sequence, temperature = 20.0 + math.sin(sequence / 10)}})
    local latest = proto.data.latest("telemetry")
    proto.status.set(string.format("Sample %d  %.2f C", sequence, latest.values.temperature))
    if sequence % 100 == 0 then
        proto.kv.set("device-a/sequence", sequence)
        proto.record.stop()
    else
        proto.set_timer("sample", 100)
    end
end

function on_kv(ctx, evt)
    if not evt.ok then proto.log("error", evt.error) end
end

function on_close(ctx)
    proto.cancel_timer("sample")
    proto.kv.set("device-a/sequence", sequence)
    proto.record.stop()
end
