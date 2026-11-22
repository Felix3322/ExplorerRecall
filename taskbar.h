// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "model.h"
enum class AppSource { Interaction=1, Pin, Launch, Window };
struct AppRecord {
    AppSource source{};
    Str name, id, path, summary, details, appId, search;
    Stamp time=0;
    uint64_t interactions=0;
    int state=2; // 0 = observed window, 1 = no matching window, 2 = insufficient identity
};
struct AppReport {
    std::vector<AppRecord> rows;
    Str note;
};
AppReport taskbarRecords();
Str recentForApp(const Str& appId);
const wchar_t* sourceName(AppSource);
