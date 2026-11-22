// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <atomic>
#include <windows.h>
enum class Text {
#define UI_TEXT(key, zh, en) key,
#include "strings.inc"
#undef UI_TEXT
    Count
};
inline std::atomic<bool> englishUI{false};
inline const wchar_t* tr(Text key){
    static constexpr const wchar_t* catalog[][2]={
#define UI_TEXT(key, zh, en) {zh,en},
#include "strings.inc"
#undef UI_TEXT
    };
    static_assert(sizeof(catalog)/sizeof(*catalog)==static_cast<size_t>(Text::Count));
    return catalog[static_cast<size_t>(key)][englishUI.load(std::memory_order_relaxed)?1:0];
}
DWORD preference(const wchar_t* name,DWORD fallback);
bool remember(const wchar_t* name,DWORD value);
