---
title: 项目知识库索引
type: doc
status: accepted
autoload: true
scope: startup
priority: 100
summary: ZDocs 入口，说明项目事实源、稳定文档和 reflection inbox 边界。
---
# 项目知识库

此目录是 Codex 内置 ZDocs 的项目事实源：稳定知识写成 Git 可追踪 Markdown，派生索引可重建，reflection inbox 只作为待审草稿。

## 快速入口

- 稳定知识从本文件开始；新增可复用项目事实时，优先放在 `.agents/zdocs/` 下的稳定 Markdown，而不是 `inbox/reflections/`。
- 当前稳定指南：`guides/zdocs-workflow.md`。
- 产品级说明与完整命令参考见 `docs/zdocs.md`。
- ZDocs 是 Codex 内置项目文档能力和 agent 可召回的事实源；它不替代 `AGENTS.md` 的启动硬规则，也不替代 `zmemory` 的本地长期记忆数据库。

## 常用命令

```shell
codex zdocs list
codex zdocs get index.md
codex zdocs search <query> --limit 5
codex zdocs doctor
```

## 写入规则

- 可长期复用的项目事实、指南、规则、计划或决策：写入 `.agents/zdocs/` 稳定文档，并保持 frontmatter 完整。
- 自动生成或待审反思：保留在 `inbox/reflections/`，经人工审查后再提升为稳定文档。
- 本地派生数据：`.agents/zdocs/.index/`，由命令重建，不作为事实源。

## 建议结构

```text
.agents/zdocs/
  index.md
  startup.md
  must/
  overview/
  architecture/
  guides/
  reference/
  memory/
    reflections/
    decisions/
    archive/
    doc-gaps.md
    lessons-learned.md
  inbox/reflections/
  inbox/promotions/
```
