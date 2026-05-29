function controls()
    return {
        {
            type = "elf_symbol_combo",
            id = "target",
            label = "变量",
        },
        {
            type = "elf_symbol_combo",
            id = "custom_target",
            label = "自定义变量",
            debounce_ms = 25,
            limit = 3,
        },
    }
end

function on_control(ctx, id, value)
    local current = proto.get_control(id)
    proto.emit("symbol", current)
end
