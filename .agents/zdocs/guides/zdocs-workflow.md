---
title: ZDocs 使用与维护工作流
type: guide
status: accepted
autoload: true
scope: startup
priority: 95
summary: 说明 ZDocs 的入口、搜索、读取、写入、inbox 审核和与 AGENTS.md/zmemory 的边界。
---
# ZDocs 使用与维护工作流

ZDocs 用于保存 Codex 可读取、可搜索、可 Git 审阅的项目事实。事实源是 `.agents/zdocs/` 下的 Markdown；`.agents/zdocs/.index/` 是可重建的本地派生索引。

## 读取路径

1. 先读 `index.md`，确认当前稳定入口和边界。
2. 用 `codex zdocs list` 查看稳定文档。
3. 用 `codex zdocs search <query> --limit 5` 搜主题；需要排查索引时再切 `--mode keyword`。
4. 用 `codex zdocs get <path>` 读取具体文档正文。

## 写入路径

- 新增稳定知识时，优先写 `overview/`、`guides/`、`decisions/` 这类面向长期复用的文档。
- 文档必须包含 `title`、`type`、`status`，建议补 `summary`、`tags`、`priority`。
- `autoload: true` 与 `scope: startup` 只用于确实应自动进入 startup 上下文的稳定文档。
- 领域词汇和 ADR 仍写入仓库既有 `CONTEXT.md` / `docs/adr/` 事实源；ZDocs 直接维护这些文件。


## 运行面支持级别

- CLI 是完整维护面：`codex zdocs` 负责 init/list/get/search/startup/create/update/remove/inbox/doctor/graph/export/repair 等管理命令；生成文档同步由 init/update 自动执行，不单独暴露 sync 命令。
- TUI 是轻量交互面：`/zdocs` 默认走 init/update 入口，随后提示用户继续用 `create/update`、`doctor`、`index status`、`inbox` 和 lessons/doc-gaps 完成轻量维护；它不应复制 CLI 的全部写入和批量维护能力。
- MCP 是工具集成面：通过 `zdocs` tool 暴露按 action catalog 约束的读写与维护动作，schema 必须与 `codex_project_docs::tool_api` 的 action catalog 保持一致。
- App Server 当前不作为独立 ZDocs 产品面；需要项目文档上下文时，由 core startup/recall 和上层运行面消费同一套 `.agents/zdocs/` 事实源。

## Inbox 审核

`inbox/reflections/` 保存自动生成或待审的反思草稿。它们是候选，不是权威事实。

当 `proposed` reflection 长期堆积时，不要直接把 inbox 当长期事实源读取。应先把跨任务复用的规则提炼到 `memory/lessons-learned.md`，再把已经总结完成的原始 reflection 归档到 `memory/archive/YYYY-MM-DD/`。

自动 reflection 维护使用 `[zdocs].reflection_review_window_days` 作为审查窗口，默认 7 天。超过审查窗口的 `draft` / `proposed` 候选会在后续写入维护时被标记为 `deprecated`；已关闭满同一窗口且未被稳定文档或 promotion suggestion 引用的条目，会成为 `codex zdocs inbox gc` 删除候选。

常用命令：

```shell
codex zdocs inbox status
codex zdocs inbox list --status proposed
codex zdocs inbox get inbox/reflections/<file>.md
codex zdocs inbox promote inbox/reflections/<file>.md
codex zdocs inbox close inbox/reflections/<file>.md
```

## 边界

- `AGENTS.md`：启动硬规则与行为约束，优先级高于 ZDocs。
- `.agents/zdocs/`：项目事实源、搜索与自动召回面。
- `zmemory`：工具侧可写长期记忆，不是仓库内 Markdown 事实源。

## Lessons Learned 与归档

`memory/lessons-learned.md` 是 ZDocs 的经验压缩层：只记录跨任务、可复用、会改变后续执行方式的规则，不记录原始任务叙事。

### 触发条件

- 当 `inbox/reflections/` 中 `status=proposed` 的条目数超过阈值时，执行一次 lessons/archive 收敛。
- 当前仓库建议把阈值设为 **50**；ZDocs 的 reflection 产量较高，过低阈值会导致频繁归档和高维护噪音。
- 阈值检查应发生在新 reflection 写入之后，避免漏掉刚生成的教训。

### 收敛步骤

1. 列出 `inbox/reflections/` 中候选 reflection，并筛出已经具备明确证据、可抽取稳定规则的条目。
2. 将重复出现、可操作的经验规则追加或合并到 `memory/lessons-learned.md`。
3. 为每条经验规则保留来源 reflection 路径，保证后续可追溯。
4. 仅把已经完成提炼的原始 reflection 移到 `memory/archive/YYYY-MM-DD/`；仍需跟进或信息不足的条目继续留在 inbox。
5. 归档后保持 inbox 与稳定文档边界清晰：archive 是历史证据，lessons 是压缩规则，accepted 文档才是稳定项目事实。

### 边界

- `memory/lessons-learned.md`：跨任务规则索引。
- `memory/archive/`：已提炼 reflection 的历史归档。
- `inbox/reflections/`：待审候选。
- `must/`、`guides/`、`architecture/`、`reference/`：成熟后的稳定项目知识。
