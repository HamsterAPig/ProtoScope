#include "protoscope/scripting/script_host.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
const std::string script = R"(
    assert(proto.ui.set_menu({
        {id="device",label="Device",children={
            {id="monitor",label="Monitor",checkable=true},
            {separator=true},
            {id="show",label="Show"},
            {id="replace",label="Replace"}
        }}
    }))
    function ui() return {id="panel",title="Panel",controls={}} end
    function on_menu(ctx,id,checked)
        if id=="monitor" then
            assert(proto.ui.show_dock("panel",checked))
            assert(not proto.ui.show_dock("missing",true))
            assert(not proto.ui.show_dock("panel","true"))
        elseif id=="show" then
            assert(proto.ui.update_menu("device",{disabled=true}))
        elseif id=="replace" then
            assert(proto.ui.set_menu({{id="monitor",label="Replacement",checkable=true}}))
        end
        proto.emit(id,checked and "true" or "false")
    end
)";

struct Fixture {
    tests::ScopedTempPath path{tests::makeUniqueTempDir("protoscope-business-ui")};
    scripting::ScriptHost host;
    void write(const std::string& value) { std::ofstream out(path.path()/"main.lua"); out<<value; }
    bool load(const std::string& value) {
        write(value); return host.loadProtocolDirectory(path.path().generic_string());
    }
    void menu(const std::string& id,bool checked=false) {
        const auto state=host.businessUiSnapshot();
        host.onMenu({},id,checked,state.runtimeGeneration,state.menuRevision);
    }
};
void menuValidation()
{
    Fixture f;
    require(f.load(script+R"(
        assert(not proto.ui.set_menu({{id="x",label="X"},{id="x",label="Duplicate"}}))
        assert(not proto.ui.set_menu({[2]={id="x",label="Sparse"}}))
        assert(not proto.ui.set_menu({{id="x",label="X",checked=true}}))
        assert(not proto.ui.set_menu({{id="x",label="X",children={}}}))
        assert(not proto.ui.update_menu("monitor",{label="Polluted",unknown=true}))
        assert(not proto.ui.update_menu("show",{label="Polluted",checked=true}))
        assert(not proto.ui.update_menu("missing",{visible=false}))
        assert(not proto.ui.show_dock("panel",true))
        local a={id="cycle",label="Cycle"}; a.children={a}
        assert(not proto.ui.set_menu({a}))
        local list={}
        for i=1,257 do list[i]={id=tostring(i),label="Too many"} end
        assert(not proto.ui.set_menu(list))
        assert(proto.ui.update_menu("replace",{visible=false}))
    )"),"valid menu with invalid atomic updates");
    const auto state=f.host.businessUiSnapshot();
    require(state.menu.size()==1 && state.menu[0].children[0].label=="Monitor","invalid declaration must not mutate");
    require(state.menu[0].children[2].label=="Show","invalid patch must not mutate");
    f.menu("monitor",true);
    require(f.host.drainEvents().size()==1,"valid click callback");
    auto next=f.host.businessUiSnapshot();
    require(next.menu[0].children[0].checked && next.dockRequests.size()==1 &&
            next.dockRequests[0].visible,"checked and dock request");
    f.menu("monitor",false);
    next=f.host.businessUiSnapshot();
    require(next.dockRequests.size()==1 && !next.dockRequests[0].visible && next.dockRevision==2,
            "dock request coalescing with monotonic revision");
    f.host.drainEvents();
    f.menu("device");
    f.menu("replace");
    f.menu("show",true);
    require(f.host.drainEvents().empty(),"submenu and invalid checked events rejected");
    f.menu("show");
    f.host.drainEvents();
    f.menu("monitor",true);
    require(f.host.drainEvents().empty(),"disabled parent rejects child event");
}
void callbackBudget()
{
    Fixture f;
    scripting::ExecutionConfig config;
    config.callbackTimeoutMs=20;
    f.host.setExecutionConfig(config);
    require(f.load(script+R"(
        function on_menu(ctx,id,checked) while true do end end
    )"),"menu budget fixture");
    f.menu("monitor",true);
    require(!f.host.lastError().empty(),"menu busy loop must be interrupted");
    f.menu("monitor",false);
    require(f.host.businessUiSnapshot().menu[0].children[0].checked,"faulted script stops menu dispatch");
    require(f.load(script),"reload recovers menu budget");
    f.menu("monitor",true);
    require(f.host.drainEvents().size()==1,"reloaded menu dispatch resumes");
}
void reloadIsolation()
{
    Fixture f;
    require(f.load(script),"initial menu");
    auto state=f.host.businessUiSnapshot();
    require(!f.load("assert(proto.ui.set_menu({{id='bad',label='Bad'}})); error('fail')"),"failed reload");
    require(f.host.businessUiSnapshot().menu[0].id=="device","staging menu must not pollute active runtime");
    require(f.load(script),"new menu");
    f.host.onMenu({},"monitor",true,state.runtimeGeneration,state.menuRevision);
    require(f.host.drainEvents().empty(),"old runtime event rejected");
    state=f.host.businessUiSnapshot();
    f.menu("replace");
    f.host.drainEvents();
    f.host.onMenu({},"monitor",true,state.runtimeGeneration,state.menuRevision);
    require(f.host.drainEvents().empty(),"old menu tree revision rejected");
    f.menu("monitor",true);
    require(f.host.drainEvents().size()==1,"replacement event accepted");
}
void workerMenu()
{
    Fixture f;
    f.write(script);
    scripting::ScriptRuntimeWorker worker;
    auto loaded=worker.loadProtocolDirectory(f.path.path().generic_string());
    require(loaded.ok,"worker load menu");
    const auto state=loaded.snapshot.businessUi;
    worker.postMenu({},"monitor",true,state.runtimeGeneration,state.menuRevision);
    worker.waitIdle();
    require(worker.snapshot().businessUi.menu[0].children[0].checked,"worker applies menu state");
    auto reloaded=worker.loadProtocolDirectory(f.path.path().generic_string());
    require(reloaded.ok,"worker reload");
    worker.postMenu({},"monitor",true,state.runtimeGeneration,state.menuRevision);
    worker.waitIdle();
    require(!worker.snapshot().businessUi.menu[0].children[0].checked,"worker rejects stale menu");
}
}

int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"menu_validation",menuValidation},{"reload_isolation",reloadIsolation},{"worker_menu",workerMenu},
        {"callback_budget",callbackBudget}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch (const std::exception& e) {++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
