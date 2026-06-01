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
---@field capacity? integer 环形缓冲容量，默认 4096
---@field overflow? 'drop_oldest' 超限策略，当前仅支持丢弃最旧字节

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
---@field size? integer 固定整帧长度；与 `len` 二选一
---@field len? ProtoStreamLenDef 变长帧定义；与 `size` 二选一
---@field crc? ProtoStreamCrcDef|false CRC 配置；省略或 `false` 表示不校验
---@field fields? ProtoStreamFieldDef[] 字段定义
---@field on_frame fun(ctx: ProtoConnectionContext, frame: ProtoStreamFrame) 完整有效帧回调

---@class ProtoStreamSchema
---@field buffer? ProtoStreamBufferDef
---@field frames ProtoStreamFrameDef[]
---@field on_error? fun(ctx: ProtoConnectionContext, err: ProtoStreamError)

---@class ProtoStreamFrame
---@field name string 帧名称
---@field raw ProtoBytes 原始完整帧
---@field fields table<string, any> 已解码字段表
---@field crc_ok boolean CRC 是否通过；未启用 CRC 时也为 true

---@class ProtoStreamError
---@field code 'overflow'|'noise_discarded'|'invalid_length'|'crc_mismatch'|'field_decode_failed'|'count_resolve_failed'
---@field message string
---@field dropped_bytes integer
---@field frame_name? string
---@field raw? ProtoBytes

return M
