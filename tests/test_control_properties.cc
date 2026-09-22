#include "protoscope/scripting/script_host.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "protoscope/ui/control_edit_state.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>

namespace {
using namespace protoscope;
using tests::require;
struct Fixture {
    tests::ScopedTempPath directory{tests::makeUniqueTempDir("protoscope-control-properties")};
    scripting::ScriptHost host;
    bool load(const std::string& script) {
        { std::ofstream out(directory.path() / "main.lua"); out << script; }
        return host.loadProtocolDirectory(directory.path().generic_string());
    }
    void open() { host.onTransportOpen({transport::ConnectionContext{}}); }
};
const std::string declaration = R"(
function controls()
    return {
        {"int","number","Number",default=3,min=0,max=10},
        {"combo","choice","Choice",options={"A","B"},default=1},
        {"btn","action","Action"}
    }
end
)";

void atomicUpdates()
{
    Fixture f;
    require(f.load(declaration + R"(
        function on_open(ctx)
            assert(proto.ui.update_controls({
                number={value=8,label="Updated",min=5,max=9,tooltip="Measured"},
                choice={options={"C"},value=0},
                action={visible=false,disabled=true}
            }))
            assert(proto.get_control("number")==8 and proto.get_control("choice")==0)
            local ok,err=proto.ui.update_controls({number={value=6},choice={value=5}})
            assert(not ok and err)
            assert(proto.get_control("number")==8)
            assert(not proto.ui.update_control("number",{max=7}))
            assert(not proto.ui.update_control("number",{value=7.5}))
            assert(not proto.ui.update_control("number",{disabled="true"}))
            assert(not proto.ui.update_control("number",{unknown=true}))
            assert(not proto.ui.update_control("choice",{options={[2]="x"}}))
            assert(not proto.ui.update_control("missing",{visible=true}))
            assert(proto.ui.update_control("number",{read_only=true}))
            proto.emit("done","")
        end
    )"), "control fixture should load");
    f.open();
    require(f.host.drainEvents().size()==1, "atomic script assertions failed");
    const auto states = f.host.controlStatesSnapshot();
    require(states[0].descriptor.label=="Updated" && states[0].descriptor.readOnly &&
            states[0].descriptor.tooltip=="Measured" && states[0].descriptor.maximum==9,
            "descriptor patch should be committed");
    require(f.host.dockSnapshots()[0].controls[0].descriptor.label=="Updated",
            "dock snapshot should use updated descriptors");
    require(!states[2].descriptor.visible && states[2].descriptor.disabled, "dynamic visibility");
}

void workerValidation()
{
    Fixture f;
    require(f.load(declaration + R"(
        function on_open(ctx)
            assert(proto.ui.update_controls({number={read_only=true},action={disabled=true}}))
        end
        function on_control(ctx,id,value) proto.emit(id,"") end
    )"), "control fixture should load");
    f.open();
    transport::ConnectionContext context;
    f.host.onControl(context,"number",4);
    f.host.onControl(context,"action",true);
    f.host.onControl(context,"choice",9);
    f.host.onControl(context,"choice",std::string("bad"));
    require(f.host.drainEvents().empty(), "worker must reject disabled/readonly/invalid events");
    require(std::get<int>(f.host.controlStatesSnapshot()[0].value)==3, "readonly value must not change");
    f.host.onControl(context,"choice",0);
    require(f.host.drainEvents().size()==1, "valid worker event must still work");
    require(!f.host.setControlValue("number",11), "restoring out-of-range UI values must fail");
}

void reloadConstraints()
{
    Fixture f;
    require(f.load(declaration), "initial load");
    require(f.host.setControlValue("number",8), "set initial value");
    require(f.load(R"(function controls() return {{"int","number","Number",default=2,min=0,max=5}} end)"),
            "changed constraints should load");
    require(std::get<int>(f.host.controlStatesSnapshot()[0].value)==2, "incompatible old value must reset");
    require(!f.load(R"(function controls() return {{"int","number","Number",default=8,min=0,max=5}} end)"),
            "invalid default must reject");
    require(std::get<int>(f.host.controlStatesSnapshot()[0].value)==2, "failed load must preserve current value");
    require(!f.load("assert(proto.ui.update_control('number',{label='polluted'}))"),
            "declaration must not mutate old descriptors");
    require(f.host.controlsSnapshot()[0].label=="Number", "failed declaration must not leak properties");
}

void runtimeGeneration()
{
    Fixture f;
    require(f.load(declaration + R"(
        function on_control(ctx,id,value) proto.emit(id,"") end
    )"), "generation fixture");
    scripting::ScriptRuntimeWorker worker;
    const auto first = worker.loadProtocolDirectory(f.directory.path().generic_string());
    const auto second = worker.loadProtocolDirectory(f.directory.path().generic_string());
    require(first.ok && second.ok && second.snapshot.runtimeGeneration > first.snapshot.runtimeGeneration,
            "successful reload must advance generation");
    require(second.snapshot.controlStates[0].descriptor.runtimeGeneration == second.snapshot.runtimeGeneration,
            "control snapshot must carry current generation");
    worker.postControl({}, "number", 5, first.snapshot.runtimeGeneration);
    worker.waitIdle();
    require(std::get<int>(worker.snapshot().controlStates[0].value) == 3, "stale control must not mutate value");
    for (const auto& batch : worker.drainOutputs()) require(batch.events.empty(), "stale callback must not fire");
    worker.postControl({}, "number", 5, second.snapshot.runtimeGeneration);
    worker.waitIdle();
    require(std::get<int>(worker.snapshot().controlStates[0].value) == 5, "current event must work");
    { std::ofstream out(f.directory.path() / "main.lua"); out << "error('load failure')"; }
    require(!worker.loadProtocolDirectory(f.directory.path().generic_string()).ok, "invalid reload must fail");
    require(worker.snapshot().runtimeGeneration == second.snapshot.runtimeGeneration,
            "failed reload must retain current generation");
}

void staleDialogAuthorization()
{
    Fixture f;
    tests::ScopedTempPath external{tests::makeUniqueTempDir("protoscope-external-dialog")};
    const auto file = external.path() / "data.txt";
    { std::ofstream out(file); out << "data"; }
    const auto source = declaration + R"(
        assert(proto.ui.alert({title="Title",message="Message"}))
        assert(proto.fs.open_file_dialog({title="File"}))
        function on_dialog(ctx,evt) proto.emit("dialog","") end
        function on_file_dialog(ctx,evt) proto.emit("file","") end
        function on_control(ctx,id,value)
            local h = proto.fs.open(")" + file.generic_string() + R"(",{mode="read"})
            assert(not h)
            proto.emit("denied","")
        end
    )";
    require(f.load(source), "dialog fixture");
    const auto dialog = f.host.drainDialogRequests().at(0);
    const auto fileDialog = f.host.drainFileDialogRequests().at(0);
    require(dialog.runtimeGeneration == f.host.runtimeGeneration() &&
            fileDialog.runtimeGeneration == f.host.runtimeGeneration(), "declaration dialogs need candidate generation");
    require(f.load(source), "dialog fixture reload");
    scripting::DialogEvent event;
    event.id = dialog.id; event.runtimeGeneration = dialog.runtimeGeneration;
    f.host.onDialogEvent({}, event);
    scripting::FileDialogEvent selected;
    selected.id = fileDialog.id; selected.state = "selected"; selected.path = file.generic_string();
    selected.runtimeGeneration = fileDialog.runtimeGeneration;
    f.host.onFileDialogEvent({}, selected);
    require(f.host.drainEvents().empty(), "stale dialogs must not reach new callbacks");
    f.host.onControl({}, "action", true, f.host.runtimeGeneration());
    require(f.host.drainEvents().size() == 1, "stale selection must not grant file access");
}

void industrialControls()
{
    Fixture f;
    const std::string source = R"(
        function controls()
            return {
                {"label","heading","Process"},
                {"readout","measured","Measured",unit="C",precision=2,default=1.25,stale_after_ms=100,show_update_time=true},
                {"indicator","ready","Ready",on_text="Connected",off_text="Disconnected"},
                {"progress","progress","Progress",default=0.25},
                {"slider_int","target","Target",min=0,max=100,default=20},
                {"slider_float","ratio","Ratio",min=0,max=1,default=0.5},
                {"radio_group","mode","Mode",options={"Auto","Manual"}},
                {"text_area","notes","Notes",max_length=32,rows=4,wrap=true}
            }
        end
        function on_open(ctx)
            assert(proto.ui.update_controls({
                measured={value=9223372036854775807},ready={value=true},
                progress={value=0.5,indeterminate=true},notes={value="line1\nline2"}
            }))
            assert(proto.get_control("measured")=="9223372036854775807.00")
            proto.set_control("measured",22.126)
            assert(proto.get_control("measured")=="22.13")
            assert(not proto.ui.update_control("notes",{value=string.rep("x",33)}))
            assert(not proto.ui.update_control("target",{value=101}))
            assert(not proto.ui.update_control("target",{min=false}))
            assert(not proto.ui.update_control("target",{max=2147483647}))
            assert(not proto.ui.update_control("ratio",{max=1e300}))
            assert(proto.ui.update_control("mode",{options={"Service"},value=0}))
            proto.emit("ready","")
        end
        function on_control(ctx,id,value) proto.emit(id,"") end
    )";
    require(f.load(source), f.host.lastError().c_str());
    auto states = f.host.controlStatesSnapshot();
    require(states.size()==8 && std::get<std::string>(states[0].value)=="Process", "industrial declarations");
    require(states[4].descriptor.commitMode==scripting::ControlCommitMode::Commit &&
            states[7].descriptor.commitMode==scripting::ControlCommitMode::Commit, "safe default commit modes");
    f.open();
    require(f.host.drainEvents().size()==1, "industrial API assertions");
    states=f.host.controlStatesSnapshot();
    require(states[1].updatedAtMs>0 && states[1].descriptor.unit=="C", "readout update metadata");
    f.host.onControl({}, "ready", false);
    f.host.onControl({}, "measured", std::string("99"));
    require(f.host.drainEvents().empty(), "measured outputs must reject input events");
    require(f.load(source), "industrial reload");
    states=f.host.controlStatesSnapshot();
    require(!std::get<bool>(states[2].value) && std::get<std::string>(states[1].value)=="1.25",
            "runtime measurements must not be retained after reload");
}

void editingDraft()
{
    using scripting::ControlCommitMode;
    scripting::ControlSnapshot snapshot;
    snapshot.descriptor.type=scripting::ControlType::InputInt;
    snapshot.descriptor.runtimeGeneration=1;
    snapshot.value=10;
    ui::ControlEditState draft;
    draft.prepare(snapshot,1);
    draft.value=20;
    require(!draft.finish(true,true,false,ControlCommitMode::Commit), "commit mode waits for release");
    snapshot.value=30;
    draft.prepare(snapshot,2);
    require(std::get<int>(draft.value)==20, "host update must not overwrite active draft");
    require(draft.finish(false,false,true,ControlCommitMode::Commit), "release submits pending edit");
    draft.prepare(snapshot,3);
    require(std::get<int>(draft.value)==30, "inactive draft follows authoritative host");
    draft.value=25;
    require(draft.finish(true,true,false,ControlCommitMode::Change), "legacy change mode remains immediate");
    snapshot.descriptor.runtimeGeneration=2;
    draft.prepare(snapshot,4);
    require(!draft.editing && !draft.dirty && std::get<int>(draft.value)==30, "reload cancels old draft");
    draft.value=40;
    draft.finish(true,true,false,ControlCommitMode::Commit);
    draft.prepare(snapshot,7);
    require(!draft.dirty && std::get<int>(draft.value)==30, "hidden/collapsed controls cannot submit abandoned edits");
    snapshot.descriptor.type=scripting::ControlType::TextArea;
    snapshot.value=std::string("first");
    draft.prepare(snapshot,8);
    draft.value=std::string("first\nsecond");
    require(!draft.finish(true,true,false,ControlCommitMode::Commit), "ordinary newline is not a commit");
    snapshot.descriptor.readOnly=true;
    draft.prepare(snapshot,9);
    require(!draft.dirty && std::get<std::string>(draft.value)=="first", "readonly transition cancels draft");
}
}

int main()
{
    int failed=0;
    for (const auto& [name,run] : std::initializer_list<std::pair<const char*,void(*)()>>{
            {"atomic_updates",atomicUpdates},{"worker_validation",workerValidation},{"reload_constraints",reloadConstraints},
            {"runtime_generation",runtimeGeneration},{"stale_dialog_authorization",staleDialogAuthorization},
            {"industrial_controls",industrialControls},{"editing_draft",editingDraft}}) {
        try { run(); std::cout << "[PASS] " << name << '\n'; }
        catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
