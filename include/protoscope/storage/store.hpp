#pragma once

#include "protoscope/data/model.hpp"
#include "protoscope/data/query.hpp"
#include "protoscope/storage/import.hpp"
#include "protoscope/storage/config.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace protoscope::storage {
struct RecordSnapshot;

struct Status {
    bool recording{false};
    bool recovered{false};
    bool faulted{false};
    std::uint64_t received{0};
    std::uint64_t queued{0};
    std::uint64_t committed{0};
    std::uint64_t failed{0};
    std::size_t queueBytes{0};
    std::string error;
    std::uint64_t lastCommittedId{0};
    std::optional<std::int64_t> lastCommittedTimeUs;
    std::optional<std::int64_t> interruptedFromUs;
    std::optional<std::int64_t> interruptedToUs;
    std::uint64_t sessionId{0},runId{0},abnormalRuns{0};
    bool uncleanRecovery{false};
};

struct Query {
    std::string dataset;
    std::optional<std::string> device;
    std::optional<std::int64_t> fromUs;
    std::optional<std::int64_t> toUs;
    std::size_t offset{0};
    std::size_t limit{200};
    // 未指定时创建跨卷快照；令牌不透明，后续发布和导入不会改变同一快照。
    std::optional<std::int64_t> snapshot;
    std::vector<data::FieldCondition> conditions;
    std::optional<data::FieldSort> sort;
};

enum class ExportFormat { Psrec, Csv };
struct ExportOptions {
    std::uint64_t maxBytes{std::numeric_limits<std::uint64_t>::max()};
    bool overwrite{true};
};

struct Completion {
    std::uint64_t task{0};
    std::string operation;
    bool ok{false};
    std::string error;
    std::vector<data::Record> records;
    std::optional<std::int64_t> snapshot;
    bool more{false};
    std::map<std::uint64_t, data::Schema> schemas;
    std::vector<std::uint64_t> rowIds;
    std::filesystem::path path;
    std::uint64_t processed{0};
    std::shared_ptr<const RecordSnapshot> snapshotLease;
};

class Store {
public:
    Store(std::filesystem::path root, std::string protocol, std::vector<data::Schema> schemas, Config config = {});
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    std::uint64_t start();
    std::uint64_t stop();
    bool publish(std::vector<data::Record> records, std::string& error);
    std::uint64_t query(Query query);
    // 导出所有匹配记录，复用固定快照、筛选及排序；不使用分页 offset/limit。
    std::uint64_t exportRecords(std::filesystem::path path,ExportFormat format,Query query={},ExportOptions options={});
    // 导入仅登记独立历史卷，不调用 publish，也不影响实时记录计数。
    std::uint64_t importRecords(std::filesystem::path path,ImportFormat format,
        std::optional<data::CsvImportMapping> mapping={},ImportLimits limits={});
    void cancel(std::uint64_t task);
    std::optional<data::Value> get(const std::string& key) const;
    std::uint64_t set(std::string key, data::Value value);
    std::uint64_t erase(std::string key);
    std::uint64_t flush();
    Status status() const;
    std::vector<Completion> poll();
    void waitIdle();
    // 宿主重载交接专用：关闭原生连接并释放单写者资格，失败时可恢复原会话。
    void suspend();
    void resume();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace protoscope::storage
