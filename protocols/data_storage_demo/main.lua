-- 连续发布、记录、历史表、异步导入导出与 KV 示例。
local sequence = 0
local snapshot
local export_dialogs = {}
local export_task
local import_dialogs = {}
local import_task

assert(proto.ui.set_menu({
    {id="telemetry_show",label="Telemetry",checkable=true,checked=true},
    {separator=true},
    {id="record_actions",label="Recording",children={
        {id="record_start",label="Start"},
        {id="record_stop",label="Stop"},
        {id="record_query",label="Query"},
        {separator=true},
        {id="export_psrec",label="Export PSREC"},
        {id="export_csv",label="Export CSV"},
        {id="export_cancel",label="Cancel Export",disabled=true},
        {separator=true},
        {id="import_psrec",label="Import PSREC"},
        {id="import_csv",label="Import CSV"},
        {id="import_cancel",label="Cancel Import",disabled=true}
    }}
}))

function on_menu(ctx,id,checked)
    if id=="telemetry_show" then
        proto.ui.show_dock("data_storage",checked)
    else
        on_control(ctx,id,checked)
    end
end

function ui()
    return {
        id = "data_storage", title = "Data Storage",
        controls = {
            {"label", "heading", "Device Telemetry"},
            {"btn", "record_start", "Start"},
            {"btn", "record_stop", "Stop"},
            {"btn", "record_query", "Query"},
            {"readout", "measured", "Measured", unit="C", precision=2, stale_after_ms=500, show_update_time=true,
                binding={dataset="telemetry",field="temperature",device="device-a"}},
            {"indicator", "connected", "Link", on_text="Connected", off_text="Disconnected"},
            {"progress", "batch_progress", "Batch", default=0},
            {"slider_int", "target", "Target", min=0, max=100, default=20},
            {"slider_float", "gain", "Gain", min=0, max=2, default=1, precision=2},
            {"radio_group", "mode", "Mode", options={"Automatic", "Manual"}},
            {"text_area", "notes", "Notes", rows=4, max_length=1024, wrap=true},
            {"data_table", "live_rows", "Live Samples", dataset="telemetry",
                max_rows=200, page_size=50, visible_rows=8,
                columns={"sequence", {field="temperature", label="Temperature", unit="C", precision=2}}},
            {"data_table", "history_rows", "Recorded Samples", dataset="telemetry", mode="history",
                page_size=200, visible_rows=8,
                columns={"sequence", {field="temperature", label="Temperature", unit="C", precision=2}}}
        },
        layout = {type="tabs", id="telemetry_pages", default="live", pages={
            {id="live", title="Live", children={
                "heading", {"record_start", "record_stop", "record_query"},
                {id="measured", fill_width=true}, {id="connected", fill_width=true},
                {id="batch_progress", fill_width=true}
            }},
            {id="settings", title="Settings", children={
                {id="target", fill_width=true}, {id="gain", fill_width=true},
                {id="mode", fill_width=true}, {id="notes", fill_width=true}
            }},
            {id="tables", title="Tables", children={
                {id="live_rows", fill_width=true}, {id="history_rows", fill_width=true}
            }}
        }}
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
    elseif id == "export_psrec" or id == "export_csv" then
        local format = id == "export_psrec" and "psrec" or "csv"
        local dialog, err = proto.fs.open_file_dialog({mode="save", title="Export Records",
            default_path="telemetry." .. format, filters={{name=format, patterns={"*." .. format}}}})
        if dialog then export_dialogs[dialog] = format else proto.log("error", err) end
    elseif id == "export_cancel" and export_task then
        proto.record.cancel(export_task)
    elseif id == "import_psrec" or id == "import_csv" then
        local format = id == "import_psrec" and "psrec" or "csv"
        local dialog, err = proto.fs.open_file_dialog({mode="open", title="Import Records",
            filters={{name=format, patterns={"*." .. format}}}})
        if dialog then import_dialogs[dialog] = format else proto.log("error", err) end
    elseif id == "import_cancel" and import_task then
        proto.record.cancel(import_task)
    end
end

function on_file_dialog(ctx, evt)
    local import_format = import_dialogs[evt.id]
    import_dialogs[evt.id] = nil
    if import_format then
        if evt.state ~= "selected" or not evt.path or import_task then return end
        local ok, task = pcall(proto.record.import, {path=evt.path, format=import_format})
        if not ok then proto.log("error", tostring(task)); return end
        import_task = task
        proto.ui.update_menu("import_cancel", {disabled=false})
        return
    end
    local format = export_dialogs[evt.id]
    export_dialogs[evt.id] = nil
    if not format or evt.state ~= "selected" or not evt.path then return end
    if export_task then return end
    local ok, task = pcall(proto.record.export, {path=evt.path, format=format, overwrite=true,
        dataset="telemetry", snapshot=snapshot})
    if not ok then proto.log("error", tostring(task)); return end
    export_task = task
    proto.ui.update_menu("export_cancel", {disabled=false})
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
    if evt.operation == "import" and evt.task == import_task then
        import_task = nil
        proto.ui.update_menu("import_cancel", {disabled=true})
        if evt.ok then
            proto.status.set("Imported " .. evt.processed .. " records")
            snapshot = nil
            proto.record.query({dataset="telemetry", limit=200})
        end
    end
    if evt.operation == "export" and evt.task == export_task then
        export_task = nil
        proto.ui.update_menu("export_cancel", {disabled=true})
        if evt.ok then proto.status.set("Exported " .. evt.processed .. " records") end
    end
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
    proto.set_control("batch_progress", (sequence % 100 + 1) / 100)
    proto.status.set(string.format("Sample %d  %.2f C", sequence, latest.values.temperature))
    if sequence % 100 == 0 then
        proto.kv.set("device-a/sequence", sequence)
    end
    proto.set_timer("sample", 100)
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
