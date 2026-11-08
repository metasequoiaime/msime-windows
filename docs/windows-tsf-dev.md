# Windows TSF 本地开发与验证

本文件补充 `windows/AGENTS.md` 的「构建与验证」：改完 TSF DLL 或 `ui/` 的文本控件后，怎样在真实宿主里快速验证，以及验证时看哪份日志。

## 快速验证 DLL 改动

适用于改了 `windows/src/` 或 `ui/src/tsf/`、需要在真实宿主里看行为的场景。先构建 x64 Release：

```powershell
cmake --build windows/build64-release --config Release
```

Windows 允许重命名正在被加载的 DLL，所以不必关掉宿主，也不必出安装包：

```powershell
$dir = "C:\Windows\System32\IME\MetasequoiaIME\msime_v<版本>"
Rename-Item "$dir\MetasequoiaImeTsf.dll" "MetasequoiaImeTsf.dll.old"
Copy-Item windows\build64-release\Release\MetasequoiaImeTsf.dll $dir\
```

这两步需要管理员权限。之后重启宿主进程（QQ、浏览器、记事本……），只有新启动的进程会加载新 DLL。

回滚：删掉新的 `MetasequoiaImeTsf.dll`，把 `.old` 改回原名，再重启宿主。

这条回路只用于本地自测；安装包、签名和发布走 `installer/` 的流程。

## 诊断日志

日志在 `%LOCALAPPDATA%\metasequoiaime\logs\`，文件名形如 `水杉IME诊断日志-tsf-<时间戳>.log`，Server 每次重启会新建一份。

排按键和提交问题时看这些字段：

| 字段 | 含义 |
|---|---|
| `stage=` | 按键走到了哪一步：`keydown-classified`、`edit-session-complete`、`deferred-replay-complete` 等 |
| `composing=` / `buffer_len=` | 当前是否在组合中、组合里的字符数 |
| `result=` | 该步骤的 HRESULT |
| `uiless=` | 这一拍 TSF 是否认为宿主自己画候选。首键与组合开始时的取值不同，能直接看出 UILess 标志是否慢了一拍 |
| `process=` | 宿主进程名，用来区分 QQ、浏览器、记事本 |
| `[host-window]` | 组合开始与结束、UI element 的创建与销毁 |

「提交 → 结束组合」的先后顺序能区分是 IME 提交成功，还是宿主中途插手。
