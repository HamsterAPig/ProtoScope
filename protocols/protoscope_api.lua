---@meta

---@alias ProtoLogLevel 'debug'|'info'|'warn'|'error'
---@alias ProtoControlType 'button'|'input_text'|'input_int'|'input_float'|'checkbox'|'combo'
---@alias ProtoDockAnchor 'left'|'left_bottom'|'right_top'|'right_mid'|'right_bottom'|'main_bottom'
---@alias ProtoControlValue boolean|integer|number|string|nil
---@alias ProtoBytes integer[]
---@alias ProtoPayload string|ProtoBytes
---@alias ProtoFormLayoutItem ProtoFormControlItem|ProtoFormControlsItem|ProtoFormGroupItem|ProtoFormCollapseItem|ProtoFormSeparatorItem|ProtoFormTextItem
---@alias ProtoTxKind 'send'|'request'
---@alias ProtoTxState 'sent'|'completed'|'timeout'|'rejected'|'dropped'|'canceled'
---@alias ProtoDialogKind 'alert'|'confirm'
---@alias ProtoDialogState 'closed'|'confirmed'|'canceled'

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

---@class ProtoConnectionContext
---@field connection_id integer
---@field kind string
---@field endpoint string
---@field timestamp_ms integer
---@field ready_for_io boolean

---@class ProtoControlDescriptor
---@field type ProtoControlType
---@field id string
---@field label string
---@field default? ProtoControlValue
---@field options? string[]

---@class ProtoDockDescriptor
---@field id string
---@field title string
---@field anchor? ProtoDockAnchor
---@field tab_group? string
---@field controls ProtoControlDescriptor[]
---@field layout? ProtoDockLayout

---@class ProtoPlotChannel
---@field label string
---@field unit? string
---@field scale? number
---@field offset? number
---@field color? string @支持 '#RRGGBB' 或 '#RRGGBBAA'。

---@class ProtoPlotSetup
---@field source? string
---@field channels ProtoPlotChannel[]
---@field reset_history? boolean
---@field view? { time_scale?: number, time_unit?: string, vertical_min?: number, vertical_max?: number, vertical_unit?: string, history_limit?: integer }

---@class ProtoPlotSample
---@field t number
---@field y number

---@class ProtoPlotAppendRequest
---@field source? string
---@field samples ProtoPlotSample[]

---@class ProtoSendOptions
---@field timeout_ms? integer
---@field tag? string

---@class ProtoRequestOptions
---@field timeout_ms? integer
---@field tag? string

---@class ProtoRequestDoneResult
---@field ok? boolean
---@field message? string

---@class ProtoTxEvent
---@field id integer
---@field kind ProtoTxKind
---@field state ProtoTxState
---@field tag string
---@field bytes integer
---@field queued_ms integer
---@field finished_ms integer
---@field error? string

---@class ProtoStatusOptions
---@field level? ProtoLogLevel

---@class ProtoDialogOptions
---@field title string
---@field message string
---@field level? ProtoLogLevel
---@field dedupe_key? string

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

---@param level ProtoLogLevel
---@param message string
function proto.log(level, message) end

---@param payload ProtoPayload
---@param opts? ProtoSendOptions
---@return integer|nil request_id
---@return string|nil error
function proto.send(payload, opts) end

---@param payload ProtoPayload
---@param opts? ProtoRequestOptions
---@return integer|nil request_id
---@return string|nil error
function proto.request(payload, opts) end

---@param result? ProtoRequestDoneResult
---@return boolean ok
---@return string|nil error
function proto.request_done(result) end

---@param name string
---@param payload any
function proto.emit(name, payload) end

---@param name string
---@param delay_ms integer
function proto.set_timer(name, delay_ms) end

---@param name string
function proto.cancel_timer(name) end

---@param text string
---@param opts? ProtoStatusOptions
function proto.status.set(text, opts) end

function proto.status.clear() end

---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.alert(opts) end

---@param opts ProtoDialogOptions
---@return integer|nil dialog_id
---@return string|nil error
function proto.ui.confirm(opts) end

---@param payload ProtoPlotSetup
function proto.plot.setup(payload) end

---@param channel_index integer
---@param payload ProtoPlotAppendRequest
function proto.plot.push(channel_index, payload) end

---@param id string
---@return ProtoControlValue
function proto.get_control(id) end

---@param id string
---@param value ProtoControlValue
function proto.set_control(id, value) end

---@param payload ProtoPayload
---@return integer
function proto.crc16_modbus(payload) end

---@param payload ProtoPayload
---@return integer
function proto.crc16_ccitt_false(payload) end

---@param payload ProtoPayload
---@return integer
function proto.crc32_ieee(payload) end

---@return ProtoDockDescriptor[]
function ui() end

---@param ctx ProtoConnectionContext
function on_open(ctx) end

---@param ctx ProtoConnectionContext
function on_close(ctx) end

---@param ctx ProtoConnectionContext
---@param message string
function on_error(ctx, message) end

---@param ctx ProtoConnectionContext
---@param id string
---@param value ProtoControlValue
function on_control(ctx, id, value) end

---@param ctx ProtoConnectionContext
---@param bytes ProtoBytes
function on_bytes(ctx, bytes) end

---@param ctx ProtoConnectionContext
---@param name string
function on_timer(ctx, name) end

---@param ctx ProtoConnectionContext
---@param evt ProtoTxEvent
function on_tx(ctx, evt) end

---@param ctx ProtoConnectionContext
---@param evt ProtoDialogEvent
function on_dialog(ctx, evt) end
