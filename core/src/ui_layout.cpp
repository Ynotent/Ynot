#include "ynot/ui_layout.hpp"

#include <algorithm>

namespace ynot {

LayoutDecision decideLayout(const LayoutInput& in) {
    LayoutDecision d{};
    const double w = in.widthDp;
    const bool desktop = in.platform == Platform::MacOS || in.platform == Platform::Windows ||
                         in.platform == Platform::Linux;

    if (w < 600) d.layout = LayoutClass::Compact;
    else if (w < 840) d.layout = LayoutClass::Medium;
    else if (w < 1280 || !desktop) d.layout = LayoutClass::Expanded;
    else d.layout = LayoutClass::Studio;
    // A desktop window can be narrow; it still gets desktop navigation chrome.
    if (desktop && d.layout == LayoutClass::Compact) d.layout = LayoutClass::Medium;

    switch (d.layout) {
        case LayoutClass::Compact: d.navigation = Navigation::BottomTabs; break;
        case LayoutClass::Medium: d.navigation = desktop ? Navigation::Sidebar : Navigation::NavigationRail; break;
        case LayoutClass::Expanded: d.navigation = desktop ? Navigation::MenuBarAndSidebar : Navigation::Sidebar; break;
        case LayoutClass::Studio: d.navigation = Navigation::MenuBarAndSidebar; break;
    }

    if (in.hasPrecisePointer) d.minTouchTargetDp = 24;
    else if (in.platform == Platform::Android) d.minTouchTargetDp = 48;
    else d.minTouchTargetDp = 44;

    const double stripWidth = in.hasPrecisePointer ? 72 : 96;
    d.visibleMixerStrips = std::max(1, static_cast<int>(w / stripWidth));
    d.showKeyboardShortcutHints = in.hasHardwareKeyboard;
    d.showPerTrackInspector = d.layout == LayoutClass::Expanded || d.layout == LayoutClass::Studio;

    if (d.layout == LayoutClass::Compact)
        d.primaryWorkspaces = {"record", "arrange", "mix", "library"};
    else
        d.primaryWorkspaces = {"arrange", "mix", "edit", "record", "library"};
    return d;
}

std::string toString(LayoutClass c) {
    switch (c) {
        case LayoutClass::Compact: return "compact";
        case LayoutClass::Medium: return "medium";
        case LayoutClass::Expanded: return "expanded";
        case LayoutClass::Studio: return "studio";
    }
    return "";
}

}  // namespace ynot
