#include "data_table_session.hpp"
#include "script_host_internal.hpp"

#include <algorithm>
#include <charconv>
#include <set>
#include <stdexcept>

namespace protoscope::scripting {
namespace {
std::string stringField(const sol::table& table,const char* name,std::string fallback={})
{
    const sol::object value=table[name];
    if (!value.valid() || value.get_type()==sol::type::lua_nil) return fallback;
    if (value.get_type()!=sol::type::string) throw std::invalid_argument(std::string(name)+" must be string");
    auto text=value.as<std::string>();
    if (text.size()>4096 || text.find('\0')!=text.npos) throw std::invalid_argument("invalid table text");
    return text;
}
std::size_t integerField(const sol::table& table,const char* name,std::size_t fallback,std::size_t low,std::size_t high)
{
    const sol::object value=table[name];
    if (!value.valid() || value.get_type()==sol::type::lua_nil) return fallback;
    if (value.get_type()!=sol::type::number || !value.is<std::int64_t>()) throw std::invalid_argument("integer required");
    const auto number=value.as<std::int64_t>();
    if (number<static_cast<std::int64_t>(low) || number>static_cast<std::int64_t>(high))
        throw std::invalid_argument(std::string(name)+" outside range");
    return static_cast<std::size_t>(number);
}
}

bool parseDataTableConfig(ControlDescriptor& control,const sol::table& table,std::string& error)
{
    if (control.type!=ControlType::DataTable) return true;
    try {
        DataTableConfig config;
        config.dataset=stringField(table,"dataset");
        if (config.dataset.empty()) throw std::invalid_argument("data_table requires dataset");
        const auto mode=stringField(table,"mode","live");
        if (mode!="live" && mode!="history") throw std::invalid_argument("table mode must be live/history");
        config.history=mode=="history";
        const sol::object device=table["device"];
        if (device.valid() && device.get_type()!=sol::type::lua_nil) config.device=stringField(table,"device");
        config.maxRows=integerField(table,"max_rows",200,1,1000);
        config.maxBytes=integerField(table,"max_bytes",4U*1024U*1024U,1024,16U*1024U*1024U);
        config.pageSize=integerField(table,"page_size",200,1,1000);
        config.visibleRows=static_cast<int>(integerField(table,"visible_rows",10,3,40));
        const sol::object columns=table["columns"];
        if (columns.valid() && columns.get_type()!=sol::type::lua_nil) {
            if (columns.get_type()!=sol::type::table) throw std::invalid_argument("columns must be array");
            const auto entries=columns.as<sol::table>();
            std::size_t count=0;
            for (const auto& [key,value]:entries) {
                if (key.get_type()!=sol::type::number || !key.is<int>() || key.as<int>()<1 || key.as<int>()>32)
                    throw std::invalid_argument("table allows at most 32 columns");
                ++count;
            }
            if (count==0) throw std::invalid_argument("empty table columns");
            for (std::size_t i=1;i<=count;++i) {
                const sol::object item=entries[i];
                DataTableColumn column;
                if (item.get_type()==sol::type::string) {
                    column.field=item.as<std::string>();column.label=column.field;
                    if (column.field.size()>4096 || column.field.find('\0')!=std::string::npos)
                        throw std::invalid_argument("invalid table column text");
                } else if (item.get_type()==sol::type::table) {
                    const auto entry=item.as<sol::table>();
                    column.field=stringField(entry,"field");column.label=stringField(entry,"label",column.field);
                    column.unit=stringField(entry,"unit");
                    column.precision=static_cast<int>(integerField(entry,"precision",3,0,12));
                } else throw std::invalid_argument("invalid table column");
                if (column.field.empty() || column.label.empty()) throw std::invalid_argument("empty column field/label");
                config.columns.push_back(std::move(column));
            }
        }
        control.dataTable=std::move(config);
        return true;
    } catch (const std::exception& exception) {error=exception.what();return false;}
}

void validateDataTables(std::vector<DockDescriptor>& docks,const std::vector<data::Schema>& schemas)
{
    std::size_t count=0;
    for (auto& dock:docks) for (auto& control:dock.controls) {
        if (!control.dataTable) continue;
        if (++count>16) throw std::invalid_argument("protocol exceeds 16 data tables");
        auto& config=*control.dataTable;
        const auto schema=std::find_if(schemas.begin(),schemas.end(),[&](const auto& s){return s.dataset==config.dataset;});
        if (schema==schemas.end()) throw std::invalid_argument("unknown table dataset");
        if (config.columns.empty()) {
            if (schema->fields.size()>32) throw std::invalid_argument("explicit columns required for large dataset");
            for (const auto& field:schema->fields)
                config.columns.push_back({field.name,field.name,{},3,field.type});
        }
        std::set<std::string> names;
        for (auto& column:config.columns) {
            const auto field=std::find_if(schema->fields.begin(),schema->fields.end(),[&](const auto& f){return f.name==column.field;});
            if (field==schema->fields.end() || !names.insert(column.field).second)
                throw std::invalid_argument("unknown or duplicate table column");
            column.type=field->type;
        }
    }
}

DataTableSession::DataTableSession(const std::vector<ControlDescriptor>& controls,ScriptDataSession& data):data_(data)
{
    for (const auto& control:controls) {
        if (!control.dataTable) continue;
        Table table;
        table.config=*control.dataTable;
        table.view.device=table.config.device;table.view.limit=table.config.pageSize;
        if (!table.config.history) {
            const auto schema=std::find_if(data.schemas().begin(),data.schemas().end(),[&](const auto& s){
                return s.dataset==table.config.dataset;
            });
            table.live=std::make_unique<data::LiveTable>(*schema,table.config.maxRows,table.config.maxBytes);
        }
        tables_.emplace(control.id,std::move(table));
    }
}

void DataTableSession::publish(const data::Record& record)
{
    for (auto& [id,table]:tables_) {
        if (!table.live || record.dataset!=table.config.dataset ||
            (table.config.device && record.device!=*table.config.device)) continue;
        table.live->append(record);
        table.dirty=true;
    }
}

const data::TableView& DataTableSession::view(const std::string& id) const {return tables_.at(id).view;}
const DataTableExportState& DataTableSession::exportState(const std::string& id) const {return tables_.at(id).exportState;}

void DataTableSession::prepareExport(const std::string& id,storage::ExportFormat format)
{
    auto& table=tables_.at(id);
    if (table.exportState.choosingPath || table.exportState.running) throw std::runtime_error("table export already active");
    const auto current=page(id);
    if (current->loading || !current->error.empty() || (table.config.history && !current->snapshot))
        throw std::runtime_error("table snapshot is not ready");
    storage::Query query;
    query.dataset=table.config.dataset;query.device=table.view.device;
    query.fromUs=table.view.fromUs;query.toUs=table.view.toUs;
    query.conditions=table.view.conditions;query.sort=table.view.sort;query.snapshot=current->snapshot;
    // 文件选择期间仍固定原视图：实时表最多 1000 行，历史表只保留卷引用和查询条件。
    std::vector<data::TableRow> rows;
    if (table.live) {
        auto all=table.view;all.offset=0;all.limit=1000;
        rows=table.live->page(all).rows;
    }
    table.exportQuery=std::move(query);table.exportRows=std::move(rows);
    table.exportLease=table.snapshotLease;table.exportFormat=format;
    table.exportState={true,false,"Choose export file"};
}

void DataTableSession::exportDialog(const std::string& id,std::uint64_t dialog) {exportDialogs_.emplace(dialog,id);}
void DataTableSession::exportError(const std::string& id,const std::string& error)
{
    auto& table=tables_.at(id);
    table.exportRows.clear();table.exportLease.reset();
    table.exportState={false,false,error};
}
void DataTableSession::cancelExport(const std::string& id)
{
    auto& table=tables_.at(id);
    if (table.exportTask) {data_.cancel(table.exportTask);table.exportState.message="Canceling export...";}
}
bool DataTableSession::fileDialog(const FileDialogEvent& event,const std::function<bool(const std::string&)>& allowed)
{
    const auto found=exportDialogs_.find(event.id);
    if (found==exportDialogs_.end()) return false;
    const auto id=found->second;
    exportDialogs_.erase(found);
    auto& table=tables_.at(id);
    try {
        if (!allowed(id)) throw std::runtime_error("table export no longer permitted");
        if (event.state!="selected") {
            exportError(id,event.error.empty() ? "Export canceled":event.error);
            return true;
        }
        if (event.kind!=FileDialogKind::SaveFile || event.path.empty())
            throw std::runtime_error("invalid table export file selection");
        table.exportTask=data_.exportTable(event.path,table.exportFormat,table.exportQuery,
            table.live ? &table.exportRows:nullptr);
        exportTasks_.emplace(table.exportTask,id);
        table.exportRows.clear();
        table.exportState={false,true,"Exporting..."};
    } catch (const std::exception& error) {exportError(id,error.what());}
    return true;
}

std::shared_ptr<const data::TablePage> DataTableSession::page(const std::string& id)
{
    auto& table=tables_.at(id);
    if (!table.page || table.dirty) {
        auto page=table.live ? table.live->page(table.view) : data::TablePage{};
        page.limit=table.view.limit;page.offset=table.view.offset;page.revision=++table.revision;
        table.page=std::make_shared<const data::TablePage>(std::move(page));
        table.dirty=false;
    }
    return table.page;
}

void DataTableSession::handle(const DataTableEvent& event)
{
    auto& table=tables_.at(event.id);
    if (event.action==DataTableAction::Select) return;
    // 刷新直接回到新快照首页，避免 UI 先翻页再刷新产生冗余查询。
    if (event.action==DataTableAction::Refresh) table.view.offset=0;
    if (event.action==DataTableAction::View) {
        data::validateTableView(event.view);
        // 只能查询声明的列，数据集与固定设备约束不能被 UI 事件绕过。
        const auto declared=[&](const std::string& field) {
            return std::any_of(table.config.columns.begin(),table.config.columns.end(),
                               [&](const auto& c){return c.field==field;});
        };
        for (const auto& condition:event.view.conditions)
            if (!declared(condition.field)) throw std::invalid_argument("unknown table filter column");
        if (event.view.sort && !declared(event.view.sort->field)) throw std::invalid_argument("unknown sort column");
        table.view=event.view;
        if (table.config.device) table.view.device=table.config.device;
    }
    if (table.live) {table.dirty=true;return;}
    const auto previous=page(event.id);
    if (table.task) data_.cancel(table.task);
    data::TablePage next;
    next.offset=table.view.offset;next.limit=table.view.limit;next.revision=++table.revision;
    if (event.action==DataTableAction::Cancel) {
        table.task=0;next.error="query canceled";next.snapshot=previous->snapshot;
    } else {
        storage::Query query;
        query.dataset=table.config.dataset;query.device=table.view.device;
        query.fromUs=table.view.fromUs;query.toUs=table.view.toUs;
        query.conditions=table.view.conditions;query.sort=table.view.sort;
        query.limit=table.view.limit;query.offset=table.view.offset;
        query.snapshot=event.action==DataTableAction::Refresh ? std::nullopt : previous->snapshot;
        next.snapshot=query.snapshot;
        try {
            table.task=data_.query(std::move(query));
            tasks_.emplace(table.task,event.id);
            next.loading=true;
        } catch (const std::exception& exception) {table.task=0;next.error=exception.what();}
    }
    table.page=std::make_shared<const data::TablePage>(std::move(next));table.dirty=false;
}

bool DataTableSession::complete(const storage::Completion& result)
{
    const auto exporting=exportTasks_.find(result.task);
    if (exporting!=exportTasks_.end()) {
        auto& table=tables_.at(exporting->second);
        table.exportTask=0;table.exportLease.reset();
        table.exportState={false,false,result.ok ? "Exported "+std::to_string(result.processed)+" rows":result.error};
        exportTasks_.erase(exporting);
        return true;
    }
    const auto task=tasks_.find(result.task);
    if (task==tasks_.end()) return false;
    auto& table=tables_.at(task->second);
    tasks_.erase(task);
    if (result.task!=table.task) return true;
    table.task=0;
    table.snapshotLease=result.snapshotLease;
    data::TablePage next;
    next.offset=table.view.offset;next.limit=table.view.limit;next.revision=++table.revision;
    next.snapshot=result.snapshot;next.more=result.more;next.error=result.error;
    if (result.ok) {
        std::map<std::uint64_t,std::shared_ptr<const data::Schema>> schemas;
        for (const auto& [version,schema]:result.schemas)
            schemas.emplace(version,std::make_shared<const data::Schema>(schema));
        for (std::size_t i=0;i<result.records.size();++i) {
            const auto& record=result.records[i];
            next.rows.push_back({result.rowIds.at(i),std::make_shared<const data::Record>(record),schemas.at(record.schemaVersion)});
        }
    }
    table.page=std::make_shared<const data::TablePage>(std::move(next));table.dirty=false;
    return true;
}

void ScriptHost::onDataTable(const transport::ConnectionContext& context,const DataTableEvent& event)
{
    if (!runtime_ || !scriptLoaded_ || executionFaulted() || event.runtimeGeneration!=runtimeGeneration_) return;
    const auto found=std::find_if(controls_.begin(),controls_.end(),[&](const auto& c){return c.id==event.id;});
    if (found==controls_.end() || !found->dataTable || !found->visible || found->disabled || found->readOnly) return;
    try {
        if (event.action==DataTableAction::ExportCsv || event.action==DataTableAction::ExportPsrec) {
            auto& tables=*runtime_->tables;
            const auto& state=tables.exportState(event.id);
            if (state.choosingPath || state.running) return;
            if (found->dataTable->history && tables.page(event.id)->revision!=event.pageRevision) return;
            try {
                const bool csv=event.action==DataTableAction::ExportCsv;
                tables.prepareExport(event.id,csv ? storage::ExportFormat::Csv:storage::ExportFormat::Psrec);
                auto opts=runtime_->lua.create_table();
                opts["title"]="Export table";
                opts["default_path"]=csv ? "table.csv":"table.psrec";
                auto filter=runtime_->lua.create_table();
                filter["name"]=csv ? "CSV":"PSREC";
                filter["patterns"]=runtime_->lua.create_table_with(1,csv ? "*.csv":"*.psrec");
                opts["filters"]=runtime_->lua.create_table_with(1,filter);
                std::string error;
                const auto dialog=protoFileDialog(FileDialogKind::SaveFile,opts,error);
                if (!dialog) throw std::runtime_error(error);
                tables.exportDialog(event.id,dialog->id);
            } catch (const std::exception& error) {tables.exportError(event.id,error.what());}
        } else if (event.action==DataTableAction::CancelExport) {
            runtime_->tables->cancelExport(event.id);
        } else if (event.action==DataTableAction::Select) {
            const auto page=runtime_->tables->page(event.id);
            if ((found->dataTable->history && page->revision!=event.pageRevision) || page->loading) return;
            if (std::none_of(page->rows.begin(),page->rows.end(),[&](const auto& row){return row.id==event.selectedRow;})) return;
            const auto selected=std::to_string(event.selectedRow);
            controlValues_[event.id]=selected;
            callbackOnControl(ScriptHostContext{context},event.id,selected);
        } else runtime_->tables->handle(event);
    } catch (const std::exception& exception) {protoLog("warn","data_table: "+std::string(exception.what()));}
}

sol::object ScriptHost::selectedTableRow(sol::state_view lua,const std::string& id)
{
    const auto empty=sol::make_object(lua,sol::lua_nil);
    // 待加载脚本不能读取旧 runtime 的选中数据，返回值也不持有内部页或可变记录引用。
    if (!runtime_ || runtime_->lua.lua_state()!=lua.lua_state() || !runtime_->tables) return empty;
    const auto descriptor=std::find_if(controls_.begin(),controls_.end(),[&](const auto& control) {
        return control.id==id && control.dataTable.has_value();
    });
    if (descriptor==controls_.end()) return empty;
    const auto selected=controlValues_.find(id);
    if (selected==controlValues_.end()) return empty;
    const auto* text=std::get_if<std::string>(&selected->second);
    if (!text || text->empty()) return empty;
    std::uint64_t rowId=0;
    const auto parsed=std::from_chars(text->data(),text->data()+text->size(),rowId);
    if (parsed.ec!=std::errc{} || parsed.ptr!=text->data()+text->size()) return empty;
    const auto page=runtime_->tables->page(id);
    if (page->loading || !page->error.empty()) return empty;
    for (const auto& row:page->rows) if (row.id==rowId) {
        auto result=runtime_->data->recordTable(lua,*row.record,*row.schema);
        result["row_id"]=*text;
        return sol::make_object(lua,result);
    }
    return empty;
}
} // namespace protoscope::scripting
