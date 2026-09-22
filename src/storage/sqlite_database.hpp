#pragma once

#include "protoscope/data/model.hpp"
#include <sqlite3.h>
#include <filesystem>
#include <stdexcept>

namespace protoscope::storage::sqlite {
inline void check(int code,sqlite3* db)
{
    if (code!=SQLITE_OK && code!=SQLITE_DONE && code!=SQLITE_ROW)
        throw std::runtime_error(sqlite3_errmsg(db));
}
class Database {
public:
    Database(const std::filesystem::path& path,bool readOnly=false)
    {
        const auto utf8=path.u8string();
        const auto code=sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()),&db_,
            (readOnly ? SQLITE_OPEN_READONLY:SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE)|SQLITE_OPEN_NOMUTEX,nullptr);
        if (code!=SQLITE_OK) {
            const std::string error=db_ ? sqlite3_errmsg(db_):"SQLite open failed";
            if (db_) sqlite3_close(db_);
            db_=nullptr;
            throw std::runtime_error(error);
        }
        sqlite3_busy_timeout(db_,5000);
    }
    ~Database() {if (db_) sqlite3_close(db_);}
    Database(const Database&)=delete;
    Database& operator=(const Database&)=delete;
    sqlite3* get() const {return db_;}
    void exec(const char* sql) {check(sqlite3_exec(db_,sql,nullptr,nullptr,nullptr),db_);}
private:
    sqlite3* db_{nullptr};
};
class Statement {
public:
    Statement(Database& db,const char* sql):db_(db.get()) {check(sqlite3_prepare_v2(db_,sql,-1,&statement_,nullptr),db_);}
    ~Statement() {sqlite3_finalize(statement_);}
    Statement(const Statement&)=delete;
    Statement& operator=(const Statement&)=delete;
    void text(int index,const std::string& value)
    {
        check(sqlite3_bind_text64(statement_,index,value.data(),value.size(),SQLITE_TRANSIENT,SQLITE_UTF8),db_);
    }
    void integer(int index,std::int64_t value) {check(sqlite3_bind_int64(statement_,index,value),db_);}
    void blob(int index,const data::Bytes& bytes)
    {
        check(sqlite3_bind_blob64(statement_,index,bytes.data(),bytes.size(),SQLITE_TRANSIENT),db_);
    }
    bool row()
    {
        const auto code=sqlite3_step(statement_);check(code,db_);return code==SQLITE_ROW;
    }
    std::int64_t integer(int index) const {return sqlite3_column_int64(statement_,index);}
    std::string text(int index) const
    {
        const auto* value=sqlite3_column_text(statement_,index);
        return value ? std::string(reinterpret_cast<const char*>(value),sqlite3_column_bytes(statement_,index)):std::string{};
    }
    data::Bytes blob(int index) const
    {
        const auto* bytes=static_cast<const std::uint8_t*>(sqlite3_column_blob(statement_,index));
        const auto size=sqlite3_column_bytes(statement_,index);
        return size==0 ? data::Bytes{}:data::Bytes(bytes,bytes+size);
    }
private:
    sqlite3* db_;
    sqlite3_stmt* statement_{nullptr};
};
} // namespace protoscope::storage::sqlite
