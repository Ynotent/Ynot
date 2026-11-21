// Menus for quantize presets and time signatures, shared by the toolbar, the
// phone "More" menu and the clip context menu, so every entry point matches.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

#include "ynot/quantize.hpp"

namespace ynotapp {

// Presets grouped by family. `ticked` marks the current choice; `offItem` adds "Off".
inline juce::PopupMenu quantizePresetMenu(const std::string& ticked,
                                          std::function<void(const std::string& presetId)> onPick,
                                          bool offItem) {
    juce::PopupMenu m;
    if (offItem) {
        m.addItem("Off", true, ticked.empty(), [onPick] { onPick({}); });
        m.addSeparator();
    }
    struct Group {
        const char* title;
        const char* prefix;
    };
    const Group groups[] = {{"Straight", "straight-"},
                            {"Triplet", "triplet-"},
                            {"Sampler swing (MPC-style)", "mpc-"},
                            {"Drum machine shuffle", "analog-"},
                            {"Vintage drum machines", "vintage-"},
                            {"Vintage drum machines", "early-"}};
    juce::String lastTitle;
    for (const auto& grp : groups) {
        if (lastTitle != grp.title) {
            m.addSectionHeader(grp.title);
            lastTitle = grp.title;
        }
        for (const auto& p : ynot::quantizePresets())
            if (juce::String(p.id).startsWith(grp.prefix)) {
                juce::PopupMenu::Item item(p.name);
                item.setTicked(p.id == ticked);
                item.setAction([onPick, id = p.id] { onPick(id); });
                m.addItem(item);
            }
    }
    return m;
}

inline juce::PopupMenu strengthMenu(double current, std::function<void(double)> onPick) {
    juce::PopupMenu m;
    for (int pct : {100, 75, 50, 25})
        m.addItem(juce::String(pct) + "%", true, std::abs(current * 100.0 - pct) < 0.5,
                  [onPick, pct] { onPick(pct / 100.0); });
    return m;
}

inline juce::PopupMenu timeSignatureMenu(int num, int den, std::function<void(int, int)> onPick) {
    juce::PopupMenu m;
    for (auto [n, d] : ynot::commonTimeSignatures())
        m.addItem(juce::String(n) + "/" + juce::String(d), true, n == num && d == den,
                  [onPick, n = n, d = d] { onPick(n, d); });
    return m;
}

}  // namespace ynotapp
