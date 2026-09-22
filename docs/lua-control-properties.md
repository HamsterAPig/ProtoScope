# 控件动态属性

现有控件可以在声明中指定 `visible`（默认 true）、`disabled`、`read_only`（默认 false）、
`tooltip`。整数与浮点输入支持 `min/max`，默认值必须满足约束。

运行期修改：

```lua
local ok, err = proto.ui.update_controls({
    target = {value = 25, min = 0, max = 100, label = "Target"},
    mode = {options = {"Auto", "Manual"}, value = 0},
    start = {disabled = true, tooltip = "Device busy"}
})
```

`update_control(id, patch)` 是单控件形式。两者成功返回 true，失败返回 nil 和错误字符串。
批量形式为 ID 到补丁的映射；不接受未知 ID、未知属性、类型错误和不满足约束的值。
任一项失败都不改变任何控件值或属性，Dock 快照与控件快照同步更新。
接口只能在运行期回调中使用，不允许加载声明修改上一版脚本的界面。

支持属性：

- `value/label/visible/disabled/read_only/tooltip`。
- `min/max`：整数、浮点输入、滑块和进度；`false` 清除该侧约束，nil 表示不修改。
  滑块必须保留有限的 `min < max`。
- `options`：普通 combo 和 radio_group，连续字符串数组，索引从 0 开始。
  缩短选项后当前下标无效时，必须在同一补丁中给出有效 value。
- `max_length`：文本 UTF-8 字节上限；`wrap`：多行换行；`indeterminate`：不确定进度。

新的原子接口不截断整数、不钳制 combo 下标；旧 `set_control` 的转换方式保持兼容，
但最终值仍须满足当前约束。属性变化不自动触发 `on_control`。
数值范围变化后若当前值越界，整批失败，应在同批提供新值。

输入事件在 worker 再次检查当前显隐、禁用、只读、类型、数值范围和选项。
恢复 UI 记忆和重载也校验当前约束；不兼容值回退到新脚本默认值。
运行期属性没有加入 UI YAML，重载按新脚本声明恢复。
只读文本仍可选择和复制；固定布局中隐藏控件的位置可能仍保留布局空间。

控件快照携带运行时代次，UI 提交时沿用该代次，worker 拒绝旧代次事件。
弹窗响应沿用原请求代次，旧文件对话框不能授予新协议路径权限。
失败重载保留原代次，成功重载或清空 runtime 才使旧事件失效。

## 工业控件

保留原有九类控件与短名称，新增：

- `label`：只读文本，默认内容为 label。
- `readout`：只读数值显示，支持 `unit`、`precision`（0..12）、`show_update_time`、
  `stale_after_ms`（0 禁用，最大一天）。`set_control` 接受有限数字或显示字符串；
  `get_control` 返回格式化字符串，不是数值。int64 直接格式化，不经 double。
- `indicator`：布尔值，颜色与 `on_text/off_text` 状态文字同时显示。
- `progress`：数值进度，默认范围 0..1；`indeterminate=true` 显示动态不确定进度。
- `slider_int/slider_float`：默认范围 0..100；浮点滑块支持 `precision`。
  滑块范围不能超过 ImGui 支持的对应类型半范围。
- `radio_group`：`options` 字符串数组，值为从 0 开始的下标。
- `text_area`：多行文本，支持只读选择复制和右键复制、`wrap`（默认 true）、
  `rows`（默认 5，2..40）、`max_length`（默认 4096 UTF-8 字节，最大 256KiB）。

四种输出控件拒绝输入事件，不记入 UI YAML，实测值不跨重载保留。
新输入值加入既有界面记忆；重载只保留 ID、类型和当前约束兼容的值。
示例见 `protocols/data_storage_demo/main.lua`，设备实测值与用户设定值使用独立控件。

## 提交与草稿

`commit_mode="change"|"commit"`：旧输入默认 change，滑块和多行文本默认 commit。
commit 模式滑块松开提交，文本及数值输入失焦提交；多行普通 Enter 插入换行。
Escape 取消尚未提交的编辑，恢复当前宿主值；change 模式已提交的值不回滚。

编辑草稿与宿主值分离，程序更新不覆盖正在编辑的草稿。提交后 worker 按当前约束
重新校验，拒绝值不会改变宿主。禁用、只读、隐藏、切换协议或停止绘制会取消草稿。
动态 label 不改变输入控件身份。

## 固定页签布局

`tabs` 是布局容器，不在 controls 数组声明：

```lua
layout = {type="tabs", id="pages", default="live", pages={
    {id="live", title="Live", children={"measured", "connected"}},
    {id="settings", title="Settings", children={"target", "notes"}}
}}
```

固定 1..64 页，页 ID 在容器内唯一；容器 ID 与协议内所有控件及其他 tabs ID 不得重复。
各页 children 使用既有布局语法，控件仍需在所有页中恰好出现一次；支持嵌套 tabs。
省略 default 时选择第一页。运行时不能增删页签或更新 options。

页选择通过 `proto.get_control("pages")`、`proto.set_control("pages", "settings")` 读写，
用户切页触发 `on_control(ctx, "pages", "settings")`，程序切页不触发回调。
选择值为稳定的页 ID，加入既有 UI YAML；重载可保留重排后的同 ID 页，
已删除的页回退到新默认页，旧代次事件不会修改新运行时。
`update_control` 可修改 value、visible、disabled、read_only、tooltip；
disabled/read_only 限制切页，页内控件仍按自身属性校验，不隐式联动。

## 业务菜单

```lua
assert(proto.ui.set_menu({
    {id="device", label="Device", children={
        {id="monitor", label="Monitor", checkable=true, checked=true},
        {separator=true},
        {id="connect", label="Connect"}
    }}
}))

function on_menu(ctx, id, checked)
    if id=="monitor" then proto.ui.show_dock("telemetry", checked) end
end
```

业务菜单位于独立的“业务”入口，不替代内置菜单。`set_menu(items)` 整批验证后替换，
空数组清除；最多 256 项、深度 8，非分隔项 id 在整棵树中唯一，文本最多 4096 字节。
勾选只适用于叶子命令；子菜单 children 不得为空。

`update_menu(id, patch)` 原子更新 label、tooltip、visible、disabled、checked；
不支持更换 ID、结构或 checkable。失败返回 nil 与错误字符串，不留下部分修改。
菜单可在脚本加载期间设置，使用待加载 runtime 的独立状态；失败重载保留旧菜单。
运行期重建菜单会增加版本，旧树上的排队点击也会被拒绝。

点击以运行时代次和菜单版本进入 worker，worker 检查当前项及祖先显隐、禁用状态，
更新勾选后调用 `on_menu(ctx,id,checked)`。普通命令 checked 为 false。
主线程不等待菜单 Lua 执行，回调使用既有 Lua 指令预算；程序更新不触发回调。
菜单状态不进入历史库或 UI YAML，重载从脚本声明重新建立。

`show_dock(id, visible)` 仅接受当前协议 `ui()` 声明的 Dock ID 和布尔值，
仅运行期可用；不控制内置窗口。请求经快照交给 GUI，并复用既有 Dock 显隐记忆。
每条请求只应用一次，不会在后续刷新中反复覆盖用户手动显隐操作。

## 数据字段绑定

实测输出可直接绑定固定数据集字段：

```lua
{"readout", "measured", "Temperature", unit="C", precision=2,
 binding={dataset="telemetry", field="temperature", device="device-a"}}
```

仅 `label/readout/indicator/progress` 支持绑定，用户设定值输入不能绑定为设备实测值。
加载时校验数据集、字段存在及类型：label 对应字符串，readout 对应数字或字符串，
indicator 对应布尔，progress 对应数字。device 省略时跟随该数据集任意设备的发布；
指定时只接收该设备，其他设备不会覆盖其值。

`publish_batch` 整批校验完成后，逐记录通知绑定和记录订阅，不按 UI 帧率抽样。
绑定只保留当前显示值，不复制历史记录。更新时间采用该条记录的接收时间。
显示转换错误不会阻断类型化记录入队；存储故障也不会阻断已校验记录的实时显示。
发布失败的错误仍须由脚本处理，不能以实时界面有更新来推断记录成功。

未收到记录显示 Waiting for data，显式空值显示 No data，超出显示范围、字符串过长
或含 NUL 等显示转换错误显示 Invalid data，并提供错误提示；不会悄悄沿用旧读数。
这些状态下 `get_control` 返回 nil；有效 readout 返回格式化字符串。
绑定值由数据集拥有，不接受 `set_control` 或属性补丁 value 覆盖，不触发 `on_control`。
原始精确值与显式 null 使用 `proto.data.latest` 读取。

绑定声明固定，重载不保留实测值，也不保存到 UI YAML。数据表和历史查询视图仍在实施范围。
# 数据表

`data_table` 使用固定数据集模式；实时表保留有界记录，历史表仅持有当前查询页，
记录不进入 `ControlValue` 或 UI YAML。每协议最多 16 个表，每表最多 32 列。

```lua
{"data_table", "samples", "Samples", dataset="telemetry", mode="history",
 page_size=200, visible_rows=10,
 columns={"sequence", {field="temperature", label="Temperature", unit="C", precision=2}}}
```

- `mode` 为 `live`（默认）或 `history`；可用 `device` 固定设备。
- `columns` 省略时使用全部模式字段；支持列隐藏、重排、调整宽度和单列排序。
- 实时表 `max_rows` 默认 200（1..1000），`max_bytes` 默认 4MiB（1KiB..16MiB）。
  字节预算针对保留数据估算，不是进程 RSS 上限；旧快照共享不可变记录。
- `page_size` 默认 200（1..1000），`visible_rows` 默认 10（3..40）。
- 界面提供类型化单条件筛选、分页、刷新和历史查询取消；筛选与排序在分页前执行。
  历史页使用固定快照，刷新回到首页并纳入新提交记录；实时页随发布滚动。
- 选择行触发 `on_control(ctx,id,row_id)`，值为十进制字符串；`get_control` 返回该选择。
  选择不跨协议重载保留；不能通过 `set_control` 或 value patch 修改。
- 历史行 ID 对应 `on_record` 查询结果中的 `record_id`；内部表查询不触发 `on_record`。
- 显隐、禁用、只读与运行时代次均在 worker 校验；旧历史页选择事件被拒绝。

本阶段不支持单元格编辑，表格导出及读取选中行内容 API 尚未接入。
