#pragma once

#include "skin/candidate_skin_catalog.h"

#include <string>
#include <vector>

// 图片模式（[toolbar] mode = "image"）下每个按钮摆在哪、多大、用什么图。悬浮工具栏与设置页的预览共用这一份，
// 两边摆出来的一模一样。坐标与尺寸都是皮肤里的 DIP，还没乘工具栏缩放。
struct ToolbarImageButtonSlot
{
    std::string key;
    double xDip = 0.0;
    double yDip = 0.0;
    double widthDip = 0.0;
    double heightDip = 0.0;
    // 内置图标的字号；0 为按钮框短边的 0.82 倍。
    double iconSizeDip = 0.0;
    // 按钮的底图与当前状态的按钮图，相对皮肤目录；为空时不画底图 / 画内置图标。
    std::string background;
    std::string image;
};

// 要显示的一个按钮：按钮键与它当前的状态名（无状态的按钮为空）。
struct ToolbarImageButtonRequest
{
    std::string key;
    std::string state;
};

// shown 是当前要显示的按钮，按工具栏的顺序：中/英状态总在，其余跟设置页的开关走。皮肤藏起来的按钮
// （visible = false）不摆；写了坐标的固定在那里；其余按 [toolbar.button_flow] 依次排开，皮肤没写
// button_flow 时不摆。宽高、图标字号与底图没写的取 [toolbar.button_defaults]。
std::vector<ToolbarImageButtonSlot> LayoutToolbarImageButtons(const CandidateSkinCatalog::Package &package,
                                                              const std::vector<ToolbarImageButtonRequest> &shown);
