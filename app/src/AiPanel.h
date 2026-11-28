// Ynot AI: the song's key and chords, and the clip tools (audio to MIDI, tempo, send to
// sampler). Everything runs on this device; results are suggestions the user applies.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class AiResultsPanel : public juce::Component {
public:
    AiResultsPanel(Session& session, const Theme& theme, Session::SongAnalysis result);
    void resized() override;
    void paint(juce::Graphics& g) override;

    // Analyses the song in the background, then opens the results window.
    static void detectKeyAndChords(Session& session, const Theme& theme);

private:
    Session& session_;
    const Theme& theme_;
    Session::SongAnalysis result_;
    juce::TextButton showChords_{"Show chords on the timeline"}, applyTune_, close_{"Close"};
    juce::Rectangle<int> chordArea_;
};

// Clip tools (audio clips). Results and problems are reported to the user.
void aiAudioToMidi(Session& session, const std::string& trackId, const std::string& clipId);
void aiDetectTempo(Session& session, const std::string& trackId, const std::string& clipId);

}  // namespace ynotapp
