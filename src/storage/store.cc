#include "protoscope/storage/store.hpp"
#include "query_functions.hpp"
#include "record_export.hpp"
#include "sqlite_database.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace protoscope::storage {
namespace {
    static_assert(SQLITE_VERSION_NUMBER == 3050004, "SQLite version must remain pinned");
    constexpr int kApplicationId = 0x50534442;
    constexpr data::ValueLimits kRecordLimits{32U * 1024U * 1024U, 16};

    using sqlite::Database;
    using sqlite::Statement;

    void initialize(Database& db, const char* schema)
    {
        {
            Statement identity(db, "PRAGMA application_id");
            if (!identity.row()) throw std::runtime_error("无法读取数据库身份");
            const auto id = identity.integer(0);
            if (id != 0 && id != kApplicationId) {
                throw std::runtime_error("数据库不属于 ProtoScope");
            }
            if (id == 0) {
                Statement tables(db, "SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'");
                if (tables.row() && tables.integer(0) != 0) {
                    throw std::runtime_error("拒绝接管未标识的已有数据库");
                }
            }
        }
        {
            Statement version(db, "PRAGMA user_version");
            if (version.row() && version.integer(0) > 1) throw std::runtime_error("不支持较新的存储格式");
        }
        {
            Statement integrity(db, "PRAGMA quick_check");
            if (!integrity.row() || integrity.text(0) != "ok") throw std::runtime_error("数据库完整性校验失败");
        }
        db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA application_id=1347634242; "
                "PRAGMA user_version=1;");
        db.exec(schema);
    }

    void metadata(Database& db, const std::string& key, const std::string& value)
    {
        Statement statement(db, "INSERT INTO metadata(key,value) VALUES(?,?) "
                                "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        statement.text(1, key);
        statement.text(2, value);
        statement.row();
    }

    std::string metadata(Database& db, const std::string& key)
    {
        Statement statement(db, "SELECT value FROM metadata WHERE key=?");
        statement.text(1, key);
        return statement.row() ? statement.text(0) : std::string{};
    }

    void validateQuery(const Query& query)
    {
        data::validateConditions(query.conditions);
        if (query.sort && (query.sort->field.empty() || query.sort->field.size()>4096 ||
                          query.sort->field.find('\0')!=query.sort->field.npos))
            throw std::invalid_argument("无效排序字段");
        if (query.limit==0 || query.limit>1000 || query.offset>static_cast<std::size_t>(INT64_MAX) ||
            (query.snapshot && *query.snapshot<0) || (query.fromUs && query.toUs && *query.fromUs>*query.toUs) ||
            query.dataset.size()>128 || (query.device && query.device->size()>4096))
            throw std::invalid_argument("历史查询参数无效");
    }

    std::filesystem::path exportPath(const std::filesystem::path& root,const std::filesystem::path& path)
    {
        if (path.empty() || path.native().size()>32768 ||
            path.native().find(std::filesystem::path::value_type{})!=std::filesystem::path::string_type::npos)
            throw std::invalid_argument("invalid record export path");
        const auto target=std::filesystem::weakly_canonical(std::filesystem::absolute(path));
        const auto records=std::filesystem::weakly_canonical(root/"records");
        const auto kv=std::filesystem::weakly_canonical(root/"kv");
        // 导出不能覆盖数据库、WAL 或未来分卷；链接先解析到真实路径再判定目录边界。
        for (auto parent=target;!parent.empty();) {
            std::error_code ec;
            if (parent==records || parent==kv ||
                std::filesystem::equivalent(parent,records,ec) || std::filesystem::equivalent(parent,kv,ec))
                throw std::invalid_argument("record export cannot target storage directories");
            const auto next=parent.parent_path();
            if (next==parent) break;
            parent=next;
        }
        return target;
    }
}

struct Store::Impl {
    struct Command {
        std::uint64_t task{0};
        std::string operation;
        std::size_t bytes{0};
        std::vector<data::Record> records;
        std::string key;
        data::Bytes value;
    };
    struct ReadCommand {
        std::uint64_t task;
        Query query;
        std::shared_ptr<std::stop_source> canceled;
        std::filesystem::path exportPath;
        ExportFormat format{ExportFormat::Psrec};
        ExportOptions exportOptions;
    };

    std::filesystem::path root;
    std::string protocol;
    Config config;
    std::map<std::string, std::pair<data::Schema, std::int64_t>> schemas;
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::condition_variable idle;
    std::deque<Command> writes;
    std::deque<ReadCommand> reads;
    std::map<std::uint64_t, std::shared_ptr<std::stop_source>> cancellations;
    std::vector<Completion> completions;
    std::map<std::string, data::Bytes> cache;
    std::size_t cacheBytes{0};
    std::size_t pendingTasks{0};
    std::size_t pendingQueries{0};
    std::size_t queuedRows{0};
    std::uint64_t nextTask{1};
    Status status;
    bool stopping{false};
    bool writing{false};
    bool reading{false};
    std::thread writer;
    std::thread reader;

    Impl(std::filesystem::path directory, std::string key, std::vector<data::Schema> declarations, Config options)
        : root(std::move(directory)), protocol(std::move(key)), config(options)
    {
        if (protocol.empty() || config.queueBytes == 0 || config.batchRows == 0 ||
            config.batchInterval.count() < 1 || config.kvValueBytes == 0 || config.kvTotalBytes == 0) {
            throw std::invalid_argument("存储配置无效");
        }
        completions.reserve(1024);
        for (const auto& schema : declarations) {
            data::validateSchema(schema);
            if (!schemas.emplace(schema.dataset, std::pair{schema, 0}).second) {
                throw std::invalid_argument("数据集重复");
            }
        }
        std::filesystem::create_directories(root / "kv");
        std::filesystem::create_directories(root / "records");
        Database records(root / "records" / "records.sqlite");
        initialize(records,
            "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
            "CREATE TABLE IF NOT EXISTS schemas(id INTEGER PRIMARY KEY,dataset TEXT NOT NULL,definition BLOB NOT NULL,"
            "UNIQUE(dataset,definition));"
            "CREATE TABLE IF NOT EXISTS records(id INTEGER PRIMARY KEY AUTOINCREMENT,dataset TEXT NOT NULL,"
            "device TEXT NOT NULL,received_us INTEGER NOT NULL,schema_id INTEGER NOT NULL,payload BLOB NOT NULL);"
            "CREATE INDEX IF NOT EXISTS records_time ON records(received_us,id);"
            "CREATE INDEX IF NOT EXISTS records_dataset_time ON records(dataset,received_us,id);");
        const auto previousProtocol = metadata(records, "protocol");
        if (!previousProtocol.empty() && previousProtocol != protocol) {
            throw std::runtime_error("存储目录属于其他协议");
        }
        Database kv(root / "kv" / "values.sqlite");
        initialize(kv, "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                       "CREATE TABLE IF NOT EXISTS values_store(key TEXT PRIMARY KEY,value BLOB NOT NULL);");
        const auto previousKvProtocol = metadata(kv, "protocol");
        if (!previousKvProtocol.empty() && previousKvProtocol != protocol) {
            throw std::runtime_error("KV 目录属于其他协议");
        }
        metadata(records, "protocol", protocol);
        metadata(kv, "protocol", protocol);
        for (auto& [name, schema] : schemas) {
            const auto encoded = data::encodeValue(data::schemaValue(schema.first), kRecordLimits);
            Statement insert(records, "INSERT OR IGNORE INTO schemas(dataset,definition) VALUES(?,?)");
            insert.text(1, name);
            insert.blob(2, encoded);
            insert.row();
            Statement find(records, "SELECT id FROM schemas WHERE dataset=? AND definition=?");
            find.text(1, name);
            find.blob(2, encoded);
            if (!find.row()) throw std::runtime_error("登记数据集模式失败");
            schema.second = find.integer(0);
        }
        // 只有已提交的记录开关触发恢复，构造本身不发送设备命令。
        status.recording = metadata(records, "recording") == "1";
        status.recovered = status.recording;
        Statement values(kv, "SELECT key,value FROM values_store");
        while (values.row()) {
            auto name = values.text(0);
            auto bytes = values.blob(1);
            data::decodeValue(bytes, {config.kvValueBytes, config.kvDepth});
            cacheBytes += name.size() + bytes.size();
            if (cacheBytes > config.kvTotalBytes) throw std::runtime_error("已提交 KV 数据超过协议总量上限");
            cache.emplace(std::move(name), std::move(bytes));
        }
        writer = std::thread([this] { writeLoop(); });
        try { reader = std::thread([this] { readLoop(); }); }
        catch (...) {
            { std::lock_guard lock(mutex); stopping = true; }
            changed.notify_all();
            writer.join();
            throw;
        }
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (const auto& [id, canceled] : cancellations) canceled->request_stop();
        }
        changed.notify_all();
        writer.join();
        reader.join();
    }

    std::uint64_t enqueue(Command command)
    {
        std::lock_guard lock(mutex);
        if (stopping || command.bytes > config.queueBytes - status.queueBytes ||
            writes.size() >= 65536 || pendingTasks >= 1024) {
            throw std::runtime_error("存储写队列已满或正在关闭");
        }
        command.task = nextTask++;
        const auto id = command.task;
        status.queueBytes += command.bytes;
        writes.push_back(std::move(command));
        ++pendingTasks;
        changed.notify_all();
        return id;
    }

    void writeOne(Database& records, Database& kv, const Command& command)
    {
        if (command.operation == "start" || command.operation == "stop") {
            const bool active = command.operation == "start";
            metadata(records, "recording", active ? "1" : "0");
            std::lock_guard lock(mutex);
            status.recording = active;
            if (active) {
                status.faulted = false;
                status.error.clear();
            }
        } else if (command.operation == "set" || command.operation == "delete") {
            // 提交前准备缓存节点，避免数据库提交后因分配失败而保留旧缓存。
            std::map<std::string, data::Bytes> staged;
            if (command.operation == "set") staged.emplace(command.key, command.value);
            std::size_t nextBytes;
            {
                std::lock_guard lock(mutex);
                const auto existing = cache.find(command.key);
                const auto oldSize = existing == cache.end() ? 0 : existing->first.size() + existing->second.size();
                nextBytes = cacheBytes - oldSize;
                if (command.operation == "set") nextBytes += command.key.size() + command.value.size();
                if (nextBytes > config.kvTotalBytes) throw std::runtime_error("KV 超过协议总量上限");
            }
            kv.exec("BEGIN IMMEDIATE");
            try {
                Statement statement(kv, command.operation == "set"
                    ? "INSERT INTO values_store(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"
                    : "DELETE FROM values_store WHERE key=?");
                statement.text(1, command.key);
                if (command.operation == "set") statement.blob(2, command.value);
                statement.row();
                kv.exec("COMMIT");
            } catch (...) {
                kv.exec("ROLLBACK");
                throw;
            }
            // 缓存只反映已经提交的数据，失败不能污染 get 的结果。
            std::lock_guard lock(mutex);
            cache.erase(command.key);
            if (command.operation == "set") cache.insert(staged.extract(staged.begin()));
            cacheBytes = nextBytes;
        }
    }

    void writeLoop()
    {
        try {
            Database records(root / "records" / "records.sqlite");
            Database kv(root / "kv" / "values.sqlite");
            records.exec("PRAGMA synchronous=FULL");
            kv.exec("PRAGMA synchronous=FULL");
            for (;;) {
                std::vector<Command> batch;
                {
                    std::unique_lock lock(mutex);
                    changed.wait(lock, [&] { return stopping || !writes.empty(); });
                    if (writes.empty() && stopping) break;
                    if (writes.front().operation == "publish" && !stopping) {
                        changed.wait_for(lock, config.batchInterval, [&] {
                            return stopping || queuedRows >= config.batchRows ||
                                   (!writes.empty() && writes.back().operation != "publish");
                        });
                    }
                    const bool publishing = writes.front().operation == "publish";
                    std::size_t rows = 0;
                    do {
                        rows += writes.front().records.size();
                        queuedRows -= writes.front().records.size();
                        batch.push_back(std::move(writes.front()));
                        writes.pop_front();
                    } while (publishing && !writes.empty() && writes.front().operation == "publish" &&
                             rows < config.batchRows);
                    writing = true;
                }
                const bool publishing = batch.front().operation == "publish";
                std::size_t rowCount = 0;
                std::size_t bytes = 0;
                for (const auto& command : batch) {
                    rowCount += command.records.size();
                    bytes += command.bytes;
                }
                std::string error;
                bool inTransaction = false;
                try {
                    if (publishing) {
                        records.exec("BEGIN IMMEDIATE");
                        inTransaction = true;
                        for (const auto& command : batch) {
                            for (const auto& record : command.records) {
                                Statement statement(records, "INSERT INTO records(dataset,device,received_us,schema_id,payload) "
                                                             "VALUES(?,?,?,?,?)");
                                statement.text(1, record.dataset);
                                statement.text(2, record.device);
                                statement.integer(3, record.receivedAtUs);
                                statement.integer(4, static_cast<std::int64_t>(record.schemaVersion));
                                statement.blob(5, data::encodeValue(data::recordValue(record), kRecordLimits));
                                statement.row();
                            }
                        }
                        records.exec("COMMIT");
                        inTransaction = false;
                    } else {
                        writeOne(records, kv, batch.front());
                    }
                } catch (const std::exception& failure) {
                    error = failure.what();
                    if (inTransaction) {
                        try { records.exec("ROLLBACK"); } catch (...) {}
                    }
                }
                {
                    std::lock_guard lock(mutex);
                    status.queueBytes -= bytes;
                    if (error.empty()) status.committed += rowCount;
                    else if (publishing || batch.front().operation == "start" || batch.front().operation == "stop") {
                        status.failed += rowCount;
                        status.faulted = true;
                        status.recording = false;
                        status.error = error;
                    }
                    if (!publishing) {
                        completions.push_back({batch.front().task, batch.front().operation, error.empty(), error});
                    }
                    writing = false;
                }
                idle.notify_all();
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            status.faulted = true;
            status.recording = false;
            status.error = error.what();
            for (const auto& command : writes) {
                status.failed += command.records.size();
                if (command.operation != "publish") completions.push_back({command.task, command.operation, false, error.what()});
            }
            writes.clear();
            queuedRows = 0;
            status.queueBytes = 0;
            writing = false;
            stopping = true;
            changed.notify_all();
            idle.notify_all();
        }
    }

    void readLoop()
    {
        for (;;) {
            ReadCommand command;
            {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stopping || !reads.empty(); });
                if (reads.empty() && stopping) break;
                command = std::move(reads.front());
                reads.pop_front();
                reading = true;
            }
            const bool exporting=!command.exportPath.empty();
            Completion result{command.task, exporting ? "export":"query", false};
            result.path=command.exportPath;
            try {
                if (command.canceled->stop_requested()) throw std::runtime_error("查询已取消");
                Database db(root / "records" / "records.sqlite", true);
                registerQueryFunctions(db.get());
                sqlite3_progress_handler(db.get(), 1000, [](void* flag) {
                    return static_cast<std::stop_source*>(flag)->stop_requested() ? 1 : 0;
                }, command.canceled.get());
                db.exec("BEGIN");
                auto cutoff = command.query.snapshot;
                {
                    Statement high(db, "SELECT coalesce(max(id),0) FROM records");
                    high.row();
                    const auto maximum = high.integer(0);
                    if (!cutoff) cutoff = maximum;
                    else if (*cutoff > maximum) throw std::runtime_error("查询快照超出已提交高水位");
                }
                result.snapshot = cutoff;
                const auto& query = command.query;
                const bool fieldQuery=!query.conditions.empty() || query.sort.has_value();
                std::string sql="SELECT r.id,r.payload FROM records r ";
                if (fieldQuery) sql+="JOIN schemas s ON s.id=r.schema_id ";
                sql+="WHERE r.id<=? AND (?='' OR r.dataset=?) AND (?=0 OR r.device=?) "
                     "AND (?=0 OR r.received_us>=?) AND (?=0 OR r.received_us<=?) ";
                if (!query.conditions.empty()) sql+="AND ps_matches(r.payload,s.definition,?) ";
                sql+="ORDER BY ";
                if (query.sort) {
                    const std::string direction=query.sort->descending ? " DESC,":" ASC,";
                    sql+="ps_field_type(r.payload,s.definition,?)"+direction+"ps_field(r.payload,s.definition,?)"+direction;
                }
                // 排序筛选在 LIMIT 之前完成；相同字段值始终以时间和记录 ID 打破平局。
                sql+="r.received_us,r.id LIMIT ? OFFSET ?";
                Statement statement(db,sql.c_str());
                statement.integer(1, *cutoff);
                statement.text(2, query.dataset);
                statement.text(3, query.dataset);
                statement.integer(4, query.device.has_value());
                statement.text(5, query.device.value_or(""));
                statement.integer(6, query.fromUs.has_value());
                statement.integer(7, query.fromUs.value_or(0));
                statement.integer(8, query.toUs.has_value());
                statement.integer(9, query.toUs.value_or(0));
                int parameter=10;
                if (!query.conditions.empty())
                    statement.blob(parameter++,data::encodeValue(data::conditionsValue(query.conditions),{128U*1024U,4}));
                if (query.sort) {
                    statement.text(parameter++,query.sort->field);
                    statement.text(parameter++,query.sort->field);
                }
                statement.integer(parameter++, exporting ? -1:static_cast<std::int64_t>(query.limit + 1));
                statement.integer(parameter, exporting ? 0:static_cast<std::int64_t>(query.offset));
                if (exporting) {
                    std::map<std::uint64_t,data::Schema> definitions;
                    Statement versions(db,"SELECT id,definition FROM schemas WHERE (?='' OR dataset=?) LIMIT 1025");
                    versions.text(1,query.dataset);versions.text(2,query.dataset);
                    std::size_t schemaBytes=0;
                    while (versions.row()) {
                        if (command.canceled->stop_requested()) throw std::runtime_error("record export canceled");
                        const auto encoded=versions.blob(1);
                        if (definitions.size()>=1024 || encoded.size()>kRecordLimits.maxBytes-schemaBytes)
                            throw std::runtime_error("export schema metadata exceeds limit");
                        schemaBytes+=encoded.size();
                        definitions.emplace(static_cast<std::uint64_t>(versions.integer(0)),
                            data::schemaFromValue(data::decodeValue(encoded,kRecordLimits)));
                    }
                    result.processed=exportRecordFile(command.exportPath,command.format,definitions,[&]() -> std::optional<data::Record> {
                        if (command.canceled->stop_requested()) throw std::runtime_error("record export canceled");
                        if (!statement.row()) return std::nullopt;
                        return data::recordFromValue(data::decodeValue(statement.blob(1),kRecordLimits));
                    },command.canceled->get_token(),command.exportOptions);
                } else {
                std::size_t resultBytes = 0;
                while (statement.row()) {
                    if (command.canceled->stop_requested()) throw std::runtime_error("查询已取消");
                    if (result.records.size() == query.limit) { result.more = true; break; }
                    const auto payload = statement.blob(1);
                    // 最多 16 个未消费查询共享结果内存预算，页大小不等于无界字节数。
                    if (payload.size() > config.queueBytes / 16 - resultBytes) {
                        throw std::runtime_error("查询页超过结果内存预算，请减小页大小");
                    }
                    auto record = data::recordFromValue(data::decodeValue(payload, kRecordLimits));
                    if (!result.schemas.contains(record.schemaVersion)) {
                        Statement schema(db, "SELECT definition FROM schemas WHERE id=?");
                        schema.integer(1, static_cast<std::int64_t>(record.schemaVersion));
                        if (!schema.row()) throw std::runtime_error("历史记录的模式版本缺失");
                        auto definition = data::schemaFromValue(data::decodeValue(schema.blob(0), kRecordLimits));
                        auto schemaBytes = sizeof(data::Schema) + definition.dataset.capacity() +
                            definition.fields.capacity() * sizeof(data::Field);
                        for (const auto& field : definition.fields) schemaBytes += field.name.capacity();
                        if (schemaBytes > config.queueBytes / 16 - resultBytes) {
                            throw std::runtime_error("查询模式超过结果内存预算，请减小页大小");
                        }
                        resultBytes += schemaBytes;
                        result.schemas.emplace(record.schemaVersion, std::move(definition));
                    }
                    data::validateRecord(result.schemas.at(record.schemaVersion), record);
                    const auto memory = payload.size() + sizeof(data::Record) + sizeof(std::uint64_t) +
                        record.values.size() * sizeof(data::Value);
                    if (memory > config.queueBytes / 16 - resultBytes) {
                        throw std::runtime_error("查询页超过结果内存预算，请减小页大小");
                    }
                    resultBytes += memory;
                    result.records.push_back(std::move(record));
                    result.rowIds.push_back(static_cast<std::uint64_t>(statement.integer(0)));
                }
                if (command.canceled->stop_requested()) throw std::runtime_error("查询已取消");
                }
                result.ok = true;
            } catch (const std::exception& error) {
                result.error = error.what();
                result.records.clear();
                result.schemas.clear();
                result.rowIds.clear();
            }
            {
                std::lock_guard lock(mutex);
                completions.push_back(std::move(result));
                cancellations.erase(command.task);
                reading = false;
            }
            idle.notify_all();
        }
    }
};

Store::Store(std::filesystem::path root, std::string protocol, std::vector<data::Schema> schemas, Config config)
    : impl_(std::make_unique<Impl>(std::move(root), std::move(protocol), std::move(schemas), config)) {}
Store::~Store() = default;

std::uint64_t Store::start() { return impl_->enqueue({0, "start", 64}); }
std::uint64_t Store::stop() { return impl_->enqueue({0, "stop", 64}); }
std::uint64_t Store::flush() { return impl_->enqueue({0, "flush", 64}); }

bool Store::publish(std::vector<data::Record> records, std::string& error)
{
    error.clear();
    std::size_t bytes = sizeof(Impl::Command) + records.capacity() * sizeof(data::Record);
    try {
        for (auto& record : records) {
            const auto schema = impl_->schemas.find(record.dataset);
            if (schema == impl_->schemas.end() || record.protocol != impl_->protocol) {
                throw std::invalid_argument("协议或数据集未声明");
            }
            data::validateRecord(schema->second.first, record);
            record.schemaVersion = static_cast<std::uint64_t>(schema->second.second);
        }
        // 整批模式校验完成后再做容量判定，非法批次不能改变记录状态。
        for (const auto& record : records) {
            auto size = record.values.capacity() * sizeof(data::Value) +
                record.protocol.capacity() + record.dataset.capacity() + record.device.capacity();
            for (const auto& value : record.values) {
                if (const auto* text = std::get_if<std::string>(&value.value)) size += text->capacity();
                if (const auto* blob = std::get_if<data::Bytes>(&value.value)) size += blob->capacity();
            }
            if (size > impl_->config.queueBytes || bytes > impl_->config.queueBytes - size) {
                std::lock_guard lock(impl_->mutex);
                impl_->status.received += records.size();
                impl_->status.failed += records.size();
                impl_->status.faulted = true;
                impl_->status.recording = false;
                impl_->status.error = "发布批次超过记录队列容量";
                throw std::runtime_error("发布批次超过记录队列容量");
            }
            bytes += size;
        }
        {
            std::lock_guard lock(impl_->mutex);
            impl_->status.received += records.size();
            if (!impl_->status.recording || impl_->status.faulted || impl_->stopping) {
                throw std::runtime_error("记录未启动或已故障");
            }
            if (bytes > impl_->config.queueBytes - impl_->status.queueBytes || impl_->writes.size() >= 65536) {
                impl_->status.faulted = true;
                impl_->status.recording = false;
                impl_->status.failed += records.size();
                impl_->status.error = "记录队列已满";
                throw std::runtime_error(impl_->status.error);
            }
            impl_->status.queueBytes += bytes;
            impl_->status.queued += records.size();
            impl_->queuedRows += records.size();
            impl_->writes.push_back({0, "publish", bytes, std::move(records)});
        }
        impl_->changed.notify_all();
        return true;
    } catch (const std::exception& failure) {
        error = failure.what();
        return false;
    }
}

std::uint64_t Store::query(Query query)
{
    validateQuery(query);
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->pendingQueries >= 16 || impl_->pendingTasks >= 1024) {
        throw std::runtime_error("查询队列已满或正在关闭");
    }
    const auto task = impl_->nextTask++;
    auto canceled = std::make_shared<std::stop_source>();
    impl_->cancellations.emplace(task, canceled);
    impl_->reads.push_back({task, std::move(query), std::move(canceled)});
    ++impl_->pendingTasks;
    ++impl_->pendingQueries;
    impl_->changed.notify_all();
    return task;
}

std::uint64_t Store::exportRecords(std::filesystem::path path,ExportFormat format,Query query,ExportOptions options)
{
    validateQuery(query);
    if (format!=ExportFormat::Psrec && format!=ExportFormat::Csv)
        throw std::invalid_argument("unsupported record export format");
    path=exportPath(impl_->root,path);
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->pendingQueries>=16 || impl_->pendingTasks>=1024)
        throw std::runtime_error("export queue full or closing");
    const auto task=impl_->nextTask++;
    auto canceled=std::make_shared<std::stop_source>();
    impl_->cancellations.emplace(task,canceled);
    impl_->reads.push_back({task,std::move(query),std::move(canceled),std::move(path),format,options});
    ++impl_->pendingTasks;++impl_->pendingQueries;
    impl_->changed.notify_all();
    return task;
}

void Store::cancel(std::uint64_t task)
{
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->cancellations.find(task);
    if (found != impl_->cancellations.end()) found->second->request_stop();
}

std::optional<data::Value> Store::get(const std::string& key) const
{
    std::lock_guard lock(impl_->mutex);
    const auto found = impl_->cache.find(key);
    return found == impl_->cache.end() ? std::nullopt
        : std::optional{data::decodeValue(found->second, {impl_->config.kvValueBytes, impl_->config.kvDepth})};
}

std::uint64_t Store::set(std::string key, data::Value value)
{
    if (key.empty() || key.size() > 4096) throw std::invalid_argument("KV 键长度必须为 1 到 4096 字节");
    auto encoded = data::encodeValue(value, {impl_->config.kvValueBytes, impl_->config.kvDepth});
    const auto size = key.size() + encoded.size() + 64;
    return impl_->enqueue({0, "set", size, {}, std::move(key), std::move(encoded)});
}

std::uint64_t Store::erase(std::string key)
{
    if (key.empty() || key.size() > 4096) throw std::invalid_argument("KV 键长度必须为 1 到 4096 字节");
    const auto size = key.size() + 64;
    return impl_->enqueue({0, "delete", size, {}, std::move(key)});
}

Status Store::status() const { std::lock_guard lock(impl_->mutex); return impl_->status; }

std::vector<Completion> Store::poll()
{
    std::lock_guard lock(impl_->mutex);
    std::vector<Completion> result;
    result.swap(impl_->completions);
    impl_->pendingTasks -= result.size();
    for (const auto& completion : result) {
        if (completion.operation == "query" || completion.operation == "export") --impl_->pendingQueries;
    }
    return result;
}

void Store::waitIdle()
{
    std::unique_lock lock(impl_->mutex);
    impl_->idle.wait(lock, [&] {
        return impl_->writes.empty() && impl_->reads.empty() && !impl_->writing && !impl_->reading;
    });
}

} // namespace protoscope::storage
