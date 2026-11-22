// SPDX-License-Identifier: GPL-3.0-only
#include "model.h"
#include <winsqlite/winsqlite3.h>
#include <shlobj.h>
#include <filesystem>
#include <fstream>
#include <mutex>
namespace fs=std::filesystem;
namespace {
struct Conn{
    sqlite3* db=nullptr;
    ~Conn(){if(db)sqlite3_close(db);}
    void exec(const char* sql){if(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(db));}
};
struct Stmt{
    Conn& c;sqlite3_stmt* s=nullptr;
    Stmt(Conn& cx,const char* sql):c(cx){if(sqlite3_prepare_v2(c.db,sql,-1,&s,nullptr)!=SQLITE_OK)throw std::runtime_error(sqlite3_errmsg(c.db));}
    ~Stmt(){sqlite3_finalize(s);}
    void text(int i,const Str& x){if(sqlite3_bind_text16(s,i,x.c_str(),int(x.size()*2),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error("bind");}
    void num(int i,Stamp x){if(sqlite3_bind_int64(s,i,x)!=SQLITE_OK)throw std::runtime_error("bind");}
    bool row(){int r=sqlite3_step(s);if(r==SQLITE_ROW)return true;if(r!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(c.db));return false;}
    Stamp n(int i){return sqlite3_column_int64(s,i);}
    Str str(int i){auto p=static_cast<const wchar_t*>(sqlite3_column_text16(s,i));return p?p:L"";}
};
std::mutex gate;
Conn db;bool ready=false;
void migrate(const fs::path& dir){
    Stmt done(db,"select 1 from settings where k='migration'");if(done.row())return;
    for(const auto name:{L"sessions.json",L"sessions.json.bak"}){
        auto path=dir/name;if(!fs::exists(path))continue;
        std::ifstream in(path,std::ios::binary);if(!in)throw std::runtime_error("cannot read legacy snapshot");
        std::string raw((std::istreambuf_iterator<char>(in)),{});if(raw.size()>3 && raw.compare(0,3,"\xef\xbb\xbf")==0)raw.erase(0,3);
        Stmt put(db,"insert into legacy values(?)");
        if(sqlite3_bind_text(put.s,1,raw.c_str(),int(raw.size()),SQLITE_TRANSIENT)!=SQLITE_OK)throw std::runtime_error("migration");
        put.row();
        Stmt valid(db,"select json_valid(j) from legacy");bool ok=valid.row() && valid.n(0)!=0;
        if(ok){
            Stmt schema(db,"select json_extract(j,'$.Version')=1 and json_type(j,'$.Items')='array' from legacy");ok=schema.row()&&schema.n(0)!=0;
        }
        if(!ok){db.exec("delete from legacy");if(name==Str(L"sessions.json.bak")||!fs::exists(dir/L"sessions.json.bak"))throw std::runtime_error("invalid legacy snapshot");continue;}
        db.exec("insert into snap(id,title,t,manual,partial) select cast(x.key as integer)+1,json_extract(x.value,'$.Name'),json_extract(x.value,'$.UtcTicks')-504911232000000000,json_extract(x.value,'$.Manual'),json_extract(x.value,'$.Partial') from legacy,json_each(j,'$.Items') x;"
                "insert or ignore into folder(s,p) select cast(x.key as integer)+1,y.value from legacy,json_each(j,'$.Items') x,json_each(x.value,'$.Paths') y where y.type='text';"
                "insert or replace into settings values('record',(select coalesce(json_extract(j,'$.Record'),1) from legacy));delete from legacy;");
        break;
    }
    db.exec("insert into settings values('migration',1)");
}
void init(){
    if(ready)return;
    PWSTR local=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&local)))throw std::runtime_error("profile");
    fs::path dir=fs::path(local)/L"ExplorerFolderSessions";CoTaskMemFree(local);
    // Allow a separate local database for portable profiles and isolated integration tests.
    wchar_t profile[32768];DWORD n=GetEnvironmentVariableW(L"EXPLORER_SESSIONS_PROFILE",profile,32768);
    if(n>0 && n<32768 && fs::path(profile).is_absolute())dir=profile;
    fs::create_directories(dir);
    if(!db.db && sqlite3_open16((dir/L"sessions.sqlite3").c_str(),&db.db)!=SQLITE_OK)throw std::runtime_error("open database");
    sqlite3_busy_timeout(db.db,3000);
    db.exec("pragma foreign_keys=on;create table if not exists snap(id integer primary key,title text not null,t integer not null,manual integer not null,partial integer not null);"
            "create table if not exists folder(s integer references snap on delete cascade,p text not null,primary key(s,p));"
            "create table if not exists settings(k text primary key,v integer);create temp table if not exists legacy(j text);begin immediate");
    try{migrate(dir);db.exec("commit");ready=true;}catch(...){db.exec("rollback");throw;}
}
Library list(){
    Library out;Stmt config(db,"select v from settings where k='record'");
    if(config.row())out.record=config.n(0)!=0;
    Stmt q(db,"select id,title,t,manual,partial,p from snap left join folder on s=id order by t desc,id desc,p");
    while(q.row()){
        if(out.snaps.empty()||out.snaps.back().id!=q.n(0))out.snaps.push_back({q.n(0),q.str(1),q.n(2),q.n(3)!=0,q.n(4)!=0,{}});
        auto p=norm(q.str(5));if(!p.empty())out.snaps.back().paths.push_back(std::move(p));
    }
    return out;
}
}
Library readLibrary(){std::lock_guard lock(gate);init();return list();}
Library writeSnap(const Open& state,const Str& name,bool manual){
    std::lock_guard lock(gate);init();auto lib=list();
    if(state.paths.empty())return lib;
    if(!manual){
        if(!lib.record)return lib;
        if(!lib.snaps.empty()){auto& prev=lib.snaps.front();
            if(prev.partial==state.partial && prev.paths.size()==state.paths.size() && std::all_of(prev.paths.begin(),prev.paths.end(),[&](auto& p){return state.paths.count(p)!=0;}))return lib;}
    }
    db.exec("begin immediate");try{
        Stmt ins(db,"insert into snap(title,t,manual,partial) values(?,?,?,?)");ins.text(1,name);ins.num(2,nowStamp());ins.num(3,manual);ins.num(4,state.partial);ins.row();
        auto id=sqlite3_last_insert_rowid(db.db);Stmt add(db,"insert into folder values(?,?)");
        for(auto& path:state.paths){sqlite3_reset(add.s);add.num(1,id);add.text(2,path);add.row();}
        db.exec("delete from snap where id in(select id from snap where manual=0 order by t desc,id desc limit -1 offset 200);commit");
    }catch(...){db.exec("rollback");throw;}
    return list();
}
void setRecording(bool on){std::lock_guard lock(gate);init();Stmt s(db,"insert or replace into settings values('record',?)");s.num(1,on);s.row();}
