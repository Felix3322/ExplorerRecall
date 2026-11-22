// SPDX-License-Identifier: GPL-3.0-only
#include "model.h"
#include <shlobj.h>
#include <shldisp.h>
#include <exdisp.h>
#include <uiautomationclient.h>
#include <wrl/client.h>
#include <cwchar>
using Microsoft::WRL::ComPtr;

struct BString { BSTR p=nullptr; ~BString(){SysFreeString(p);} Str text()const{return p?Str(p,SysStringLen(p)):Str();} };
struct Handle {HANDLE h=nullptr;~Handle(){if(h && h!=INVALID_HANDLE_VALUE)CloseHandle(h);} };
static bool cls(HWND h,const wchar_t* wanted){wchar_t b[96]{};GetClassNameW(h,b,96);return wcscmp(b,wanted)==0;}
static BOOL CALLBACK countHosts(HWND h,LPARAM p){if(cls(h,L"ShellTabWindowClass"))++*reinterpret_cast<int*>(p);return TRUE;}
static BOOL CALLBACK collectFrames(HWND h,LPARAM p){
    if(cls(h,L"CabinetWClass")||cls(h,L"ExploreWClass"))(*reinterpret_cast<std::map<HWND,int>*>(p))[h]=0;
    return TRUE;
}
static bool readTabs(HWND frame,int hosts,Paths& paths){
    ComPtr<IUIAutomation> u; if(FAILED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&u))))return false;
    ComPtr<IUIAutomationElement> root,strip;
    ComPtr<IUIAutomationCondition> cond;
    VARIANT id;VariantInit(&id);id.vt=VT_BSTR;id.bstrVal=SysAllocString(L"TabListView");
    HRESULT hr=u->CreatePropertyCondition(UIA_AutomationIdPropertyId,id,&cond);VariantClear(&id);
    if(FAILED(hr)||FAILED(u->ElementFromHandle(frame,&root))||FAILED(root->FindFirst(TreeScope_Descendants,cond.Get(),&strip))||!strip)return false;
    VARIANT type;VariantInit(&type);type.vt=VT_I4;type.lVal=UIA_TabItemControlTypeId;
    ComPtr<IUIAutomationCondition> items;ComPtr<IUIAutomationElementArray> tabs;
    if(FAILED(u->CreatePropertyCondition(UIA_ControlTypePropertyId,type,&items))||FAILED(strip->FindAll(TreeScope_Children,items.Get(),&tabs)))return false;
    int n=0,ok=0;tabs->get_Length(&n);
    for(int i=0;i<n;i++){
        ComPtr<IUIAutomationElement> tab; BString name,help;
        if(FAILED(tabs->GetElement(i,&tab)))continue;
        tab->get_CurrentName(&name.p);Str path=norm(name.text());
        if(path.empty()){tab->get_CurrentHelpText(&help.p);path=norm(help.text());}
        if(!path.empty()){paths.insert(path);ok++;}
    }
    return n>0 && ok==n && n>=hosts;
}
static Open collect(){
    Open out;std::map<HWND,int> frames;EnumWindows(collectFrames,reinterpret_cast<LPARAM>(&frames));out.windows=int(frames.size());
    ComPtr<IShellWindows> windows;
    if(FAILED(CoCreateInstance(CLSID_ShellWindows,nullptr,CLSCTX_ALL,IID_PPV_ARGS(&windows))))throw std::runtime_error("shell");
    long n=0;if(FAILED(windows->get_Count(&n)))throw std::runtime_error("shell count");
    for(long i=0;i<n;i++){
        VARIANT index;VariantInit(&index);index.vt=VT_I4;index.lVal=i;
        ComPtr<IDispatch> disp,doc;ComPtr<IWebBrowser2> web;
        if(FAILED(windows->Item(index,&disp))||!disp||FAILED(disp.As(&web)))continue;
        SHANDLE_PTR handle=0;if(FAILED(web->get_HWND(&handle)))continue;
        auto frame=frames.find(reinterpret_cast<HWND>(handle));if(frame==frames.end())continue;
        ComPtr<IShellFolderViewDual> view;ComPtr<Folder> folder;ComPtr<Folder2> folder2;ComPtr<FolderItem> item;BString b;
        if(FAILED(web->get_Document(&doc))||!doc||FAILED(doc.As(&view))||FAILED(view->get_Folder(&folder))||!folder||
           FAILED(folder.As(&folder2))||FAILED(folder2->get_Self(&item))||!item||FAILED(item->get_Path(&b.p))){out.partial=true;continue;}
        frame->second++;Str path=norm(b.text());if(!path.empty())out.paths.insert(path);
    }
    for(auto& [h,seen]:frames){int hosts=0;EnumChildWindows(h,countHosts,reinterpret_cast<LPARAM>(&hosts));
        if((seen==0 || seen<hosts) && !readTabs(h,hosts,out.paths))out.partial=true;}
    return out;
}

// Only this read-only probe subprocess may be terminated on timeout.
// Its one inherited handle is an anonymous mapping owned by the parent.
constexpr DWORD mapBytes=4*1024*1024, signature=0x53455332;
struct Header {DWORD magic,windows,partial,chars;};
struct View {void* p;~View(){if(p)UnmapViewOfFile(p);}};
int probeChild(HANDLE mapping){
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))return 2;
    int code=1;
    try{
        auto state=collect();View v{MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,mapBytes)};
        if(!v.p)throw std::runtime_error("map");
        auto h=static_cast<Header*>(v.p);auto data=reinterpret_cast<wchar_t*>(h+1);size_t total=0;
        for(auto& path:state.paths){
            if(total+path.size()+1>(mapBytes-sizeof(Header))/2)throw std::runtime_error("size");
            std::copy(path.begin(),path.end(),data+total);total+=path.size();data[total++]=0;
        }
        *h={signature,DWORD(state.windows),DWORD(state.partial),DWORD(total)};code=0;
    }catch(...){}
    CoUninitialize();return code;
}
Open probe(){
    SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
    Handle mapping{CreateFileMappingW(INVALID_HANDLE_VALUE,&sa,PAGE_READWRITE,0,mapBytes,nullptr)};
    if(!mapping.h)throw std::runtime_error("mapping");
    SIZE_T sz=0;InitializeProcThreadAttributeList(nullptr,1,0,&sz);
    std::vector<BYTE> buffer(sz);auto attrs=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(buffer.data());
    if(!InitializeProcThreadAttributeList(attrs,1,0,&sz))throw std::runtime_error("attributes");
    struct AttrCleanup{LPPROC_THREAD_ATTRIBUTE_LIST p;~AttrCleanup(){DeleteProcThreadAttributeList(p);}} cleanup{attrs};
    if(!UpdateProcThreadAttribute(attrs,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&mapping.h,sizeof(HANDLE),nullptr,nullptr))throw std::runtime_error("handle list");
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof(start);start.lpAttributeList=attrs;PROCESS_INFORMATION pi{};
    Str exe=selfExe(),cmd=L"\""+exe+L"\" --probe "+std::to_wstring(reinterpret_cast<uintptr_t>(mapping.h));
    if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW,nullptr,nullptr,&start.StartupInfo,&pi))throw std::runtime_error("probe start");
    Handle proc{pi.hProcess},thread{pi.hThread};
    if(WaitForSingleObject(proc.h,8000)!=WAIT_OBJECT_0){TerminateProcess(proc.h,4);throw std::runtime_error("Explorer timeout");}
    DWORD ec=1;GetExitCodeProcess(proc.h,&ec);if(ec)throw std::runtime_error("Explorer read");
    View v{MapViewOfFile(mapping.h,FILE_MAP_READ,0,0,mapBytes)};if(!v.p)throw std::runtime_error("map");
    auto h=static_cast<Header*>(v.p);if(h->magic!=signature||h->chars>(mapBytes-sizeof(Header))/2)throw std::runtime_error("response");
    Open out;out.windows=h->windows;out.partial=h->partial!=0;auto data=reinterpret_cast<wchar_t*>(h+1);
    for(size_t i=0;i<h->chars;){size_t len=wcsnlen_s(data+i,h->chars-i);if(len==h->chars-i)throw std::runtime_error("response");out.paths.emplace(data+i,len);i+=len+1;}
    return out;
}

static void branch(HKEY parent,const std::vector<BYTE>& pidl,std::map<Str,Stamp,CaseLess>& found,int depth){
    if(depth>100)return;
    for(DWORD i=0;;i++){wchar_t name[256];DWORD len=256;FILETIME ft{};
        auto rc=RegEnumKeyExW(parent,i,name,&len,nullptr,nullptr,nullptr,&ft);
        if(rc==ERROR_NO_MORE_ITEMS)break;if(rc!=ERROR_SUCCESS)break;
        DWORD bytes=0;if(RegGetValueW(parent,nullptr,name,RRF_RT_REG_BINARY,nullptr,nullptr,&bytes)!=0||bytes<2||bytes>65536)continue;
        std::vector<BYTE> item(bytes);if(RegGetValueW(parent,nullptr,name,RRF_RT_REG_BINARY,nullptr,item.data(),&bytes)!=0)continue;
        USHORT cb=0;memcpy(&cb,item.data(),2);if(cb<2||cb>bytes)continue;
        std::vector<BYTE> child(pidl.begin(),pidl.end()-2);child.insert(child.end(),item.begin(),item.begin()+cb);child.resize(child.size()+2);
        std::vector<wchar_t> path(32768);
        if(SHGetPathFromIDListEx(reinterpret_cast<PCIDLIST_ABSOLUTE>(child.data()),path.data(),DWORD(path.size()),GPFIDL_DEFAULT)){
            Str p=norm(path.data());if(!p.empty()){auto& t=found[p];t=std::max(t,(Stamp(ft.dwHighDateTime)<<32)|ft.dwLowDateTime);}}
        HKEY sub=nullptr;if(!RegOpenKeyExW(parent,name,0,KEY_READ,&sub)){branch(sub,child,found,depth+1);RegCloseKey(sub);}
    }
}
std::vector<Row> history(){
    std::map<Str,Stamp,CaseLess> found;
    for(auto name:{LR"(Software\Classes\Local Settings\Software\Microsoft\Windows\Shell\BagMRU)",LR"(Software\Microsoft\Windows\Shell\BagMRU)"}){
        HKEY key=nullptr;if(!RegOpenKeyExW(HKEY_CURRENT_USER,name,0,KEY_READ,&key)){branch(key,{0,0},found,0);RegCloseKey(key);}
    }
    std::vector<Row> out;for(auto& [p,t]:found)out.push_back({p,t});
    std::stable_sort(out.begin(),out.end(),[](auto& a,auto& b){return a.time>b.time;});return out;
}
void launchFolder(const Str& raw){
    Str p=norm(raw);if(p.empty())throw std::runtime_error("invalid path");
    auto attr=GetFileAttributesW(p.c_str());if(attr==INVALID_FILE_ATTRIBUTES || !(attr&FILE_ATTRIBUTE_DIRECTORY))throw std::runtime_error("missing or inaccessible");
    wchar_t windows[MAX_PATH];GetWindowsDirectoryW(windows,MAX_PATH);Str exe=Str(windows)+L"\\explorer.exe";
    if(p.back()==L'\\')p+=L'.';Str cmd=L"\""+exe+L"\" /n,\""+p+L"\"";
    STARTUPINFOW si{sizeof(si)};PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&si,&pi))throw std::runtime_error("launch");
    CloseHandle(pi.hProcess);CloseHandle(pi.hThread);
}
