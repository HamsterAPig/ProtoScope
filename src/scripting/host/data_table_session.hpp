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
    const DataTableExportState& exportState(const std::string& id) const;
    void prepareExport(const std::string& id,storage::ExportFormat format);
    void exportDialog(const std::string& id,std::uint64_t dialog);
    void exportError(const std::string& id,const std::string& error);
    bool fileDialog(const FileDialogEvent& event,const std::function<bool(const std::string&)>& allowed);
    void cancelExport(const std::string& id);
private:
    struct Table {
        DataTableConfig config;
        data::TableView view;
        std::unique_ptr<data::LiveTable> live;
        std::shared_ptr<const data::TablePage> page;
        std::shared_ptr<const storage::RecordSnapshot> snapshotLease;
        std::uint64_t task{0};
        std::uint64_t revision{0};
        bool dirty{true};
        DataTableExportState exportState;
        storage::ExportFormat exportFormat{storage::ExportFormat::Csv};
        storage::Query exportQuery;
        std::vector<data::TableRow> exportRows;
        std::shared_ptr<const storage::RecordSnapshot> exportLease;
        std::uint64_t exportTask{0};
    };
    ScriptDataSession& data_;
    std::map<std::string,Table> tables_;
    std::map<std::uint64_t,std::string> tasks_;
    std::map<std::uint64_t,std::string> exportTasks_;
    std::map<std::uint64_t,std::string> exportDialogs_;
};
} // namespace protoscope::scripting
