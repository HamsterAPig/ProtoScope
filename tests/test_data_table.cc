#include "protoscope/scripting/script_host.hpp"
#include "test_helpers.hpp"
#include "protoscope/data/psrec.hpp"
#include "protoscope/data/record_csv.hpp"

#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace protoscope;
using tests::require;
const std::string script=R"(
    assert(proto.ui.get_selected_row("history")==nil)
    function data() return {{id="samples",fields={{name="n",type="int64",nullable=false}}}} end
    function ui() return {id="panel",title="Tables",controls={
        {"data_table","live","Live",dataset="samples",max_rows=3,page_size=2,columns={{field="n",label="Number"}}},
        {"data_table","history","History",dataset="samples",mode="history",page_size=2}
    }} end
    function on_open(ctx) proto.record.start() end
    function on_record(ctx,evt)
        assert(evt.ok,evt.error)
        assert(evt.operation=="start" or evt.operation=="stop","internal table task leaked")
        if evt.operation=="start" then
            local rows={}
            for i=1,5 do rows[i]={dataset="samples",values={n=i}} end
            assert(proto.data.publish_batch(rows))
            proto.record.stop()
        elseif evt.operation=="stop" then
            assert(proto.ui.get_selected_row("live")==nil)
            proto.emit("ready","")
        end
    end
    function on_control(ctx,id,value)
        local row=assert(proto.ui.get_selected_row(id))
        assert(row.row_id==value and row.dataset=="samples" and math.type(row.values.n)=="integer")
        local n=row.values.n
        row.values.n=-1
        assert(proto.ui.get_selected_row(id).values.n==n)
        assert(proto.ui.get_selected_row("missing")==nil)
        proto.emit(id,value)
    end
)";
struct Fixture {
    tests::ScopedTempPath directory{tests::makeUniqueTempDir("protoscope-data-table")};
    scripting::ScriptHost host;
    Fixture() {host.setStorageRoot(directory.path()/"data");}
    bool load(const std::string& value=script) {
        {std::ofstream out(directory.path()/"main.lua");out<<value;}
        return host.loadProtocolDirectory(directory.path().generic_string());
    }
    void wait(const std::function<bool()>& done) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(4);
        do {host.tick(0);if(done())return;std::this_thread::sleep_for(std::chrono::milliseconds(1));}
        while(std::chrono::steady_clock::now()<deadline);
        throw std::runtime_error("table task timeout");
    }
    void publish() {
        host.onTransportOpen({transport::ConnectionContext{}});
        wait([&]{return !host.drainEvents().empty();});
    }
    scripting::ControlSnapshot control(std::size_t index) {return host.controlStatesSnapshot().at(index);}
    void send(scripting::DataTableEvent event) {
        event.runtimeGeneration=host.runtimeGeneration();host.onDataTable({},event);
    }
};
void liveAndHistory()
{
    Fixture f;
    require(f.load(),"table declaration");
    f.publish();
    const auto live=f.control(0);
    require(live.tablePage && live.tablePage->rows.size()==2 && live.tablePage->more,"bounded live first page");
    require(std::get<std::int64_t>(live.tablePage->rows[0].record->values[0].value)==3,"old live rows evicted");
    data::TableView view;
    view.limit=2;view.sort=data::FieldSort{"n",true};
    f.send({.id="history",.action=scripting::DataTableAction::View,.view=view});
    f.wait([&]{return !f.control(1).tablePage->loading;});
    auto first=f.control(1);
    require(first.tablePage->error.empty() && first.tablePage->rows.size()==2,"history query page");
    require(std::get<std::int64_t>(first.tablePage->rows[0].record->values[0].value)==5,"history sorted across all rows");
    f.publish();
    view.offset=2;
    f.send({.id="history",.action=scripting::DataTableAction::View,.view=view});
    f.wait([&]{return !f.control(1).tablePage->loading;});
    const auto second=f.control(1);
    require(second.tablePage->snapshot==first.tablePage->snapshot &&
            std::get<std::int64_t>(second.tablePage->rows[0].record->values[0].value)==3,"history freezes snapshot between pages");
    f.send({.id="history",.pageRevision=first.tablePage->revision,.action=scripting::DataTableAction::Select,
            .selectedRow=first.tablePage->rows[0].id});
    require(f.host.drainEvents().empty(),"selection from replaced page rejected");
    f.send({.id="history",.pageRevision=second.tablePage->revision,.action=scripting::DataTableAction::Select,
            .selectedRow=second.tablePage->rows[0].id});
    require(f.host.drainEvents().size()==1,"current page selection callback");
    f.send({.id="history",.action=scripting::DataTableAction::Refresh});
    f.wait([&]{return !f.control(1).tablePage->loading;});
    const auto refreshed=f.control(1);
    require(refreshed.tableView->offset==0 && refreshed.tablePage->offset==0,
            "refresh returns to first page without a preceding view request");
    require(refreshed.tablePage->snapshot!=first.tablePage->snapshot &&
            refreshed.tableView->sort->descending,"refresh includes new rows and retains sort");
    const auto generation=f.host.runtimeGeneration();
    require(f.load(),"table reload");
    f.host.onDataTable({},{.id="history",.runtimeGeneration=generation,.action=scripting::DataTableAction::Refresh});
    require(!f.control(1).tablePage->loading,"old runtime table event rejected");
    require(f.control(0).tablePage->rows.empty() && std::get<std::string>(f.control(1).value).empty(),
            "table rows and selection not reused on reload");
}
void replacementAndValidation()
{
    Fixture f;
    require(f.load(),"query replacement declaration");
    f.publish();
    data::TableView view;
    view.conditions={{"n",data::CompareOp::Greater,{std::int64_t{1}}}};
    f.send({.id="history",.action=scripting::DataTableAction::View,.view=view});
    view.conditions={{"n",data::CompareOp::Equal,{std::int64_t{4}}}};
    f.send({.id="history",.action=scripting::DataTableAction::View,.view=view});
    f.wait([&]{return !f.control(1).tablePage->loading;});
    auto page=f.control(1).tablePage;
    require(page->rows.size()==1 && std::get<std::int64_t>(page->rows[0].record->values[0].value)==4,
            "late canceled result cannot overwrite replacement");
    require(f.host.drainEvents().empty(),"internal table queries do not enter on_record");
    f.send({.id="history",.action=scripting::DataTableAction::View,.view=view});
    f.send({.id="history",.action=scripting::DataTableAction::Cancel});
    require(f.control(1).tablePage->snapshot==page->snapshot,"cancel preserves the fixed query snapshot");
    f.send({.id="history",.action=scripting::DataTableAction::Refresh});
    f.send({.id="history",.action=scripting::DataTableAction::Cancel});
    require(!f.control(1).tablePage->loading && !f.control(1).tablePage->error.empty(),"cancel is immediate UI state");
    require(!f.host.setControlValue("history",std::string("1")),"table selection cannot bypass worker table event");
    auto bad=script;
    bad.replace(bad.find("dataset=\"samples\",max_rows"),std::string("dataset=\"samples\"").size(),"dataset=\"missing\"");
    require(!f.load(bad),"unknown table dataset rejected");
}
void selectedLiveRowEviction()
{
    Fixture f;
    require(f.load(),"selected row declaration");
    f.publish();
    const auto page=f.control(0).tablePage;
    f.send({.id="live",.pageRevision=page->revision,.action=scripting::DataTableAction::Select,
            .selectedRow=page->rows[0].id});
    require(f.host.drainEvents().size()==1,"live selection exposes copied typed row in callback");
    f.publish();
    require(f.host.lastError().empty(),"evicted selected row returns nil");
    f.send({.id="live",.pageRevision=page->revision,.action=scripting::DataTableAction::Select,
            .selectedRow=page->rows[0].id});
    require(f.host.drainEvents().empty(),"evicted live row cannot be selected again");
}
std::vector<std::int64_t> exportedValues(const std::filesystem::path& path,bool csv)
{
    std::ifstream input(path,std::ios::binary);
    std::vector<std::int64_t> values;
    const auto read=[&](auto& reader) {
        while (const auto record=reader.next()) values.push_back(std::get<std::int64_t>(record->values[0].value));
    };
    if (csv) {data::RecordCsvReader reader(input);read(reader);}
    else {data::PsrecReader reader(input);read(reader);}
    return values;
}
scripting::FileDialogRequest beginExport(Fixture& f,std::size_t index,bool csv)
{
    const auto current=f.control(index);
    f.send({.id=current.descriptor.id,.pageRevision=current.tablePage->revision,
            .action=csv ? scripting::DataTableAction::ExportCsv:scripting::DataTableAction::ExportPsrec});
    auto dialogs=f.host.drainFileDialogRequests();
    require(dialogs.size()==1 && dialogs[0].kind==scripting::FileDialogKind::SaveFile,"table opens save dialog");
    require(f.control(index).tableExport.choosingPath,"table reports pending file selection");
    return dialogs[0];
}
void selectExport(Fixture& f,const scripting::FileDialogRequest& request,const std::filesystem::path& path)
{
    f.host.onFileDialogEvent({},{.id=request.id,.kind=scripting::FileDialogKind::SaveFile,
        .state="selected",.path=path.generic_string(),.runtimeGeneration=request.runtimeGeneration});
}
void exportFrozenViews()
{
    Fixture f;
    require(f.load(script+R"(
        function on_file_dialog(ctx,event) error("internal table dialog leaked") end
        function on_close(ctx) assert(proto.data.publish({dataset="samples",values={n=99}})) end
    )"),"table export fixture");
    f.publish();
    data::TableView view;
    view.limit=1;view.offset=1;view.sort=data::FieldSort{"n",true};
    view.conditions={{"n",data::CompareOp::Greater,{std::int64_t{3}}}};
    f.send({.id="live",.action=scripting::DataTableAction::View,.view=view});
    const auto live=beginExport(f,0,true);
    f.send({.id="live",.action=scripting::DataTableAction::ExportCsv});
    require(f.host.drainFileDialogRequests().empty(),"duplicate export ignored while dialog pending");
    f.host.onTransportClose({transport::ConnectionContext{}});
    require(f.host.lastError().empty(),"live update while choosing export destination");
    f.send({.id="live",.action=scripting::DataTableAction::View,.view={}});
    const auto csv=f.directory.path()/"live.csv";
    selectExport(f,live,csv);
    f.wait([&]{return !f.control(0).tableExport.running;});
    require(exportedValues(csv,true)==std::vector<std::int64_t>{5,4},
            "live export freezes all filtered rows and sort, ignoring pagination and later data");
    require(f.control(0).tableExport.message=="Exported 2 rows" && f.host.lastError().empty(),
            "internal export reports success without Lua dialog callback");

    view.conditions={{"n",data::CompareOp::Greater,{std::int64_t{1}}}};
    f.send({.id="history",.action=scripting::DataTableAction::View,.view=view});
    f.wait([&]{return !f.control(1).tablePage->loading;});
    const auto history=beginExport(f,1,false);
    f.publish();
    f.send({.id="history",.action=scripting::DataTableAction::Refresh});
    f.wait([&]{return !f.control(1).tablePage->loading;});
    const auto psrec=f.directory.path()/"history.psrec";
    selectExport(f,history,psrec);
    f.wait([&]{return !f.control(1).tableExport.running;});
    require(exportedValues(psrec,false)==std::vector<std::int64_t>{5,4,3,2},
            "history export retains original snapshot/filter/order despite refresh and appended records");
    require(f.host.drainEvents().empty(),"internal export does not emit on_control or on_record events");
}
void exportRejectionAndCancellation()
{
    Fixture f;require(f.load(),"table export failures fixture");f.publish();
    auto request=beginExport(f,0,true);
    f.host.onFileDialogEvent({},{.id=request.id,.kind=scripting::FileDialogKind::SaveFile,
        .state="canceled",.runtimeGeneration=request.runtimeGeneration});
    require(f.control(0).tableExport.message=="Export canceled","file selection cancellation releases export");
    request=beginExport(f,0,true);
    scripting::FileIoConfig config;config.maxWriteFileSizeBytes=1;f.host.setFileIoConfig(config);
    const auto path=f.directory.path()/"preserved.csv";
    {std::ofstream file(path);file<<"original";}
    selectExport(f,request,path);
    f.wait([&]{return !f.control(0).tableExport.running;});
    {std::ifstream file(path);std::string text;file>>text;require(text=="original","quota failure preserves old file");}
    require(f.control(0).tableExport.message.find("max_write_file_size_bytes")!=std::string::npos,"quota error visible in table");
    config.maxWriteFileSizeBytes=1024*1024;config.allowProtocolDir=false;config.allowDialogPaths=false;
    f.host.setFileIoConfig(config);
    request=beginExport(f,0,false);
    const auto denied=f.directory.path()/"denied.psrec";
    selectExport(f,request,denied);
    require(!std::filesystem::exists(denied) &&
            f.control(0).tableExport.message.find("not authorized")!=std::string::npos,"unauthorized export denied");
    config.enabled=false;f.host.setFileIoConfig(config);
    f.send({.id="live",.action=scripting::DataTableAction::ExportCsv});
    require(f.host.drainFileDialogRequests().empty() && !f.control(0).tableExport.choosingPath,"disabled file IO rejects export");
    config.enabled=true;config.allowDialogPaths=true;f.host.setFileIoConfig(config);
    request=beginExport(f,0,false);
    require(f.load(),"reload with pending table export");
    selectExport(f,request,denied);
    require(!std::filesystem::exists(denied) && !f.control(0).tableExport.running,"old-generation save result rejected");

    Fixture blocked;
    require(blocked.load(script+R"(
        function on_close(ctx) assert(proto.ui.update_control("live",{disabled=true})) end
    )"),"disabled table export fixture");
    blocked.publish();
    request=beginExport(blocked,0,false);
    blocked.host.onTransportClose({transport::ConnectionContext{}});
    const auto blockedPath=blocked.directory.path()/"blocked.psrec";
    selectExport(blocked,request,blockedPath);
    require(!std::filesystem::exists(blockedPath) &&
            blocked.control(0).tableExport.message=="table export no longer permitted","worker rechecks disabled state after file dialog");
    blocked.send({.id="live",.action=scripting::DataTableAction::ExportCsv});
    require(blocked.host.drainFileDialogRequests().empty(),"disabled table rejects export event");
}
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"live_history",liveAndHistory},{"replacement_validation",replacementAndValidation},
        {"selected_live_eviction",selectedLiveRowEviction},{"export_frozen_views",exportFrozenViews},
        {"export_rejection_cancellation",exportRejectionAndCancellation}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
