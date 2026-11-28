// The big position counter in the transport bar: bars | beats | ticks above minutes:seconds,
// with the tempo and time signature at the playhead. Click it to switch which is large.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "QuantizeMenus.h"
#include "Session.h"
#include "Theme.h"

namespace ynotapp {

class CounterDisplay : public juce::Component, private juce::Timer {
public:
    CounterDisplay(Session& session, const Theme& theme) : session_(session), theme_(theme) { startTimerHz(30); }
    ~CounterDisplay() override { stopTimer(); }

    void mouseUp(const juce::MouseEvent&) override {
        barsFirst_ = !barsFirst_;
        repaint();
    }

    void paint(juce::Graphics& g) override {
        auto r = getLocalBounds().toFloat();
        g.setColour(theme_.background);
        g.fillRoundedRectangle(r, 6.f);
        g.setColour(session_.isRecording() ? theme_.record : theme_.border);
        g.drawRoundedRectangle(r.reduced(0.5f), 6.f, 1.f);
        if (!session_.hasProject()) return;
        const auto& p = session_.project();
        const int64_t tick = std::max<int64_t>(0, session_.positionTick());
        const int64_t bar = tick / p.barTicks() + 1, beat = (tick % p.barTicks()) / p.beatTicks() + 1,
                      sub = tick % p.beatTicks();
        const juce::String bars = juce::String(bar).paddedLeft(' ', 3) + " | " + juce::String(beat) + " | " +
                                  juce::String(sub).paddedLeft('0', 3);
        const double secs = std::max(0.0, session_.positionSeconds());
        const int mins = static_cast<int>(secs / 60.0);
        const juce::String time = juce::String(mins) + ":" + juce::String(secs - mins * 60.0, 3).paddedLeft('0', 6);
        auto inner = getLocalBounds().reduced(10, 4);
        auto right = inner.removeFromRight(70);
        g.setColour(theme_.text);
        g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), static_cast<float>(inner.getHeight()) * 0.52f,
                                    juce::Font::bold));
        g.drawText(barsFirst_ ? bars : time, inner.removeFromTop(inner.getHeight() * 3 / 5), juce::Justification::centredLeft);
        g.setColour(theme_.textMuted);
        g.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), static_cast<float>(inner.getHeight()) * 0.75f,
                                    juce::Font::plain));
        g.drawText(barsFirst_ ? time : bars, inner, juce::Justification::centredLeft);
        g.setFont(juce::FontOptions(theme_.bodySize - 1.f));
        g.drawText(bpmText(p.tempoAt(tick)) + " BPM", right.removeFromTop(right.getHeight() / 2), juce::Justification::centredRight);
        g.drawText(juce::String(p.timeSigNumerator) + "/" + juce::String(p.timeSigDenominator), right,
                   juce::Justification::centredRight);
    }

private:
    void timerCallback() override {
        const int64_t t = session_.positionTick();
        if (t != lastTick_ || session_.isRecording() != lastRec_) {
            lastTick_ = t;
            lastRec_ = session_.isRecording();
            repaint();
        }
    }
    Session& session_;
    const Theme& theme_;
    bool barsFirst_ = true, lastRec_ = false;
    int64_t lastTick_ = -1;
};

}  // namespace ynotapp
