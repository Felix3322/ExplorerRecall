# Windows 能留下哪些会话线索？

本机实测：预先订阅 WinEvent 后，打开一个实际文件夹并正常关闭，对同一 HWND 收到了 CREATE 和 DESTROY。这个实验没有开启常驻窗口监控，下面列的是可用的数据源。

| 来源 | 能知道什么 | 何时可用 |
|---|---|---|
| [WinEvent](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook) 的 [CREATE / DESTROY](https://learn.microsoft.com/en-us/windows/win32/winauto/event-constants) | 窗口生命周期。过滤 OBJID_WINDOW、CHILDID_SELF，再识别 Explorer 顶层窗体。创建时元数据可能还未就绪，销毁时应使用之前缓存的路径。 | 订阅期间；不能补查监听前的关闭事件。 |
| [DShellWindowsEvents](https://learn.microsoft.com/en-us/windows/win32/shell/dshellwindowsevents) | Shell 窗口注册和撤销，可触发重新枚举文件夹。一个注册事件与一个可见标签页之间的关系需要按系统版本验证。 | 订阅期间。 |
| [GetWindowPlacement](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowplacement) 和 [GetWindowDesktopId](https://learn.microsoft.com/en-us/windows/win32/api/shobjidl_core/nf-shobjidl_core-ivirtualdesktopmanager-getwindowdesktopid) | 保存普通位置、显示状态及虚拟桌面 ID，帮助辨认同一路径的不同窗口。 | 窗口尚在时采样。 |
| [SHChangeNotifyRegister](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shchangenotifyregister) | Shell 项目改名、移动和删除等通知，可用来更新已保存的路径。 | 订阅期间，不能作为关窗证据。 |
| [安全事件 4688](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/auditing/event-4688)、[4689](https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/auditing/event-4689) | 进程创建和退出。可对照时间和 PID；同一进程内可有多个窗口。 | 相应审计已启用且日志仍保留时可以回查。 |
| [Jump Lists](https://learn.microsoft.com/en-us/windows/win32/shell/taskbar-extensions) 和本工具已有的 ShellBags | 最近项目、曾浏览的文件夹，可生成恢复候选。 | 痕迹仍在时可以回查，无法给出崩溃时的精确窗口清单。 |
| [Application Recovery and Restart](https://learn.microsoft.com/en-us/windows/win32/recovery/using-application-recovery-and-restart) | 应用注册自己的恢复回调，在支持的故障场景中保存自身状态。 | 依赖目标应用主动支持。 |

适合 ExplorerRecall 的组合是：启动时枚举现存窗口；收到创建通知后补全路径、位置和桌面；收到销毁通知后按缓存写入关闭时间，并定期重新枚举。需要同时保存 PID、HWND 和首次观察时间，避免句柄复用造成误配。工具关闭期间应在时间线上保留空白。
