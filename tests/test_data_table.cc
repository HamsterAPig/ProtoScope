#include "protoscope/scripting/script_host.hpp"
#include "test_helpers.hpp"

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
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"live_history",liveAndHistory},{"replacement_validation",replacementAndValidation},
        {"selected_live_eviction",selectedLiveRowEviction}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
