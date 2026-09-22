#include "record_session.hpp"
#include "sqlite_database.hpp"

namespace protoscope::storage {
namespace {
constexpr data::ValueLimits limits{64U*1024U,4,128};
void identity(const std::string& value,bool allowEmpty=false)
{
    if (allowEmpty && value.empty()) return;
    if (value.size()!=32 || value.find_first_not_of("0123456789abcdef")!=std::string::npos)
        throw std::invalid_argument("invalid session volume identity");
}
void validate(const RecordSessionState& state)
{
    identity(state.activeIdentity,true);identity(state.pendingIdentity,true);
    if (state.session>INT64_MAX || state.run>INT64_MAX || state.abnormalRuns>state.run ||
        state.lastCommittedId<0 || state.error.size()>4096 ||
        (state.interruptedFromUs.has_value()!=state.interruptedToUs.has_value()) ||
        (state.interruptedFromUs && *state.interruptedFromUs>*state.interruptedToUs))
        throw std::invalid_argument("invalid record session state");
    if (state.pendingIdentity.empty()) {
        if (state.pendingFirstId || state.pendingOpenedUs) throw std::invalid_argument("inconsistent empty rotation intent");
    } else if (state.pendingIdentity==state.activeIdentity || state.pendingFirstId<1 ||
               state.pendingFirstId<=state.lastCommittedId || state.pendingOpenedUs<0)
        throw std::invalid_argument("invalid pending rotation range");
}
data::Value nullable(std::optional<std::int64_t> value) {return value ? data::Value{*value}:data::Value{};}
data::Bytes encode(const RecordSessionState& state)
{
    validate(state);
    return data::encodeValue({data::Value::Object{
        {"version",{std::int64_t{1}}},{"session",{static_cast<std::int64_t>(state.session)}},
        {"run",{static_cast<std::int64_t>(state.run)}},{"abnormal",{static_cast<std::int64_t>(state.abnormalRuns)}},
        {"recording",{state.recording}},{"clean",{state.cleanExit}},{"faulted",{state.faulted}},
        {"active",{state.activeIdentity}},{"pending",{state.pendingIdentity}},{"error",{state.error}},
        {"first",{state.pendingFirstId}},{"opened",{state.pendingOpenedUs}},{"last",{state.lastCommittedId}},
        {"time",nullable(state.lastCommittedTimeUs)},{"from",nullable(state.interruptedFromUs)},
        {"to",nullable(state.interruptedToUs)}
    }},limits);
}
RecordSessionState decode(const data::Bytes& bytes)
{
    const auto value=data::decodeValue(bytes,limits);
    const auto& fields=std::get<data::Value::Object>(value.value);
    const auto integer=[&](const char* key){return std::get<std::int64_t>(fields.at(key).value);};
    const auto text=[&](const char* key){return std::get<std::string>(fields.at(key).value);};
    const auto boolean=[&](const char* key){return std::get<bool>(fields.at(key).value);};
    const auto optional=[&](const char* key)->std::optional<std::int64_t> {
        const auto& item=fields.at(key).value;
        if (std::holds_alternative<std::monostate>(item)) return {};
        return std::get<std::int64_t>(item);
    };
    if (fields.size()!=16 || integer("version")!=1) throw std::runtime_error("unsupported record session format");
    RecordSessionState result;
    result.session=static_cast<std::uint64_t>(integer("session"));
    result.run=static_cast<std::uint64_t>(integer("run"));result.abnormalRuns=static_cast<std::uint64_t>(integer("abnormal"));
    result.recording=boolean("recording");result.cleanExit=boolean("clean");result.faulted=boolean("faulted");
    result.activeIdentity=text("active");result.pendingIdentity=text("pending");result.error=text("error");
    result.pendingFirstId=integer("first");result.pendingOpenedUs=integer("opened");result.lastCommittedId=integer("last");
    result.lastCommittedTimeUs=optional("time");result.interruptedFromUs=optional("from");result.interruptedToUs=optional("to");
    validate(result);return result;
}
void checkCatalog(sqlite::Database& db,const std::string& protocol)
{
    sqlite::Statement app(db,"PRAGMA application_id");app.row();
    sqlite::Statement version(db,"PRAGMA user_version");version.row();
    if (app.integer(0)!=0x50534958 || version.integer(0)!=1)
        throw std::runtime_error("record session requires owned catalog");
    sqlite::Statement owner(db,"SELECT value FROM metadata WHERE key='protocol'");
    if (!owner.row() || owner.text(0)!=protocol) throw std::runtime_error("record session catalog protocol mismatch");
}
}
RecordSession::RecordSession(std::filesystem::path root,std::string protocol)
    :index_(std::filesystem::weakly_canonical(std::filesystem::absolute(root))/"index.sqlite"),
     protocol_(std::move(protocol))
{
    if (!std::filesystem::is_regular_file(index_) || std::filesystem::is_symlink(index_) ||
        std::filesystem::weakly_canonical(index_)!=index_)
        throw std::runtime_error("record session catalog path is missing or linked");
    sqlite::Database db(index_,true);checkCatalog(db,protocol_);
    sqlite::Statement state(db,"SELECT value FROM metadata WHERE key='record_session'");
    if (state.row()) state_=decode(state.blob(0));
}
RecordSessionState RecordSession::state() const {std::lock_guard lock(mutex_);return state_;}
void RecordSession::save(RecordSessionState next)
{
    const auto bytes=encode(next);
    sqlite::Database db(index_);checkCatalog(db,protocol_);
    db.exec("PRAGMA synchronous=FULL; BEGIN IMMEDIATE");
    try {
        sqlite::Statement state(db,"INSERT INTO metadata(key,value) VALUES('record_session',?) "
                                   "ON CONFLICT(key) DO UPDATE SET value=excluded.value");
        state.blob(1,bytes);state.row();db.exec("COMMIT");
    } catch (...) {db.exec("ROLLBACK");throw;}
    state_=std::move(next);
}
void RecordSession::beginRun()
{
    std::lock_guard lock(mutex_);auto next=state_;
    if (next.run==INT64_MAX) throw std::overflow_error("record run ID exhausted");
    if (!next.cleanExit) ++next.abnormalRuns;
    ++next.run;next.cleanExit=false;save(std::move(next));
}
void RecordSession::cleanExit()
{
    std::lock_guard lock(mutex_);auto next=state_;next.cleanExit=true;save(std::move(next));
}
void RecordSession::start()
{
    std::lock_guard lock(mutex_);auto next=state_;
    if (!next.recording) {
        if (next.session==INT64_MAX) throw std::overflow_error("record session ID exhausted");
        ++next.session;
    }
    next.recording=true;next.faulted=false;next.error.clear();save(std::move(next));
}
void RecordSession::stop()
{
    std::lock_guard lock(mutex_);auto next=state_;next.recording=false;save(std::move(next));
}
void RecordSession::prepareRotation(std::string id,std::int64_t firstId,std::int64_t openedAtUs)
{
    std::lock_guard lock(mutex_);
    if (!state_.pendingIdentity.empty()) throw std::runtime_error("previous record rotation requires recovery");
    auto next=state_;next.pendingIdentity=std::move(id);next.pendingFirstId=firstId;next.pendingOpenedUs=openedAtUs;
    save(std::move(next));
}
void RecordSession::commitRotation(const std::string& id)
{
    std::lock_guard lock(mutex_);
    if (id.empty() || state_.pendingIdentity!=id) throw std::runtime_error("rotation commit identity mismatch");
    auto next=state_;next.activeIdentity=id;next.pendingIdentity.clear();
    next.pendingFirstId=0;next.pendingOpenedUs=0;save(std::move(next));
}
void RecordSession::cancelRotation(const std::string& id)
{
    std::lock_guard lock(mutex_);
    if (id.empty() || state_.pendingIdentity!=id) throw std::runtime_error("rotation cancellation identity mismatch");
    auto next=state_;next.pendingIdentity.clear();next.pendingFirstId=0;next.pendingOpenedUs=0;save(std::move(next));
}
void RecordSession::committed(std::int64_t id,std::int64_t receivedAtUs)
{
    std::lock_guard lock(mutex_);
    if (id<state_.lastCommittedId || !state_.pendingIdentity.empty())
        throw std::runtime_error("record commit position regressed or rotation incomplete");
    auto next=state_;next.lastCommittedId=id;next.lastCommittedTimeUs=receivedAtUs;save(std::move(next));
}
void RecordSession::fault(std::string error,std::optional<std::int64_t> fromUs,std::optional<std::int64_t> toUs)
{
    std::lock_guard lock(mutex_);auto next=state_;
    next.faulted=true;next.error=std::move(error);next.interruptedFromUs=fromUs;next.interruptedToUs=toUs;
    save(std::move(next));
}
} // namespace protoscope::storage
