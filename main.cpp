// SPDX-License-Identifier: GPL-3.0-only
#include "ui_text.h"
#include "model.h"
#include "taskbar.h"
#include <uxtheme.h>
#include <commctrl.h>
#include <windowsx.h>
#include <atomic>
#include <array>
#include <shellapi.h>
#include <shlwapi.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
namespace {
constexpr UINT deliver=WM_APP+7;
enum Id {Tabs=100,Search,Filter,List,Save,Restore,Whole,Refresh,Auto,Sessions,Name,Dates,Info,Foot,Title,AppFilter,Details,Language};
struct SortOrder {int column;bool descending;};
SortOrder orders[]={{1,true},{3,false},{3,false},{-1,false}};
unsigned languageGeneration=0;
void refreshLanguage();
AppReport apps;bool readingApps=false;Str appsError;HWND detailWin=nullptr,detailEdit=nullptr;unsigned detailNumber=0;
const COLORREF background=RGB(255,255,255),ink=RGB(30,49,67),muted=RGB(69,92,111),accent=RGB(22,99,121);
HBRUSH canvas=nullptr;HIMAGELIST heights=nullptr;bool contrast=false;
HWND mainWin;HFONT font,titleFont;bool reading=false,readingHistory=false,restoring=false,valid=false,everRead=false,initDb=false,writing=false;
bool pendingSave=false;unsigned probeGeneration=0;Str pendingName; Open live;Library library;
std::vector<Row> visits,sessionRows,liveRows;std::vector<size_t> matches;
Str notice,liveError,dbError,historyError;
std::mutex queueMutex;std::condition_variable queueReady;std::deque<std::function<void()>> dbQueue;
bool shuttingDown=false;std::atomic<int> dbTasks{0};
HWND ctl(int id){return GetDlgItem(mainWin,id);}
int mode(){return std::max(0,int(SendMessageW(ctl(Tabs),LB_GETCURSEL,0,0)));}
int sel(int id){return int(SendMessageW(ctl(id),CB_GETCURSEL,0,0));}
Str getText(int id){int n=GetWindowTextLengthW(ctl(id));Str s(n+1,L'\0');GetWindowTextW(ctl(id),s.data(),n+1);s.resize(n);return s;}
void post(std::function<void()> action){
    auto task=new std::function<void()>(std::move(action));
    if(!PostMessageW(mainWin,deliver,0,reinterpret_cast<LPARAM>(task)))delete task;
}
Str errorText(const std::exception& e){const char* p=e.what();int n=MultiByteToWideChar(CP_UTF8,0,p,-1,nullptr,0);Str s(n,L'\0');if(n)MultiByteToWideChar(CP_UTF8,0,p,-1,s.data(),n);if(n)s.pop_back();return s;}
void enqueue(std::function<void()> task){
    {std::lock_guard lock(queueMutex);++dbTasks;dbQueue.push_back(std::move(task));}queueReady.notify_one();
}
void dbLoop(){
    for(;;){std::function<void()> task;
        {std::unique_lock lock(queueMutex);queueReady.wait(lock,[]{return !dbQueue.empty()||shuttingDown;});
         if(dbQueue.empty())break;task=std::move(dbQueue.front());dbQueue.pop_front();}
        task();--dbTasks;
    }
}
const std::vector<Row>& source(){return mode()==0?visits:mode()==1?sessionRows:liveRows;}
struct DisplayRow {
    std::array<Str,4> text;
    Stamp time=0;
    int state=2;
};
std::vector<DisplayRow> display;
bool dataDirty=true,stateDirty=true;
unsigned long long displayRevision=1;
void prepareRows(){
    bool app=mode()==3;const auto& src=source();
    const size_t count=app?apps.rows.size():src.size();
    if(dataDirty || display.size()!=count){
        display.clear();display.resize(count);
        for(size_t i=0;i<count;i++){
            auto& row=display[i];
            if(app){
                const auto& r=apps.rows[i];row.time=r.time;row.text[2]=r.name;
                row.text[3]=Str(sourceName(r.source))+L"  ·  "+r.summary;
            }else{
                const auto& r=src[i];row.time=r.time;row.text[3]=r.path;auto n=r.path.find_last_of(L'\\');
                row.text[2]=n+1<r.path.size()?r.path.substr(n+1):r.path;
            }
            row.text[1]=timestamp(row.time);
        }
        stateDirty=true;dataDirty=false;
    }
    if(!stateDirty)return;
    ++displayRevision;
    for(size_t i=0;i<count;i++){
        auto& r=display[i];
        r.state=app?apps.rows[i].state:!valid?2:live.paths.count(src[i].path)?0:live.partial?2:1;
        r.text[0]=tr(r.state==0?Text::Open:r.state==1?(app?Text::NoWindow:Text::Closed):Text::Unknown);
    }
    stateDirty=false;
}
const Str& cell(size_t index,int col){
    static const Str empty;
    return index<display.size()&&col>=0&&col<4?display[index].text[col]:empty;
}
void status(){
    Str s=std::to_wstring(matches.size())+tr(Text::Rows);
    s+=valid ? tr(Text::OpenCount)+std::to_wstring(live.paths.size())+tr(Text::Folders)+(live.partial?tr(Text::PartialPaths):L"") : everRead? tr(Text::Unverified):tr(Text::Detecting);
    for(auto p:{&liveError,&dbError,&historyError,&notice})if(!p->empty())s+=L"    "+*p;
    if(mode()==3)s=std::to_wstring(matches.size())+tr(Text::AppRows)+(readingApps?tr(Text::LoadingInline):L"")+L"    "+appsError;
    static Str lastFooter;if(lastFooter!=s){SetWindowTextW(ctl(Foot),s.c_str());lastFooter=s;}
    EnableWindow(ctl(Details),mode()==3 && ListView_GetSelectedCount(ctl(List))==1);
    EnableWindow(ctl(Restore),mode()!=3 && !restoring && ListView_GetSelectedCount(ctl(List))>0);
    EnableWindow(ctl(Whole),!restoring && mode()==1 && sel(Sessions)>=0);
    EnableWindow(ctl(Save),initDb && !writing && !pendingSave);
    EnableWindow(ctl(Auto),initDb);
}
void sortHeader(){
    const auto order=orders[mode()];
    HWND header=ListView_GetHeader(ctl(List));
    for(int col=0;col<Header_GetItemCount(header);++col){
        HDITEMW item{};item.mask=HDI_FORMAT;
        if(!Header_GetItem(header,col,&item))continue;
        int next=item.fmt&~(HDF_SORTUP|HDF_SORTDOWN);
        if(col==order.column)next|=order.descending?HDF_SORTDOWN:HDF_SORTUP;
        if(next!=item.fmt){item.fmt=next;Header_SetItem(header,col,&item);}
    }
}
void sortRows(std::vector<size_t>& indices){
    const auto order=orders[mode()];if(order.column<0)return;
    const wchar_t* locale=englishUI?L"en-US":L"zh-CN";
    std::stable_sort(indices.begin(),indices.end(),[&](size_t ia,size_t ib){
        const auto& a=display[ia];const auto& b=display[ib];
        if(order.column==1 && (a.time==0)!=(b.time==0))return b.time==0;
        if(order.column==1)return order.descending?a.time>b.time:a.time<b.time;
        if(order.column==0)return order.descending?a.state>b.state:a.state<b.state;
        int cmp=CompareStringEx(locale,NORM_IGNORECASE|SORT_DIGITSASNUMBERS,a.text[order.column].c_str(),-1,b.text[order.column].c_str(),-1,nullptr,nullptr,0);
        return cmp==(order.descending?CSTR_GREATER_THAN:CSTR_LESS_THAN);
    });
}
void rebuild(bool keep=false){
    prepareRows();
    const auto& src=source();auto list=ctl(List);
    int top=keep?ListView_GetTopIndex(list):0;
    Str q=getText(Search);int f=sel(Filter),d=sel(Dates);
    const Stamp day=86400LL*10000000;
    Stamp cutoff=0;
    if(mode()==0 && d>0){SYSTEMTIME s{},u{};GetLocalTime(&s);s.wHour=s.wMinute=s.wSecond=s.wMilliseconds=0;
        FILETIME ft{};TzSpecificLocalTimeToSystemTime(nullptr,&s,&u);SystemTimeToFileTime(&u,&ft);
        cutoff=((Stamp(ft.dwHighDateTime)<<32)|ft.dwLowDateTime)-(d==1?0:d==2?6:29)*day;}
    std::vector<size_t> next;next.reserve(mode()==3?apps.rows.size():src.size());
    if(mode()==3){
        int origin=sel(AppFilter);
        for(size_t i=0;i<apps.rows.size();++i){const auto& r=apps.rows[i];
            if(origin>0&&origin!=int(r.source))continue;
            if(f==1 && r.state!=0)continue;if(f==2 && r.state==0)continue;
            if(q.empty()||StrStrIW(r.search.c_str(),q.c_str()))next.push_back(i);}
    }else for(size_t i=0;i<src.size();i++){const auto& row=src[i];if(row.time<cutoff||(!q.empty()&&!StrStrIW(row.path.c_str(),q.c_str())))continue;
        if(f>0 && (!valid || (display[i].state==0)!=(f==1)))continue;
        next.push_back(i);}
    sortRows(next);
    std::set<size_t> selected;
    if(keep && next!=matches){
        for(int i=ListView_GetNextItem(list,-1,LVNI_SELECTED);i>=0;i=ListView_GetNextItem(list,i,LVNI_SELECTED))
            if(size_t(i)<matches.size())selected.insert(matches[i]);
    }
    // When sorting, follow the underlying record rather than its old display position.
    bool preserve=keep && matches==next;
    SendMessageW(list,WM_SETREDRAW,FALSE,0);
    if(!preserve)ListView_SetItemState(list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
    matches.swap(next);ListView_SetItemCountEx(list,int(matches.size()),LVSICF_NOSCROLL);
    if(!selected.empty())for(size_t i=0;i<matches.size();++i)if(selected.count(matches[i]))
        ListView_SetItemState(list,int(i),LVIS_SELECTED,LVIS_SELECTED);
    if(!keep&&!matches.empty())ListView_EnsureVisible(list,0,FALSE);
    if(keep && top>=int(matches.size())&&!matches.empty())ListView_EnsureVisible(list,int(matches.size()-1),FALSE);
    SendMessageW(list,WM_SETREDRAW,TRUE,0);InvalidateRect(list,nullptr,FALSE);status();
}
void pickSession(){
    sessionRows.clear();int i=sel(Sessions);if(i>=0 && i<int(library.snaps.size())){
        auto& snap=library.snaps[i];for(auto& p:snap.paths)sessionRows.push_back({p,snap.time});
    }
    if(mode()==1){dataDirty=true;rebuild();}
}
void applyLib(Library x,bool chooseLatest,bool relabel=false){
    long long id=0;int old=sel(Sessions);
    if(old>=0&&old<int(library.snaps.size()))id=library.snaps[old].id;
    bool same=!chooseLatest && x.snaps.size()==library.snaps.size() && (x.snaps.empty() || x.snaps.front().id==library.snaps.front().id);
    library=std::move(x);initDb=true;dbError.clear();
    if(relabel || !same || sel(Sessions)<0){
        SendMessageW(ctl(Sessions),WM_SETREDRAW,FALSE,0);SendMessageW(ctl(Sessions),CB_RESETCONTENT,0,0);int chosen=0,i=0;
        for(auto& snap:library.snaps){
            auto label=timestamp(snap.time)+L"    "+(snap.manual?snap.title:Str(tr(Text::AutoTitle)))+L"    ·    "+std::to_wstring(snap.paths.size())+tr(Text::Folders)+(snap.partial?tr(Text::Partial):L"");
            SendMessageW(ctl(Sessions),CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label.c_str()));
            if(id==snap.id&&!chooseLatest)chosen=i;++i;
        }
        SendMessageW(ctl(Sessions),CB_SETCURSEL,chosen,0);SendMessageW(ctl(Sessions),WM_SETREDRAW,TRUE,0);InvalidateRect(ctl(Sessions),nullptr,TRUE);pickSession();
    }
    status();
}
void persist(Open state,Str name,bool manual){
    if(manual)writing=true;
    enqueue([state=std::move(state),name=std::move(name),manual]{
        try{auto lib=writeSnap(state,name,manual);post([lib=std::move(lib),manual]()mutable{
            if(manual){writing=false;notice=tr(Text::Saved);}applyLib(std::move(lib),manual);});}
        catch(const std::exception& e){post([msg=errorText(e),manual]{if(manual)writing=false;dbError=tr(Text::SaveFailed)+msg;status();});}
    });
}
void accept(Open state){
    bool changed=!valid||live.paths.size()!=state.paths.size()||live.paths!=state.paths||live.partial!=state.partial;
    everRead=valid=true;liveError.clear();live=std::move(state);
    if(changed){
        liveRows.clear();for(auto& p:live.paths)liveRows.push_back({p,0});
        if(initDb && Button_GetCheck(ctl(Auto))==BST_CHECKED && !live.paths.empty())persist(live,tr(Text::AutoTitle),false);
        stateDirty=true;
        if(mode()==2){dataDirty=true;rebuild(false);}else if(mode()!=3)rebuild(true);
    }
    if(pendingSave){
        pendingSave=false;
        if(live.paths.empty())notice=tr(Text::EmptySnapshot);
        else persist(live,pendingName,true);
    }
    status();
}
void poll(){
    if(reading||restoring)return;reading=true;auto generation=probeGeneration;
    std::thread([generation]{try{auto state=probe();post([state=std::move(state),generation]()mutable{reading=false;if(generation==probeGeneration)accept(std::move(state));});}
        catch(const std::exception& e){post([msg=errorText(e),generation]{reading=false;if(generation!=probeGeneration)return;valid=false;stateDirty=true;pendingSave=false;liveError=tr(Text::ProbeFailed)+msg;rebuild(true);});}
    }).detach();
}
void readHistory(){
    if(readingHistory)return;readingHistory=true;historyError=tr(Text::LoadingHistory);status();
    std::thread([]{HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        try{auto rows=history();post([rows=std::move(rows)]()mutable{readingHistory=false;historyError.clear();visits=std::move(rows);if(mode()==0){dataDirty=true;rebuild();}else status();});}
        catch(const std::exception& e){post([msg=errorText(e)]{readingHistory=false;historyError=tr(Text::HistoryFailed)+msg;status();});}
        if(SUCCEEDED(hr))CoUninitialize();
    }).detach();
}
void beginRestore(bool whole){
    if(restoring||mode()==3)return;
    Paths paths;
    if(whole){for(auto& r:sessionRows)paths.insert(r.path);}
    else{const auto& src=source();auto list=ctl(List);
        for(int i=ListView_GetNextItem(list,-1,LVNI_SELECTED);i>=0;i=ListView_GetNextItem(list,i,LVNI_SELECTED))
            if(i<int(matches.size()))paths.insert(src[matches[i]].path);}
    if(paths.empty())return;
    if(paths.size()>30 && MessageBoxW(mainWin,(tr(Text::Selected)+std::to_wstring(paths.size())+tr(Text::BatchTail)).c_str(),tr(Text::Batch),MB_OKCANCEL|MB_ICONQUESTION)!=IDOK)return;
    restoring=true;++probeGeneration;notice=tr(Text::Restoring);status();
    std::thread([paths=std::move(paths)]{
        try{
            auto before=probe();int skipped=0,opened=0;Paths pending;Str errors;int failed=0;
            auto fail=[&](const Str& p,const Str& cause){if(++failed<=12)errors+=p+L"  ·  "+cause+L"\n";};
            for(auto& p:paths){
                if(before.paths.count(p)){skipped++;continue;}
                try{launchFolder(p);pending.insert(p);Sleep(100);}catch(...){fail(p,tr(Text::Inaccessible));}
            }
            auto deadline=GetTickCount64()+10000;Open final=before;
            while(!pending.empty()&&GetTickCount64()<deadline){
                Sleep(350);final=probe();for(auto i=pending.begin();i!=pending.end();){
                    if(final.paths.count(*i)){i=pending.erase(i);opened++;}else ++i;}
            }
            for(auto& p:pending)fail(p,tr(Text::Requested));
            post([final=std::move(final),opened,skipped,failed,errors]()mutable{
                restoring=false;notice=tr(Text::NewOpen)+std::to_wstring(opened)+tr(Text::AlreadyOpen)+std::to_wstring(skipped)+tr(Text::NotConfirmed)+std::to_wstring(failed);
                accept(std::move(final));if(failed)MessageBoxW(mainWin,errors.c_str(),tr(Text::UnconfirmedFolders),MB_OK|MB_ICONINFORMATION);
            });
        }catch(const std::exception& e){post([msg=errorText(e)]{restoring=false;notice=tr(Text::RestoreFailed)+msg;status();poll();});}
    }).detach();
}
void readApps(){
    if(readingApps)return;readingApps=true;appsError.clear();status();
    const unsigned started=languageGeneration;
    std::thread([started]{
        auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        try{auto found=taskbarRecords();post([found=std::move(found),started]()mutable{
            if(started!=languageGeneration){readingApps=false;if(mode()==3)readApps();return;}
            apps=std::move(found);appsError=apps.note;readingApps=false;if(mode()==3){dataDirty=true;rebuild();}else status();
        });}catch(const std::exception& e){post([m=errorText(e)]{readingApps=false;appsError=tr(Text::ReadFailed)+m;status();});}
        if(SUCCEEDED(hr))CoUninitialize();
    }).detach();
}
int px(int x){return MulDiv(x,mainWin?GetDpiForWindow(mainWin):GetDpiForSystem(),96);}
LRESULT CALLBACK detailProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_CREATE){detailEdit=CreateWindowExW(0,L"EDIT",nullptr,WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL,
        0,0,1,1,h,nullptr,GetModuleHandleW(nullptr),nullptr);SendMessageW(detailEdit,WM_SETFONT,(WPARAM)font,FALSE);return 0;}
    if(m==WM_SIZE){MoveWindow(detailEdit,px(16),px(12),std::max(1,int(LOWORD(l))-px(32)),std::max(1,int(HIWORD(l))-px(24)),TRUE);return 0;}
    if(m==WM_DESTROY){detailWin=detailEdit=nullptr;++detailNumber;return 0;}
    return DefWindowProcW(h,m,w,l);
}
void showDetails(){
    if(mode()!=3||ListView_GetSelectedCount(ctl(List))!=1)return;
    int i=ListView_GetNextItem(ctl(List),-1,LVNI_SELECTED);if(i<0||size_t(i)>=matches.size())return;
    const auto& r=apps.rows[matches[i]];
    if(!detailWin){
        WNDCLASSW c{};c.lpszClassName=L"ExplorerSessionsAppDetails";c.hInstance=GetModuleHandleW(nullptr);c.lpfnWndProc=detailProc;c.hCursor=LoadCursorW(nullptr,IDC_ARROW);c.hbrBackground=GetSysColorBrush(COLOR_WINDOW);RegisterClassW(&c);
        RECT box{};GetWindowRect(mainWin,&box);
        detailWin=CreateWindowW(c.lpszClassName,tr(Text::DetailTitle),WS_OVERLAPPEDWINDOW,box.left+px(48),box.top+px(48),px(670),px(425),mainWin,nullptr,c.hInstance,nullptr);
    }
    if(!detailWin)return;
    Str content=r.name+L"\r\n"+sourceName(r.source)+L"   ·   "+cell(matches[i],0)+L"\r\n\r\n"+r.summary+L"\r\n";
    if(r.time)content+=tr(Text::TimestampLabel)+timestamp(r.time)+L"\r\n";
    if(!r.path.empty())content+=tr(Text::Target)+r.path+L"\r\n";if(!r.id.empty())content+=tr(Text::AppId)+r.id+L"\r\n";
    content+=L"\r\n"+r.details;
    auto generation=++detailNumber;
    if(r.appId.empty())SetWindowTextW(detailEdit,content.c_str());
    else{
        content+=tr(Text::RecentLabel);
        SetWindowTextW(detailEdit,(content+tr(Text::Loading)).c_str());
        std::thread([id=r.appId,generation,content]{
            auto hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            Str recent;try{recent=recentForApp(id);}catch(...){recent=tr(Text::RecentFailed);}
            if(SUCCEEDED(hr))CoUninitialize();
            post([generation,text=content+recent]{if(detailWin&&generation==detailNumber)SetWindowTextW(detailEdit,text.c_str());});
        }).detach();
    }
    ShowWindow(detailWin,SW_SHOWNORMAL);SetForegroundWindow(detailWin);SetFocus(detailEdit);
}
void move(int id,int x,int y,int w,int h){MoveWindow(ctl(id),px(x),px(y),std::max(1,px(w)),std::max(1,px(h)),TRUE);}
void layout(){
    RECT r{};GetClientRect(mainWin,&r);int w=MulDiv(r.right,96,GetDpiForWindow(mainWin)),h=MulDiv(r.bottom,96,GetDpiForWindow(mainWin));if(w<1||!ctl(List))return;
    const int rail=150,left=rail+22,right=w-22;
    move(Tabs,12,24,rail-24,160);
    move(Title,left,15,206,36);move(Language,right-314,19,122,145);move(Auto,right-182,19,182,28);
    move(Info,left,60,right-left,23);move(Sessions,left,56,right-left,360);
    move(Search,left,97,right-left-333,29);move(Filter,right-321,97,125,240);
    move(Dates,right-187,97,95,210);move(AppFilter,right-187,97,95,240);move(Refresh,right-83,96,83,30);
    move(List,left,143,right-left,h-239);
    ListView_SetColumnWidth(ctl(List),3,px(std::max(190,right-left-475-22)));
    move(Restore,left,h-80,119,32);move(Whole,left+125,h-80,128,32);move(Details,left,h-80,134,32);
    move(Name,right-318,h-79,182,29);move(Save,right-130,h-80,130,32);move(Foot,left,h-36,right-left,24);
}
void viewMode(){
    const Text pages[]={Text::History,Text::Sessions,Text::Current,Text::Taskbar};SetWindowTextW(ctl(Title),tr(pages[mode()]));
    bool sessions=mode()==1;ShowWindow(ctl(Sessions),sessions?SW_SHOW:SW_HIDE);ShowWindow(ctl(Info),sessions?SW_HIDE:SW_SHOW);
    ShowWindow(ctl(Whole),sessions?SW_SHOW:SW_HIDE);ShowWindow(ctl(Dates),mode()==0?SW_SHOW:SW_HIDE);
    bool app=mode()==3;
    ShowWindow(ctl(AppFilter),app?SW_SHOW:SW_HIDE);ShowWindow(ctl(Details),app?SW_SHOW:SW_HIDE);
    for(auto id:{Name,Save,Restore})ShowWindow(ctl(id),app?SW_HIDE:SW_SHOW);
    SetWindowTextW(ctl(Info),app?tr(Text::AppHint):
        mode()==0?tr(Text::HistoryHint):tr(Text::CurrentHint));
    for(int i:{2,3}){LVCOLUMNW col{LVCF_TEXT};col.pszText=const_cast<wchar_t*>(app?(i==2?tr(Text::Program):tr(Text::Summary)):(i==2?tr(Text::Folder):tr(Text::Path)));ListView_SetColumn(ctl(List),i,&col);}
    SendMessageW(ctl(Search),EM_SETCUEBANNER,TRUE,(LPARAM)(app?tr(Text::SearchApps):tr(Text::SearchFolders)));
    dataDirty=true;sortHeader();rebuild();
    if(app)readApps();
}
HWND add(int id,const wchar_t* cls,const wchar_t* label,DWORD style,DWORD ex=0){
    HWND h=CreateWindowExW(ex,cls,label,WS_CHILD|WS_VISIBLE|style,0,0,1,1,mainWin,reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
    SendMessageW(h,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);return h;
}
void makeCombo(int id,std::initializer_list<const wchar_t*> labels){
    HWND h=add(id,WC_COMBOBOXW,L"",CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP);
    for(auto label:labels)SendMessageW(h,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));SendMessageW(h,CB_SETCURSEL,0,0);
}
void refreshLanguage(){
    ++languageGeneration;
    if(detailWin)DestroyWindow(detailWin);
    SetWindowTextW(mainWin,tr(Text::Title));
    const std::pair<int,Text> labels[]={{Title,Text::Title},{Auto,Text::AutoRecord},{Refresh,Text::Refresh},{Restore,Text::OpenSelected},
        {Whole,Text::RestoreSession},{Details,Text::Details},{Save,Text::SaveSession}};
    for(auto label:labels)SetWindowTextW(ctl(label.first),tr(label.second));
    auto relabel=[](int id,std::initializer_list<Text> values){
        int selected=sel(id);auto combo=ctl(id);SendMessageW(combo,CB_RESETCONTENT,0,0);
        for(auto key:values)SendMessageW(combo,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(tr(key)));
        SendMessageW(combo,CB_SETCURSEL,std::max(selected,0),0);
    };
    relabel(Filter,{Text::EveryState,Text::OpenFilter,Text::NotSeen});
    relabel(Dates,{Text::EveryDate,Text::Today,Text::Week,Text::Month});
    relabel(AppFilter,{Text::EverySource,Text::Interaction,Text::Pin,Text::LaunchRecord,Text::Window});
    int page=mode();SendMessageW(ctl(Tabs),LB_RESETCONTENT,0,0);
    for(auto key:{Text::History,Text::Sessions,Text::Current,Text::Taskbar})SendMessageW(ctl(Tabs),LB_ADDSTRING,0,(LPARAM)tr(key));
    SendMessageW(ctl(Tabs),LB_SETCURSEL,page,0);
    int i=0;for(auto key:{Text::State,Text::Time}){
        LVCOLUMNW c{LVCF_TEXT};c.pszText=const_cast<wchar_t*>(tr(key));ListView_SetColumn(ctl(List),i++,&c);
    }
    SendMessageW(ctl(Name),EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(tr(Text::NamePrompt)));
    notice.clear();
    if(initDb)applyLib(library,false,true);
    apps.rows.clear();appsError.clear();
    viewMode();
    InvalidateRect(ctl(Tabs),nullptr,TRUE);
}
// Keep at most one outstanding wheel/track position during a burst of input.
struct ScrollInput {int target=-1,remainder=0;bool queued=false;};
constexpr UINT applyScroll=WM_APP+81;
LRESULT CALLBACK listInput(HWND list,UINT msg,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR ref){
    auto& input=*reinterpret_cast<ScrollInput*>(ref);
    auto schedule=[&](int position){
        SCROLLINFO si{sizeof(si),SIF_RANGE|SIF_PAGE};
        if(!GetScrollInfo(list,SB_VERT,&si))return;
        input.target=std::clamp(position,si.nMin,std::max(si.nMin,si.nMax-int(si.nPage)+1));
        if(!input.queued){input.queued=PostMessageW(list,applyScroll,0,0)!=FALSE;}
    };
    if(msg==WM_MOUSEWHEEL && !(GET_KEYSTATE_WPARAM(w)&(MK_SHIFT|MK_CONTROL))){
        UINT lines=3;SystemParametersInfoW(SPI_GETWHEELSCROLLLINES,0,&lines,0);
        if(lines==WHEEL_PAGESCROLL)lines=std::max(1,ListView_GetCountPerPage(list));
        input.remainder+=GET_WHEEL_DELTA_WPARAM(w)*int(std::min(lines,UINT(1000)));
        int steps=input.remainder/WHEEL_DELTA;input.remainder%=WHEEL_DELTA;
        if(steps)schedule((input.target>=0?input.target:ListView_GetTopIndex(list))-steps);
        return 0;
    }
    if(msg==WM_VSCROLL&&(LOWORD(w)==SB_THUMBTRACK||LOWORD(w)==SB_THUMBPOSITION)){
        SCROLLINFO si{sizeof(si),SIF_TRACKPOS};
        schedule(GetScrollInfo(list,SB_VERT,&si)?si.nTrackPos:int(HIWORD(w)));return 0;
    }
    if(msg==applyScroll){
        int pos=input.target;input.target=-1;input.queued=false;RECT item{};
        if(pos>=0&&ListView_GetItemRect(list,0,&item,LVIR_BOUNDS)){
            auto diff=pos-ListView_GetTopIndex(list);if(diff)ListView_Scroll(list,0,diff*(item.bottom-item.top));
        }
        return 0;
    }
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(list,listInput,id);
    return DefSubclassProc(list,msg,w,l);
}
void initUI(){
    int dpi=GetDpiForWindow(mainWin);font=CreateFontW(-MulDiv(10,dpi,72),0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    titleFont=CreateFontW(-MulDiv(16,dpi,72),0,0,0,FW_BOLD,0,0,0,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,0,L"Microsoft YaHei UI");
    SendMessageW(add(Title,L"STATIC",tr(Text::Title),0),WM_SETFONT,reinterpret_cast<WPARAM>(titleFont),TRUE);
    add(Auto,L"BUTTON",tr(Text::AutoRecord),BS_AUTOCHECKBOX|WS_TABSTOP);
    makeCombo(Language,{L"简体中文",L"English"});SendMessageW(ctl(Language),CB_SETCURSEL,englishUI?1:0,0);
    HWND nav=add(Tabs,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|LBS_OWNERDRAWFIXED|LBS_HASSTRINGS);
    SendMessageW(nav,LB_SETITEMHEIGHT,0,px(38));SetWindowTheme(nav,L"Explorer",nullptr);
    for(auto text:{tr(Text::History),tr(Text::Sessions),tr(Text::Current),tr(Text::Taskbar)})SendMessageW(nav,LB_ADDSTRING,0,(LPARAM)text);
    SendMessageW(nav,LB_SETCURSEL,0,0);
    add(Info,L"STATIC",L"",SS_LEFT);makeCombo(Sessions,{});
    auto edit=add(Search,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,WS_EX_CLIENTEDGE);SendMessageW(edit,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(tr(Text::SearchFolders)));
    makeCombo(Filter,{tr(Text::EveryState),tr(Text::OpenFilter),tr(Text::NotSeen)});makeCombo(Dates,{tr(Text::EveryDate),tr(Text::Today),tr(Text::Week),tr(Text::Month)});
    makeCombo(AppFilter,{tr(Text::EverySource),tr(Text::Interaction),tr(Text::Pin),tr(Text::LaunchRecord),tr(Text::Window)});SendMessageW(ctl(AppFilter),CB_SETDROPPEDWIDTH,px(165),0);
    add(Refresh,L"BUTTON",tr(Text::Refresh),WS_TABSTOP);
    auto list=add(List,WC_LISTVIEWW,L"",LVS_REPORT|LVS_OWNERDATA|LVS_SHOWSELALWAYS|WS_TABSTOP,0);
    static ScrollInput scrollInput;SetWindowSubclass(list,listInput,1,reinterpret_cast<DWORD_PTR>(&scrollInput));
    SetWindowTheme(list,L"Explorer",nullptr);heights=ImageList_Create(1,px(31),ILC_COLOR32,0,1);ListView_SetImageList(list,heights,LVSIL_SMALL);
    ListView_SetExtendedListViewStyle(list,LVS_EX_FULLROWSELECT|LVS_EX_DOUBLEBUFFER|LVS_EX_LABELTIP);
    int i=0,width[]={108,159,208,600};
    for(auto label:{tr(Text::State),tr(Text::Time),tr(Text::Folder),tr(Text::Path)}){LVCOLUMNW col{LVCF_TEXT|LVCF_WIDTH};col.cx=px(width[i]);col.pszText=const_cast<wchar_t*>(label);ListView_InsertColumn(list,i++,&col);}
    add(Restore,L"BUTTON",tr(Text::OpenSelected),WS_TABSTOP);add(Whole,L"BUTTON",tr(Text::RestoreSession),WS_TABSTOP);
    add(Details,L"BUTTON",tr(Text::Details),WS_TABSTOP);
    auto name=add(Name,L"EDIT",L"",ES_AUTOHSCROLL|WS_TABSTOP,WS_EX_CLIENTEDGE);SendMessageW(name,EM_LIMITTEXT,100,0);
    SendMessageW(name,EM_SETCUEBANNER,TRUE,reinterpret_cast<LPARAM>(tr(Text::NamePrompt)));add(Save,L"BUTTON",tr(Text::SaveSession),WS_TABSTOP);add(Foot,L"STATIC",L"",SS_LEFT|SS_ENDELLIPSIS);
    viewMode();layout();
    enqueue([]{try{auto lib=readLibrary();post([lib=std::move(lib)]()mutable{
        auto on=lib.record;Button_SetCheck(ctl(Auto),on?BST_CHECKED:BST_UNCHECKED);applyLib(std::move(lib),false);
        if(on && valid && !live.paths.empty())persist(live,tr(Text::AutoTitle),false);
    });}catch(const std::exception& e){post([msg=errorText(e)]{dbError=tr(Text::DatabaseFailed)+msg;status();});}});
    readHistory();poll();SetTimer(mainWin,1,5000,nullptr);
}
void fill(HDC dc,const RECT& rect,COLORREF color){SetDCBrushColor(dc,color);FillRect(dc,&rect,(HBRUSH)GetStockObject(DC_BRUSH));}
// Cache only a few viewports. Scrolling reuses overlapping rows; sorting reuses the
// same source records. A new data generation, column geometry or size invalidates tiles.
struct RowTiles {
    HDC dc=nullptr; HBITMAP bitmap=nullptr; HGDIOBJ original=nullptr;
    int width=0,height=0,capacity=0,cursor=0,pad=0,bar=0;
    unsigned long long revision=0;
    RECT columns[4]{};
    std::vector<size_t> owners;
    std::vector<UINT> styles;
    std::map<size_t,int> slots;
    void release(){
        if(dc&&original)SelectObject(dc,original);
        if(bitmap)DeleteObject(bitmap);
        if(dc)DeleteDC(dc);
        dc=nullptr;bitmap=nullptr;original=nullptr;
    }
    ~RowTiles(){release();}
    void configure(HWND list,HDC dest){
        RECT client{},row{},cols[4]{};GetClientRect(list,&client);
        if(!ListView_GetItemRect(list,0,&row,LVIR_BOUNDS))return;
        int w=client.right,h=row.bottom-row.top;
        if(w<=0||h<=0)return;
        auto header=ListView_GetHeader(list);
        for(int c=0;c<4;c++){Header_GetItemRect(header,c,cols+c);MapWindowPoints(header,list,(POINT*)(cols+c),2);}
        int cap=int(std::min<long long>(std::max(16,(int(client.bottom)/h+2)*4),16LL*1024*1024/(4LL*w*h)));
        if(!dc||width!=w||height!=h||capacity!=cap){
            release();width=w;height=h;capacity=cap;revision=0;
            if(cap<1)return;
            dc=CreateCompatibleDC(dest);bitmap=CreateCompatibleBitmap(dest,w,h*cap);
            if(!dc||!bitmap){release();return;}
            original=SelectObject(dc,bitmap);SelectObject(dc,font);SetBkMode(dc,TRANSPARENT);
            owners.assign(cap,SIZE_MAX);styles.assign(cap,0);pad=px(7);bar=px(3);
        }
        if(revision!=displayRevision||memcmp(columns,cols,sizeof(cols))!=0){
            memcpy(columns,cols,sizeof(cols));revision=displayRevision;cursor=0;slots.clear();
            std::fill(owners.begin(),owners.end(),SIZE_MAX);
        }
    }
    void paint(int i,const DisplayRow& data,UINT bits){
        const COLORREF base[]={RGB(232,246,238),RGB(244,246,249),RGB(255,245,221)};
        const COLORREF active[]={RGB(190,229,205),RGB(217,227,239),RGB(246,223,174)};
        const COLORREF labels[]={RGB(19,103,61),RGB(75,89,107),RGB(126,82,11)};
        const bool selected=(bits&LVIS_SELECTED)!=0;
        RECT r{0,i*height,width,(i+1)*height};fill(dc,r,selected?active[data.state]:base[data.state]);
        RECT line=r;line.top=line.bottom-1;fill(dc,line,RGB(255,255,255));
        if(selected){RECT strip=r;strip.right=bar;fill(dc,strip,accent);}
        for(int c=0;c<4;c++){
            RECT box{columns[c].left+pad,r.top,columns[c].right-pad,r.bottom};
            if(box.right<=0||box.left>=width||box.right<=box.left)continue;
            SetTextColor(dc,c==0?labels[data.state]:c==1?muted:ink);
            DrawTextW(dc,data.text[c].c_str(),int(data.text[c].size()),&box,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
        if(bits&LVIS_FOCUSED){InflateRect(&r,-2,-2);DrawFocusRect(dc,&r);}
    }
    int tile(size_t key,UINT bits){
        auto it=slots.find(key);int slot;
        if(it!=slots.end()){slot=it->second;if(styles[slot]==bits)return slot;}
        else{
            slot=cursor++%capacity;slots.erase(owners[slot]);owners[slot]=key;slots[key]=slot;
        }
        styles[slot]=bits;paint(slot,display[key],bits);return slot;
    }
};
LRESULT drawList(NMLVCUSTOMDRAW* d){
    static RowTiles tiles;
    if(contrast)return CDRF_DODEFAULT;
    HWND list=d->nmcd.hdr.hwndFrom;HDC dest=d->nmcd.hdc;
    if(d->nmcd.dwDrawStage==CDDS_PREPAINT){
        tiles.configure(list,dest);return tiles.dc?CDRF_NOTIFYITEMDRAW:CDRF_DODEFAULT;
    }
    auto i=size_t(d->nmcd.dwItemSpec);
    if(d->nmcd.dwDrawStage!=CDDS_ITEMPREPAINT||!tiles.dc||i>=matches.size()||matches[i]>=display.size())return CDRF_DODEFAULT;
    UINT bits=ListView_GetItemState(list,int(i),LVIS_SELECTED|LVIS_FOCUSED);
    if(GetFocus()!=list)bits&=~LVIS_FOCUSED;
    RECT r{};ListView_GetItemRect(list,int(i),&r,LVIR_BOUNDS);
    if(!RectVisible(dest,&r))return CDRF_SKIPDEFAULT;
    int slot=tiles.tile(matches[i],bits);
    BitBlt(dest,0,r.top,tiles.width,tiles.height,tiles.dc,0,slot*tiles.height,SRCCOPY);
    return CDRF_SKIPDEFAULT;
}
void drawNavigation(const DRAWITEMSTRUCT& d){
    if(d.itemID==UINT(-1))return;
    const bool selected=(d.itemState&ODS_SELECTED)!=0;
    const COLORREF bg=contrast?GetSysColor(selected?COLOR_HIGHLIGHT:COLOR_WINDOW):selected?RGB(223,235,241):RGB(245,247,249);
    const COLORREF fg=contrast?GetSysColor(selected?COLOR_HIGHLIGHTTEXT:COLOR_WINDOWTEXT):selected?accent:ink;
    RECT r=d.rcItem;fill(d.hDC,r,bg);
    if(selected&&!contrast){RECT marker=r;marker.right=marker.left+px(3);marker.top+=px(10);marker.bottom-=px(10);fill(d.hDC,marker,accent);}
    wchar_t label[128]{};SendMessageW(d.hwndItem,LB_GETTEXT,d.itemID,(LPARAM)label);
    int saved=SaveDC(d.hDC);SetBkMode(d.hDC,TRANSPARENT);SetTextColor(d.hDC,fg);SelectObject(d.hDC,font);
    r.left+=px(12);DrawTextW(d.hDC,label,-1,&r,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
    if((d.itemState&ODS_FOCUS)&&!(d.itemState&ODS_NOFOCUSRECT)){RECT f=d.rcItem;InflateRect(&f,-2,-2);DrawFocusRect(d.hDC,&f);}
    RestoreDC(d.hDC,saved);
}
LRESULT CALLBACK wndProc(HWND h,UINT m,WPARAM w,LPARAM l){
    switch(m){
    case WM_CREATE:mainWin=h;initUI();return 0;
    case WM_ERASEBKGND:if(!contrast){RECT r{};GetClientRect(h,&r);fill((HDC)w,r,background);r.right=px(150);fill((HDC)w,r,RGB(245,247,249));return 1;}break;
    case WM_CTLCOLORLISTBOX:if(!contrast&&(HWND)l==ctl(Tabs)){static HBRUSH rail=CreateSolidBrush(RGB(245,247,249));return (LRESULT)rail;}break;
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize={px(870),px(510)};return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:if(!contrast){SetTextColor((HDC)w,GetDlgCtrlID((HWND)l)==Title?ink:muted);SetBkColor((HDC)w,background);return (LRESULT)canvas;}break;
    case WM_DRAWITEM:{auto d=reinterpret_cast<DRAWITEMSTRUCT*>(l);if(d->CtlID==Tabs){drawNavigation(*d);return TRUE;}break;}
    case WM_SIZE:layout();return 0;
    case WM_COMMAND:{
        int id=LOWORD(w),event=HIWORD(w);
        if(id==Tabs && event==LBN_SELCHANGE){viewMode();return 0;}
        if(id==Language && event==CBN_SELCHANGE){
            englishUI=sel(Language)==1;
            bool saved=remember(L"Language",englishUI?1:0);
            refreshLanguage();if(!saved){notice=tr(Text::PreferencesUnsaved);status();}
            return 0;
        }
        if(id==Search && event==EN_CHANGE){SetTimer(h,2,90,nullptr);return 0;}
        if(event==CBN_SELCHANGE&&(id==Filter||id==Dates||id==Sessions||id==AppFilter)){if(id==Sessions)pickSession();else rebuild();return 0;}
        if(event!=BN_CLICKED)break;
        if(id==Save){pendingSave=true;pendingName=getText(Name);if(pendingName.empty())pendingName=tr(Text::Manual);
            notice=tr(Text::Saving);status();poll();}
        if(id==Restore||id==Whole)beginRestore(id==Whole);
        if(id==Details)showDetails();
        if(id==Refresh){if(mode()==3)readApps();else{if(mode()==0)readHistory();poll();}}
        if(id==Auto){bool on=Button_GetCheck(ctl(Auto))==BST_CHECKED;
            enqueue([on]{try{setRecording(on);}catch(const std::exception& e){post([on,msg=errorText(e)]{Button_SetCheck(ctl(Auto),on?BST_UNCHECKED:BST_CHECKED);dbError=tr(Text::SettingsFailed)+msg;status();});}});
            if(on && valid && !live.paths.empty())persist(live,tr(Text::AutoTitle),false);}
        return 0;}
    case WM_NOTIFY:{
        auto hdr=reinterpret_cast<NMHDR*>(l);
        if(hdr->idFrom!=List)break;
        if(hdr->code==LVN_COLUMNCLICK){
            int col=reinterpret_cast<NMLISTVIEW*>(l)->iSubItem;if(col<0||col>3)return 0;
            auto& order=orders[mode()];order.descending=col==order.column?!order.descending:col==1;order.column=col;
            bool saved=remember((L"Sort"+std::to_wstring(mode())).c_str(),DWORD(col*2+order.descending));
            if(!saved)notice=tr(Text::PreferencesUnsaved);
            sortHeader();rebuild(true);return 0;
        }
        if(hdr->code==NM_CUSTOMDRAW)return drawList(reinterpret_cast<NMLVCUSTOMDRAW*>(l));
        if(hdr->code==LVN_GETDISPINFOW){auto d=reinterpret_cast<NMLVDISPINFOW*>(l);auto& a=d->item;
            if(a.iItem<0||size_t(a.iItem)>=matches.size()||!(a.mask&LVIF_TEXT))return 0;
            const Str& v=cell(matches[a.iItem],a.iSubItem);
            if(a.pszText && a.cchTextMax>0)wcsncpy_s(a.pszText,a.cchTextMax,v.c_str(),_TRUNCATE);return 0;}
        if(hdr->code==LVN_ITEMCHANGED||hdr->code==LVN_ODSTATECHANGED){status();return 0;}
        if(hdr->code==NM_DBLCLK||hdr->code==NM_RETURN){if(mode()==3)showDetails();else beginRestore(false);return 0;}break;}
    case WM_TIMER:if(w==1)poll();if(w==2){KillTimer(h,2);rebuild();}if(w==3 && dbTasks==0){KillTimer(h,3);DestroyWindow(h);}return 0;
    case deliver:{std::unique_ptr<std::function<void()>> action(reinterpret_cast<std::function<void()>*>(l));(*action)();return 0;}
    case WM_CLOSE:KillTimer(h,1);if(dbTasks>0||writing){ShowWindow(h,SW_HIDE);SetTimer(h,3,100,nullptr);}else DestroyWindow(h);return 0;
    case WM_DESTROY:PostQuitMessage(0);return 0;
    }
    return DefWindowProcW(h,m,w,l);
}
}
int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,wchar_t*,int){
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if(argc==3 && wcscmp(argv[1],L"--probe")==0){auto mapping=reinterpret_cast<HANDLE>(_wcstoui64(argv[2],nullptr,10));LocalFree(argv);return probeChild(mapping);}LocalFree(argv);
    HANDLE mutex=CreateMutexW(nullptr,FALSE,L"Local\\ExplorerFolderSessions.Native.v1");
    if(mutex && GetLastError()==ERROR_ALREADY_EXISTS){auto old=FindWindowW(L"ExplorerSessionsNative",nullptr);if(old){ShowWindow(old,SW_RESTORE);SetForegroundWindow(old);}CloseHandle(mutex);return 0;}
    DWORD language=preference(L"Language",2);
    englishUI=language<2?language==1:PRIMARYLANGID(GetUserDefaultUILanguage())!=LANG_CHINESE;
    for(int i=0;i<4;++i){DWORD value=preference((L"Sort"+std::to_wstring(i)).c_str(),99);
        if(value<8)orders[i]={int(value/2),bool(value%2)};}
    SetProcessDPIAware();INITCOMMONCONTROLSEX common{sizeof(common),ICC_LISTVIEW_CLASSES|ICC_STANDARD_CLASSES};InitCommonControlsEx(&common);
    WNDCLASSW wc{};wc.hInstance=inst;wc.lpszClassName=L"ExplorerSessionsNative";wc.lpfnWndProc=wndProc;wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    HIGHCONTRASTW hc{sizeof(hc)};SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);contrast=(hc.dwFlags&HCF_HIGHCONTRASTON)!=0;
    canvas=contrast?GetSysColorBrush(COLOR_BTNFACE):CreateSolidBrush(background);wc.hbrBackground=canvas;
    SHSTOCKICONINFO icon{sizeof(icon)};if(SUCCEEDED(SHGetStockIconInfo(SIID_FOLDEROPEN,SHGSI_ICON|SHGSI_LARGEICON,&icon)))wc.hIcon=icon.hIcon;RegisterClassW(&wc);
    std::thread dbThread(dbLoop);
    HWND h=CreateWindowExW(0,wc.lpszClassName,tr(Text::Title),WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,px(1050),px(655),nullptr,nullptr,inst,nullptr);
    if(h){ShowWindow(h,SW_SHOW);UpdateWindow(h);MSG msg{};while(GetMessageW(&msg,nullptr,0,0)>0){if(!IsDialogMessageW(h,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}}
    {std::lock_guard lock(queueMutex);shuttingDown=true;}queueReady.notify_one();dbThread.join();
    // Other workers may still be waiting on a filesystem or an unresponsive shell namespace.
    ExitProcess(h?0:1);
}
