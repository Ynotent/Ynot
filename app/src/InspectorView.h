// Track inspector (left of the tracks on larger screens): everything about the focused track
// in one place. Name, input, instrument, effects, volume, pan, mute/solo/record.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ArrangeView.h"
#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class InspectorView : public juce::Component {
public:
    InspectorView(Session& session, ArrangeView& arrange, const Theme& theme);
    void refresh();  // track or project changed
    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    const ynot::Track* track() const;

    Session& session_;
    ArrangeView& arrange_;
    const Theme& theme_;
    std::string trackId_;
    juce::Label name_, kind_, chainTitle_{{}, "Instrument & effects"}, chain_, volLabel_{{}, "Volume"},
        panLabel_{{}, "Pan"}, inputLabel_{{}, "Input"};
    juce::TextButton input_, editChain_{"Instrument & effects..."}, mute_{"M"}, solo_{"S"}, arm_{"R"};
    juce::Slider volume_, pan_;
    bool updating_ = false;
};

}  // namespace ynotapp
