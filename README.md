# ProtoScope

ProtoScope 当前是一个最小可编译的 C++20 工程骨架，已经接入并固定以下第三方仓库：

- `3rdparty/spdlog`
- `3rdparty/yaml-cpp`
- `3rdparty/imgui`
- `3rdparty/libdwarf-code`

## 快速开始

首次拉取：

```powershell
git submodule update --init --recursive
cmake -S . -B build
cmake --build build
```

运行：

```powershell
.\build\Debug\ProtoScope.exe
```

## 当前状态

- 已完成最小 CMake 工程初始化
- 已完成 `spdlog` 与 `yaml-cpp` 链接验证骨架
- `imgui` 与 `libdwarf-code` 已拉取，但暂未接入主目标

后续如果要继续推进，可在此基础上增加：

- ELF / DWARF 解析层
- ImGui 桌面 UI
- 配置文件加载
- 单元测试与 CI

