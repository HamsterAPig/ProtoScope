---@meta

---@alias ProtoLogLevel '"debug"'|'"info"'|'"warn"'|'"error"'
---@alias ProtoControlType '"button"'|'"input_text"'|'"input_int"'|'"input_float"'|'"checkbox"'|'"combo"'
---@alias ProtoDockAnchor '"left"'|'"left_bottom"'|'"right_top"'|'"right_mid"'|'"right_bottom"'|'"main_bottom"'
---@alias ProtoControlValue boolean|integer|number|string|nil
---@alias ProtoBytes integer[]
---@alias ProtoPayload string|ProtoBytes

---@class ProtoConnectionContext
---@field connection_id integer 连接 ID。
---@field kind string 连接类型，例如 serial、tcp_client、tcp_server。
---@field endpoint string 端点描述。

---@class ProtoControlDescriptor
---@field type ProtoControlType 控件类型。
---@field id string 控件 ID。
---@field label string 控件标签。
---@field default? ProtoControlValue 默认值；combo 为从 1 开始的选项序号。
---@field options? string[] combo 控件选项。

---@class ProtoDockDescriptor
---@field id string Dock 唯一 ID。
---@field title string Dock 标题。
---@field anchor? ProtoDockAnchor 停靠位置，默认 left_bottom。
---@field tab_group? string 标签页分组。
---@field controls ProtoControlDescriptor[] 控件列表。

---@class ProtoEventPayload
---@field [string] any

---@class ProtoPlotChannel
---@field label string 通道名称。
---@field unit? string 通道单位。
---@field offset? number 通道显示偏移。

---@class ProtoPlotSetup
---@field source? string 数据来源名称。
---@field reset_history? boolean 是否清空历史数据。
---@field time_scale? number 时间缩放。
---@field time_unit? string 时间单位。
---@field vertical_min? number 垂直轴最小值。
---@field vertical_max? number 垂直轴最大值。
---@field vertical_unit? string 垂直轴单位。
---@field history_limit? integer 历史采样保留上限。
---@field channels ProtoPlotChannel[] 通道定义。

---@class ProtoPlotSample
---@field t number 时间戳。
---@field y number 采样值。

---@class ProtoPlotPushPayload
---@field samples ProtoPlotSample[] 采样点数组。

---@class ProtoPlotApi
local plot_api = {}

---配置 Lua 波形通道和显示参数。
---@param payload ProtoPlotSetup
function plot_api.setup(payload) end

---向指定通道追加采样点；channel_index 从 1 开始。
---@param channel_index integer
---@param payload ProtoPlotPushPayload
function plot_api.push(channel_index, payload) end

---@class ProtoApi
---@field plot ProtoPlotApi
proto = {}
proto.plot = plot_api

---写入脚本日志。
---@param level ProtoLogLevel
---@param message string
function proto.log(level, message) end

---发送字节数组或字符串。
---@param payload ProtoPayload
function proto.send(payload) end

---发出结构化脚本事件。
---@param name string
---@param payload any
function proto.emit(name, payload) end

---读取控件当前值。
---@param id string
---@return ProtoControlValue
function proto.get_control(id) return nil end

---设置控件当前值。
---@param id string
---@param value ProtoControlValue
function proto.set_control(id, value) end

---设置一次性定时器。
---@param name string
---@param delay_ms integer
function proto.set_timer(name, delay_ms) end

---取消定时器。
---@param name string
function proto.cancel_timer(name) end

---计算 Modbus CRC16。
---@param payload ProtoPayload
---@return integer
function proto.crc16_modbus(payload) return 0 end

---计算 CRC16/CCITT-FALSE。
---@param payload ProtoPayload
---@return integer
function proto.crc16_ccitt_false(payload) return 0 end

---计算 IEEE CRC32。
---@param payload ProtoPayload
---@return integer
function proto.crc32_ieee(payload) return 0 end

---定义 Dock UI。
---@return ProtoDockDescriptor[]
function ui() return {} end

---连接打开回调。
---@param ctx ProtoConnectionContext
function on_open(ctx) end

---连接关闭回调。
---@param ctx ProtoConnectionContext
function on_close(ctx) end

---连接错误回调。
---@param ctx ProtoConnectionContext
---@param message string
function on_error(ctx, message) end

---控件变化回调。
---@param ctx ProtoConnectionContext
---@param id string
---@param value ProtoControlValue
function on_control(ctx, id, value) end

---收到字节回调。
---@param ctx ProtoConnectionContext
---@param bytes ProtoBytes
function on_bytes(ctx, bytes) end

---定时器触发回调。
---@param ctx ProtoConnectionContext
---@param name string
function on_timer(ctx, name) end
