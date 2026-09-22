#include "protoscope/scripting/script_host.hpp"
#include "protoscope/scripting/script_runtime_worker.hpp"
#include "test_helpers.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace protoscope;
using tests::require;
struct Fixture {
    tests::ScopedTempPath directory{tests::makeUniqueTempDir("protoscope-script-data")};
    scripting::ScriptHost host;
    Fixture() { host.setStorageRoot(directory.path() / "data"); }
    bool load(const std::string& source) {
        { std::ofstream file(directory.path() / "main.lua"); file << source; }
        return host.loadProtocolDirectory(directory.path().generic_string());
    }
    void open() {
        transport::ConnectionContext connection;
        connection.connectionId = 1;
        connection.readyForIo = true;
        host.onTransportOpen({connection});
    }
    void done() {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (std::chrono::steady_clock::now() < deadline) {
            host.tick(0);
            if (!host.drainEvents().empty()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        for (const auto& log : host.drainLogs()) std::cerr << log.message << '\n';
        throw std::runtime_error("script completion timeout");
    }
};

const std::string declaration = R"(
function data()
    return {{id="sample", fields={
        {name="counter",type="int64",nullable=false},
        {name="reading",type="double"},
        {name="raw",type="bytes"},
        {name="note",type="string"}
    }}}
end
function row(n)
    return {dataset="sample", device="device-a", device_time_us=123,
        values={counter=n,reading=1.5,raw=proto.data.bytes("a\0b"),note=proto.data.null}}
end
)";

void fieldBindings()
{
    Fixture f;
    const std::string source=declaration+R"(
        function controls() return {
            {"readout","count","Count",precision=0,binding={dataset="sample",field="counter",device="device-a"}},
            {"readout","reading","Reading",binding={dataset="sample",field="reading"}},
            {"label","note","Note",binding={dataset="sample",field="note"}}
        } end
        function on_open(ctx)
            assert(proto.get_control("count")==nil)
            assert(proto.data.publish(row(9223372036854775807)))
            assert(proto.get_control("count")=="9223372036854775807")
            assert(proto.get_control("reading")=="1.50")
            assert(proto.get_control("note")==nil)
            assert(not proto.ui.update_control("count",{value=8}))
            proto.set_control("count",5)
            assert(proto.get_control("count")=="9223372036854775807")
            local other=row(10);other.device="device-b";other.values.note="Other device"
            assert(proto.data.publish(other))
            assert(proto.get_control("count")=="9223372036854775807")
            assert(proto.get_control("note")=="Other device")
            local missing=row(2);missing.values.reading=nil
            assert(not pcall(proto.data.publish_batch,{row(1),missing}))
            assert(proto.get_control("count")=="9223372036854775807")
            local null=row(12);null.values.reading=proto.data.null;null.values.note=proto.data.null
            assert(proto.data.publish(null))
            assert(proto.get_control("reading")==nil and proto.get_control("note")==nil)
            proto.emit("done","")
        end
    )";
    require(f.load(source),"field binding declaration");
    f.open();
    require(f.host.drainEvents().size()==1,"field binding assertions");
    require(f.host.controlStatesSnapshot()[0].updatedAtMs>0,"bound value has receive timestamp");
    require(f.host.controlStatesSnapshot()[1].dataState==scripting::ControlDataState::Null,"explicit null clears display");
    require(f.load(source),"binding reload");
    require(f.host.controlStatesSnapshot()[0].updatedAtMs==0,"measurements not reused");
    auto invalid=source;
    invalid.replace(invalid.find("field=\"counter\""),std::string("field=\"counter\"").size(),"field=\"missing\"");
    require(!f.load(invalid),"unknown binding field rejected during load");
    invalid=source;
    invalid.replace(invalid.find("\"readout\",\"count\""),std::string("\"readout\",\"count\"").size(),"\"slider_int\",\"count\"");
    require(!f.load(invalid),"user setpoint cannot masquerade as measurement binding");
}

void bindingFailures()
{
    Fixture f;
    require(f.load(R"(
        function data() return {{id="state",fields={
            {name="active",type="bool"},{name="progress",type="double"},{name="note",type="string"}
        }}} end
        function controls() return {
            {"indicator","active","Active",binding={dataset="state",field="active"}},
            {"progress","progress","Progress",binding={dataset="state",field="progress"}},
            {"label","note","Note",max_length=4,binding={dataset="state",field="note"}}
        } end
        function on_open(ctx) proto.record.start() end
        function on_record(ctx,evt)
            assert(evt.ok,evt.error)
            if evt.operation=="start" then
                assert(proto.data.publish({dataset="state",values={active=true,progress=5,note="too long"}}))
                assert(proto.get_control("active")==true)
                assert(proto.get_control("progress")==nil and proto.get_control("note")==nil)
                proto.record.stop()
            elseif evt.operation=="stop" then
                assert(proto.record.status().committed==1)
                proto.emit("done","")
            end
        end
    )"),"invalid display fixture");
    f.open();f.done();
    const auto controls=f.host.controlStatesSnapshot();
    require(controls[1].dataState==scripting::ControlDataState::Invalid &&
            controls[2].dataState==scripting::ControlDataState::Invalid,"invalid display has explicit state");
    require(!controls[1].dataError.empty(),"display error details");
    f.host.setStorageRoot({});
    require(f.load(declaration+R"(
        function controls() return {
            {"readout","live","Live",precision=0,binding={dataset="sample",field="counter"}}
        } end
        function on_open(ctx)
            assert(not pcall(proto.data.publish,row(42)))
            assert(proto.get_control("live")=="42")
            proto.emit("done","")
        end
    )"),"storage error binding fixture");
    f.open();
    require(f.host.drainEvents().size()==1,"storage failure must not suppress live binding");
}

void queryFields()
{
    Fixture f;
    require(f.load(declaration+R"(
        function on_open(ctx) proto.record.start() end
        function on_record(ctx,evt)
            assert(evt.ok,evt.error)
            if evt.operation=="start" then
                proto.data.publish_batch({row(2),row(4),row(1),row(3)})
                proto.record.stop()
            elseif evt.operation=="stop" then
                assert(not pcall(proto.record.query,{conditions={{field="counter",op="SQL",value=1}}}))
                proto.record.query({dataset="sample",limit=1,
                    conditions={{field="counter",op="ge",value=2},{field="note",op="is_null"}},
                    sort={field="counter",descending=true}})
            elseif evt.operation=="query" then
                assert(#evt.records==1 and evt.more and evt.records[1].values.counter==4)
                proto.emit("done","")
            end
        end
    )"),"Lua field query fixture");
    f.open();f.done();
}

void recording()
{
    Fixture f;
    require(f.load(declaration + R"(
        function on_open(ctx) proto.record.start() end
        function on_record(ctx, evt)
            assert(evt.ok, evt.error)
            if evt.operation == "start" then
                assert(proto.data.publish_batch({row(9223372036854775807),row(2)}))
                assert(proto.data.latest("sample").values.counter == 2)
                proto.record.stop()
            elseif evt.operation == "stop" then
                local status=proto.record.status()
                assert(status.committed==2 and status.queued==2 and not status.recording)
                proto.record.query({dataset="sample",limit=1})
            elseif evt.operation == "query" then
                assert(#evt.records==1)
                local value=evt.records[1]
                assert(value.device=="device-a" and value.device_time_us==123)
                if evt.more then
                    assert(value.values.counter==9223372036854775807)
                    assert(value.values.raw:to_hex(3)=="61 00 62")
                    assert(proto.data.is_null(value.values.note))
                    proto.record.query({dataset="sample",limit=1,offset=1,snapshot=evt.snapshot})
                else
                    assert(value.values.counter==2)
                    proto.emit("done","")
                end
            end
        end
    )"), f.host.lastError().c_str());
    f.open();
    const auto wakeup = f.host.nextWakeupAtMs();
    const auto wallTime = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    require(wakeup && *wakeup >= wallTime - 1000 && *wakeup <= wallTime + 1000,
            "存储期限必须使用宿主调度的 epoch 毫秒时基");
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    require(wakeup && f.host.nextWakeupAtMs() == wakeup, "读取快照不能持续推迟存储任务轮询期限");
    f.done();
}

void kvAndReload()
{
    Fixture f;
    const std::string source = R"(
        function on_open(ctx)
            local previous=proto.kv.get("settings")
            if previous then
                assert(previous.n==9223372036854775807 and previous.list[2]==false)
                assert(proto.data.is_null(previous.list[3]))
                proto.emit("recovered","")
            else
                proto.kv.set("settings",{n=9223372036854775807,list={"hello",false,proto.data.null}})
            end
        end
        function on_kv(ctx, evt)
            assert(evt.ok,evt.error)
            if evt.operation=="set" then
                assert(proto.kv.get("settings").n==9223372036854775807)
                proto.kv.flush()
            elseif evt.operation=="flush" then proto.emit("done","") end
        end
    )";
    require(f.load(source), f.host.lastError().c_str());
    f.open(); f.done();
    require(!f.load("proto.kv.set('settings', 42)"), "声明阶段写入必须拒绝");
    f.open(); f.done();
    require(f.load(source), f.host.lastError().c_str());
    f.open(); f.done();
}

void validation()
{
    Fixture f;
    require(f.load(declaration + R"(
        function on_open(ctx)
            local cycle={} cycle.child=cycle
            assert(not pcall(proto.kv.set,"cycle",cycle))
            assert(not pcall(proto.kv.set,"mixed",{[1]=1,key=2}))
            assert(not pcall(proto.kv.set,"sparse",{[2]=1}))
            assert(not pcall(proto.kv.set,"function",function() end))
            assert(not pcall(proto.kv.set,"huge",string.rep("x",262145)))
            assert(not pcall(proto.kv.set,"nan",0/0))
            local deep={} local current=deep
            for i=1,20 do current.child={} current=current.child end
            assert(not pcall(proto.kv.set,"deep",deep))
            assert(not pcall(proto.data.publish_batch,{row(1),{dataset="sample",values={counter=2}}}))
            assert(proto.data.latest("sample")==nil)
            assert(proto.data.publish(row(3)))
            assert(proto.data.latest("sample").values.counter==3)
            assert(proto.record.status().committed==0)
            assert(not pcall(proto.record.query,{limit=-1}))
            proto.emit("done","")
        end
    )"), f.host.lastError().c_str());
    f.open(); f.done();
}

void declarations()
{
    Fixture f;
    require(!f.load("function data() return {{id='x',fields={{name='v',type='invalid'}}}} end"),
            "非法模式必须拒绝");
    require(!f.load("function data() while true do end end"), "data 声明必须受加载预算保护");
    require(f.load(""), f.host.lastError().c_str());
    require(!std::filesystem::exists(f.directory.path() / "data"), "旧脚本不得创建存储目录");
}

void workerCallbacks()
{
    Fixture f;
    require(f.load(R"(
        function on_open(ctx) proto.kv.set("worker",123) end
        function on_kv(ctx,evt)
            assert(evt.ok and proto.kv.get("worker")==123)
            proto.emit("done","")
        end
    )"), "worker script fixture");
    scripting::ScriptRuntimeWorker worker;
    scripting::ScriptRuntimeWorkerConfig config;
    config.storageRoot = f.directory.path() / "worker-data";
    worker.configure(config);
    require(worker.loadProtocolDirectory(f.directory.path().generic_string()).ok, "worker data load");
    transport::ConnectionContext connection;
    connection.connectionId = 7;
    worker.postTransportOpen({connection});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < deadline) {
        worker.postTick(0);
        worker.waitIdle();
        for (const auto& output : worker.drainOutputs())
            if (!output.events.empty()) {
                require(worker.snapshot().nextWakeupAtMs.has_value(), "pending storage polling must wake UI scheduler");
                return;
            }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("worker storage callback timeout");
}

void recordExport()
{
    Fixture f;
    require(f.load(declaration+R"(
        local exports=0
        function on_open(ctx)
            assert(not pcall(proto.record.export,{path="../outside.psrec"}))
            assert(not pcall(proto.record.export,{path="x",format="unknown"}))
            proto.record.start()
        end
        function on_record(ctx,evt)
            if evt.operation=="start" then
                assert(evt.ok,evt.error)
                for i=1,4 do proto.data.publish(row(i)) end
                proto.record.stop()
            elseif evt.operation=="stop" then
                assert(evt.ok,evt.error)
                proto.record.export({path="result.psrec",dataset="sample",format="psrec",
                    conditions={{field="counter",op="gt",value=2}},sort={field="counter",descending=true}})
            elseif evt.operation=="export" then
                exports=exports+1
                if exports==1 then
                    assert(evt.ok and evt.processed==2 and evt.snapshot and evt.path,evt.error)
                    assert(#evt.records==0)
                    proto.record.export({path="result.psrec",format="csv"})
                elseif exports==2 then
                    assert(not evt.ok and evt.processed==0)
                    proto.emit("done","")
                end
            end
        end
    )"),"record export Lua fixture");
    f.open();f.done();
    require(std::filesystem::exists(f.directory.path()/"result.psrec"),"Lua export file exists");
    scripting::FileIoConfig disabled;disabled.enabled=false;f.host.setFileIoConfig(disabled);
    require(f.load(R"(
        function on_open(ctx)
            assert(not pcall(proto.record.export,{path="disabled.psrec"}))
            proto.emit("done","")
        end
    )"),"disabled file I/O fixture");
    f.open();
    require(f.host.drainEvents().size()==1,"record export obeys file I/O disable");
    require(!std::filesystem::exists(f.directory.path()/"disabled.psrec"),"disabled export creates no file");
}
}

int main()
{
    int failed=0;
    for (const auto& [name, run] : std::initializer_list<std::pair<const char*,void(*)()>>{
             {"recording",recording},{"kv_reload",kvAndReload},{"validation",validation},
             {"declarations",declarations},{"worker_callbacks",workerCallbacks},{"field_bindings",fieldBindings},
             {"binding_failures",bindingFailures},{"query_fields",queryFields},{"record_export",recordExport}}) {
        try { std::cout << "[RUN] " << name << std::endl; run(); std::cout << "[PASS] " << name << std::endl; }
        catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
    }
    return failed ? 1 : 0;
}
