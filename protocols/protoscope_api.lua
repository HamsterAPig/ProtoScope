---@meta

--[[
ProtoScope 脚本 API 定义文件。
这里只提供 LuaLS / EmmyLua 需要的类型、结构和回调签名，
不包含任何实际运行逻辑。
]]

-- 基础枚举：覆盖日志、控件、停靠、传输和弹窗状态。
---@alias ProtoLogLevel 'debug'|'info'|'warn'|'error'
---@alias ProtoControlType 'button'|'input_text'|'input_int'|'input_float'|'checkbox'|'combo'|'elf_symbol_combo'
---@alias ProtoDockAnchor 'left'|'left_bottom'|'right_top'|'right_mid'|'right_bottom'|'main_bottom'
---@alias ProtoControlValue boolean|integer|number|string|ProtoElfSymbolValue|nil
---@alias ProtoBytes integer[]
---@alias ProtoPayload string|ProtoBytes
---@alias ProtoFormLayoutItem ProtoFormControlItem|ProtoFormControlsItem|ProtoFormGroupItem|ProtoFormCollapseItem|ProtoFormSeparatorItem|ProtoFormTextItem
---@alias ProtoTxKind 'send'|'request'
---@alias ProtoTxState 'sent'|'completed'|'timeout'|'rejected'|'dropped'|'canceled'
---@alias ProtoDialogKind 'alert'|'confirm'
---@alias ProtoDialogState 'closed'|'confirmed'|'canceled'

-- 表格布局：用于以表格方式组织停靠面板中的控件。
---@class ProtoTableCell
---@field control? string
---@field spacer? boolean

---@class ProtoTableLayout
---@field kind 'table'
---@field columns integer
---@field borders? boolean
---@field resizable? boolean
---@field row_bg? boolean
---@field sizing? 'stretch'
---@field rows ProtoTableCell[][]

-- 表单布局：用于把控件按分组、折叠、文本说明等结构组合起来。
---@class ProtoFormControlItem
---@field control string

---@class ProtoFormControlsItem
---@field controls string[]

---@class ProtoFormGroupItem
---@field group string
---@field items ProtoFormLayoutItem[]

---@class ProtoFormCollapseItem
---@field collapse string
---@field default_open? boolean
---@field items ProtoFormLayoutItem[]

---@class ProtoFormSeparatorItem
---@field separator true

---@class ProtoFormTextItem
---@field text string

---@class ProtoFormLayout
---@field kind 'form'
---@field items ProtoFormLayoutItem[]

---@alias ProtoDockLayout ProtoTableLayout|ProtoFormLayout

-- 连接上下文：宿主在打开、关闭、收发数据时传入的基础运行信息。
---@class ProtoConnectionContext
---@field connection_id integer
---@field kind string
---@field endpoint string
---@field timestamp_ms integer
---@field ready_for_io boolean

-- 控件描述：声明一个可交互控件及其默认值、枚举选项。
---@class ProtoControlDescriptor
---@field type ProtoControlType
---@field id string
---@field label string
---@field default? ProtoControlValue
---@field options? string[]
---@field debounce_ms? integer @elf_symbol_combo 输入消抖毫秒数，默认 150。
---@field limit? integer @elf_symbol_combo 单次候选结果上限，默认 64。

-- ElfStaticView 静态地址候选：value 使用十六进制字符串，避免 64 位地址精度丢失。
---@class ProtoElfSymbolValue
---@field label string
---@field value string
---@field type string

-- 停靠面板描述：定义一个脚本 UI 面板的标题、锚点和控件布局。
---@class ProtoDockDescriptor
---@field id string
---@field title string
---@field anchor? ProtoDockAnchor
---@field tab_group? string
---@field controls ProtoControlDescriptor[]
---@field layout? ProtoDockLayout

-- 波形通道描述：定义曲线显示名称、单位、缩放和颜色。
---@class ProtoPlotChannel
---@field label string
---@field unit? string
---@field ratio? number @原始值先按 `actual = raw * ratio` 转成实际值。
---@field scale? number
---@field offset? number
---@field color? string @支持 '#RRGGBB' 或 '#RRGGBBAA'。

-- 波形初始化参数：用于一次性配置波形来源、通道和视图范围。
---@class ProtoPlotSetup
---@field source? string
---@field channels ProtoPlotChannel[]
---@field reset_history? boolean
---@field view? { time_scale?: number, time_unit?: string, vertical_min?: number, vertical_max?: number, vertical_unit?: string, history_limit?: integer }

-- 单个波形采样点：t 是横轴时间，y 是纵轴数值。
---@class ProtoPlotSample
---@field t number
---@field y number

-- 波形追加请求：向已有通道持续推送采样点。
---@class ProtoPlotAppendRequest
---@field source? string
---@field samples ProtoPlotSample[]

-- 发送类操作的附加参数：常用于超时和业务标签标记。
---@class ProtoSendOptions
---@field timeout_ms? integer
---@field tag? string

-- 请求类操作的附加参数：和发送类似，但语义上表示等待响应。
---@class ProtoRequestOptions
---@field timeout_ms? integer
---@field tag? string

-- 请求完成回传结果：脚本在收到响应后可上报最终处理状态。
---@class ProtoRequestDoneResult
---@field ok? boolean
---@field message? string

-- 传输事件：描述一次发送/请求在宿主侧的生命周期变化。
---@class ProtoTxEvent
---@field id integer
---@field kind ProtoTxKind
---@field state ProtoTxState
---@field tag string
---@field bytes integer
---@field queued_ms integer
---@field finished_ms integer
---@field error? string

-- 状态栏配置：控制状态文本的提示级别。
---@class ProtoStatusOptions
---@field level? ProtoLogLevel

-- 弹窗参数：用于 alert / confirm 的标题、内容和去重键。
---@class ProtoDialogOptions
---@field title string
---@field message string
---@field level? ProtoLogLevel
---@field dedupe_key? string

-- 弹窗事件：回传弹窗的最终状态与确认结果。
---@class ProtoDialogEvent
---@field id integer
---@field kind ProtoDialogKind
---@field state ProtoDialogState
---@field confirmed? boolean
---@field title string
---@field message string
---@field level ProtoLogLevel
---@field dedupe_key string
---@field timestamp_ms integer

proto = proto or {}
proto.plot = proto.plot or {}
proto.status = proto.status or {}
proto.ui = proto.ui or {}
proto.bits = proto.bits or {}

-- 记录脚本日志，便于调试协议、状态流转和异常定位。
---@param level ProtoLogLevel
---@param message string
function proto.log(level, message) end

-- 发送一段原始载荷到宿主，通常用于下行串口数据或主动写入。
---@param payload ProtoPayload
---@param opts? ProtoSendOptions
---@return integer|nil request_id
---@return string|nil error
function proto.send(payload, opts) end

-- 发起一次带响应语义的请求，适合需要等待对端应答的场景。
---@param payload ProtoPayload
---@param opts? ProtoRequestOptions
---@return integer|nil request_id
---@return string|nil error
function proto.request(payload, opts) end

-- 标记当前请求已经完成，宿主可据此结束等待并释放关联状态。
---@param result? ProtoRequestDoneResult
---@return boolean ok
---@return string|nil error
function proto.request_done(result) end

-- 向脚本内部广播一个自定义事件，供同脚本的其他逻辑分发处理。
---@param name string
---@param payload any
function proto.emit(name, payload) end

-- 统计 bitmask 中置 1 的位数，常用于按通道掩码推导动态数组长度。
---@param value integer
---@return integer
function proto.bits.count(value) end

-- 创建或刷新一次定时器，适合轮询、延迟重试或 UI 延迟刷新。
---@param name string
---@param delay_ms integer
function proto.set_timer(name, delay_ms) end

-- 取消指定定时器，避免超时回调在脚本不需要时继续触发。
---@param name string
function proto.cancel_timer(name) end

-- 设置状态栏文本，适合反馈当前处理进度、错误或提示信息。
---@param text string
---@param opts? ProtoStatusOptions
function proto.status.set(text, opts) end

-- 清空状态栏文本，恢复为无状态提示。
function proto.status.clear() end

-- 弹出提示型对话框，适合告警、信息确认前的提示展示。
---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.alert(opts) end

-- 弹出确认型对话框，常用于需要用户二次确认的操作。
---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.confirm(opts) end

-- 配置波形视图，通常在连接建立后先调用一次完成通道初始化。
---@param payload ProtoPlotSetup
function proto.plot.setup(payload) end

-- 追加波形采样点，适合持续推送实时数据。
---@param channel_index integer
---@param payload ProtoPlotAppendRequest
function proto.plot.push(channel_index, payload) end

-- 读取某个控件的当前值，常用于界面联动或提交前取回最新状态。
---@param id string
---@return ProtoControlValue
function proto.get_control(id) end

-- 设置某个控件的值，适合脚本根据协议结果反向驱动界面状态。
---@param id string
---@param value ProtoControlValue
function proto.set_control(id, value) end

-- 计算 Modbus CRC16，适合构造或校验常见串口帧尾。
---@param payload ProtoPayload
---@return integer
function proto.crc16_modbus(payload) end

-- 计算 CCITT-FALSE CRC16，适合部分自定义协议或设备协议。
---@param payload ProtoPayload
---@return integer
function proto.crc16_ccitt_false(payload) end

-- 计算 IEEE CRC32，适合需要 32 位校验的协议场景。
---@param payload ProtoPayload
---@return integer
function proto.crc32_ieee(payload) end

-- 返回当前脚本要展示的 Dock 面板描述列表。
---@return ProtoDockDescriptor[]
function ui() end

-- 连接打开回调：宿主建立连接后触发，用于初始化状态、定时器或 UI。
---@param ctx ProtoConnectionContext
function on_open(ctx) end

-- 连接关闭回调：连接断开时触发，适合清理缓存、定时器和临时状态。
---@param ctx ProtoConnectionContext
function on_close(ctx) end

-- 错误回调：脚本或宿主运行时发生异常时触发，用于记录和提示。
---@param ctx ProtoConnectionContext
---@param message string
function on_error(ctx, message) end

-- 控件变化回调：当 UI 控件被用户修改时触发。
---@param ctx ProtoConnectionContext
---@param id string
---@param value ProtoControlValue
function on_control(ctx, id, value) end

-- 可选流解析 schema：定义后由宿主负责组帧、CRC 和字段解码。
---@return ProtoStreamSchema|nil
function stream() end

-- 原始字节回调：宿主收到串口/输入字节流后调用脚本解析。
---@param ctx ProtoConnectionContext
---@param bytes ProtoBytes
function on_bytes(ctx, bytes) end

-- 定时器回调：由 proto.set_timer 创建的定时器到期后触发。
---@param ctx ProtoConnectionContext
---@param name string
function on_timer(ctx, name) end

-- 传输事件回调：用于跟踪 send/request 的发送结果、超时或失败。
---@param ctx ProtoConnectionContext
---@param evt ProtoTxEvent
function on_tx(ctx, evt) end

-- 弹窗事件回调：用于接收 alert / confirm 的最终状态。
---@param ctx ProtoConnectionContext
---@param evt ProtoDialogEvent
function on_dialog(ctx, evt) end
