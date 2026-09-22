#include "protoscope/storage/store.hpp"
#include "record_query.hpp"
#include "record_session.hpp"
#include "record_volume_coordinator.hpp"
#include "record_export.hpp"
#include "protoscope/data/table.hpp"
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

    using sqlite::Database;
    using sqlite::Statement;

    std::int64_t wallTimeUs()
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

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
        std::filesystem::path importPath;
        ImportFormat importFormat{ImportFormat::Psrec};
        std::optional<data::CsvImportMapping> importMapping;
        ImportLimits importLimits;
        std::vector<data::Record> exportRows;
        std::map<std::uint64_t,data::Schema> exportSchemas;
        std::size_t exportBytes{0};
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
    std::size_t pendingExportBytes{0};
    std::size_t queuedRows{0};
    std::uint64_t nextTask{1};
    Status status;
    bool stopping{false};
    bool writing{false};
    bool reading{false};
    std::thread writer;
    std::thread reader;
    std::unique_ptr<VolumeCatalog> catalog;
    std::shared_ptr<void> writerLease;
    std::unique_ptr<RecordQueryService> queries;
    std::unique_ptr<RecordSession> session;
    std::unique_ptr<RecordVolumeCoordinator> volumes;
    bool faultMetadataDirty{false};
    std::mutex maintenanceMutex;
    std::chrono::steady_clock::time_point nextMaintenance{};

    Impl(std::filesystem::path directory, std::string key, std::vector<data::Schema> declarations, Config options)
        : root(std::move(directory)), protocol(std::move(key)), config(options)
    {
        if (protocol.empty() || config.queueBytes == 0 || config.batchRows == 0 ||
            config.batchInterval.count() < 1 || config.kvValueBytes == 0 || config.kvTotalBytes == 0 ||
            !config.recordMaxBytes || !config.maxVolumeBytes ||
            config.recordMaxAge.count()<0 || config.maintenanceInterval.count()<1) {
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
        // 在打开活动库前取得目录进程锁，防止另一进程写入或清理相同记录根。
        catalog=std::make_unique<VolumeCatalog>(root/"records",protocol);
        writerLease=catalog->claimWriter();
        session=std::make_unique<RecordSession>(root/"records",protocol);
        Database kv(root / "kv" / "values.sqlite");
        initialize(kv, "CREATE TABLE IF NOT EXISTS metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL);"
                       "CREATE TABLE IF NOT EXISTS values_store(key TEXT PRIMARY KEY,value BLOB NOT NULL);");
        const auto previousKvProtocol = metadata(kv, "protocol");
        if (!previousKvProtocol.empty() && previousKvProtocol != protocol) {
            throw std::runtime_error("KV 目录属于其他协议");
        }
        metadata(kv, "protocol", protocol);
        Statement values(kv, "SELECT key,value FROM values_store");
        while (values.row()) {
            auto name = values.text(0);
            auto bytes = values.blob(1);
            data::decodeValue(bytes, {config.kvValueBytes, config.kvDepth});
            cacheBytes += name.size() + bytes.size();
            if (cacheBytes > config.kvTotalBytes) throw std::runtime_error("已提交 KV 数据超过协议总量上限");
            cache.emplace(std::move(name), std::move(bytes));
        }
        openVolumes(true);
        status.uncleanRecovery=!session->state().cleanExit;
        status.recording=session->state().recording;
        status.recovered=status.recording;
        status.interruptedFromUs=session->state().interruptedFromUs;
        status.interruptedToUs=session->state().interruptedToUs;
        session->beginRun();
        if (status.recording) session->start();
        status.sessionId=session->state().session;status.runId=session->state().run;
        status.abnormalRuns=session->state().abnormalRuns;
        writer = std::thread([this] { writeLoop(); });
        try { reader = std::thread([this] { readLoop(); }); }
        catch (...) {
            { std::lock_guard lock(mutex); stopping = true; }
            changed.notify_all();
            writer.join();
            throw;
        }
    }

    void openVolumes(bool recover)
    {
        std::vector<data::Schema> declarations;
        for (const auto& [name,schema]:schemas) declarations.push_back(schema.first);
        volumes=std::make_unique<RecordVolumeCoordinator>(root/"records",protocol,declarations,
            *catalog,*session,wallTimeUs(),recover && session->state().recording);
        for (auto& [name,schema]:schemas)
            schema.second=static_cast<std::int64_t>(volumes->schemaIds().at(name));
        // 热重载失败回退时保留旧快照，仅替换新查询的活动源。
        if (!queries) queries=std::make_unique<RecordQueryService>(volumes->info().path,*catalog,config.queueBytes);
        else if (queries->activePath()!=volumes->info().path)
            queries->switchActive(volumes->info().path,[](std::shared_ptr<int>){});
        std::lock_guard lock(mutex);
        status.lastCommittedId=static_cast<std::uint64_t>(session->state().lastCommittedId);
        status.lastCommittedTimeUs=session->state().lastCommittedTimeUs;
    }

    ~Impl()
    {
        {
            std::lock_guard lock(mutex);
            stopping = true;
            for (const auto& [id, canceled] : cancellations) canceled->request_stop();
        }
        changed.notify_all();
        if (writer.joinable()) writer.join();
        if (reader.joinable()) reader.join();
        try {
            if (writerLease) {
                persistFault();
                session->cleanExit();
            }
        } catch (...) {
            // 退出时无法写入干净标记就保留异常状态，下次恢复不得假称上次正常关闭。
        }
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

    void writeOne(Database& kv, const Command& command)
    {
        if (command.operation == "start" || command.operation == "stop") {
            const bool active = command.operation == "start";
            persistFault();
            if (active) session->start(); else session->stop();
            std::lock_guard lock(mutex);
            status.recording = active;
            status.sessionId=session->state().session;
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

    void markInterrupted(const std::vector<data::Record>& records)
    {
        faultMetadataDirty=true;
        for (const auto& record:records) {
            if (!status.interruptedFromUs || record.receivedAtUs<*status.interruptedFromUs)
                status.interruptedFromUs=record.receivedAtUs;
            if (!status.interruptedToUs || record.receivedAtUs>*status.interruptedToUs)
                status.interruptedToUs=record.receivedAtUs;
        }
    }

    void maintain(bool force=false,std::stop_token stop={})
    {
        std::lock_guard maintenanceLock(maintenanceMutex);
        const auto now=std::chrono::steady_clock::now();
        if (!force && now<nextMaintenance) return;
        nextMaintenance=now+config.maintenanceInterval;
        const auto wall=std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        // 提前腾出一成空间，避免短查询占用封存卷时下一批立即撞到硬上限。
        // 低水位暂时无法达到不算故障，但实际总量超过配置上限仍必须明确失败。
        const auto cleanupTarget=config.recordMaxBytes-config.recordMaxBytes/10;
        auto result=catalog->retain({cleanupTarget,config.recordMaxAge},wall,stop);
        if (result.capacityExceeded || result.expiredPinned) {
            // 仅释放缓存自身持有的快照；当前历史页和正在导出的卷仍由引用保护。
            queries->expireUnpinned();
            result=catalog->retain({cleanupTarget,config.recordMaxAge},wall,stop);
        }
        if (result.bytes>config.recordMaxBytes)
            throw std::runtime_error("record capacity exceeded; no eligible sealed volume");
    }

    void maintenanceFault(const std::string& error)
    {
        std::lock_guard lock(mutex);
        status.faulted=true;status.recording=false;status.error=error;
        faultMetadataDirty=true;
    }
    void persistFault()
    {
        Status failure;
        {
            std::lock_guard lock(mutex);
            if (!faultMetadataDirty) return;
            failure=status;faultMetadataDirty=false;
        }
        if (!failure.faulted) return;
        try {session->fault(failure.error.substr(0,4096),failure.interruptedFromUs,failure.interruptedToUs);}
        catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            status.error="record fault metadata commit failed: "+std::string(error.what());
        }
    }

    void writeLoop()
    {
        try {
            Database kv(root / "kv" / "values.sqlite");
            kv.exec("PRAGMA synchronous=FULL");
            for (;;) {
                std::vector<Command> batch;
                {
                    std::unique_lock lock(mutex);
                    changed.wait_for(lock, config.maintenanceInterval, [&] {
                        return stopping || !writes.empty() || faultMetadataDirty;
                    });
                    if (writes.empty() && stopping) break;
                    if (writes.empty()) {
                        const bool active=status.recording;
                        lock.unlock();
                        persistFault();
                        if (active) {
                            try {maintain();} catch(const std::exception& error) {maintenanceFault(error.what());}
                        }
                        continue;
                    }
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
                try {
                    if (publishing) {
                        {
                            std::lock_guard lock(mutex);
                            if (status.faulted) throw std::runtime_error(status.error);
                        }
                        maintain();
                        // 相邻发布命令合为一次事务，移动记录避免再复制整批负载。
                        std::vector<data::Record> combined;
                        combined.reserve(rowCount);
                        for (auto& command:batch)
                            for (auto& record:command.records) combined.push_back(std::move(record));
                        try {volumes->append(combined,wallTimeUs(),config.maxVolumeBytes,queries.get());}
                        catch (...) {
                            // 故障区间仍使用原记录时间；移回后统一走失败计数。
                            auto record=combined.begin();
                            for (auto& command:batch)
                                for (auto& target:command.records) target=std::move(*record++);
                            throw;
                        }
                    } else {
                        if (batch.front().operation=="start") maintain(true);
                        writeOne(kv, batch.front());
                    }
                } catch (const std::exception& failure) {
                    error = failure.what();
                }
                {
                    std::lock_guard lock(mutex);
                    status.queueBytes -= bytes;
                    if (error.empty()) {
                        status.committed += rowCount;
                        if (publishing && rowCount) {
                            status.lastCommittedId=static_cast<std::uint64_t>(volumes->info().lastId);
                            status.lastCommittedTimeUs=volumes->info().lastReceivedTimeUs;
                        }
                    }
                    else if (publishing || batch.front().operation == "start" || batch.front().operation == "stop") {
                        status.failed += rowCount;
                        status.faulted = true;
                        status.recording = false;
                        status.error = error;
                        for (const auto& command:batch) markInterrupted(command.records);
                    }
                    if (!publishing) {
                        completions.push_back({batch.front().task, batch.front().operation, error.empty(), error});
                    }
                }
                if (publishing && error.empty()) {
                    if (rowCount) {
                        const auto committed=statusSnapshot();
                        try {session->committed(static_cast<std::int64_t>(committed.lastCommittedId),
                                                committed.lastCommittedTimeUs.value_or(0));}
                        catch(const std::exception& failure) {maintenanceFault(failure.what());}
                    }
                    try {maintain();} catch(const std::exception& failure) {maintenanceFault(failure.what());}
                }
                persistFault();
                {std::lock_guard lock(mutex);writing=false;}
                idle.notify_all();
            }
        } catch (const std::exception& error) {
            std::lock_guard lock(mutex);
            status.faulted = true;
            status.recording = false;
            status.error = error.what();
            for (const auto& command : writes) {
                status.failed += command.records.size();
                markInterrupted(command.records);
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

    Status statusSnapshot() const {std::lock_guard lock(mutex);return status;}

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
            const bool importing=!command.importPath.empty();
            Completion result{command.task, importing ? "import":exporting ? "export":"query", false};
            result.path=importing ? command.importPath:command.exportPath;
            try {
                if (command.canceled->stop_requested()) throw std::runtime_error("查询已取消");
                if (importing) {
                    auto staged=stageRecordImport(root/"records",protocol,command.importPath,command.importFormat,
                        command.importMapping,command.importLimits,command.canceled->get_token());
                    // 暂存文件已计入全局容量；登记前不足则失败，不暴露半批导入。
                    maintain(true,command.canceled->get_token());
                    const auto now=std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    // 索引提交是可见性的唯一边界；提交后才报告成功，不触发实时发布。
                    const auto volume=catalog->adopt(staged,now,command.canceled->get_token());
                    result.processed=volume.records;
                    result.ok=true;
                } else if (command.exportBytes) {
                    std::size_t index=0;
                    result.processed=exportRecordFile(command.exportPath,command.format,command.exportSchemas,
                        [&]() -> std::optional<data::Record> {
                            if (index==command.exportRows.size()) return {};
                            return std::move(command.exportRows[index++]);
                        },command.canceled->get_token(),command.exportOptions);
                    result.ok=true;
                } else result=exporting ? queries->exportRecords(command.exportPath,command.format,command.query,
                                                          command.exportOptions,command.canceled->get_token()):
                                   queries->query(command.query,command.canceled->get_token());
                result.task=command.task;
            } catch (const std::exception& error) {
                result.error = error.what();
                result.records.clear();
                result.schemas.clear();
                result.rowIds.clear();
            }
            {
                std::lock_guard lock(mutex);
                completions.push_back(std::move(result));
                pendingExportBytes-=command.exportBytes;
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
                impl_->markInterrupted(records);
                throw std::runtime_error("发布批次超过记录队列容量");
            }
            bytes += size;
        }
        {
            std::lock_guard lock(impl_->mutex);
            impl_->status.received += records.size();
            if (!impl_->status.recording || impl_->status.faulted || impl_->stopping) {
                if (impl_->status.faulted) {
                    impl_->status.failed+=records.size();
                    impl_->markInterrupted(records);
                }
                throw std::runtime_error("记录未启动或已故障");
            }
            if (bytes > impl_->config.queueBytes - impl_->status.queueBytes || impl_->writes.size() >= 65536) {
                impl_->status.faulted = true;
                impl_->status.recording = false;
                impl_->status.failed += records.size();
                impl_->status.error = "记录队列已满";
                impl_->markInterrupted(records);
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
        impl_->changed.notify_all();
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

std::uint64_t Store::exportRows(std::filesystem::path path,ExportFormat format,std::vector<data::Record> rows,
    std::map<std::uint64_t,data::Schema> schemas,ExportOptions options)
{
    if (format!=ExportFormat::Psrec && format!=ExportFormat::Csv)
        throw std::invalid_argument("unsupported record export format");
    if (rows.size()>1000 || schemas.size()>1000) throw std::invalid_argument("live export exceeds row/schema limit");
    path=exportPath(impl_->root,path);
    std::size_t bytes=sizeof(Impl::ReadCommand);
    const auto add=[&](std::size_t size) {
        if (size>impl_->config.queueBytes || bytes>impl_->config.queueBytes-size)
            throw std::runtime_error("live export exceeds memory budget");
        bytes+=size;
    };
    for (const auto& [id,schema]:schemas) {
        if (!id || id>INT64_MAX) throw std::invalid_argument("invalid export schema ID");
        data::validateSchema(schema);
        add(data::encodeValue(data::schemaValue(schema)).size()+sizeof(data::Schema)+128);
    }
    for (const auto& row:rows) {
        if (row.protocol!=impl_->protocol) throw std::invalid_argument("export protocol mismatch");
        data::validateRecord(schemas.at(row.schemaVersion),row);
        add(data::recordMemoryBytes(row));
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->pendingQueries>=16 || impl_->pendingTasks>=1024 ||
        bytes>impl_->config.queueBytes-impl_->pendingExportBytes)
        throw std::runtime_error("live export queue full or closing");
    const auto task=impl_->nextTask++;
    auto canceled=std::make_shared<std::stop_source>();
    Impl::ReadCommand command{task,{},canceled,path,format,options};
    command.exportRows=std::move(rows);command.exportSchemas=std::move(schemas);command.exportBytes=bytes;
    impl_->cancellations.emplace(task,std::move(canceled));
    impl_->reads.push_back(std::move(command));
    impl_->pendingExportBytes+=bytes;
    ++impl_->pendingTasks;++impl_->pendingQueries;
    impl_->changed.notify_all();
    return task;
}

std::uint64_t Store::importRecords(std::filesystem::path path,ImportFormat format,
    std::optional<data::CsvImportMapping> mapping,ImportLimits limits)
{
    if (format!=ImportFormat::Psrec && format!=ImportFormat::Csv && format!=ImportFormat::MappedCsv)
        throw std::invalid_argument("unsupported import format");
    if ((format==ImportFormat::MappedCsv)!=mapping.has_value() || !limits.sourceBytes || !limits.stagedBytes)
        throw std::invalid_argument("invalid import mapping or limits");
    if (path.empty() || path.native().size()>32768 ||
        path.native().find(std::filesystem::path::value_type{})!=std::filesystem::path::string_type::npos)
        throw std::invalid_argument("invalid import path");
    if (mapping) {
        data::validateSchema(mapping->schema);
        std::size_t bytes=data::encodeValue(data::schemaValue(mapping->schema),{256U*1024U,16}).size();
        bytes+=mapping->protocol.size()+mapping->device.size()+mapping->receivedColumn.size()+
            mapping->deviceColumn.value_or("").size()+mapping->deviceTimeColumn.value_or("").size()+
            mapping->nullToken.value_or("").size();
        for (const auto& [field,column]:mapping->fields) bytes+=field.size()+column.size()+64;
        if (bytes>256U*1024U) throw std::invalid_argument("import mapping exceeds 256 KiB");
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopping || impl_->pendingQueries>=16 || impl_->pendingTasks>=1024)
        throw std::runtime_error("import queue full or closing");
    const auto task=impl_->nextTask++;
    auto canceled=std::make_shared<std::stop_source>();
    Impl::ReadCommand command{task,{},canceled};
    command.importPath=std::move(path);command.importFormat=format;
    command.importMapping=std::move(mapping);command.importLimits=limits;
    command.importLimits.stagedBytes=std::min(command.importLimits.stagedBytes,impl_->config.recordMaxBytes);
    impl_->cancellations.emplace(task,std::move(canceled));
    impl_->reads.push_back(std::move(command));
    ++impl_->pendingTasks;++impl_->pendingQueries;
    impl_->changed.notify_all();
    return task;
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
        if (completion.operation == "query" || completion.operation == "export" || completion.operation == "import")
            --impl_->pendingQueries;
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
void Store::suspend()
{
    if (!impl_->writerLease) return;
    waitIdle();
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping=true;
    }
    impl_->changed.notify_all();
    if (impl_->writer.joinable()) impl_->writer.join();
    if (impl_->reader.joinable()) impl_->reader.join();
    impl_->persistFault();
    impl_->volumes.reset();
    try {impl_->session->cleanExit();}
    catch (...) {impl_->writerLease.reset();throw;}
    impl_->writerLease.reset();
}
void Store::resume()
{
    if (impl_->writer.joinable() || impl_->reader.joinable()) return;
    impl_->writerLease=impl_->catalog->claimWriter();
    try {
        impl_->session=std::make_unique<RecordSession>(impl_->root/"records",impl_->protocol);
        impl_->openVolumes(false);
        impl_->session->beginRun();
        std::lock_guard lock(impl_->mutex);
        const auto persisted=impl_->session->state();
        impl_->status.runId=persisted.run;
        impl_->status.sessionId=persisted.session;
        impl_->status.abnormalRuns=persisted.abnormalRuns;
        // 暂停期间可能有替换实例提交启停或故障，不能沿用旧内存中的录制开关。
        impl_->status.recording=persisted.recording && !persisted.faulted;
        impl_->status.faulted=persisted.faulted;
        impl_->status.error=persisted.error;
        impl_->status.interruptedFromUs=persisted.interruptedFromUs;
        impl_->status.interruptedToUs=persisted.interruptedToUs;
    } catch (...) {impl_->volumes.reset();impl_->writerLease.reset();throw;}
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping=false;
    }
    try {
        impl_->writer=std::thread([state=impl_.get()]{state->writeLoop();});
        impl_->reader=std::thread([state=impl_.get()]{state->readLoop();});
    } catch (...) {
        {std::lock_guard lock(impl_->mutex);impl_->stopping=true;}
        impl_->changed.notify_all();
        if (impl_->writer.joinable()) impl_->writer.join();
        impl_->volumes.reset();
        impl_->writerLease.reset();
        throw;
    }
}

} // namespace protoscope::storage
