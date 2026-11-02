#pragma once

#include <windows.h>

//
// Message define
//
inline UINT WM_SHOW_MAIN_WINDOW;
inline UINT WM_HIDE_MAIN_WINDOW;
inline UINT WM_MOVE_CANDIDATE_WINDOW;
inline const UINT WM_CLS_DICT_CACHE = WM_USER + 100;
inline const UINT WM_DELETE_CANDIDATE = WM_USER + 101;
inline const UINT WM_LANGBAR_RIGHTCLICK = WM_USER + 102;
inline const UINT WM_IMEACTIVATE = WM_USER + 103;
inline const UINT WM_IMEDEACTIVATE = WM_USER + 104;
inline const UINT WM_IMESWITCH = WM_USER + 105;
inline const UINT WM_PUNCSWITCH = WM_USER + 106;
inline const UINT WM_DOUBLESINGLEBYTESWITCH = WM_USER + 107;
inline const UINT UPDATE_FTB_STATUS = WM_USER + 108;
inline const UINT WM_PIN_TO_TOP_CANDIDATE = WM_USER + 109;
inline const UINT WM_COMMIT_CANDIDATE = WM_USER + 110;
inline const UINT WM_CLEAR_IME_ENGINE_CACHE = WM_USER + 111;
inline const UINT WM_FIX_CANDIDATE_POSITION = WM_USER + 113;
inline const UINT WM_CLEAR_CANDIDATE_POSITION = WM_USER + 114;
inline const UINT UPDATE_FTB_ENGLISH_INPUT_MODE = WM_USER + 115;
inline const UINT WM_APPLY_IME_CONFIG = WM_USER + 116;
inline const UINT UPDATE_FTB_CAPS_LOCK = WM_USER + 117;
inline const UINT WM_APPLY_IME_INPUT_SCHEME = WM_USER + 118;
inline const UINT WM_REFRESH_CHARACTER_SET = WM_USER + 119;
inline const UINT WM_PAGE_CANDIDATE = WM_USER + 120;
inline const UINT WM_SHOW_CARET_STATE = WM_USER + 121;
inline const UINT WM_HIDE_CARET_STATE = WM_USER + 122;
inline const UINT WM_MOVE_CARET_STATE = WM_USER + 123;
// 候选框翻页箭头被点击。与 WM_PAGE_CANDIDATE 同参，但不受「鼠标滚轮翻页」开关约束。
inline const UINT WM_PAGE_CANDIDATE_ARROW = WM_USER + 124;
inline const UINT UPDATE_FTB_INPUT_MODE = WM_USER + 125;
// 卷轴展开面板里点选了候选。wParam 是候选的绝对下标，lParam 是 new 出来的 std::wstring*（显示文字），
// 由宿主窗口过程接管并释放。
inline const UINT WM_COMMIT_CANDIDATE_AT = WM_USER + 126;
// 卷轴展开面板打开或滚到底部，请求再展开一批候选。
inline const UINT WM_EXPAND_CANDIDATES = WM_USER + 127;
// 候选窗自己投递给自己：切换卷轴展开面板。放到消息队列里做，免得在鼠标分发途中改场景。
inline const UINT WM_TOGGLE_CANDIDATE_EXPAND = WM_USER + 128;
// 竖排译文面板被点了外面（presenter 的 Popup 关闭回调）：请工作线程关掉面板状态。
inline const UINT WM_CLOSE_TRANSLATION_PANEL = WM_USER + 129;
// 候选词展开面板里点选了某条译义。wParam 是它在面板里的下标（0 起），与数字键 1..N 上屏同一条路径。
inline const UINT WM_COMMIT_EXPANSION_PANEL_ITEM = WM_USER + 130;

// wParam of WM_PAGE_CANDIDATE / WM_PAGE_CANDIDATE_ARROW; lParam carries how many pages to move.
inline constexpr WPARAM CANDIDATE_PAGE_PREVIOUS = 0;
inline constexpr WPARAM CANDIDATE_PAGE_NEXT = 1;
