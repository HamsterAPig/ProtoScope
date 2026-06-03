---@meta

--[[
流解析 schema 的 LuaLS 类型定义。
本文件只提供补全与注释，不参与实际解析逻辑。
]]

local M = {}

---@alias ProtoStreamValueType
---| 'u8'
---| 'i8'
---| 'u16_be'
---| 'u16_le'
---| 'i16_be'
---| 'i16_le'
---| 'u32_be'
---| 'u32_le'
---| 'i32_be'
---| 'i32_le'
---| 'f32_be'
---| 'f32_le'
---| 'bytes'

---@alias ProtoStreamLengthMeans 'payload'|'frame'
---@alias ProtoStreamCrcOrder 'hi_lo'|'lo_hi'
---@alias ProtoStreamCrcType 'crc16_modbus'|'crc16_ccitt_false'|'crc32_ieee'
---@alias ProtoStreamRawOutputMode 'full'|'omit' @full 兼容旧脚本；omit 可减少 frame.raw 展开成本。
---@alias ProtoStreamFieldCount integer|string|ProtoStreamCountExpression

---@class ProtoStreamCountExpression
---@field op 'const'|'value'|'field'|'div'|'sub'|'mul'|'remaining'|'if_flag'|'case'|'bit_count' 纯 C++ count 表达式操作
---@field value? integer 常量值，或 `sub` 扣减值
---@field name? string `op='field'` 时引用的字段名
---@field field? string 算术、`if_flag` 或 `case` 使用的已解析字段名
---@field by? integer|ProtoStreamCountExpression `div`/`mul` 参数；`sub` 也兼容整数 `by`
---@field expr? ProtoStreamFieldCount 算术表达式的左操作数；未写时使用 `field`
---@field unit? integer `remaining` 的元素宽度；未写时使用当前字段类型宽度
---@field exclude_crc? boolean `remaining` 是否排除帧尾 CRC，默认 true
---@field mask? integer `if_flag` 使用的位掩码
---@field then? ProtoStreamFieldCount `if_flag` 命中时的数量表达式
---@field else? ProtoStreamFieldCount `if_flag` 未命中时的数量表达式
---@field cases? table<integer, ProtoStreamFieldCount> `case` 分支，key 为字段整数值
---@field default? ProtoStreamFieldCount `case` 未匹配时的默认数量

---@class ProtoStreamBufferDef
---@field capacity? integer 初始环形缓冲容量，默认 4096；无损模式下会在宿主预算内自动扩容
---@field max_capacity? integer 无损模式自动扩容上限，默认 256MiB
---@field overflow? 'drop_oldest' 显式丢弃最旧字节；省略时默认不丢弃，容量不足会先扩容并报告预算耗尽
---@field near_overflow_threshold_ratio? number 接近溢出告警阈值比例，默认 0.8
---@field near_overflow_notify? boolean 是否启用接近溢出告警
---@field popup_enabled? boolean 是否允许宿主弹出接近溢出提示

---@class ProtoStreamLenDef
---@field offset integer 长度字段起始 offset，Lua 下标从 1 开始
---@field type ProtoStreamValueType 长度字段类型，必须是整数类型
---@field means? ProtoStreamLengthMeans `payload` 表示 payload 长度，`frame` 表示整帧长度
---@field extra? integer `means='payload'` 时附加的非 payload 长度

---@class ProtoStreamCrcDef
---@field type ProtoStreamCrcType CRC 算法
---@field order? ProtoStreamCrcOrder 帧尾 CRC 字节顺序，默认 `lo_hi`

---@class ProtoStreamFieldDef
---@field name string 字段名
---@field type ProtoStreamValueType 字段类型
---@field offset? integer 字段起始 offset，Lua 下标从 1 开始；不写时按前一字段顺延
---@field count? ProtoStreamFieldCount 字段数量，可写整数、已解析字段名或纯 C++ count 表达式 table；旧 `function(...)` 写法已彻底废弃且不再兼容

---@class ProtoStreamFrameDef
---@field name string 帧名称，必须唯一
---@field header ProtoBytes 用于同步的固定帧头字节序列
---@field size? integer 固定整帧长度；与 `len` / `runtime_profile` 三选一
---@field len? ProtoStreamLenDef 变长帧定义；与 `size` / `runtime_profile` 三选一
---@field runtime_profile? boolean 声明该帧长度与通道映射由 `proto.stream.set_profile()` 在运行时提供
---@field crc? ProtoStreamCrcDef|false CRC 配置；省略或 `false` 表示不校验
---@field fields? ProtoStreamFieldDef[] 字段定义
---@field on_frame? fun(ctx: ProtoConnectionContext, frame: ProtoStreamFrame) 完整有效帧回调；未定义 `on_batch` 时必填

---@class ProtoStreamSchema
---@field buffer? ProtoStreamBufferDef
---@field frames ProtoStreamFrameDef[]
---@field raw_output? ProtoStreamRawOutputMode 是否向 Lua frame 暴露 raw；默认 full 兼容旧脚本。
---@field on_batch? fun(ctx: ProtoConnectionContext, frames: ProtoStreamFrame[]) 完整有效帧批量回调；定义后优先于各 frame 的 `on_frame`
---@field on_error? fun(ctx: ProtoConnectionContext, err: ProtoStreamError)

--- Lua stream schema 的 `buffer.capacity` 是初始容量，`buffer.max_capacity` 控制无损扩容上限。
--- 宿主 YAML `receive.stream_buffer.*` 只控制接近溢出告警阈值和弹窗，不改变 schema 容量。

---@class ProtoStreamFrame
---@field name string 帧名称
---@field raw ProtoBytes 原始完整帧
---@field fields table<string, any> 已解码字段表
---@field crc_ok boolean CRC 是否通过；未启用 CRC 时也为 true
---@field channel_map? integer[] 当前帧生效的业务通道映射，Lua 侧使用 1-based 编号

---@class ProtoStreamRuntimeProfile
---@field frame string 目标 frame 名称
---@field length integer 完整帧长度
---@field channel_map? integer[] 业务通道映射，Lua 侧使用 1-based 编号

---@class ProtoStreamError
---@field code 'overflow'|'noise_discarded'|'invalid_length'|'crc_mismatch'|'field_decode_failed'|'count_resolve_failed'
---@field message string
---@field dropped_bytes integer
---@field frame_name? string
---@field raw? ProtoBytes

return M
