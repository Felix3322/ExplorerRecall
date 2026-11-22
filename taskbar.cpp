// SPDX-License-Identifier: GPL-3.0-only
#include "ui_text.h"
#include "taskbar.h"
#include <shlobj.h>
#include <appmodel.h>
#include <shobjidl.h>
#include <propkey.h>
#include <dwmapi.h>
#include <wrl/client.h>
#include <array>
#include <cstring>
using Microsoft::WRL::ComPtr;
namespace {
const Str explorerKey=LR"(Software\Microsoft\Windows\CurrentVersion\Explorer\)";
Str leaf(const Str& p){auto split=p.find_last_of(L"\\/");return split==Str::npos?p:p.substr(split+1);}
Str displayId(const Str& id){
    auto sep=id.find(L'_'),app=id.find(L'!');
    return sep!=Str::npos && app!=Str::npos && sep<app ? id.substr(0,sep) : id;
}
Str prop(IPropertyStore* s,REFPROPERTYKEY k){
    PROPVARIANT v{};Str out;
    if(s && SUCCEEDED(s->GetValue(k,&v)) && v.vt==VT_LPWSTR && v.pwszVal)out=v.pwszVal;
    PropVariantClear(&v);return out;
}
Str known(const Str& raw){
    if(raw.size()<38 || raw.front()!=L'{' || raw[37]!=L'}')return raw;
    GUID g{};if(FAILED(CLSIDFromString(raw.substr(0,38).c_str(),&g)))return raw;
    PWSTR path=nullptr;Str out=raw;
    if(SUCCEEDED(SHGetKnownFolderPath(g,KF_FLAG_DONT_VERIFY,nullptr,&path)))out=Str(path)+raw.substr(38);
    CoTaskMemFree(path);return out;
}
struct Window {Str name,path,appId,title;DWORD pid;bool explorerFolder=false;};
BOOL CALLBACK visibleWindow(HWND h,LPARAM data){
    if(!IsWindowVisible(h) || h==GetShellWindow())return TRUE;
    auto ex=GetWindowLongPtrW(h,GWL_EXSTYLE);
    if((ex&WS_EX_TOOLWINDOW) || (GetWindow(h,GW_OWNER) && !(ex&WS_EX_APPWINDOW)))return TRUE;
    DWORD cloaked=0;DwmGetWindowAttribute(h,DWMWA_CLOAKED,&cloaked,sizeof(cloaked));if(cloaked)return TRUE;
    wchar_t title[1024]{};if(!GetWindowTextW(h,title,1024))return TRUE;
    Window w{};w.title=title;GetWindowThreadProcessId(h,&w.pid);
    if(HANDLE p=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,w.pid)){
        wchar_t buf[32768];DWORD n=32768;if(QueryFullProcessImageNameW(p,0,buf,&n))w.path.assign(buf,n);
        wchar_t id[256];UINT32 m=256;if(GetApplicationUserModelId(p,&m,id)==ERROR_SUCCESS)w.appId=id;CloseHandle(p);
    }
    ComPtr<IPropertyStore> props;if(SUCCEEDED(SHGetPropertyStoreForWindow(h,IID_PPV_ARGS(&props)))){
        Str id=prop(props.Get(),PKEY_AppUserModel_ID);if(!id.empty())w.appId=id;}
    wchar_t cls[80]{},win[MAX_PATH]{};GetClassNameW(h,cls,80);GetWindowsDirectoryW(win,MAX_PATH);
    w.explorerFolder=!_wcsicmp(w.path.c_str(),(Str(win)+L"\\explorer.exe").c_str()) &&
       (!wcscmp(cls,L"CabinetWClass")||!wcscmp(cls,L"ExploreWClass"));
    if(w.appId.empty()&&w.explorerFolder)w.appId=L"Microsoft.Windows.Explorer";
    w.name=w.path.empty()?w.title:leaf(w.path);reinterpret_cast<std::vector<Window>*>(data)->push_back(std::move(w));return TRUE;
}
struct Key {HKEY h=nullptr;~Key(){if(h)RegCloseKey(h);}};
template<class Fn> bool values(const Str& key,Fn fn){
    Key k;if(RegOpenKeyExW(HKEY_CURRENT_USER,key.c_str(),0,KEY_QUERY_VALUE,&k.h)!=ERROR_SUCCESS)return false;
    DWORD maxName=0,maxBytes=0;
    if(RegQueryInfoKeyW(k.h,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,&maxName,&maxBytes,nullptr,nullptr)!=ERROR_SUCCESS)return false;
    std::vector<wchar_t> name(maxName+2);std::vector<BYTE> buf(maxBytes+1);
    for(DWORD i=0;;++i){DWORD n=DWORD(name.size()),sz=DWORD(buf.size()),type=0;
        auto e=RegEnumValueW(k.h,i,name.data(),&n,nullptr,&type,buf.data(),&sz);
        if(e==ERROR_NO_MORE_ITEMS)break;if(e==ERROR_SUCCESS)fn(Str(name.data(),n),type,buf.data(),sz);}
    return true;
}
Str rot(Str v){for(auto& c:v){if(c>=L'A'&&c<=L'Z')c=wchar_t(L'A'+(c-L'A'+13)%26);else if(c>=L'a'&&c<=L'z')c=wchar_t(L'a'+(c-L'a'+13)%26);}return v;}
struct Pin {Str name,path,id,file;Stamp time;};
std::vector<Pin> pins(){
    std::vector<Pin> out;PWSTR base=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData,KF_FLAG_DONT_VERIFY,nullptr,&base)))return out;
    Str dir=Str(base)+LR"(\Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar\)";CoTaskMemFree(base);
    WIN32_FIND_DATAW fd{};HANDLE find=FindFirstFileW((dir+L"*.lnk").c_str(),&fd);if(find==INVALID_HANDLE_VALUE)return out;
    do{
        Pin pin;pin.file=dir+fd.cFileName;pin.name=fd.cFileName;pin.name.resize(pin.name.size()-4);
        pin.time=(Stamp(fd.ftLastWriteTime.dwHighDateTime)<<32)|fd.ftLastWriteTime.dwLowDateTime;
        ComPtr<IShellLinkW> sl;ComPtr<IPersistFile> file;ComPtr<IPropertyStore> ps;
        if(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&sl)))&&SUCCEEDED(sl.As(&file))&&SUCCEEDED(file->Load(pin.file.c_str(),STGM_READ))){
            wchar_t buf[32768]{};if(SUCCEEDED(sl->GetPath(buf,32768,nullptr,SLGP_RAWPATH))){
                pin.path=buf;if(pin.path.find(L'%')!=Str::npos){wchar_t expanded[32768];auto size=ExpandEnvironmentStringsW(buf,expanded,32768);if(size&&size<=32768)pin.path=expanded;}}
            if(SUCCEEDED(sl.As(&ps)))pin.id=prop(ps.Get(),PKEY_AppUserModel_ID);
        }out.push_back(std::move(pin));
    }while(FindNextFileW(find,&fd));FindClose(find);return out;
}
}
const wchar_t* sourceName(AppSource s){
    switch(s){case AppSource::Interaction:return tr(Text::Interaction);case AppSource::Pin:return tr(Text::Pin);
    case AppSource::Launch:return tr(Text::LaunchRecord);default:return tr(Text::Window);}
}
AppReport taskbarRecords(){
    AppReport report;auto shortcuts=pins();std::vector<Window> windows;
    EnumWindows(visibleWindow,reinterpret_cast<LPARAM>(&windows));
    std::map<Str,const Pin*,CaseLess> pinById;
    for(auto& p:shortcuts)if(!p.id.empty())pinById[p.id]=&p;
    const std::array<Str,4> keys={L"AppSwitched",L"AppLaunch",L"ShowJumpView",L"AppBadgeUpdated"};
    const std::array<Str,4> labels={tr(Text::Switches),tr(Text::Launches),tr(Text::JumpView),tr(Text::Badges)};
    std::map<Str,std::map<int,DWORD>,CaseLess> stats;
    int readable=0;
    for(int i=0;i<4;i++)readable+=values(explorerKey+L"FeatureUsage\\"+keys[i],[&](const Str& id,DWORD type,const BYTE* v,DWORD n){
        if(!id.empty()&&n==4&&type==REG_DWORD){DWORD value;memcpy(&value,v,4);stats[id][i]=value;}
    });
    auto add=[&](AppRecord row){
        if(row.source!=AppSource::Window){
            bool identifiable=!norm(row.path).empty() && leaf(row.path).size()>4 && _wcsicmp(leaf(row.path).substr(leaf(row.path).size()-4).c_str(),L".exe")==0;
            row.state=identifiable?1:2;
            for(auto& w:windows){
                bool sameId=!row.appId.empty() && !w.appId.empty() && !_wcsicmp(row.appId.c_str(),w.appId.c_str());
                if(w.explorerFolder && !_wcsicmp(row.appId.c_str(),L"Microsoft.Windows.Explorer"))sameId=true;
                bool sameExe=identifiable && (row.appId.empty() || w.appId.empty()) && !w.path.empty() && !_wcsicmp(w.path.c_str(),row.path.c_str());
                if(sameId||sameExe){row.state=0;row.details+=tr(Text::MatchLabel)+std::to_wstring(w.pid)+L"  ·  "+w.title;}
            }
        }
        row.search=row.name+L"\n"+row.id+L"\n"+row.path+L"\n"+row.summary+L"\n"+row.details;
        report.rows.push_back(std::move(row));
    };
    for(auto& [id,counts]:stats){
        AppRecord r;r.source=AppSource::Interaction;r.id=id;r.path=norm(known(id));r.name=r.path.empty()?displayId(id):leaf(r.path);if(r.path.empty()&&id[0]!=L'{')r.appId=id;
        auto pin=pinById.find(id);if(pin!=pinById.end()){r.name=pin->second->name;r.path=pin->second->path;}
        r.details=tr(Text::RegistrySource)+explorerKey+tr(Text::UsageHint);
        for(auto& [i,count]:counts){if(i!=3)r.interactions+=count;if(!r.summary.empty())r.summary+=L"   /   ";r.summary+=labels[i]+L" "+std::to_wstring(count);r.details+=L"\r\n"+keys[i]+L"  ·  "+std::to_wstring(count);}
        add(std::move(r));
    }
    for(auto& p:shortcuts){AppRecord r;r.source=AppSource::Pin;r.name=p.name;r.path=p.path;r.id=r.appId=p.id;r.time=p.time;
        r.summary=p.path.empty()?p.file:p.path;r.details=tr(Text::Shortcut)+p.file+tr(Text::ShortcutTime);add(std::move(r));}
    for(auto guid:{L"{CEBFF5CD-ACE2-4F4F-9178-9926F41749EA}",L"{F4E57C4B-2036-45F0-A9AB-443BCFE33D9F}"}){
        Str key=explorerKey+L"UserAssist\\"+guid+L"\\Count";values(key,[&](const Str& raw,DWORD type,const BYTE* v,DWORD n){
            if(type!=REG_BINARY||n!=72)return;
            Stamp time;DWORD launches,focus;memcpy(&time,v+60,8);memcpy(&launches,v+4,4);memcpy(&focus,v+8,4);
            Str id=rot(raw);if(id.empty()||id.compare(0,5,L"UEME_")==0)return;
            if(time<0 || time>nowStamp()+864000000000LL)time=0;
            AppRecord r;r.source=AppSource::Launch;r.id=id;r.path=norm(known(id));r.name=r.path.empty()?id:leaf(r.path);r.time=time;
            if(r.path.empty()&&id.front()!=L'{')r.appId=id;
            r.summary=tr(Text::LaunchCount)+std::to_wstring(launches)+tr(Text::FocusCount)+std::to_wstring(focus);
            r.details=tr(Text::RegistrySource)+key+tr(Text::LaunchTime);add(std::move(r));
        });
    }
    auto observed=nowStamp();
    for(auto& w:windows){AppRecord r;r.source=AppSource::Window;r.name=w.name;r.path=w.path;r.id=r.appId=w.appId;r.state=0;
        r.time=observed;r.summary=w.title;r.details=L"PID  ·  "+std::to_wstring(w.pid)+tr(Text::WindowLabel)+w.title+tr(Text::ObservedAt);add(std::move(r));}
    std::stable_sort(report.rows.begin(),report.rows.end(),[](const auto& a,const auto& b){
        if(a.source!=b.source)return int(a.source)<int(b.source);if(a.interactions!=b.interactions)return a.interactions>b.interactions;if(a.time!=b.time)return a.time>b.time;return CaseLess{}(a.name,b.name);});
    if(!readable)report.note=tr(Text::NoCounters);
    return report;
}
Str recentForApp(const Str& appId){
    if(appId.empty())return tr(Text::NoId);
    ComPtr<IApplicationDocumentLists> lists;ComPtr<IObjectArray> items;
    if(FAILED(CoCreateInstance(CLSID_ApplicationDocumentLists,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&lists)))||
       FAILED(lists->SetAppID(appId.c_str()))||FAILED(lists->GetList(ADLT_RECENT,20,IID_PPV_ARGS(&items))))return tr(Text::NoRecents);
    UINT n=0;items->GetCount(&n);Str result;
    for(UINT i=0;i<n;i++){ComPtr<IShellItem> item;Str name;PWSTR text=nullptr;
        if(SUCCEEDED(items->GetAt(i,IID_PPV_ARGS(&item)))){
            if(FAILED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING,&text)))item->GetDisplayName(SIGDN_NORMALDISPLAY,&text);
            if(text){name=text;CoTaskMemFree(text);}
        }else{ComPtr<IShellLinkW> link;wchar_t buf[32768]{};if(SUCCEEDED(items->GetAt(i,IID_PPV_ARGS(&link)))&&SUCCEEDED(link->GetPath(buf,32768,nullptr,SLGP_RAWPATH)))name=buf;}
        if(!name.empty())result+=name+L"\r\n";}
    return result.empty()?tr(Text::NoRecents):result;
}
