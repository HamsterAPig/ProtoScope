#pragma once

#include "protoscope/storage/store.hpp"

#include <sol/sol.hpp>
#include <functional>

namespace protoscope::scripting {

// 每个 Lua runtime 独占会话；异步结果不跨重载投递。
class ScriptDataSession {
public:
    ScriptDataSession(std::filesystem::path root, std::string protocol);
    void registerApi(sol::state_view lua, sol::table& proto);
    void loadSchemas(sol::state_view lua);
    void activate();
    void waitIdle();
    bool needsPoll() const;
    std::uint64_t nextPollAtMs() const;
    std::vector<storage::Completion> poll();
    sol::table completionTable(sol::state_view lua, const storage::Completion& completion) const;
    const std::vector<data::Schema>& schemas() const { return schemas_; }
    void setPublishObserver(std::function<void(const data::Record&)> observer) { publishObserver_ = std::move(observer); }
    std::uint64_t query(storage::Query query) { return store().query(std::move(query)); }
    void cancel(std::uint64_t task) { if (store_) store_->cancel(task); }
    using ExportAuthorizer=std::function<std::pair<std::filesystem::path,std::uint64_t>(const std::string&)>;
    void setExportAuthorizer(ExportAuthorizer authorizer) { exportAuthorizer_=std::move(authorizer); }

private:
    storage::Store& store();
    void requireActive() const;
    data::Record parseRecord(const sol::table& input) const;
    sol::table recordTable(sol::state_view lua, const data::Record& record,
                           const data::Schema& schema) const;
    bool publish(const sol::table& rows);
    std::filesystem::path root_;
    std::string protocol_;
    std::vector<data::Schema> schemas_;
    std::map<std::string, data::Record> latest_;
    std::unique_ptr<storage::Store> store_;
    bool active_{false};
    bool faultReported_{false};
    std::uint64_t nextPollAtMs_{0};
    std::function<void(const data::Record&)> publishObserver_;
    ExportAuthorizer exportAuthorizer_;
};

} // namespace protoscope::scripting
