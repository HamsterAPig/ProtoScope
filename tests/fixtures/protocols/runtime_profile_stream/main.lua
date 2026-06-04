local function on_stream_frame(ctx, frame)
    proto.emit("runtime_profile_frame", string.format("%s|map=%d", frame.name, #(frame.channel_map or {})))
end

function on_bytes(ctx, bytes)
    proto.emit("legacy_bytes", tostring(#bytes))
end

function stream()
    return {
        buffer = { capacity = 64, overflow = "drop_oldest" },
        frames = {
            {
                name = "dynamic_profile",
                header = { 0xFF, 0x26 },
                runtime_profile = true,
                crc = { type = "crc16_modbus", order = "hi_lo" },
                fields = {
                    { name = "values", type = "i16_be", offset = 3, count = { op = "remaining", unit = 2 } },
                },
                on_frame = on_stream_frame,
            },
        },
    }
end
