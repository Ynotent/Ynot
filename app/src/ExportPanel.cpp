#include "ExportPanel.h"

namespace ynotapp {

namespace {
const int kBitDepths[] = {16, 24, 32};
const int kBitrates[] = {128, 192, 256, 320};
const double kRates[] = {44100, 48000, 88200, 96000, 192000};

// Renders on a background thread with a progress bar; reports on the message thread.
class ExportThread : public juce::ThreadWithProgressWindow {
public:
    ExportThread(ExportJob job, juce::File file)
        : juce::ThreadWithProgressWindow("Exporting " + file.getFileName(), true, true),
          job_(std::move(job)), file_(std::move(file)) {}

    void run() override {
        error_ = runExport(job_, file_, [this](double p) {
            setProgress(p);
            setStatusMessage(p < 0.9 ? "Rendering..." : "Encoding...");
            return !threadShouldExit();
        });
    }

    void threadComplete(bool cancelled) override {
        const auto file = file_;
        auto error = cancelled ? juce::String("Export cancelled.") : error_;
        std::string warnings;
        for (const auto& i : job_.issues) warnings += "\n- " + i.message;
        juce::MessageManager::callAsync([file, error, warnings, self = this] {
            delete self;  // the progress window has finished with us
            if (error.isNotEmpty()) {
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Export", error);
                return;
            }
            juce::String msg = "Saved " + file.getFileName() + " (" +
                               juce::File::descriptionOfSizeInBytes(file.getSize()) + ").";
            if (!warnings.empty()) msg << "\n\nNote:" << juce::String(warnings);
#if JUCE_MAC || JUCE_WINDOWS || JUCE_LINUX
            juce::AlertWindow::showOkCancelBox(juce::MessageBoxIconType::InfoIcon, "Export finished", msg,
                                               "Show in folder", "OK", nullptr,
                                               juce::ModalCallbackFunction::create([file](int r) {
                                                   if (r == 1) file.revealToUser();
                                               }));
#else
            juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::InfoIcon, "Export finished", msg);
#endif
        });
    }

private:
    ExportJob job_;
    juce::File file_;
    juce::String error_;
};
}  // namespace

ExportPanel::ExportPanel(Session& session, const Theme& theme) : session_(session), theme_(theme) {
    formats_ = availableExportFormats();
    for (size_t i = 0; i < formats_.size(); ++i) format_.addItem(formatName(formats_[i]), static_cast<int>(i) + 1);
    format_.setSelectedId(1, juce::dontSendNotification);
    format_.onChange = [this] { updateQualityChoices(); };
    for (size_t i = 0; i < std::size(kRates); ++i)
        rate_.addItem(JuceAudioDevice::rateLabel(kRates[i]), static_cast<int>(i) + 1);
    int rateId = 2;
    for (size_t i = 0; i < std::size(kRates); ++i)
        if (std::abs(kRates[i] - session_.sampleRate()) < 0.5) rateId = static_cast<int>(i) + 1;
    rate_.setSelectedId(rateId, juce::dontSendNotification);
    rate_.onChange = [this] { updateQualityChoices(); };
    range_.addItem("Whole song", 1);
    range_.addItem("Loop only (exact length, no tail)", 2);
    range_.setItemEnabled(2, session_.hasProject() && session_.project().loop.valid());
    range_.setSelectedId(1, juce::dontSendNotification);
    for (auto* l : {&formatLabel_, &qualityLabel_, &rateLabel_, &rangeLabel_}) {
        l->setColour(juce::Label::textColourId, theme.textMuted);
        addAndMakeVisible(*l);
    }
    note_.setColour(juce::Label::textColourId, theme.textMuted);
    note_.setFont(juce::FontOptions(theme.bodySize - 1.f));
    note_.setJustificationType(juce::Justification::topLeft);
    for (juce::Component* c : std::initializer_list<juce::Component*>{&format_, &quality_, &rate_, &range_, &normalize_,
                                                                       &export_, &cancel_, &note_})
        addAndMakeVisible(*c);
    export_.onClick = [this] { startExport(); };
    cancel_.onClick = [this] {
        if (auto* w = findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(0);
    };
    updateQualityChoices();
    setSize(460, 330);
}

ExportSettings ExportPanel::settings() const {
    ExportSettings s;
    s.format = formats_[static_cast<size_t>(std::max(0, format_.getSelectedId() - 1))];
    const int q = std::max(0, quality_.getSelectedId() - 1);
    if (s.format == ExportFormat::Wav) s.bitDepth = kBitDepths[std::min(q, 2)];
    if (s.format == ExportFormat::Flac) s.bitDepth = kBitDepths[std::min(q, 1)];
    if (isLossy(s.format)) s.bitrateKbps = kBitrates[std::min(q, 3)];
    s.sampleRate = kRates[static_cast<size_t>(std::max(0, rate_.getSelectedId() - 1))];
    s.loopOnly = range_.getSelectedId() == 2;
    s.normalize = normalize_.getToggleState();
    return s;
}

void ExportPanel::updateQualityChoices() {
    const auto f = formats_[static_cast<size_t>(std::max(0, format_.getSelectedId() - 1))];
    quality_.clear(juce::dontSendNotification);
    if (f == ExportFormat::Wav) {
        quality_.addItem("16-bit (CD, dithered)", 1);
        quality_.addItem("24-bit (studio)", 2);
        quality_.addItem("32-bit float (for further mixing)", 3);
        quality_.setSelectedId(2, juce::dontSendNotification);
    } else if (f == ExportFormat::Flac) {
        quality_.addItem("16-bit (CD, dithered)", 1);
        quality_.addItem("24-bit (studio)", 2);
        quality_.setSelectedId(2, juce::dontSendNotification);
    } else {
        for (size_t i = 0; i < std::size(kBitrates); ++i)
            quality_.addItem(juce::String(kBitrates[i]) + " kbps", static_cast<int>(i) + 1);
        quality_.setSelectedId(3, juce::dontSendNotification);
    }
    auto s = settings();
    juce::String note;
    if (isLossy(f) && std::abs(exportRateFor(s) - s.sampleRate) > 0.5)
        note << formatName(f) << " is made at " << JuceAudioDevice::rateLabel(exportRateFor(s)) << ". ";
    if (f == ExportFormat::Mp3 || f == ExportFormat::M4a)
        note << "Good for sharing and streaming. ";
    if (f == ExportFormat::Wav || f == ExportFormat::Flac) note << "Full quality, for mastering or distribution. ";
#if JUCE_MAC || JUCE_IOS
    note << "MP3 isn't offered on Apple devices yet; M4A plays everywhere MP3 does.";
#endif
    note_.setText(note, juce::dontSendNotification);
}

void ExportPanel::startExport() {
    if (!session_.hasProject()) return;
    const auto s = settings();
    const auto name = juce::File::createLegalFileName(juce::String(session_.project().name).isEmpty()
                                                          ? juce::String("Ynot song")
                                                          : juce::String(session_.project().name));
    const auto start = juce::File::getSpecialLocation(juce::File::userMusicDirectory)
                           .getChildFile(name + (s.loopOnly ? " (loop)" : "") + formatExtension(s.format));
    chooser_ = std::make_unique<juce::FileChooser>("Export as " + formatName(s.format), start,
                                                   "*" + formatExtension(s.format));
    auto& session = session_;
    juce::Component::SafePointer<ExportPanel> safe(this);
    chooser_->launchAsync(juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                              juce::FileBrowserComponent::warnAboutOverwriting,
                          [safe, &session, s](const juce::FileChooser& fc) {
                              auto file = fc.getResult();
                              if (file == juce::File()) return;
                              if (file.getFileExtension().isEmpty()) file = file.withFileExtension(formatExtension(s.format));
                              if (safe)
                                  if (auto* w = safe->findParentComponentOfClass<juce::DialogWindow>()) w->exitModalState(1);
                              auto job = session.prepareExport(s);  // plugins load here, on the message thread
                              auto* t = new ExportThread(std::move(job), file);
                              t->launchThread();
                          });
}

void ExportPanel::paint(juce::Graphics& g) { g.fillAll(theme_.surface); }

void ExportPanel::resized() {
    auto r = getLocalBounds().reduced(16);
    auto row = [&](juce::Label& label, juce::Component& c) {
        auto line = r.removeFromTop(30);
        label.setBounds(line.removeFromLeft(110));
        c.setBounds(line);
        r.removeFromTop(8);
    };
    row(formatLabel_, format_);
    row(qualityLabel_, quality_);
    row(rateLabel_, rate_);
    row(rangeLabel_, range_);
    normalize_.setBounds(r.removeFromTop(26));
    auto buttons = r.removeFromBottom(32);
    cancel_.setBounds(buttons.removeFromRight(100));
    buttons.removeFromRight(8);
    export_.setBounds(buttons.removeFromRight(120));
    note_.setBounds(r.reduced(0, 4));
}

void ExportPanel::show(Session& session, const Theme& theme) {
    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned(new ExportPanel(session, theme));
    o.dialogTitle = "Export";
    o.dialogBackgroundColour = theme.surface;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.launchAsync();
}

}  // namespace ynotapp
