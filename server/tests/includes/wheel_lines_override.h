#pragma once

#include "msimeui/ScrollAxis.h"

// Makes msimeui::SystemWheelScrollLines answer `lines` while it lives, so wheel tests run
// at a chosen "lines to scroll" setting without touching the machine's.
class WheelLinesOverride
{
  public:
    explicit WheelLinesOverride(UINT lines)
        : previousLines_(Lines()), previousSource_(msimeui::SetWheelScrollLinesSource(&Answer))
    {
        Lines() = lines;
    }

    // Gives back the setting it replaced, an enclosing override's included.
    ~WheelLinesOverride()
    {
        Lines() = previousLines_;
        msimeui::SetWheelScrollLinesSource(previousSource_);
    }

    WheelLinesOverride(const WheelLinesOverride &) = delete;
    WheelLinesOverride &operator=(const WheelLinesOverride &) = delete;

  private:
    static UINT &Lines()
    {
        static UINT lines = 3;
        return lines;
    }

    static UINT Answer()
    {
        return Lines();
    }

    UINT previousLines_;
    msimeui::WheelScrollLinesSource previousSource_;
};
