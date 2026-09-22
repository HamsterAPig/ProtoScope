# SQLite

固定版本：3.50.4（3500400）。

来源：https://sqlite.org/2025/sqlite-amalgamation-3500400.zip

下载归档的 SHA-256：
`1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032`

仅保留上游 `sqlite3.c`、`sqlite3.h`、`sqlite3ext.h`，内容未作修改。
SQLite 属于 public domain，授权声明见各源码文件开头。
以 `protoscope_sqlite` 静态目标构建，启用线程支持并禁用动态扩展加载。
