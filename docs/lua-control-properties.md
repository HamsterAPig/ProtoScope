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
- `min/max`：仅整数与浮点输入，`false` 清除该侧约束，nil 表示不修改。
- `options`：目前仅普通 combo，连续字符串数组，索引从 0 开始。
  缩短选项后当前下标无效时，必须在同一补丁中给出有效 value。

新的原子接口不截断整数、不钳制 combo 下标；旧 `set_control` 的转换方式保持兼容，
但最终值仍须满足当前约束。属性变化不自动触发 `on_control`。
数值范围变化后若当前值越界，整批失败，应在同批提供新值。

输入事件在 worker 再次检查当前显隐、禁用、只读、类型、数值范围和选项。
恢复 UI 记忆和重载也校验当前约束；不兼容值回退到新脚本默认值。
运行期属性没有加入 UI YAML，重载按新脚本声明恢复。
只读文本仍可选择和复制；固定布局中隐藏控件的位置可能仍保留布局空间。

当前还没有实现新的工业控件、commit_mode/编辑草稿、运行时代次校验、业务菜单、
数据绑定和历史表。这些不由本接口的基础实现替代。
