// Responsive layout rules shared by every UI shell.
//
// The shells render natively, but *which* workspace layout, control sizes and
// navigation pattern to use is decided here, so an iPad, an Android tablet and
// a small Windows laptop make the same decision for the same window.
#pragma once

#include <string>
#include <vector>

#include "ynot/platform.hpp"

namespace ynot {

enum class LayoutClass {
    Compact,   // phones, portrait: one workspace at a time, bottom tab navigation
    Medium,    // phones landscape / small tablets: arrange + sliding mixer/editor sheet
    Expanded,  // tablets / small laptop windows: arrange + docked side panel
    Studio,    // desktop: multi-pane, detachable windows, dense mixer
};

enum class Navigation { BottomTabs, NavigationRail, Sidebar, MenuBarAndSidebar };

struct LayoutInput {
    Platform platform;
    double widthDp;   // logical points/dp, not pixels
    double heightDp;
    bool hasPrecisePointer;  // mouse/trackpad attached (incl. iPad + trackpad)
    bool hasHardwareKeyboard;
};

struct LayoutDecision {
    LayoutClass layout;
    Navigation navigation;
    double minTouchTargetDp;  // 44 on Apple, 48 on Android (platform HIG), 24 with precise pointer
    int visibleMixerStrips;
    bool showKeyboardShortcutHints;
    bool showPerTrackInspector;
    std::vector<std::string> primaryWorkspaces;  // ordered for the navigation control
};

LayoutDecision decideLayout(const LayoutInput& in);
std::string toString(LayoutClass c);

}  // namespace ynot
