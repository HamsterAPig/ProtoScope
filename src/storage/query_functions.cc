#include "query_functions.hpp"
#include "protoscope/data/query.hpp"
#include <sqlite3.h>
#include <stdexcept>

namespace protoscope::storage {
namespace {
data::Value decode(sqlite3_value* value)
{
    const auto* bytes=static_cast<const std::uint8_t*>(sqlite3_value_blob(value));
    const auto size=static_cast<std::size_t>(sqlite3_value_bytes(value));
    return data::decodeValue({bytes,size},{32U*1024U*1024U,16});
}
void evaluate(sqlite3_context* ctx,int,sqlite3_value** args) noexcept
{
    try {
        const auto record=data::recordFromValue(decode(args[0]));
        const auto schema=data::schemaFromValue(decode(args[1]));
        data::validateRecord(schema,record);
        const auto mode=reinterpret_cast<std::intptr_t>(sqlite3_user_data(ctx));
        if (mode==0) {
            sqlite3_result_int(ctx,data::matches(record,schema,data::conditionsFromValue(decode(args[2]))));
            return;
        }
        const auto* text=sqlite3_value_text(args[2]);
        const std::string field(reinterpret_cast<const char*>(text),sqlite3_value_bytes(args[2]));
        const auto* value=data::fieldValue(record,schema,field);
        if (!value) {sqlite3_result_null(ctx);return;}
        if (mode==1) {sqlite3_result_int(ctx,static_cast<int>(value->value.index()));return;}
        std::visit([&](const auto& item) {
            using T=std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T,std::monostate>) sqlite3_result_null(ctx);
            else if constexpr (std::is_same_v<T,std::int64_t>) sqlite3_result_int64(ctx,item);
            else if constexpr (std::is_same_v<T,double>) sqlite3_result_double(ctx,item);
            else if constexpr (std::is_same_v<T,bool>) sqlite3_result_int(ctx,item);
            else if constexpr (std::is_same_v<T,std::string>)
                sqlite3_result_text64(ctx,item.data(),item.size(),SQLITE_TRANSIENT,SQLITE_UTF8);
            else if constexpr (std::is_same_v<T,data::Bytes>)
                sqlite3_result_blob64(ctx,item.data(),item.size(),SQLITE_TRANSIENT);
            else throw std::invalid_argument("record fields must be scalars");
        },value->value);
    } catch (const std::exception& error) {sqlite3_result_error(ctx,error.what(),-1);}
    catch (...) {sqlite3_result_error(ctx,"query field decoding failed",-1);}
}
}
void registerQueryFunctions(sqlite3* db)
{
    const char* names[]={"ps_matches","ps_field_type","ps_field"};
    for (std::intptr_t mode=0;mode<3;++mode)
        if (sqlite3_create_function_v2(db,names[mode],3,SQLITE_UTF8|SQLITE_DETERMINISTIC|SQLITE_DIRECTONLY,
                reinterpret_cast<void*>(mode),evaluate,nullptr,nullptr,nullptr)!=SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(db));
}
} // namespace protoscope::storage
