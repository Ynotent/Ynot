// Controls for Ynot Tune (built-in pitch correction) on one track.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class TunePanel : public juce::Component {
public:
    TunePanel(Session& session, const Theme& theme, std::string trackId, std::string pluginId);
    void resized() override;
    void paint(juce::Graphics& g) override;
    static void show(Session& session, const Theme& theme, const std::string& trackId, const std::string& pluginId);

private:
    double param(const char* key, double def) const;
    void set(const char* key, double value);

    Session& session_;
    const Theme& theme_;
    std::string trackId_, pluginId_;
    juce::Label keyLabel_{{}, "Key"}, scaleLabel_{{}, "Scale"}, speedLabel_{{}, "Retune speed"},
        humanizeLabel_{{}, "Humanize"}, flexLabel_{{}, "Flex-tune"}, mixLabel_{{}, "Mix"}, hint_;
    juce::ComboBox key_, scale_;
    juce::Slider speed_, humanize_, flex_, mix_;
};

}  // namespace ynotapp
