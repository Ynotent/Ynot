// Ynot application entry point (all platforms).
//
//   Ynot                        normal GUI launch
//   Ynot --smoke-test <dir>     headless check of the JUCE <-> core integration (CI)
//   Ynot --open <dir>           open a project at launch (--show-keys: on-screen keyboard open)
//   Ynot --simulate-audio       run the audio callback in real time without hardware (UI tests)
//   Ynot --screenshot <png> <w> <h> [--open <dir>] [--layout-as ios|android|macos|windows]
//                               render the UI at a size and exit (layout checks)
#include <juce_gui_extra/juce_gui_extra.h>

#include <iostream>

#include "MainComponent.h"
#include "AudioImporter.h"
#include "PluginSandbox.h"

namespace ynotapp {

static int runSmokeTest(const juce::File& dir) {
    int failures = 0;
    auto expect = [&](bool ok, const char* what) {
        std::cout << (ok ? "  ok   " : "  FAIL ") << what << std::endl;
        failures += ok ? 0 : 1;
    };
    JucePluginHost host;
    Session session(host);
    dir.deleteRecursively();
    session.newDemo(dir);
    expect(session.project().tracks.size() == 3, "demo project created through the app session");

    auto ios = session.compatibilityFor(ynot::Platform::IOS);
    expect(ios.count(ynot::Severity::Blocking) == 1, "iOS report blocks the AU-only instrument");

    ynot::PackageMediaProvider media(dir.getFullPathName().toStdString());
    auto mix = ynot::renderProject(session.project(), media, host, 48000, 0.5);
    double peak = 0;
    for (float s : mix.samples) peak = std::max(peak, static_cast<double>(std::abs(s)));
    expect(peak > 0.05, "mixdown through JucePluginHost is audible");

    auto devices = session.device().enumerate();
    std::cout << "  info " << devices.size() << " audio device(s) visible to JUCE on this host" << std::endl;
    bool audio = session.startAudio();
    std::cout << "  info live audio " << (audio ? "started" : "unavailable here: " + session.device().lastError())
              << std::endl;
    session.stopAudio();

    // Record 1 s of a 440 Hz tone onto Vocals through the same callback the device uses.
    std::string vocalsId;
    for (auto& t : session.project().tracks)
        if (t.name == "Vocals") vocalsId = t.id;
    session.edit([&](ynot::Project& p) {
        for (auto& t : p.tracks) t.armed = t.id == vocalsId;
    });
    session.simulateInputsForTesting(2);
    {   // Monitoring: input is audible through the track while the transport is stopped.
        session.setMonitoring(true);
        std::vector<float> in(256, 0.25f), silence(256), outL(256), outR(256);
        const float* ins[2] = {in.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        session.processForTesting(ins, 2, outs, 2, 256);
        expect(std::abs(outL[100]) > 0.05f && std::abs(session.positionSeconds()) < 1e-9,
               "armed track monitors its input with the transport stopped");
        session.setMonitoring(false);
    }
    session.setCountInBars(1);  // 1 bar of 4/4 at 120 BPM = 2 s of click before recording
    std::string recError;
    expect(session.startRecording(recError), "recording starts on the armed track");
    if (!recError.empty()) std::cout << "       " << recError << std::endl;
    {
        const int block = 256;
        std::vector<float> in(block), silence(block), outL(block), outR(block);
        const float* ins[2] = {in.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        for (int b = 0; b < 3 * 48000 / block; ++b) {  // 2 s count-in + 1 s take
            for (int i = 0; i < block; ++i)
                in[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f *
                                                             static_cast<float>(b * block + i) / 48000.0f);
            session.processForTesting(ins, 2, outs, 2, block);
        }
    }
    const size_t clipsBefore = session.project().findTrack(vocalsId)->audioClips.size();
    auto rec = session.stopRecording();
    const auto* vox = session.project().findTrack(vocalsId);
    expect(rec.newClips.size() == 1 && rec.droppedFrames == 0, "take committed with no dropped audio");
    expect(vox->audioClips.size() >= clipsBefore, "take placed on the Vocals track");
    bool takeOk = false;
    for (auto& c : vox->audioClips)
        if (!rec.newClips.empty() && c.id == rec.newClips[0])
            if (auto* m = session.project().findMedia(c.mediaId))
                takeOk = m->origin == "recording" && m->frames > 47000 && m->frames < 49000 && c.startTick == 0 &&
                         dir.getChildFile(m->fileName).existsAsFile();
    expect(takeOk, "count-in not recorded; take starts at the record point and is stored as media");

    {   // MIDI: play the on-screen keyboard into the armed Keys track while recording.
        std::string keysId;
        for (auto& t : session.project().tracks)
            if (t.name == "Keys") keysId = t.id;
        session.edit([&](ynot::Project& p) {
            for (auto& t : p.tracks) t.armed = t.id == keysId;
        });
        const size_t before = session.project().findTrack(keysId)->midiClips.size();
        session.setCountInBars(0);
        std::string err;
        expect(session.startRecording(err), "MIDI recording starts on the armed instrument track");
        const int block = 256;
        std::vector<float> silence(block), outL(block), outR(block);
        const float* ins[2] = {silence.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        float livePeak = 0;
        for (int b = 0; b < 48000 / block; ++b) {
            if (b == 20) session.keyboardState().noteOn(1, 64, 0.8f);
            if (b == 120) session.keyboardState().noteOff(1, 64, 0.f);
            session.processForTesting(ins, 2, outs, 2, block);
            if (b > 25 && b < 115)
                for (float v : outL) livePeak = std::max(livePeak, std::abs(v));
            juce::Thread::sleep(1);  // let the collector see real time pass, as a device would
        }
        expect(livePeak > 0.05f, "on-screen keyboard plays the armed instrument live");
        auto res = session.stopRecording();
        const auto* keys = session.project().findTrack(keysId);
        bool noteOk = false;
        if (keys->midiClips.size() == before + 1)
            for (auto& n : keys->midiClips.back().notes) noteOk |= n.pitch == 64 && n.lengthTick > 0;
        expect(res.newClips.size() == 1 && noteOk, "MIDI take committed with the played note");
    }

    {   // Punch + quantize: only the punch range is recorded and replaced, notes land on the grid.
        std::string keysId;
        for (auto& t : session.project().tracks)
            if (t.name == "Keys") keysId = t.id;
        const int64_t bar = session.project().barTicks();
        auto notesAt = [&](int64_t from, int64_t to) {
            int n = 0;
            for (auto& c : session.project().findTrack(keysId)->midiClips)
                for (auto& note : c.notes)
                    if (note.tick >= 0 && note.tick < c.lengthTick && c.startTick + note.tick >= from &&
                        c.startTick + note.tick < to)
                        ++n;
            return n;
        };
        const int bar1Before = notesAt(0, bar);
        session.edit([&](ynot::Project& p) { for (auto& t : p.tracks) t.armed = t.id == keysId; });
        session.setCountInBars(0);
        session.setPunchRange(bar, 2 * bar);  // bar 2
        session.setPunchEnabled(true);
        session.setRecordQuantize("straight-1-16");
        session.seekToTick(5 * bar);          // punch ignores the playhead
        std::string err;
        expect(session.startRecording(err), "punch recording starts (pre-roll from bar 1)");
        const int block = 256;  // bar = 96000 frames at 120 BPM: punch-in at block 375, out at 750
        std::vector<float> silence(block), outL(block), outR(block);
        const float* ins[2] = {silence.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        for (int b = 0; b < 850; ++b) {
            if (b == 100) session.keyboardState().noteOn(1, 50, 0.8f);   // pre-roll: ignored
            if (b == 140) session.keyboardState().noteOff(1, 50, 0.f);
            if (b == 452) session.keyboardState().noteOn(1, 67, 0.8f);   // inside the punch: kept
            if (b == 520) session.keyboardState().noteOff(1, 67, 0.f);
            if (b == 790) session.keyboardState().noteOn(1, 72, 0.8f);   // after punch-out: ignored
            if (b == 820) session.keyboardState().noteOff(1, 72, 0.f);
            session.processForTesting(ins, 2, outs, 2, block);
            juce::Thread::sleep(1);
        }
        auto res = session.stopRecording();
        session.setPunchEnabled(false);
        session.setRecordQuantize({});
        bool onlyInside = res.newClips.size() == 1, onGrid = false;
        for (auto& c : session.project().findTrack(keysId)->midiClips)
            if (!res.newClips.empty() && c.id == res.newClips[0]) {
                onlyInside &= c.notes.size() == 1 && c.notes[0].pitch == 67;
                onGrid = !c.notes.empty() && (c.startTick + c.notes[0].tick) % (ynot::kTicksPerQuarter / 4) == 0;
            }
        expect(onlyInside, "punch keeps only the note played inside the range");
        expect(onGrid, "quantize-while-recording puts it on the 1/16 grid");
        expect(notesAt(0, bar) == bar1Before && notesAt(bar, 2 * bar) == 1,
               "old notes inside the range are replaced; bar 1 is untouched");
    }

    {   // Out-of-process scanning: a crashing or hanging plugin only kills the helper.
        const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        juce::String detail;
        using O = JucePluginHost::ScanOutcome;
        expect(host.scanOneOutOfProcess(self, "YnotScanTest", "ok", 30000, detail) == O::Found,
               "scanner helper reports a plugin");
        expect(host.scanOneOutOfProcess(self, "YnotScanTest", "crash", 30000, detail) == O::Crashed,
               "a plugin that crashes the scanner is caught; Ynot keeps running");
        expect(host.scanOneOutOfProcess(self, "YnotScanTest", "hang", 3000, detail) == O::TimedOut,
               "a plugin that hangs the scanner times out and is skipped");
        auto cache = juce::File::createTempFile(".xml");
        host.saveCache(cache);
        JucePluginHost fresh;
        fresh.loadCache(cache);
        cache.deleteFile();
        expect(fresh.knownPlugins().getNumTypes() == host.knownPlugins().getNumTypes(),
               "scan results are remembered between launches");
    }

    {   // Plugin sandbox: DSP in a helper process; crashes and slow plugins can't hurt the host.
        const auto self = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        auto spec = [&](juce::String format, juce::String id, juce::String state = {}) {
            SandboxedProcessor::Spec sp;
            sp.executable = self;
            sp.formatName = format;
            sp.identifier = id;
            sp.state.append(state.toRawUTF8(), state.getNumBytesAsUTF8());
            sp.sampleRate = 48000;
            sp.maxBlock = 256;
            sp.displayName = id;
            return sp;
        };
        const int n = 256;
        std::vector<float> l(n), r(n), l2(n), r2(n);
        auto fill = [&](std::vector<float>& a, std::vector<float>& b, int block) {
            for (int i = 0; i < n; ++i) a[static_cast<size_t>(i)] = b[static_cast<size_t>(i)] =
                std::sin(0.05f * static_cast<float>(block * n + i));
        };

        // 1. A builtin effect in a sandbox sounds identical to in-process, with no added latency.
        SandboxedProcessor boxed(spec("Builtin", ynot::kBuiltinGain, R"({"gainDb": -6})"));
        ynot::PluginInstance gainInst = ynot::makeBuiltin(ynot::kBuiltinGain, "Gain");
        gainInst.params["gainDb"] = -6.0;
        auto local = ynot::BuiltinPluginHost{}.load(gainInst, 48000, n);
        double maxDiff = 0;
        for (int b = 0; b < 50; ++b) {
            fill(l, r, b);
            fill(l2, r2, b);
            boxed.process(l.data(), r.data(), n, nullptr, 0);
            local->process(l2.data(), r2.data(), n, nullptr, 0);
            for (int i = 0; i < n; ++i) maxDiff = std::max(maxDiff, static_cast<double>(std::abs(l[size_t(i)] - l2[size_t(i)])));
        }
        expect(boxed.ok() && maxDiff < 1e-6 && boxed.lateBlocks() == 0,
               "sandboxed plugin output matches in-process exactly (same block, no latency)");

        // 2. Parameters and state cross the process boundary.
        SandboxedProcessor test(spec("YnotSandboxTest", "gain2", "my-plugin-state"));
        fill(l, r, 0);
        const float before = l[100];
        test.process(l.data(), r.data(), n, nullptr, 0);
        test.setParameter("gain", 0.75);  // -> x3
        fill(l2, r2, 0);
        test.process(l2.data(), r2.data(), n, nullptr, 0);
        juce::MemoryBlock st;
        expect(std::abs(l[100] - 2 * before) < 1e-5f && std::abs(l2[100] - 3 * before) < 1e-5f &&
                   test.getState(st) && st.toString() == "my-plugin-state",
               "sandbox passes audio, parameter changes and plugin state");

        // 3. A plugin that crashes mid-playback is switched off; the host keeps running.
        SandboxedProcessor crashy(spec("YnotSandboxTest", "crash-after:5"));
        bool crashedNoticed = false, dryAfterCrash = false;
        for (int b = 0; b < 40 && !crashedNoticed; ++b) {
            fill(l, r, b);
            crashy.process(l.data(), r.data(), n, nullptr, 0);
            juce::Thread::sleep(5);
            crashedNoticed = crashy.pollCrashed();
        }
        fill(l, r, 99);
        const float dry = l[50];
        crashy.process(l.data(), r.data(), n, nullptr, 0);
        dryAfterCrash = std::abs(l[50] - dry) < 1e-9f;
        expect(crashedNoticed && dryAfterCrash, "a plugin that crashes is detected and bypassed; Ynot keeps running");

        // 4. A plugin too slow for real time never stalls the audio thread.
        SandboxedProcessor slow(spec("YnotSandboxTest", "slow"));
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        for (int b = 0; b < 20; ++b) {
            fill(l, r, b);
            slow.process(l.data(), r.data(), n, nullptr, 0);
        }
        const double perBlockMs = (juce::Time::getMillisecondCounterHiRes() - t0) / 20.0;
        expect(slow.lateBlocks() > 0 && perBlockMs < 4.0,
               "a too-slow plugin is skipped per block without stalling audio");
        std::cout << "  info sandbox: slow plugin skipped " << slow.lateBlocks() << "/20 blocks, "
                  << juce::String(perBlockMs, 2) << " ms per call" << std::endl;
    }

    {   // Audio import: every fixture this platform can decode, and a clear reason for the rest.
        auto fixtures = juce::File::getCurrentWorkingDirectory().getChildFile("app/tests/fixtures");
        if (auto* env = std::getenv("YNOT_FIXTURES")) fixtures = juce::File(env);
        if (!fixtures.isDirectory()) {
            std::cout << "  info audio import fixtures not found (run from the repo root); skipped" << std::endl;
        } else {
#if JUCE_MAC || JUCE_IOS
            const juce::StringArray mustWork{"wav", "flac", "ogg", "mp3", "m4a", "mp4"}, mustFail{"wmv", "wma"};
#elif JUCE_WINDOWS
            const juce::StringArray mustWork{"wav", "flac", "ogg", "mp3", "wma", "wmv"}, mustFail{};
#else
            const juce::StringArray mustWork{"wav", "flac", "ogg", "mp3"}, mustFail{"mp4", "m4a", "wmv", "wma"};
#endif
            bool allOk = true, reasonsOk = true;
            for (auto& ext : mustWork) {
                auto d = decodeAudioFile(fixtures.getChildFile("tone." + ext));
                // 0.5 s of a 440 Hz tone at 44.1 kHz (lossy codecs may pad a little).
                const bool good = d.ok && std::abs(d.audio.sampleRate - 44100.0) < 1.0 && d.audio.frames() >= 21000 &&
                                  d.audio.frames() <= 26000;
                std::cout << "  info import ." << ext << ": " << (good ? "ok via " + d.formatName : "FAILED " + d.error)
                          << std::endl;
                allOk &= good;
            }
            for (auto& ext : mustFail) {
                auto d = decodeAudioFile(fixtures.getChildFile("tone." + ext));
                std::cout << "  info import ." << ext << " (not supported here): "
                          << (d.ok ? juce::String("unexpectedly decoded") : d.error.upToFirstOccurrenceOf(".", true, false))
                          << std::endl;
                reasonsOk &= d.ok || d.error.isNotEmpty();
            }
            expect(allOk, "audio files this platform supports are decoded correctly");
            expect(reasonsOk, "unsupported formats give a clear reason");

            // Import through the session (background decode -> new track -> clip), like the UI does.
            const size_t tracksBefore = session.project().tracks.size();
            const auto err = session.importAudioFileNow(fixtures.getChildFile("tone.mp3"), {}, 0);
            const auto& tracks = session.project().tracks;
            expect(err.isEmpty() && tracks.size() == tracksBefore + 1 && tracks.back().name == "tone" &&
                       tracks.back().audioClips.size() == 1,
                   "importing an MP3 creates a track named after the file with the audio on it");
        }
    }

    session.save();
    auto reopened = ynot::loadProjectPackage(dir.getFullPathName().toStdString());
    expect(ynot::projectToJson(reopened) == ynot::projectToJson(session.project()), "save/reopen is lossless");
    std::cout << (failures ? "SMOKE TEST FAILED" : "smoke test passed") << std::endl;
    return failures ? 1 : 0;
}

// Drives the session's audio callback in real time without hardware (UI tests on machines
// with no sound card). Same thread model as a real device: a separate high-priority thread.
class SimulatedAudio : private juce::HighResolutionTimer {
public:
    explicit SimulatedAudio(Session& s) : session_(s), in_(256, 0.f), outL_(256), outR_(256) {
        session_.simulateInputsForTesting(2);
        startTimer(5);  // ~256 frames at 48 kHz per 5.3 ms
    }
    ~SimulatedAudio() override { stopTimer(); }

private:
    void hiResTimerCallback() override {
        const float* ins[2] = {in_.data(), in_.data()};
        float* outs[2] = {outL_.data(), outR_.data()};
        session_.processForTesting(ins, 2, outs, 2, 240);  // 240 frames = 5 ms at 48 kHz
    }
    Session& session_;
    std::vector<float> in_, outL_, outR_;
};

class MainWindow : public juce::DocumentWindow, private juce::KeyListener {
public:
    MainWindow(const juce::String& name, Session& session, JucePluginHost& host)
        : DocumentWindow(name, juce::Colours::black, DocumentWindow::allButtons) {
        // Shortcuts must work even when nothing inside the window has keyboard focus
        // (e.g. right after a popup menu closes): unhandled keys reach the window, which
        // forwards them to the main component.
        addKeyListener(this);
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(session, host), true);
#if JUCE_IOS || JUCE_ANDROID
        setFullScreen(true);
#else
        setResizable(true, true);
        setResizeLimits(360, 480, 10000, 10000);
        centreWithSize(getWidth(), getHeight());
#endif
        setVisible(true);
        if (auto* c = getContentComponent()) c->grabKeyboardFocus();
    }
    // Shortcuts keep working when the user returns to the window.
    void activeWindowStatusChanged() override {
        DocumentWindow::activeWindowStatusChanged();
        if (isActiveWindow())
            if (auto* c = getContentComponent())
                if (!c->hasKeyboardFocus(true)) c->grabKeyboardFocus();
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

private:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override {
        auto* mc = dynamic_cast<MainComponent*>(getContentComponent());
        return mc != nullptr && mc->keyPressed(key);
    }
    bool keyStateChanged(bool isKeyDown, juce::Component*) override {
        auto* mc = dynamic_cast<MainComponent*>(getContentComponent());
        return mc != nullptr && mc->keyStateChanged(isKeyDown);
    }
};

juce::File pluginCacheFile() {
    return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
        .getChildFile("Ynot").getChildFile("plugins.xml");
}

class YnotApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Ynot"; }
    const juce::String getApplicationVersion() override { return ynot::kCoreVersion; }
    // The plugin-scan helper and test modes run alongside the real app.
    bool moreThanOneInstanceAllowed() override {
        const auto args = getCommandLineParameters();
        return args.contains("--scan-one") || args.contains("--plugin-sandbox") || args.contains("--smoke-test") ||
               args.contains("--screenshot");
    }

    void initialise(const juce::String& commandLine) override {
        auto args = juce::StringArray::fromTokens(commandLine, true);
        if (args.contains("--plugin-sandbox")) {  // plugin sandbox helper: runs one plugin, no window
#if JUCE_MAC
            juce::Process::setDockIconVisible(false);
#endif
            const int rc = runSandboxChild(args);
            if (rc != 0) {
                setApplicationReturnValue(rc);
                quit();
            }
            return;  // keeps running (message loop alive for the plugin) until the host says quit
        }
        if (args.contains("--scan-one")) {  // plugin-scan helper process: no window, no audio
#if JUCE_MAC
            juce::Process::setDockIconVisible(false);
#endif
            setApplicationReturnValue(JucePluginHost::runScanChild(args));
            quit();
            return;
        }
        if (args.contains("--smoke-test")) {
            auto dirArg = args[args.indexOf("--smoke-test") + 1].unquoted();
            auto dir = dirArg.isNotEmpty() ? juce::File(dirArg)
                                           : juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ynot-smoke.ynot");
            setApplicationReturnValue(runSmokeTest(dir));
            quit();
            return;
        }
        if (args.contains("--layout-as"))
            MainComponent::layoutPlatformOverride =
                ynot::platformFromString(args[args.indexOf("--layout-as") + 1].toStdString());
        host_ = std::make_unique<JucePluginHost>();
        host_->loadCache(pluginCacheFile());  // plugins scanned on a previous launch
        host_->setSandboxExecutable(juce::File::getSpecialLocation(juce::File::currentExecutableFile));
        session_ = std::make_unique<Session>(*host_);
        if (args.contains("--open")) {
            try {
                session_->open(juce::File(args[args.indexOf("--open") + 1].unquoted()));
            } catch (const std::exception& e) {
                std::cerr << "could not open project: " << e.what() << std::endl;
            }
        }
        window_ = std::make_unique<MainWindow>(getApplicationName(), *session_, *host_);
        if (args.contains("--show-keys"))
            if (auto* mc = dynamic_cast<MainComponent*>(window_->getContentComponent())) mc->showKeyboard(true);
        if (args.contains("--screenshot")) {
            int i = args.indexOf("--screenshot");
            juce::File png(args[i + 1].unquoted());
            int w = args[i + 2].getIntValue(), h = args[i + 3].getIntValue();
            auto* content = window_->getContentComponent();
            content->setSize(w, h);
            juce::Timer::callAfterDelay(300, [content, png] {
                auto img = content->createComponentSnapshot(content->getLocalBounds(), true, 2.0f);
                png.deleteFile();
                juce::FileOutputStream out(png);
                juce::PNGImageFormat().writeImageToStream(img, out);
                out.flush();
                quit();
            });
            return;
        }
        if (args.contains("--simulate-audio")) simulated_ = std::make_unique<SimulatedAudio>(*session_);
        else session_->startAudio();  // default device; the user can change it in Audio settings
    }

    void shutdown() override {
        shutdownSandboxChild();
        simulated_.reset();
        window_.reset();
        session_.reset();
        host_.reset();
    }

    void systemRequestedQuit() override {
        if (session_ && session_->hasProject()) session_->save();
        quit();
    }

private:
    std::unique_ptr<JucePluginHost> host_;
    std::unique_ptr<Session> session_;
    std::unique_ptr<MainWindow> window_;
    std::unique_ptr<SimulatedAudio> simulated_;
};

}  // namespace ynotapp

START_JUCE_APPLICATION(ynotapp::YnotApplication)
