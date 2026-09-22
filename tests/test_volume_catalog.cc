#include "../src/storage/volume_catalog.hpp"
#include "../src/storage/sqlite_database.hpp"
#include "protoscope/data/psrec.hpp"
#include "test_helpers.hpp"

#include <fstream>
#include <iostream>

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
}
int main()
{
    int failed=0;
    for (const auto& [name,run]:std::initializer_list<std::pair<const char*,void(*)()>>{
        {"adoption_pins",adoptionAndPins},{"identity_protection",identityProtection}}) {
        try {run();std::cout<<"[PASS] "<<name<<'\n';}
        catch(const std::exception& e){++failed;std::cerr<<"[FAIL] "<<name<<": "<<e.what()<<'\n';}
    }
    return failed?1:0;
}
