#pragma once

#include "script_data_session.hpp"
#include "protoscope/scripting/script_host.hpp"

namespace protoscope::scripting {
bool parseDataTableConfig(ControlDescriptor& control,const sol::table& table,std::string& error);
void validateDataTables(std::vector<DockDescriptor>& docks,const std::vector<data::Schema>& schemas);

class DataTableSession {
public:
    DataTableSession(const std::vector<ControlDescriptor>& controls,ScriptDataSession& data);
    void publish(const data::Record& record);
    void handle(const DataTableEvent& event);
    bool complete(const storage::Completion& result);
    std::shared_ptr<const data::TablePage> page(const std::string& id);
    const data::TableView& view(const std::string& id) const;
private:
    struct Table {
        DataTableConfig config;
        data::TableView view;
        std::unique_ptr<data::LiveTable> live;
        std::shared_ptr<const data::TablePage> page;
        std::uint64_t task{0};
        std::uint64_t revision{0};
        bool dirty{true};
    };
    ScriptDataSession& data_;
    std::map<std::string,Table> tables_;
    std::map<std::uint64_t,std::string> tasks_;
};
} // namespace protoscope::scripting
