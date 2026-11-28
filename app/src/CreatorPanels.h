// Windows for working with other people's music the right way:
//   * ClearancePanel: every outside sample in the song, who owns it, and its clearance status;
//     exports a clearance sheet to send to labels and publishers.
//   * InspirePanel: original ideas written in the style of a reference song (tempo, key and
//     feel), checked so the melody is clearly different. The reference audio isn't kept.
//   * SoundPacksPanel: Ynot's own royalty-free one-shots, breaks and loops.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class ClearancePanel : public juce::Component, private juce::ListBoxModel {
public:
    ClearancePanel(Session& session, const Theme& theme);
    void resized() override;
    void paint(juce::Graphics& g) override;
    static void show(Session& session, const Theme& theme);

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void selectedRowsChanged(int row) override;
    void reload();
    void saveDetails();

    Session& session_;
    const Theme& theme_;
    std::vector<ynot::SampleUse> uses_;
    juce::ListBox list_;
    juce::Label summary_, hint_;
    juce::ComboBox status_;
    struct Field {
        juce::Label label;
        juce::TextEditor edit;
    };
    std::array<Field, 7> fields_;  // title, artist, master owner, publisher, source time, licence, notes
    juce::TextButton save_{"Save details"}, export_{"Export clearance sheet (CSV)..."}, close_{"Close"};
    std::unique_ptr<juce::FileChooser> chooser_;
    int selected_ = -1;
};

class InspirePanel : public juce::Component {
public:
    InspirePanel(Session& session, const Theme& theme, ynot::ReferenceProfile ref, std::string referenceName);
    ~InspirePanel() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    // Pick a reference song, analyse it in the background, then open the window.
    static void chooseReferenceAndShow(Session& session, const Theme& theme);

private:
    void generate(uint32_t seed);

    Session& session_;
    const Theme& theme_;
    ynot::ReferenceProfile ref_;
    std::string refName_;
    ynot::InspiredIdea idea_;
    juce::ComboBox bars_;
    juce::ToggleButton useTempo_{"Set the song to this tempo"};
    juce::TextButton preview_{"Preview"}, another_{"Try another idea"}, addTracks_{"Add as tracks (editable)"},
        toSampler_{"Make it a sample (Sampler)"}, close_{"Close"};
    juce::Label status_;
};

class SoundPacksPanel : public juce::Component, private juce::ListBoxModel {
public:
    SoundPacksPanel(Session& session, const Theme& theme);
    ~SoundPacksPanel() override;
    void resized() override;
    void paint(juce::Graphics& g) override;
    static void show(Session& session, const Theme& theme, std::function<void(const std::string& samplerTrackId)> onSampler = {});
    std::function<void(const std::string&)> onSampler;

private:
    int getNumRows() override;
    void paintListBoxItem(int row, juce::Graphics& g, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked(int row, const juce::MouseEvent&) override;
    const ynot::PackSound* selected() const;
    void preview();

    Session& session_;
    const Theme& theme_;
    juce::ListBox list_;
    juce::TextButton preview_{"Preview"}, stop_{"Stop"}, toSampler_{"Add to Sampler"}, asClip_{"Add as audio clip"},
        close_{"Close"};
    juce::Label hint_, status_;
};

}  // namespace ynotapp
