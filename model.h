// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
using Str=std::wstring;
using Stamp=long long; // Windows FILETIME in UTC
struct CaseLess { bool operator()(const Str& a,const Str& b) const {
    return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_LESS_THAN;
} };
using Paths=std::set<Str,CaseLess>;
struct Open { Paths paths; bool partial=false; };
struct Row { Str path; Stamp time=0; };
struct Snap { long long id=0; Str title; Stamp time=0; bool manual=false,partial=false; std::vector<Str> paths; };
struct Library { std::vector<Snap> snaps; bool record=true; };
inline Stamp nowStamp(){FILETIME f; GetSystemTimeAsFileTime(&f); return (Stamp(f.dwHighDateTime)<<32)|f.dwLowDateTime;}
inline Str timestamp(Stamp t){
    if(!t)return L"—";
    FILETIME u{DWORD(t),DWORD(t>>32)}; SYSTEMTIME utc{},s{};
    if(!FileTimeToSystemTime(&u,&utc))return L"—";
    struct ZoneCache { DYNAMIC_TIME_ZONE_INFORMATION zone{};std::map<WORD,TIME_ZONE_INFORMATION> years;ULONGLONG expires=0; };
    thread_local ZoneCache cache;
    auto ticks=GetTickCount64();
    if(ticks>=cache.expires){GetDynamicTimeZoneInformation(&cache.zone);cache.years.clear();cache.expires=ticks+60000;}
    auto [entry,inserted]=cache.years.try_emplace(utc.wYear);
    if(inserted && !GetTimeZoneInformationForYear(utc.wYear,&cache.zone,&entry->second)){cache.years.erase(entry);return L"—";}
    if(!SystemTimeToTzSpecificLocalTime(&entry->second,&utc,&s))return L"—";
    wchar_t b[32];swprintf_s(b,L"%04d-%02d-%02d %02d:%02d:%02d",s.wYear,s.wMonth,s.wDay,s.wHour,s.wMinute,s.wSecond);return b;
}
inline Str norm(Str p){
    if(p.find_first_of(L"\"<>|*/?")!=Str::npos || p.find(wchar_t(0))!=Str::npos) return {};
    bool drive=p.size()>=3 && iswalpha(p[0]) && p[1]==L':' && p[2]==L'\\';
    bool unc=p.size()>4 && p.compare(0,2,L"\\\\")==0 && p[2]!=L'.' && p.find(L'\\',2)!=Str::npos;
    if((!drive && !unc)||p.find(L':',drive?2:0)!=Str::npos)return {};
    while(p.size()>3 && p.back()==L'\\')p.pop_back();
    return p;
}
inline Str selfExe(){std::vector<wchar_t> b(32768);DWORD n=GetModuleFileNameW(nullptr,b.data(),DWORD(b.size()));return Str(b.data(),n);}
std::vector<Row> history();
Open probe();
int probeChild(HANDLE mapping);
void launchFolder(const Str& path);
Library readLibrary();
Library writeSnap(const Open&,const Str&,bool manual);
void setRecording(bool on);
