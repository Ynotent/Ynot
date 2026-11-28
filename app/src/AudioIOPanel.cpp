#include "AudioIOPanel.h"

#include <cmath>

namespace ynotapp {

juce::String AudioIOPanel::bufferLabel(int frames, double sampleRate) {
    const double ms = sampleRate > 0 ? 1000.0 * frames / sampleRate : 0.0;
    const auto msText = ms < 10 ? juce::String(ms, 1) : juce::String(juce::roundToInt(ms));
    return juce::String(frames) + " samples (" + msText + " ms)";
}

AudioIOPanel::AudioIOPanel(Session& session, const Theme& theme) : session_(session), theme_(theme) {
    for (auto* l : {&rateLabel_, &bufferLabel_, &outputLabel_, &inputsTitle_}) {
        l->setColour(juce::Label::textColourId, theme.textMuted);
        addAndMakeVisible(*l);
    }
    deviceLabel_.setFont(juce::FontOptions(theme.bodySize + 1.f, juce::Font::bold));
    latency_.setColour(juce::Label::textColourId, theme.textMuted);
    latency_.setFont(juce::FontOptions(theme.bodySize - 1.f));
    for (juce::Component* c :
         std::initializer_list<juce::Component*>{&deviceLabel_, &latency_, &rate_, &buffer_, &output_, &changeDevice_})
        addAndMakeVisible(*c);
    rate_.onChange = [this] {
        const int i = rate_.getSelectedId() - 1;
        if (i < 0 || i >= static_cast<int>(rates_.size())) return;
        const auto err = session_.setSampleRate(rates_[static_cast<size_t>(i)]);
        if (err.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Sample rate", err);
        refresh();
    };
    buffer_.onChange = [this] {
        const int i = buffer_.getSelectedId() - 1;
        if (i < 0 || i >= static_cast<int>(buffers_.size())) return;
        const auto err = session_.setBufferSize(buffers_[static_cast<size_t>(i)]);
        if (err.isNotEmpty())
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Buffer size", err);
        refresh();
    };
    output_.onChange = [this] {
        if (output_.getSelectedId() > 0) session_.setMainOutputPair(output_.getSelectedId() - 1);
    };
    changeDevice_.onClick = [this] { showDeviceSelector(); };
    refresh();
    setSize(520, 520);
    startTimerHz(30);
}

AudioIOPanel::~AudioIOPanel() { stopTimer(); }

void AudioIOPanel::refresh() {
    auto* dev = session_.device().manager().getCurrentAudioDevice();
    deviceLabel_.setText(dev ? dev->getTypeName() + ": " + dev->getName() : juce::String("No audio device"),
                         juce::dontSendNotification);
    rates_ = session_.availableSampleRates();
    rate_.clear(juce::dontSendNotification);
    for (size_t i = 0; i < rates_.size(); ++i) {
        rate_.addItem(JuceAudioDevice::rateLabel(rates_[i]), static_cast<int>(i) + 1);
        if (std::abs(rates_[i] - session_.sampleRate()) < 0.5) rate_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    }
    buffers_ = session_.availableBufferSizes();
    buffer_.clear(juce::dontSendNotification);
    for (size_t i = 0; i < buffers_.size(); ++i) {
        buffer_.addItem(bufferLabel(buffers_[i], session_.sampleRate()), static_cast<int>(i) + 1);
        if (buffers_[i] == session_.bufferSize()) buffer_.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
    }
    output_.clear(juce::dontSendNotification);
    for (int p = 0; p < session_.numOutputPairs(); ++p) output_.addItem(session_.outputPairName(p), p + 1);
    output_.setSelectedId(std::min(session_.mainOutputPair(), session_.numOutputPairs() - 1) + 1, juce::dontSendNotification);
    juce::String lat = "Lower buffers feel tighter when playing and monitoring; raise it if you hear clicks or dropouts.";
    if (dev)
        lat = "Round trip " + juce::String(session_.monitoringLatencyMs(), 1) +
              " ms (input, buffer and output, as the driver reports it). " + lat;
    latency_.setText(lat, juce::dontSendNotification);
    inputLevels_.assign(static_cast<size_t>(std::max(0, session_.numInputChannels())), 0.f);
    lastRate_ = session_.sampleRate();
    lastBuffer_ = session_.bufferSize();
    lastDevice_ = dev ? dev->getName() : juce::String();
    resized();
    repaint();
}

void AudioIOPanel::timerCallback() {
    // The device can change from elsewhere (its own settings window, unplugging an interface).
    auto* dev = session_.device().manager().getCurrentAudioDevice();
    const auto devName = dev ? dev->getName() : juce::String();
    if (static_cast<int>(inputLevels_.size()) != session_.numInputChannels() ||
        std::abs(lastRate_ - session_.sampleRate()) > 0.5 || lastBuffer_ != session_.bufferSize() ||
        lastDevice_ != devName) {
        refresh();
        return;
    }
    for (size_t c = 0; c < inputLevels_.size(); ++c)
        inputLevels_[c] = std::max(session_.takeInputPeak(static_cast<int>(c)), inputLevels_[c] * 0.86f);
    repaint(meterArea_);
}

void AudioIOPanel::paint(juce::Graphics& g) {
    g.fillAll(theme_.surface);
    if (inputLevels_.empty()) {
        g.setColour(theme_.textMuted);
        g.drawText("This device has no inputs enabled.", meterArea_, juce::Justification::topLeft);
        return;
    }
    // One row per input: label, level bar (-60..0 dB), and the clip zone.
    const int rowH = std::clamp(meterArea_.getHeight() / static_cast<int>(inputLevels_.size()), 14, 26);
    g.setFont(juce::FontOptions(theme_.bodySize - 1.f));
    for (size_t c = 0; c < inputLevels_.size(); ++c) {
        auto row = juce::Rectangle<int>(meterArea_.getX(), meterArea_.getY() + static_cast<int>(c) * rowH,
                                        meterArea_.getWidth(), rowH - 3);
        if (row.getBottom() > meterArea_.getBottom()) break;
        g.setColour(theme_.text);
        auto name = juce::String("In ") + juce::String(static_cast<int>(c) + 1);
        const auto dev = juce::String(session_.inputChannelName(static_cast<int>(c)));
        if (dev.isNotEmpty() && !(dev.containsIgnoreCase("input") && dev.length() <= 9)) name << "  " << dev;
        g.drawText(name, row.removeFromLeft(170), juce::Justification::centredLeft, true);
        g.setColour(theme_.background);
        g.fillRoundedRectangle(row.toFloat(), 3.f);
        const float v = inputLevels_[c];
        const float db = v > 0 ? 20.f * std::log10(v) : -100.f;
        const float pos = juce::jlimit(0.f, 1.f, (db + 60.f) / 60.f);
        g.setColour(db > -1.f ? theme_.record : db > -12.f ? theme_.accent : theme_.secondary);
        g.fillRoundedRectangle(row.toFloat().withWidth(static_cast<float>(row.getWidth()) * pos), 3.f);
    }
}

void AudioIOPanel::resized() {
    auto r = getLocalBounds().reduced(16);
    auto top = r.removeFromTop(28);
    changeDevice_.setBounds(top.removeFromRight(140));
    deviceLabel_.setBounds(top);
    r.removeFromTop(8);
    auto row = [&](juce::Label& l, juce::Component& c) {
        auto line = r.removeFromTop(30);
        l.setBounds(line.removeFromLeft(110));
        c.setBounds(line);
        r.removeFromTop(6);
    };
    row(rateLabel_, rate_);
    row(bufferLabel_, buffer_);
    row(outputLabel_, output_);
    latency_.setBounds(r.removeFromTop(40));
    r.removeFromTop(6);
    inputsTitle_.setBounds(r.removeFromTop(22));
    meterArea_ = r;
}

void AudioIOPanel::showDeviceSelector() {
    // Every driver this OS offers (CoreAudio, ASIO, WASAPI, iOS, Oboe), with all channels.
    auto* selector = new juce::AudioDeviceSelectorComponent(session_.device().manager(), 0, 64, 2, 64, false, false,
                                                            true, false);
    selector->setSize(560, 480);
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(selector);
    o.dialogTitle = "Audio Device";
    o.dialogBackgroundColour = theme_.surface;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

void AudioIOPanel::show(Session& session, const Theme& theme) {
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new AudioIOPanel(session, theme));
    o.dialogTitle = "Audio Interface";
    o.dialogBackgroundColour = theme.surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = true;
    o.launchAsync();
}

}  // namespace ynotapp
