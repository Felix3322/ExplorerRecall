// SPDX-License-Identifier: GPL-3.0-only
#include "ui_text.h"
namespace {constexpr wchar_t location[]=L"Software\\ExplorerFolderSessions\\Interface";}
DWORD preference(const wchar_t* name,DWORD fallback){
    DWORD value=0,size=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,location,name,RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS?value:fallback;
}
bool remember(const wchar_t* name,DWORD value){
    return RegSetKeyValueW(HKEY_CURRENT_USER,location,name,REG_DWORD,&value,sizeof(value))==ERROR_SUCCESS;
}
