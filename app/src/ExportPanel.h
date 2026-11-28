// The Export window: choose format and options, then a save location; renders in the
// background with a progress bar.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "AudioExporter.h"
#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class ExportPanel : public juce::Component {
public:
    ExportPanel(Session& session, const Theme& theme);
    void resized() override;
    void paint(juce::Graphics& g) override;
    ExportSettings settings() const;

    // Shows the panel in a dialog window.
    static void show(Session& session, const Theme& theme);

private:
    void updateQualityChoices();
    void startExport();

    Session& session_;
    const Theme& theme_;
    std::vector<ExportFormat> formats_;
    juce::Label formatLabel_{{}, "Format"}, qualityLabel_{{}, "Quality"}, rateLabel_{{}, "Sample rate"},
        rangeLabel_{{}, "Range"}, note_;
    juce::ComboBox format_, quality_, rate_, range_;
    juce::ToggleButton normalize_{"Normalize (loudest peak at -1 dB)"};
    juce::TextButton export_{"Export..."}, cancel_{"Cancel"}, reviewSamples_{"Review samples..."};
    juce::Label clearance_;
    std::unique_ptr<juce::FileChooser> chooser_;
};

}  // namespace ynotapp
