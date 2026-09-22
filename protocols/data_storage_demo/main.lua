-- 工业控件、数据发布、记录、固定快照查询和 KV 示例；尚不包含文件交换和历史表。
local sequence = 0
local snapshot

function ui()
    return {
        id = "data_storage", title = "Data Storage",
        controls = {
            {"label", "heading", "Device Telemetry"},
            {"btn", "record_start", "Start"},
            {"btn", "record_stop", "Stop"},
            {"btn", "record_query", "Query"},
            {"readout", "measured", "Measured", unit="C", precision=2, stale_after_ms=500, show_update_time=true},
            {"indicator", "connected", "Link", on_text="Connected", off_text="Disconnected"},
            {"progress", "batch_progress", "Batch", default=0},
            {"slider_int", "target", "Target", min=0, max=100, default=20},
            {"slider_float", "gain", "Gain", min=0, max=2, default=1, precision=2},
            {"radio_group", "mode", "Mode", options={"Automatic", "Manual"}},
            {"text_area", "notes", "Notes", rows=4, max_length=1024, wrap=true}
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
    proto.set_control("connected", true)
    sequence = proto.kv.get("device-a/sequence") or 0
    proto.record.start()
end

function on_record(ctx, evt)
    if not evt.ok then
        proto.log("error", evt.error)
        return
    end
    if evt.operation == "start" then
        proto.set_control("batch_progress", 0)
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
    proto.set_control("measured", latest.values.temperature)
    proto.set_control("batch_progress", (sequence % 100 + 1) / 100)
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
    proto.set_control("connected", false)
    proto.cancel_timer("sample")
    proto.kv.set("device-a/sequence", sequence)
    proto.record.stop()
end
