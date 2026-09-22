#include "script_data_session.hpp"
#include "script_host_internal.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace protoscope::scripting {
namespace {
struct NullValue {};
constexpr std::size_t valueLimit = 256U * 1024U;

std::optional<std::int64_t> dataInteger(const sol::object& object)
{
    if (!object.valid() || object.get_type()!=sol::type::number) return {};
    // 快照令牌和设备时间不能经过 int 或 double 中转，否则会截断或丢失精度。
    auto* state=object.lua_state();
    sol::stack::push(state,object);
    const bool integer=lua_isinteger(state,-1);
    const auto value=integer ? lua_tointeger(state,-1):0;
    lua_pop(state,1);
    if (!integer) return {};
    return static_cast<std::int64_t>(value);
}

std::uint64_t nextPollTime()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()) + 20;
}

// 在复制字符串和递归分配前扣除预算，不能只在序列化结束后才检查大小。
struct Decoder {
    std::size_t remaining{valueLimit};
    std::unordered_set<const void*> ancestors;
    void consume(std::size_t size)
    {
        if (size > remaining) throw std::invalid_argument("data value exceeds 256 KiB");
        remaining -= size;
    }
    data::Value read(const sol::object& object, std::size_t depth = 0)
    {
        if (depth > 16) throw std::invalid_argument("data value exceeds depth 16");
        consume(9);
        switch (object.get_type()) {
        case sol::type::lua_nil: return {};
        case sol::type::boolean: return {object.as<bool>()};
        case sol::type::number: {
            sol::stack::push(object.lua_state(), object);
            const bool integer = lua_isinteger(object.lua_state(), -1);
            lua_pop(object.lua_state(), 1);
            if (integer) return {object.as<std::int64_t>()};
            const auto number = object.as<double>();
            if (!std::isfinite(number)) throw std::invalid_argument("non-finite data number");
            return {number};
        }
        case sol::type::string: {
            const auto text = object.as<std::string_view>();
            consume(text.size());
            return {std::string(text)};
        }
        case sol::type::userdata:
            if (object.is<NullValue>()) return {};
            if (object.is<ProtoBuffer>()) {
                const auto& bytes = object.as<const ProtoBuffer&>().bytes;
                consume(bytes.size());
                return {bytes};
            }
            break;
        case sol::type::table: {
            const auto table = object.as<sol::table>();
            const void* identity = table.pointer();
            if (!ancestors.insert(identity).second) throw std::invalid_argument("cyclic data table");
            data::Value::Object fields;
            std::map<std::int64_t, data::Value> elements;
            for (const auto& pair : table) {
                if (pair.first.get_type() == sol::type::string) {
                    const auto key = pair.first.as<std::string_view>();
                    consume(key.size());
                    fields.emplace(std::string(key), read(pair.second, depth + 1));
                } else if (pair.first.get_type() == sol::type::number) {
                    const auto key = dataInteger(pair.first);
                    if (!key || *key < 1 || *key > static_cast<std::int64_t>(valueLimit))
                        throw std::invalid_argument("invalid data array index");
                    elements.emplace(*key, read(pair.second, depth + 1));
                } else {
                    throw std::invalid_argument("data object requires string keys");
                }
            }
            ancestors.erase(identity);
            if (!elements.empty()) {
                if (!fields.empty() || elements.rbegin()->first != static_cast<std::int64_t>(elements.size()))
                    throw std::invalid_argument("mixed or sparse data table");
                data::Value::Array values;
                for (auto& [key, value] : elements) values.push_back(std::move(value));
                return {std::move(values)};
            }
            return {std::move(fields)};
        }
        default: break;
        }
        throw std::invalid_argument("unsupported data value");
    }
};

sol::object toLua(sol::state_view lua, const data::Value& value)
{
    return std::visit([&](const auto& item) -> sol::object {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, std::monostate>)
            return sol::make_object(lua, NullValue{});
        else if constexpr (std::is_same_v<T, data::Bytes>)
            return sol::make_object(lua, ProtoBuffer{item});
        else if constexpr (std::is_same_v<T, data::Value::Array>) {
            auto result = lua.create_table();
            for (std::size_t i = 0; i < item.size(); ++i) result[i + 1] = toLua(lua, item[i]);
            return result;
        } else if constexpr (std::is_same_v<T, data::Value::Object>) {
            auto result = lua.create_table();
            for (const auto& [key, child] : item) result[key] = toLua(lua, child);
            return result;
        } else return sol::make_object(lua, item);
    }, value.value);
}

std::vector<sol::table> tableArray(const sol::table& table, std::size_t maximum)
{
    std::map<std::int64_t, sol::table> items;
    for (const auto& pair : table) {
        const auto key = dataInteger(pair.first);
        if (!key || *key < 1 || *key > static_cast<std::int64_t>(maximum) ||
            pair.second.get_type() != sol::type::table)
            throw std::invalid_argument("expected a bounded dense array of tables");
        items.emplace(*key, pair.second.as<sol::table>());
    }
    if (!items.empty() && items.rbegin()->first != static_cast<std::int64_t>(items.size()))
        throw std::invalid_argument("sparse array");
    std::vector<sol::table> result;
    for (const auto& [key, value] : items) result.push_back(value);
    return result;
}

std::string protocolKey(const std::string& protocol)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char c : protocol) hash = (hash ^ c) * 1099511628211ULL;
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}
} // namespace

ScriptDataSession::ScriptDataSession(std::filesystem::path root, std::string protocol,storage::Config config)
    : root_(root.empty() ? root : root / protocolKey(protocol)), config_(config),protocol_(std::move(protocol)) {}

void ScriptDataSession::requireActive() const
{
    if (!active_) throw std::runtime_error("data/storage API is unavailable during script declaration");
}

storage::Store& ScriptDataSession::store()
{
    requireActive();
    if (root_.empty()) throw std::runtime_error("scripting.storage.root_dir is not configured");
    if (!store_) {
        store_ = std::make_unique<storage::Store>(root_, protocol_, schemas_,config_);
        nextPollAtMs_ = nextPollTime();
    }
    return *store_;
}

void ScriptDataSession::loadSchemas(sol::state_view lua)
{
    const sol::object declaration = lua["data"];
    if (!declaration.valid() || declaration.get_type() == sol::type::lua_nil) return;
    if (!declaration.is<sol::protected_function>()) throw std::invalid_argument("data must be a function");
    sol::protected_function function = declaration;
    auto result = function();
    if (!result.valid()) throw std::runtime_error(protectedCallError(result));
    sol::object object = result;
    if (object.get_type() != sol::type::table) throw std::invalid_argument("data() must return schemas");
    std::unordered_set<std::string> names;
    for (const auto& dataset : tableArray(object.as<sol::table>(), 128)) {
        data::Schema schema;
        schema.dataset = dataset.get<std::string>("id");
        const sol::table fields = dataset["fields"];
        for (const auto& field : tableArray(fields, 1024)) {
            const auto type = field.get<std::string>("type");
            static const std::map<std::string, data::FieldType> types{
                {"int64", data::FieldType::Int64}, {"double", data::FieldType::Double},
                {"bool", data::FieldType::Bool}, {"string", data::FieldType::String},
                {"bytes", data::FieldType::Bytes}};
            const auto iter = types.find(type);
            if (iter == types.end()) throw std::invalid_argument("unknown data field type");
            schema.fields.push_back({field.get<std::string>("name"), iter->second,
                                     field.get_or("nullable", true)});
        }
        data::validateSchema(schema);
        if (!names.insert(schema.dataset).second) throw std::invalid_argument("duplicate dataset");
        schemas_.push_back(std::move(schema));
    }
}

void ScriptDataSession::activate()
{
    active_ = true;
    // 声明数据集的协议主动恢复记录；完全未使用数据 API 的旧协议不创建目录。
    if (!root_.empty() && (!schemas_.empty() || std::filesystem::exists(root_ / "kv" / "values.sqlite")))
        store();
}
void ScriptDataSession::waitIdle() { if (store_) store_->waitIdle(); }
bool ScriptDataSession::needsPoll() const { return store_ != nullptr; }
std::uint64_t ScriptDataSession::nextPollAtMs() const { return nextPollAtMs_; }
std::vector<storage::Completion> ScriptDataSession::poll()
{
    if (!store_) return {};
    nextPollAtMs_ = nextPollTime();
    auto result = store_->poll();
    const auto status = store_->status();
    // 后台提交失败没有发布任务 ID，另发一次故障通知，不能只等待脚本主动查询。
    if (status.faulted && !faultReported_) {
        storage::Completion fault;
        fault.operation = "fault";
        fault.error = status.error;
        result.push_back(std::move(fault));
    }
    faultReported_ = status.faulted;
    return result;
}

data::Record ScriptDataSession::parseRecord(const sol::table& input) const
{
    data::Record record;
    record.protocol = protocol_;
    record.dataset = input.get<std::string>("dataset");
    record.device = input.get_or<std::string>("device", "");
    record.receivedAtUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const sol::object deviceTime = input["device_time_us"];
    if (deviceTime.valid() && deviceTime.get_type() != sol::type::lua_nil) {
        record.deviceTimeUs = dataInteger(deviceTime);
        if (!record.deviceTimeUs) throw std::invalid_argument("device_time_us must be int64");
    }
    const auto schema = std::find_if(schemas_.begin(), schemas_.end(),
        [&](const auto& value) { return value.dataset == record.dataset; });
    if (schema == schemas_.end()) throw std::invalid_argument("unknown dataset");
    const sol::table fields = input["values"];
    Decoder decoder;
    std::unordered_set<std::string> known;
    for (const auto& field : schema->fields) {
        known.insert(field.name);
        sol::object value = fields[field.name];
        // 缺失字段必须显式写成 proto.data.null，禁止静默补入旧值或空值。
        if (!value.valid() || value.get_type() == sol::type::lua_nil)
            throw std::invalid_argument("missing data field: " + field.name);
        auto decoded = decoder.read(value);
        if (field.type == data::FieldType::Double && std::holds_alternative<std::int64_t>(decoded.value))
            decoded.value = static_cast<double>(std::get<std::int64_t>(decoded.value));
        record.values.push_back(std::move(decoded));
    }
    for (const auto& pair : fields)
        if (pair.first.get_type() != sol::type::string || !known.contains(pair.first.as<std::string>()))
            throw std::invalid_argument("unknown data field");
    data::validateRecord(*schema, record);
    return record;
}

sol::table ScriptDataSession::recordTable(sol::state_view lua, const data::Record& record,
                                         const data::Schema& schema) const
{
    auto result = lua.create_table();
    result["protocol"] = record.protocol;
    result["dataset"] = record.dataset;
    result["device"] = record.device;
    result["received_at_us"] = record.receivedAtUs;
    result["schema_version"] = record.schemaVersion;
    if (record.deviceTimeUs) result["device_time_us"] = *record.deviceTimeUs;
    auto values = lua.create_table();
    for (std::size_t i = 0; i < schema.fields.size(); ++i)
        values[schema.fields[i].name] = toLua(lua, record.values.at(i));
    result["values"] = values;
    return result;
}

bool ScriptDataSession::publish(const sol::table& rows)
{
    requireActive();
    std::vector<data::Record> records;
    std::size_t bytes = 0;
    for (const auto& row : tableArray(rows, 1000)) {
        auto record = parseRecord(row);
        bytes += data::encodeValue(data::recordValue(record), {valueLimit * 2, 16}).size();
        if (bytes > 8U * 1024U * 1024U) throw std::invalid_argument("publish batch exceeds 8 MiB");
        records.push_back(std::move(record));
    }
    // 最新值按数据集有界保存；记录故障不能阻断实时监控。
    for (const auto& record : records) {
        latest_[record.dataset] = record;
        if (publishObserver_) publishObserver_(record);
    }
    auto& target = store();
    if (target.status().recording) {
        std::string error;
        if (!target.publish(std::move(records), error)) throw std::runtime_error(error);
    }
    return true;
}

sol::table ScriptDataSession::completionTable(sol::state_view lua, const storage::Completion& event) const
{
    auto result = lua.create_table();
    result["task"] = event.task;
    result["operation"] = event.operation;
    result["ok"] = event.ok;
    result["error"] = event.error;
    result["more"] = event.more;
    result["processed"] = event.processed;
    if (!event.path.empty()) {
        const auto utf8=event.path.generic_u8string();
        result["path"] = std::string(utf8.begin(),utf8.end());
    }
    if (event.snapshot) result["snapshot"] = *event.snapshot;
    auto rows = lua.create_table();
    for (std::size_t i = 0; i < event.records.size(); ++i) {
        const auto& record = event.records[i];
        auto row=recordTable(lua, record, event.schemas.at(record.schemaVersion));
        if (i<event.rowIds.size()) row["record_id"]=event.rowIds[i];
        rows[i + 1] = row;
    }
    result["records"] = rows;
    if (store_) {
        const auto status = store_->status();
        auto current = lua.create_table();
        current["received"] = status.received; current["queued"] = status.queued;
        current["committed"] = status.committed; current["failed"] = status.failed;
        current["faulted"] = status.faulted; current["error"] = status.error;
        current["last_committed_id"]=status.lastCommittedId;
        if (status.lastCommittedTimeUs) current["last_committed_time_us"]=*status.lastCommittedTimeUs;
        if (status.interruptedFromUs) current["interrupted_from_us"]=*status.interruptedFromUs;
        if (status.interruptedToUs) current["interrupted_to_us"]=*status.interruptedToUs;
        result["status"] = current;
    }
    return result;
}

void ScriptDataSession::registerApi(sol::state_view lua, sol::table& proto)
{
    lua.new_usertype<NullValue>("ProtoDataNull", sol::no_constructor,
        sol::meta_function::equal_to, [](const NullValue&, const NullValue&) { return true; });
    auto data = lua.create_table();
    data["null"] = NullValue{};
    data.set_function("is_null", [](const sol::object& value) { return value.is<NullValue>(); });
    data.set_function("bytes", [](const std::string& text) {
        if (text.size() > valueLimit) throw std::invalid_argument("bytes exceeds 256 KiB");
        return ProtoBuffer{std::vector<std::uint8_t>(text.begin(), text.end())};
    });
    // Lua 持有的闭包只保存裸状态指针，避免 lua_close 回收闭包时析构 registry 引用。
    data.set_function("publish", [this, state = lua.lua_state()](const sol::table& row) {
        sol::state_view lua(state);
        auto rows = lua.create_table(); rows[1] = row; return publish(rows);
    });
    data.set_function("publish_batch", [this](const sol::table& rows) { return publish(rows); });
    data.set_function("latest", [this, state = lua.lua_state()](const std::string& dataset) -> sol::object {
        sol::state_view lua(state);
        requireActive();
        const auto iter = latest_.find(dataset);
        if (iter == latest_.end()) return sol::make_object(lua, sol::nil);
        const auto schema = std::find_if(schemas_.begin(), schemas_.end(),
            [&](const auto& value) { return value.dataset == dataset; });
        return recordTable(lua, iter->second, *schema);
    });
    proto["data"] = data;
    auto kv = lua.create_table();
    kv.set_function("get", [this, state = lua.lua_state()](const std::string& key) -> sol::object {
        sol::state_view lua(state);
        requireActive();
        const auto value = store_ ? store_->get(key) : std::optional<data::Value>{};
        return value ? toLua(lua, *value) : sol::make_object(lua, sol::nil);
    });
    kv.set_function("set", [this](const std::string& key, const sol::object& value) {
        auto decoded = Decoder{}.read(value);
        return store().set(key, std::move(decoded));
    });
    kv.set_function("delete", [this](const std::string& key) { return store().erase(key); });
    kv.set_function("flush", [this] { return store().flush(); });
    proto["kv"] = kv;
    auto record = lua.create_table();
    record.set_function("start", [this] { return store().start(); });
    record.set_function("stop", [this] { return store().stop(); });
    record.set_function("cancel", [this](std::uint64_t task) { store().cancel(task); });
    record.set_function("status", [this, state = lua.lua_state()] {
        sol::state_view lua(state);
        const auto status = store().status();
        auto table = lua.create_table();
        table["recording"] = status.recording; table["recovered"] = status.recovered;
        table["faulted"] = status.faulted; table["received"] = status.received;
        table["queued"] = status.queued; table["committed"] = status.committed;
        table["failed"] = status.failed; table["queue_bytes"] = status.queueBytes;
        table["error"] = status.error;
        table["last_committed_id"]=status.lastCommittedId;
        if (status.lastCommittedTimeUs) table["last_committed_time_us"]=*status.lastCommittedTimeUs;
        if (status.interruptedFromUs) table["interrupted_from_us"]=*status.interruptedFromUs;
        if (status.interruptedToUs) table["interrupted_to_us"]=*status.interruptedToUs;
        return table;
    });
    const auto queryOptions=[](const sol::table& options) {
        storage::Query query;
        query.dataset = options.get_or<std::string>("dataset", "");
        if (const auto value = options.get<sol::optional<std::string>>("device")) query.device = *value;
        auto integer = [&](const char* key) -> std::optional<std::int64_t> {
            sol::object value = options[key];
            if (!value.valid() || value.get_type() == sol::type::lua_nil) return {};
            const auto result = dataInteger(value);
            if (!result) throw std::invalid_argument(std::string(key) + " must be integer");
            return result;
        };
        query.fromUs = integer("from_us"); query.toUs = integer("to_us"); query.snapshot = integer("snapshot");
        const auto offset = integer("offset").value_or(0), limit = integer("limit").value_or(200);
        if (offset < 0 || limit < 1 || limit > 1000) throw std::invalid_argument("invalid query page");
        query.offset = static_cast<std::size_t>(offset); query.limit = static_cast<std::size_t>(limit);
        const sol::object filters=options["conditions"];
        if (filters.valid() && filters.get_type()!=sol::type::lua_nil) {
            if (filters.get_type()!=sol::type::table) throw std::invalid_argument("conditions must be array");
            static const std::map<std::string,data::CompareOp> operations{
                {"eq",data::CompareOp::Equal},{"ne",data::CompareOp::NotEqual},{"lt",data::CompareOp::Less},
                {"le",data::CompareOp::LessEqual},{"gt",data::CompareOp::Greater},{"ge",data::CompareOp::GreaterEqual},
                {"contains",data::CompareOp::Contains},{"is_null",data::CompareOp::IsNull},{"not_null",data::CompareOp::NotNull}};
            for (const auto& condition:tableArray(filters.as<sol::table>(),16)) {
                const auto operation=operations.find(condition.get<std::string>("op"));
                if (operation==operations.end()) throw std::invalid_argument("unknown field comparison");
                query.conditions.push_back({condition.get<std::string>("field"),operation->second,
                                            Decoder{}.read(condition["value"])});
            }
        }
        const sol::object sort=options["sort"];
        if (sort.valid() && sort.get_type()!=sol::type::lua_nil) {
            if (sort.get_type()!=sol::type::table) throw std::invalid_argument("sort must be table");
            const auto entry=sort.as<sol::table>();
            const sol::object descending=entry["descending"];
            if (descending.valid() && descending.get_type()!=sol::type::lua_nil && descending.get_type()!=sol::type::boolean)
                throw std::invalid_argument("descending must be boolean");
            query.sort=data::FieldSort{entry.get<std::string>("field"),entry.get_or("descending",false)};
        }
        return query;
    };
    record.set_function("query", [this,queryOptions](const sol::table& options) {
        return store().query(queryOptions(options));
    });
    record.set_function("export", [this,queryOptions](const sol::table& options) {
        requireActive();
        if (!exportAuthorizer_) throw std::runtime_error("record export authorization unavailable");
        const auto format=options.get_or<std::string>("format","psrec");
        if (format!="psrec" && format!="csv") throw std::invalid_argument("export format must be psrec/csv");
        const sol::object overwrite=options["overwrite"];
        if (overwrite.valid() && overwrite.get_type()!=sol::type::lua_nil && overwrite.get_type()!=sol::type::boolean)
            throw std::invalid_argument("overwrite must be boolean");
        const auto [path,maxBytes]=exportAuthorizer_(options.get<std::string>("path"));
        return store().exportRecords(path,format=="psrec" ? storage::ExportFormat::Psrec:storage::ExportFormat::Csv,
            queryOptions(options),{maxBytes,options.get_or("overwrite",false)});
    });
    record.set_function("import", [this](const sol::table& options) {
        requireActive();
        if (!importAuthorizer_) throw std::runtime_error("record import authorization unavailable");
        const auto format=options.get_or<std::string>("format","psrec");
        if (format!="psrec" && format!="csv" && format!="mapped_csv")
            throw std::invalid_argument("import format must be psrec/csv/mapped_csv");
        std::optional<data::CsvImportMapping> mapping;
        const sol::object definition=options["mapping"];
        if (format=="mapped_csv") {
            if (definition.get_type()!=sol::type::table) throw std::invalid_argument("mapping must be table");
            Decoder{}.read(definition);
            const auto table=definition.as<sol::table>();
            const auto dataset=table.get<std::string>("dataset");
            const auto schema=std::find_if(schemas_.begin(),schemas_.end(),
                [&](const auto& entry){return entry.dataset==dataset;});
            if (schema==schemas_.end()) throw std::invalid_argument("mapping dataset must be declared");
            mapping.emplace();mapping->schema=*schema;mapping->protocol=protocol_;
            mapping->device=table.get_or<std::string>("device","");
            mapping->receivedColumn=table.get_or<std::string>("received_column","received_at_us");
            const auto optionalText=[&](const char* name) -> std::optional<std::string> {
                const sol::object value=table[name];
                if (!value.valid() || value.get_type()==sol::type::lua_nil) return {};
                if (value.get_type()!=sol::type::string) throw std::invalid_argument(std::string(name)+" must be string");
                return value.as<std::string>();
            };
            mapping->deviceColumn=optionalText("device_column");
            mapping->deviceTimeColumn=optionalText("device_time_column");
            mapping->nullToken=optionalText("null_token");
            const sol::object fields=table["fields"];
            if (fields.get_type()!=sol::type::table) throw std::invalid_argument("mapping fields must be table");
            for (const auto& [key,value]:fields.as<sol::table>()) {
                if (key.get_type()!=sol::type::string || value.get_type()!=sol::type::string)
                    throw std::invalid_argument("mapping fields require string keys and columns");
                mapping->fields.emplace(key.as<std::string>(),value.as<std::string>());
            }
        } else if (definition.valid() && definition.get_type()!=sol::type::lua_nil)
            throw std::invalid_argument("mapping is only valid for mapped_csv");
        const auto [path,maxBytes]=importAuthorizer_(options.get<std::string>("path"));
        storage::ImportLimits limits;limits.sourceBytes=maxBytes;
        return store().importRecords(path,format=="psrec" ? storage::ImportFormat::Psrec:
            format=="csv" ? storage::ImportFormat::Csv:storage::ImportFormat::MappedCsv,std::move(mapping),limits);
    });
    proto["record"] = record;
}
} // namespace protoscope::scripting
