---@meta

--[[
ProtoScope 脚本 API 定义文件。
这里只提供 LuaLS / EmmyLua 需要的类型、结构和回调签名，
不包含任何实际运行逻辑。
本文件由 protocols/protoscope_api_manifest.json 生成，请勿手写修改。
]]

-- 基础枚举：覆盖日志、控件、停靠、传输和弹窗状态。
---@alias ProtoLogLevel 'debug'|'info'|'warn'|'error'
---@alias ProtoControlType 'button'|'input_text'|'input_int'|'input_float'|'checkbox'|'combo'|'elf_symbol_combo'|'value_table'
---@alias ProtoControlShortType 'btn'|'text'|'int'|'float'|'check'|'select'|'symbol'|'values'
---@alias ProtoDockAnchor 'left'|'left_bottom'|'right_top'|'right_mid'|'right_bottom'|'main_bottom'
---@alias ProtoControlValue boolean|integer|number|string|ProtoElfSymbolValue|ProtoValueTableUpdate|ProtoValueTableSnapshot|nil
---@alias ProtoBytes integer[]
---@alias ProtoPayload string|ProtoBytes|ProtoBuffer
---@alias ProtoTransportKind 'tcp_client'|'tcp_server'|'serial'|'udp_peer'
---@alias ProtoTxKind 'send'|'request'
---@alias ProtoTxState 'sent'|'completed'|'timeout'|'rejected'|'dropped'|'canceled'
---@alias ProtoRequestGuardState 'active'|'retrying'|'halted'|'reset'
---@alias ProtoDialogKind 'alert'|'confirm'
---@alias ProtoDialogState 'closed'|'confirmed'|'canceled'
---@alias ProtoFileDialogKind 'open_file'|'save_file'|'open_dir'
---@alias ProtoFileDialogState 'selected'|'canceled'|'error'

---@class ProtoBuffer
local ProtoBuffer = {}

-- 返回缓冲区字节数。
---@return integer
function ProtoBuffer:size() end

-- 按 0 基偏移切片，返回新的轻量二进制缓冲。
---@param offset integer
---@param size integer
---@return ProtoBuffer
function ProtoBuffer:slice(offset, size) end

-- 转成十六进制文本，适合小数据调试。
---@param max_bytes? integer
---@return string
function ProtoBuffer:to_hex(max_bytes) end

-- 转成 Lua number[]，仅建议小数据调试使用。
---@param max_bytes? integer
---@return ProtoBytes
function ProtoBuffer:bytes(max_bytes) end

-- Layout Tree：精确写法使用 type + children/rows；语法糖允许字符串 control、字符串数组 flow，以及省略 type 的 column 容器。
---@alias ProtoControlLabelPosition 'left'|'right'
---@alias ProtoLayoutStringArray string[]
---@alias ProtoLayoutNode string|ProtoLayoutStringArray|ProtoColumnLayoutNode|ProtoFlowLayoutNode|ProtoInlineGroupLayoutNode|ProtoTableLayoutNode|ProtoGroupLayoutNode|ProtoCollapseLayoutNode|ProtoControlLayoutNode|ProtoTextLayoutNode|ProtoSeparatorLayoutNode|ProtoSpacerLayoutNode
---@alias ProtoInlineGroupChildNode ProtoControlLayoutNode|ProtoTextLayoutNode

---@class ProtoColumnLayoutNode
---@field type? 'column' @省略 type 且未命中其他糖写法时，默认作为 column 容器解析。
---@field children? ProtoLayoutNode[]
---@field controls? string[]

---@class ProtoFlowLayoutNode
---@field type 'flow'
---@field spacing? number
---@field run_spacing? number
---@field children? ProtoLayoutNode[]
---@field controls? string[]

---@class ProtoInlineGroupLayoutNode
---@field type 'inline_group'
---@field spacing? number
---@field min_width? number @组最小宽度约束，必须为正数；不压缩组内控件。
---@field fill_width? boolean @在 flow 中吃掉当前行剩余宽度；组内可搭配一个 fill_width control。
---@field children? ProtoInlineGroupChildNode[]
---@field controls? string[]

---@class ProtoTableLayoutNode
---@field type 'table'
---@field columns integer
---@field borders? boolean
---@field resizable? boolean
---@field row_bg? boolean
---@field sizing? 'stretch'
---@field rows ProtoLayoutNode[][]

---@class ProtoGroupLayoutNode
---@field type 'group'
---@field title string
---@field children ProtoLayoutNode[]

---@class ProtoCollapseLayoutNode
---@field type 'collapse'
---@field title string
---@field default_open? boolean
---@field children ProtoLayoutNode[]

---@class ProtoControlLayoutNode
---@field type? 'control'
---@field id string
---@field min_width? number @控件最小宽度约束，必须为正数；只在 layout control 节点上生效。
---@field max_width? number @控件最大宽度约束，必须为正数；同时设置时要求 min_width <= max_width。
---@field fill_width? boolean @在 flow 中作为行尾填充项，吃掉当前行剩余宽度。

---@class ProtoTextLayoutNode
---@field type? 'text'
---@field text string

---@class ProtoSeparatorLayoutNode
---@field type? 'separator'
---@field separator? boolean @语法糖：{ separator = true } 等价于 { type = 'separator' }。

---@class ProtoSpacerLayoutNode
---@field type? 'spacer'
---@field spacer? boolean @语法糖：{ spacer = true } 等价于 { type = 'spacer' }。

---@alias ProtoDockLayout ProtoLayoutNode

-- 连接上下文：宿主在打开、关闭、收发数据时传入的基础运行信息。
---@class ProtoConnectionContext
---@field connection_id integer
---@field kind ProtoTransportKind
---@field endpoint string
---@field timestamp_ms integer
---@field ready_for_io boolean

-- 控件描述：声明一个可交互控件及其默认值、枚举选项；也可用 { 'text', 'id', 'label' } 这类位置参数糖。
---@alias ProtoControlDescriptorEntry ProtoControlDescriptor|ProtoControlShortcutDescriptor
---@class ProtoControlDescriptor
---@field type ProtoControlType
---@field id string
---@field label string
---@field label_position? ProtoControlLabelPosition @标签相对控件的位置，默认 left；button 始终使用 label 作为按钮文本。
---@field short_label? string @紧凑布局下显示的显式短标签；悬浮时显示完整 label。
---@field compact_label_below? number @布局宽度低于该正数阈值且 short_label 存在时显示短标签。
---@field default? ProtoControlValue
---@field options? string[]
---@field rows? ProtoValueTableRowEntry[] @value_table 行定义；普通行可用 id 或 { id, label, unit }，bit 行用 id+bits，range 行用 start_id+len。
---@field debounce_ms? integer @elf_symbol_combo 输入消抖毫秒数；未设置时使用 gui.elf_symbol_combo.debounce_ms，默认 300。
---@field limit? integer @elf_symbol_combo 候选结果上限；未设置时使用 gui.elf_symbol_combo.limit，默认 10。

---@class ProtoControlShortcutDescriptor : ProtoControlDescriptor
---@field [1] ProtoControlType|ProtoControlShortType
---@field [2] string
---@field [3] string

-- ElfStaticView 静态地址候选：value 使用十六进制字符串，避免 64 位地址精度丢失。
---@class ProtoElfSymbolValue
---@field label string
---@field value string
---@field type string

-- value_table 显示表：row id 只用于内部匹配，界面只显示 label/value/unit，note 通过悬浮提示展示；bit 源支持非负整数、整数字符串、HEX 字符串、ProtoBuffer 或 number[]。
---@class ProtoValueTableBitRow
---@field bit integer @bit 下标，0 表示最低位；整数源按 64 位读取，字节源按字节低位优先读取。
---@field label string
---@field values? table<integer,string> @[0]/[1] 显示映射；未提供时显示 "0"/"1"。
---@field unit? string
---@field note? string

---@alias ProtoValueTableRowEntry ProtoValueTableRow|ProtoValueTablePlainRowShortcut
---@class ProtoValueTableRow
---@field id? integer @普通行 row id，或 bit 源 id。
---@field label? string @普通行 label；bit 源行本身不显示时可只写 bits。
---@field unit? string
---@field note? string
---@field bits? ProtoValueTableBitRow[] @声明后仅展开这些 bit 行，未声明 bit 不显示。
---@field start_id? integer @range 行起始 id。
---@field len? integer @range 行长度。
---@field labels? string[] @range 行 label 数组，数量不少于 len。
---@field units? string[] @range 行 unit 数组。

---@class ProtoValueTablePlainRowShortcut : ProtoValueTableRow
---@field [1] integer @普通行 row id。
---@field [2] string @普通行 label。
---@field [3]? string @普通行 unit。

---@class ProtoValueTableUpdate
---@field start_id? integer @range 更新起始 id；与 values 一起使用。
---@field values? any[] @range 更新原始值数组；也可用 table 数字键直接按 row id 更新。

---@class ProtoValueTableSnapshotRow
---@field label string
---@field value string
---@field unit string
---@field note? string

---@alias ProtoValueTableSnapshot ProtoValueTableSnapshotRow[]

-- 停靠面板描述：定义一个脚本 UI 面板的标题、锚点和控件布局。
---@class ProtoDockDescriptor
---@field id string
---@field title string
---@field anchor? ProtoDockAnchor
---@field tab_group? string
---@field controls ProtoControlDescriptorEntry[]
---@field layout? ProtoDockLayout|ProtoLayoutNode

-- 波形 bit 拆分显示配置：启用后该通道按固定数字轨道显示本通道原始 y 值的 bit。
-- bit 取值来自 proto.plot.push(channel, { samples = { { y = <非负整数 bitfield> } } })，
-- 不使用 ratio / scale / offset；负数、NaN 和无法表示的浮点值按 0 处理。
---@class ProtoPlotBitDisplay
---@field enabled? boolean @省略时 table 写法默认启用；设为 false 可保留配置但关闭 bit 显示。
---@field first_bit? integer @起始 bit，默认 0，范围 0..63。
---@field bit_count? integer @显示 bit 数，默认 8，范围 1..64，且 first_bit + bit_count 不超过 64。
---@field y_offset? number @bit 轨道组纵向偏移，默认 0。

-- 波形通道描述：定义曲线显示名称、单位、缩放、颜色和可选 bit 显示。
---@class ProtoPlotChannel
---@field label string
---@field unit? string
---@field ratio? number @原始值先按 `actual = raw * ratio` 转成实际值。
---@field scale? number
---@field offset? number
---@field color? string @支持 '#RRGGBB' 或 '#RRGGBBAA'。
---@field line_width? number @主波形区线宽，范围 0.5 到 8.0；省略时使用默认线宽。
---@field bit_display? boolean|ProtoPlotBitDisplay @启用 bit 拆分显示；true 等价于默认 8-bit 配置；需要向同一通道 push 非负整数 bitfield。

-- 波形初始化参数：用于一次性配置波形来源、通道和视图范围。
---@class ProtoPlotSetup
---@field source? string
---@field channels ProtoPlotChannel[]
---@field reset_history? boolean
---@field time_scale? number
---@field time_unit? string
---@field vertical_min? number
---@field vertical_max? number
---@field vertical_unit? string
---@field history_limit? integer @省略或 0 表示不裁剪历史。

-- 单个波形采样点：t 是横轴时间，y 是纵轴数值。
---@class ProtoPlotSample
---@field t number
---@field y number

-- 波形追加请求：向已有通道持续推送采样点。
---@class ProtoPlotAppendRequest
---@field source? string
---@field samples? ProtoPlotSample[]
---@field t0? number @compact series 起始时间；与 dt/values 一起使用。
---@field dt? number @compact series 相邻采样时间间隔；与 t0/values 一起使用。
---@field values? number[] @compact series 数值序列，可减少 Lua 小表创建。

-- 发送类操作的附加参数：常用于超时和业务标签标记。
---@class ProtoSendOptions
---@field timeout_ms? integer
---@field tag? string

-- 请求类操作的附加参数：和发送类似，但语义上表示等待响应。
---@class ProtoRequestOptions
---@field timeout_ms? integer
---@field tag? string

-- 受保护请求的附加参数：max_attempts 只统计当前这一次请求。
---@class ProtoGuardedRequestOptions : ProtoRequestOptions
---@field max_attempts? integer

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
---@field file_job_id? integer
---@field offset? integer
---@field total? integer
---@field progress? number
---@field error? string
---@field guarded? boolean
---@field attempt? integer
---@field max_attempts? integer
---@field guard_state? ProtoRequestGuardState

-- 状态栏配置：控制状态文本的提示级别。
---@class ProtoStatusOptions
---@field level? ProtoLogLevel

-- 弹窗窗口配置：控制脚本弹窗的初始尺寸、位置和交互能力。
---@class ProtoDialogWindowOptions
---@field width? number
---@field height? number
---@field x? number
---@field y? number
---@field resizable? boolean
---@field movable? boolean
---@field auto_resize? boolean

-- 弹窗参数：用于 alert / confirm 的标题、内容、去重键和窗口配置。
---@class ProtoDialogOptions
---@field title string
---@field message string
---@field level? ProtoLogLevel
---@field dedupe_key? string
---@field window? ProtoDialogWindowOptions

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

---@class ProtoFileDialogFilter
---@field name string
---@field patterns string[]

---@class ProtoFileDialogOptions
---@field mode? 'open'|'save'
---@field title? string
---@field default_path? string
---@field filters? ProtoFileDialogFilter[]

---@class ProtoDirDialogOptions
---@field title? string
---@field default_path? string

---@class ProtoFileOpenOptions
---@field mode? 'read'|'write'|'append'
---@field binary? boolean
---@field create_dirs? boolean
---@field overwrite? boolean

---@class ProtoFileReadOptions
---@field max_bytes? integer

---@class ProtoFileStat
---@field size integer
---@field mtime_ms integer
---@field is_file boolean
---@field is_dir boolean

---@class ProtoSendFileOptions
---@field kind? 'send'|'request'
---@field chunk_size? integer
---@field tag? string

---@class ProtoFileDialogEvent
---@field id integer
---@field kind ProtoFileDialogKind
---@field state ProtoFileDialogState
---@field path? string
---@field error? string
---@field timestamp_ms integer

---@alias ProtoStreamRawOutputMode 'full'|'omit'
---@alias ProtoStreamFieldOutputMode 'compat'|'fields_only'

---@class ProtoStreamFrame
---@field name string
---@field raw? ProtoBytes
---@field fields table<string, any>
---@field crc_ok boolean
---@field channel_map? integer[]

---@class ProtoStreamValueTargetControl
---@field id? string @目标 value_table 控件 id；也可使用 control 或 control_id。
---@field control? string
---@field control_id? string
---@field start_field? string @起始 row id 字段名；与 start_id 二选一。
---@field start_id? integer @固定起始 row id；与 start_field 二选一。
---@field values_field string @寄存器数组字段名。

---@class ProtoStreamValueTargets
---@field controls ProtoStreamValueTargetControl[]

---@class ProtoStreamSchema
---@field raw_output? ProtoStreamRawOutputMode
---@field low_overhead? boolean
---@field field_output? ProtoStreamFieldOutputMode
---@field on_batch? fun(ctx: ProtoConnectionContext, frames: ProtoStreamFrame[])
---@field on_error? fun(ctx: ProtoConnectionContext, err: ProtoStreamError)

---@class ProtoStreamRuntimeProfile
---@field frame string
---@field length integer
---@field channel_map? integer[]

proto = proto or {}
proto.plot = proto.plot or {}
proto.status = proto.status or {}
proto.ui = proto.ui or {}
proto.fs = proto.fs or {}
proto.bits = proto.bits or {}
proto.stream = proto.stream or {}

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

-- 发起受保护请求：当前请求所有 attempt 都失败后进入 guarded 熔断，后续 guarded 请求不再发送。
---@param payload ProtoPayload
---@param opts? ProtoGuardedRequestOptions
---@return integer|nil request_id
---@return string|nil error
function proto.request_guarded(payload, opts) end

-- 解除 guarded 请求熔断；不会影响普通 proto.request/proto.send。
---@return boolean ok
function proto.reset_request_guard() end

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

-- 为声明了 runtime_profile 的 frame 设置运行时整帧长度与通道映射。
---@param profile ProtoStreamRuntimeProfile
---@return boolean ok
---@return string|nil error
function proto.stream.set_profile(profile) end

-- 清除一个 frame 或全部 frame 的运行时 profile。
---@param frame_name? string
---@return boolean ok
---@return string|nil error
function proto.stream.clear_profile(frame_name) end

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

-- 打开系统文件选择对话框，结果通过 on_file_dialog 异步返回。
---@param opts ProtoFileDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.fs.open_file_dialog(opts) end

-- 打开系统目录选择对话框，授权所选目录及子路径。
---@param opts ProtoDirDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.fs.open_dir_dialog(opts) end

-- 打开已授权路径，返回宿主文件句柄。
---@param path string
---@param opts? ProtoFileOpenOptions
---@return integer|nil handle
---@return string|nil error
function proto.fs.open(path, opts) end

-- 从文件句柄读取一个二进制分块；EOF 固定返回 nil, "eof"。
---@param handle integer
---@param opts? ProtoFileReadOptions
---@return ProtoBuffer|nil chunk
---@return string|nil error
function proto.fs.read(handle, opts) end

-- 向文件句柄写入二进制数据。
---@param handle integer
---@param chunk ProtoPayload
---@return boolean ok
---@return string|nil error
function proto.fs.write(handle, chunk) end

-- 关闭文件句柄。
---@param handle integer
---@return boolean ok
---@return string|nil error
function proto.fs.close(handle) end

-- 查询已授权路径的文件状态。
---@param path string
---@return ProtoFileStat|nil info
---@return string|nil error
function proto.fs.stat(path) end

-- 由宿主分块读取文件并排队发送，适合纯透传。
---@param path string
---@param opts? ProtoSendFileOptions
---@return integer|nil job_id
---@return string|nil error
function proto.fs.send_file(path, opts) end

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

-- 设置某个控件的值，适合脚本根据协议结果反向驱动界面状态；value_table 可传数字 row id 映射或 { start_id, values }。
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

-- 返回当前脚本要展示的 Dock 面板描述；单 dock 可直接返回 ProtoDockDescriptor，多 dock 返回数组。
---@return ProtoDockDescriptor|ProtoDockDescriptor[]
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

-- 示波器启动/暂停请求回调：返回 true 才允许宿主切换工具栏状态，启动/暂停动作由脚本自行处理。
---@param ctx ProtoConnectionContext
---@param current_running boolean
---@param target_running boolean
---@return boolean allow_toggle
function on_oscilloscope_toggle(ctx, current_running, target_running) end

-- 传输事件回调：用于跟踪 send/request 的发送结果、超时或失败。
---@param ctx ProtoConnectionContext
---@param evt ProtoTxEvent
function on_tx(ctx, evt) end

-- 弹窗事件回调：用于接收 alert / confirm 的最终状态。
---@param ctx ProtoConnectionContext
---@param evt ProtoDialogEvent
function on_dialog(ctx, evt) end

-- 文件对话框事件回调：用于接收文件/目录选择、取消或错误。
---@param ctx ProtoConnectionContext
---@param evt ProtoFileDialogEvent
function on_file_dialog(ctx, evt) end
