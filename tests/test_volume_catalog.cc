#include "../src/storage/volume_catalog.hpp"
#include "../src/storage/sqlite_database.hpp"
#include "protoscope/data/psrec.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using namespace protoscope;
using tests::require;
template<class F> void rejects(F&& function)
{
    bool failed=false;try {function();} catch(const std::exception&) {failed=true;}
    require(failed,"invalid volume/catalog must be rejected");
}
void source(const std::filesystem::path& path,bool revised=false)
{
    const data::Schema schema{"samples",{{revised ? "second":"first",data::FieldType::Int64,false}}};
    std::ofstream stream(path,std::ios::binary);
    data::PsrecWriter writer(stream,{{9,schema}});
    writer.append({"source","samples","device",1,{},9,{{std::int64_t{42}}}});
    writer.finish();
}
void adoptionAndPins()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-catalog"));
    const auto root=directory.path()/"records",input=directory.path()/"source.psrec";
    std::filesystem::path retained;
    {
        storage::VolumeCatalog catalog(root,"protocol");
        require(catalog.pinAll()->volumes().empty(),"empty catalog");
        source(input);
        auto staged=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
        require(catalog.pinAll()->volumes().empty(),"unregistered staging excluded");
        const auto volume=catalog.adopt(staged,100);
        retained=volume.path;
        require(volume.records==1 && volume.schemaIds.at(9)>0,"register volume and local-to-global schema map");
        require(std::filesystem::exists(retained) && !std::filesystem::exists(root/".staging"/volume.identity),
                "complete volume relocated within root");
        require(!catalog.isPinned(volume.id),"not occupied before query");
        auto lease=catalog.pinAll();
        require(lease->volumes().size()==1 && catalog.isPinned(volume.id),"lease protects volume");
        lease.reset();require(!catalog.isPinned(volume.id),"lease release");
        source(input,true);
        auto other=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
        const auto second=catalog.adopt(other,101);
        require(second.schemaIds.at(9)!=volume.schemaIds.at(9),"same local version with different schema isolated");
        require(catalog.schemas().size()==2,"global schema registry retains both definitions");
        const data::Schema legacy{"samples",{{"first",data::FieldType::Int64,false}}};
        const auto legacyMap=catalog.registerSchemas({{1,legacy}});
        require(legacyMap.at(1)==volume.schemaIds.at(9),"legacy and imported equal schemas share global identity");
        require(volume.idBase>second.idBase && volume.idBase>1000000,
                "import IDs occupy disjoint stable ranges above active records");
    }
    require(std::filesystem::exists(retained),"registered volume survives staging owner destruction");
    storage::VolumeCatalog reopened(root,"protocol");
    require(reopened.pinAll()->volumes().size()==2,"catalog restart restoration");
    rejects([&]{storage::VolumeCatalog foreign(root,"other");});
}
void identityProtection()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-catalog-identity"));
    const auto root=directory.path()/"records",input=directory.path()/"source.psrec";
    source(input);
    storage::VolumeCatalog catalog(root,"protocol");
    {
        auto staged=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
        {std::ofstream file(staged.info().path.parent_path()/"unknown");file<<"untouched";}
        rejects([&]{catalog.adopt(staged,100);});
        require(catalog.pinAll()->volumes().empty(),"failed adoption does not publish index row");
    }
    auto staged=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
    const auto registered=catalog.adopt(staged,100);
    {std::ofstream file(registered.path.parent_path()/"owner");file<<"foreign";}
    rejects([&]{catalog.pinAll();});
    require(std::filesystem::exists(registered.path),"identity mismatch cannot delete registered file");
    const auto foreign=directory.path()/"foreign";
    std::filesystem::create_directories(foreign);
    {storage::sqlite::Database db(foreign/"index.sqlite");db.exec("CREATE TABLE unrelated(value)");}
    rejects([&]{storage::VolumeCatalog other(foreign,"protocol");});
}
void retentionBoundaries()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-retention"));
    const auto root=directory.path()/"records",input=directory.path()/"source.psrec";
    source(input);
    storage::VolumeCatalog catalog(root,"protocol");
    auto oldest=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
    const auto first=catalog.adopt(oldest,10);
    auto pinned=catalog.pinAll();
    storage::VolumeCatalog otherCatalog(root,"protocol");
    require(otherCatalog.isPinned(first.id),"catalog instances share query occupancy");
    auto newer=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
    const auto second=catalog.adopt(newer,20);
    // 活动库、临时占用和记录根外的文件不能成为清理候选。
    {std::ofstream file(root/"records.sqlite");file<<std::string(8192,'a');}
    {std::ofstream file(root/"records.sqlite-wal");file<<std::string(4096,'w');}
    {std::ofstream file(root/".staging"/"unregistered");file<<"unregistered";}
    const auto bytes=catalog.diskBytes();
    require(bytes>=12288+std::filesystem::file_size(first.path)+std::filesystem::file_size(second.path),
            "capacity includes active database WAL and sealed volumes");
    const auto retained=otherCatalog.retain({UINT64_MAX,std::chrono::microseconds(50)},100);
    require(retained.removedVolumes==1 && retained.expiredPinned && !retained.capacityExceeded,
            "expired occupied volume skipped while next eligible volume removed");
    require(std::filesystem::exists(first.path) && !std::filesystem::exists(second.path),
            "query lease protects its volume");
    pinned.reset();
    const auto capacity=catalog.retain({1,std::chrono::hours(24*30)},100);
    require(capacity.removedVolumes==1 && capacity.capacityExceeded,
            "capacity limit removes remaining oldest volume and reports impossible quota");
    require(std::filesystem::exists(root/"records.sqlite") && std::filesystem::exists(root/"records.sqlite-wal") &&
            std::filesystem::exists(root/".staging"/"unregistered") && std::filesystem::exists(input),
            "capacity failure never deletes active staging or external source files");
    require(catalog.pinAll()->volumes().empty(),"removed volumes disappear from query index");
}
void retentionIdentityAndRecovery()
{
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-retention-recovery"));
    const auto root=directory.path()/"records",input=directory.path()/"source.psrec";
    source(input);
    storage::CatalogVolume volume;
    {
        storage::VolumeCatalog catalog(root,"protocol");
        auto staged=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
        volume=catalog.adopt(staged,1);
        std::stop_source canceled;canceled.request_stop();
        rejects([&]{catalog.retain({1,std::chrono::microseconds(0)},100,canceled.get_token());});
        require(std::filesystem::exists(volume.path),"cancel before retirement preserves volume");
        // 模拟清理意图已提交、文件还未删除时异常退出。
        storage::sqlite::Database db(root/"index.sqlite");
        storage::sqlite::Statement intent(db,"INSERT INTO metadata VALUES(?,?)");
        intent.text(1,"retiring:"+std::to_string(volume.id));intent.text(2,volume.identity);intent.row();
    }
    {
        storage::VolumeCatalog recovered(root,"protocol");
        require(recovered.pinAll()->volumes().empty() && !std::filesystem::exists(volume.path.parent_path()),
                "restart completes committed retirement");
        auto staged=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
        volume=recovered.adopt(staged,1);
        {std::ofstream owner(volume.path.parent_path()/"owner");owner<<"foreign";}
        rejects([&]{recovered.retain({1,std::chrono::microseconds(0)},100);});
        require(std::filesystem::exists(volume.path),"ownership mismatch blocks destructive retirement");
    }
    // 独立根覆盖数据库已删除但标记仍在的恢复窗口。
    const auto other=directory.path()/"partial";
    {
        storage::VolumeCatalog catalog(other,"protocol");
        auto staged=storage::stageRecordImport(other,"protocol",input,storage::ImportFormat::Psrec);
        volume=catalog.adopt(staged,1);
        storage::sqlite::Database db(other/"index.sqlite");
        storage::sqlite::Statement intent(db,"INSERT INTO metadata VALUES(?,?)");
        intent.text(1,"retiring:"+std::to_string(volume.id));intent.text(2,volume.identity);intent.row();
        std::filesystem::remove(volume.path);
    }
    storage::VolumeCatalog recovered(other,"protocol");
    require(recovered.pinAll()->volumes().empty() && !std::filesystem::exists(volume.path.parent_path()),
            "restart handles partial file removal without recursive cleanup");
}
void directoryProcessLock()
{
#ifdef _WIN32
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-directory-lock"));
    std::shared_ptr<const storage::PinnedVolumes> lease;
    {
        storage::VolumeCatalog catalog(directory.path(),"protocol");
        lease=catalog.pinAll();
    }
    // 即使宿主销毁，外部快照引用仍须保留进程锁，避免新进程清理占用卷。
    wchar_t executable[32768]{};
    require(GetModuleFileNameW(nullptr,executable,32768)>0,"probe executable path");
    std::wstring command=L"\""+std::wstring(executable)+L"\" --probe-lock \""+directory.path().wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,
                          &startup,&process)!=0,"start lock probe");
    CloseHandle(process.hThread);
    const auto wait=WaitForSingleObject(process.hProcess,10000);
    if (wait!=WAIT_OBJECT_0) {TerminateProcess(process.hProcess,3);WaitForSingleObject(process.hProcess,10000);}
    DWORD code=3;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
    require(wait==WAIT_OBJECT_0 && code==0,"other process rejected while snapshot holds directory lock");
    lease.reset();
    storage::VolumeCatalog reopened(directory.path(),"protocol");
    require(reopened.pinAll()->volumes().empty(),"directory lock released with last owner");
#endif
}
void retirementDeleteFailure()
{
#ifdef _WIN32
    tests::ScopedTempPath directory(tests::makeUniqueTempDir("protoscope-retention-locked-file"));
    const auto root=directory.path()/"records",input=directory.path()/"source.psrec";
    source(input);
    storage::CatalogVolume volume;
    {
        storage::VolumeCatalog catalog(root,"protocol");
        auto staged=storage::stageRecordImport(root,"protocol",input,storage::ImportFormat::Psrec);
        volume=catalog.adopt(staged,1);
        HANDLE held=CreateFileW(volume.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
                                 OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        require(held!=INVALID_HANDLE_VALUE,"hold sealed database without delete sharing");
        bool failed=false;
        try {catalog.retain({1,std::chrono::microseconds(0)},100);}
        catch(const std::exception&) {failed=true;}
        CloseHandle(held);
        require(failed && std::filesystem::exists(volume.path),"file-in-use failure preserves database");
        storage::sqlite::Database db(root/"index.sqlite",true);
        storage::sqlite::Statement intent(db,"SELECT value FROM metadata WHERE key=?");
        intent.text(1,"retiring:"+std::to_string(volume.id));
        require(intent.row() && intent.text(0)==volume.identity,"failed removal retains committed recovery intent");
        require(catalog.pinAll()->volumes().empty(),"retiring volume cannot gain new query leases");
    }
    storage::VolumeCatalog recovered(root,"protocol");
    require(!std::filesystem::exists(volume.path) && recovered.pinAll()->volumes().empty(),
            "restart retries exact validated artifact after file-in-use failure");
#endif
}
}
int main(int argc,char** argv)
{
    if (argc==3 && std::string(argv[1])=="--probe-lock") {
        try {storage::VolumeCatalog catalog(argv[2],"protocol");return 2;}
        catch(const std::exception&) {return 0;}
    }
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"adoption_pins",adoptionAndPins},{"identity_protection",identityProtection},
        {"retention_boundaries",retentionBoundaries},{"retention_recovery",retentionIdentityAndRecovery},
        {"directory_process_lock",directoryProcessLock},{"retirement_delete_failure",retirementDeleteFailure}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
