# ProtoScope 发布检查清单

本文档记录公开发布前必须确认的检查项。目标是让发布质量可复现，而不是依赖发布人的临时记忆。

## 发布前提

- 发布分支或标签已经包含目标功能、文档和测试。
- 工作树干净，没有未提交的源码、协议示例或生成文件。
- 子模块已初始化：

```powershell
git submodule update --init --recursive
```

- 正式 GitHub Release 使用 `vX.Y.Z` 标签触发。
- 手动演练发布只能从 `develop` 触发 GitHub Actions 的 `workflow_dispatch`。

## 本地验证

发布前至少运行：

```powershell
git diff --check
python tools/generate_luals_api.py --check
cmake -S . -B build-local -DCMAKE_BUILD_TYPE=Release -DPROTOSCOPE_ENABLE_GUI=ON
cmake --build build-local --target protoscope_tests --config Release
cmake --build build-local --target ProtoScope --config Release
ctest --test-dir build-local --output-on-failure --build-config Release
```

如果本地使用 MSVC/Ninja，也可以按 Release CI 的参数重新配置：

```powershell
cmake -S . -B build -G "Ninja" `
  -DCMAKE_BUILD_TYPE=Release `
  -DPROTOSCOPE_ENABLE_GUI=ON `
  -DCMAKE_C_COMPILER=cl `
  -DCMAKE_CXX_COMPILER=cl
cmake --build build --config Release
ctest --test-dir build --output-on-failure --build-config Release
```

## Lua API 与内置示例

改动 Lua 宿主 API、LuaLS manifest、内置协议或模板时，必须同步确认：

- `protocols/protoscope_api_manifest.json` 与 `protocols/protoscope_api.lua` 一致。
- `python tools/generate_luals_api.py --check` 通过。
- `protocols/README.md` 说明新 API 的最小用法。
- `docs/user-manual.md` 说明用户可见行为。
- `protocols/templates/README.md` 列出新增模板。
- 默认协议和可复制模板能被 `protoscope_tests` 加载。

示波器相关示例还要覆盖两类范式：

- 纯 Lua 或无设备 ACK：`on_oscilloscope_toggle()` 立即调用 `proto.oscilloscope.set_running()` 并返回 `true`。
- 真实设备：`on_oscilloscope_toggle()` 只发起启停请求并返回 `false`，等 ACK 后再调用 `proto.oscilloscope.set_running()`。

历史数据限制继续复用现有波形配置：

```lua
proto.plot.setup({
  history_limit = proto.get_control("history_limit") or 5000,
  reset_history = false,
  channels = {
    { label = "CH1", unit = "V" },
  },
})
```

## GitHub Actions Gate

PR 合并前必须通过 `Merge CI`：

- 对 PR 相对目标分支的差异执行 `git diff --check`。
- 显式执行 `python tools/generate_luals_api.py --check`。
- 使用 Windows 2022 + MSVC x64 配置 Release 构建。
- 执行 `cmake --build build --config Release`。
- 执行 `ctest --test-dir build --output-on-failure --build-config Release`。

发布时必须通过 `Release CI`：

- 校验发布标签格式或手动演练来源分支。
- 对上一个正式 `vX.Y.Z` 标签到当前 HEAD 的差异执行 `git diff --check`。
- 显式执行 `python tools/generate_luals_api.py --check`。
- 使用 MSVC Release 构建 `ProtoScope.exe`。
- 用 `dumpbin /DEPENDENTS` 确认不依赖 MinGW/MSYS 运行时 DLL。
- 执行完整 CTest。
- 生成独立 `.exe`、包含 `README.md` / `LICENSE` / docs 目录 / Lua 协议样板的 `.zip` 和 SHA256 校验文件，并在上传前展开 zip 校验必需文件。
- 基于上一个正式 `vX.Y.Z` 标签到当前 HEAD 的提交生成发布说明。
- 创建或更新草稿 GitHub Release；重复运行时也要刷新 release notes。

## 发布资产人工检查

从 GitHub Actions artifact 或草稿 Release 下载资产后，至少检查：

```powershell
Get-FileHash -Algorithm SHA256 .\ProtoScope-windows-x64-<tag>.exe
Expand-Archive .\ProtoScope-windows-x64-<tag>.zip -DestinationPath .\release-check
.\ProtoScope-windows-x64-<tag>.exe --diagnose-renderer-probe --renderer=d3d11
.\ProtoScope-windows-x64-<tag>.exe --diagnose --renderer=d3d11
```

人工打开 GUI 后确认：

- zip 内的发布目录包含 `ProtoScope.exe`、`README.md`、`LICENSE`、README 引用的核心 `docs/*.md`、`protocols/protoscope_api.lua` 和 `protocols/templates/oscilloscope_control/main.lua`。
- 默认协议列表可见，`default_protocol`、`lua_waveform_demo` 和半双工示例可加载。
- Lua Dock 控件能触发脚本回调。
- 波形 Dock 的播放/暂停按钮能触发 `on_oscilloscope_toggle()`。
- 清空历史和 `history_limit` 控件对示例波形生效。
- 诊断日志目录中没有新的启动失败或 renderer 初始化错误。

## 发布说明

草稿 Release 至少包含：

- 版本号和发布日期。
- 基于 Git 提交生成的变更摘要。
- 资产列表和 SHA256 校验说明。
- 如果 Lua API、内置示例或模板有变化，Release CI 会自动加入 `Lua API and protocol templates` 段；发布前仍需人工确认迁移影响和推荐示例是否足够清楚。
