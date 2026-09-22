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

业务菜单、数据绑定和历史表仍在后续实施范围。
