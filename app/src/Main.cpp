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
    {   // While recording, the waveform is available to draw: 1 s of the tone after the count-in.
        const auto wave = session.liveWaveform(vocalsId);
        float top = 0, bottom = 0;
        for (int k = 0; k < wave.count; ++k) top = std::max(top, wave.hi[k]), bottom = std::min(bottom, wave.lo[k]);
        expect(std::abs(wave.count - 48000 / Session::kLivePeakFrames) <= 2 && std::abs(top - 0.5f) < 0.01f &&
                   std::abs(bottom + 0.5f) < 0.01f,
               "the recording's waveform is drawn live (peaks follow the input while recording)");
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
            const juce::StringArray mustWork{"wav", "flac", "ogg", "mp3", "wma", "wmv", "m4a", "mp4"}, mustFail{};
#else
            const juce::StringArray mustWork{"wav", "flac", "ogg", "mp3"}, mustFail{"mp4", "m4a", "wmv", "wma"};
#endif
            bool allOk = true, reasonsOk = true;
            for (auto& ext : mustWork) {
                auto d = decodeAudioFile(fixtures.getChildFile("tone." + ext));
                // 0.5 s of a 440 Hz tone at 44.1 kHz (lossy codecs may pad a little).
                const bool good = d.ok && std::abs(d.audio.sampleRate - 44100.0) < 1.0 && d.audio.frames() >= 21000 &&
                                  d.audio.frames() <= 26000;
                float importPeak = 0;
                for (float v : d.audio.samples) importPeak = std::max(importPeak, std::abs(v));
                std::cout << "  info import ." << ext << ": " << (good ? "ok via " + d.formatName : "FAILED " + d.error)
                          << " (" << d.formatName << ", " << d.audio.sampleRate << " Hz, " << d.audio.channels
                          << " ch, " << d.audio.frames() << " frames, peak " << importPeak << ")" << std::endl;
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

    {   // Sample rate: switching keeps the playhead's musical position, is saved with the project,
        // and a take recorded at 96 kHz keeps its rate and the right length on the timeline.
        const int64_t bar2 = ynot::kTicksPerQuarter * 4;
        session.seekToTick(bar2);
        auto err = session.setSampleRate(96000);
        auto near = [](double a, double b) { return std::abs(a - b) < 0.5; };
        expect(err.isEmpty() && near(session.sampleRate(), 96000) && session.positionTick() == bar2 &&
                   near(session.project().sampleRateHint, 96000),
               "switching to 96 kHz keeps the playhead in place and is saved with the project");
        expect(session.setSampleRate(12345).isNotEmpty() && near(session.sampleRate(), 96000),
               "an unsupported rate is refused with a message");
        session.edit([&](ynot::Project& p) {
            for (auto& t : p.tracks) t.armed = t.id == vocalsId;
        });
        session.setPunchEnabled(false);
        session.setCountInBars(0);
        std::string recErr;
        const bool started = session.startRecording(recErr);
        const int block = 512, rate = 96000;
        std::vector<float> in(block), silence(block), outL(block), outR(block);
        const float* ins[2] = {in.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        for (int b = 0; b < rate / block; ++b) {  // 1 s
            for (int i = 0; i < block; ++i)
                in[static_cast<size_t>(i)] = 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f *
                                                             static_cast<float>(b * block + i) / float(rate));
            session.processForTesting(ins, 2, outs, 2, block);
        }
        auto res = session.stopRecording();
        bool take96Ok = false;
        const double oneSecondTicks = static_cast<double>(ynot::ticksSpanning(session.project(), bar2, rate, rate));
        for (auto& c : session.project().findTrack(vocalsId)->audioClips)
            if (started && !res.newClips.empty() && c.id == res.newClips[0])
                if (auto* m = session.project().findMedia(c.mediaId))
                    take96Ok = near(m->sampleRate, 96000) && std::abs(m->frames - rate) < block &&
                             std::abs(static_cast<double>(c.lengthTick) - oneSecondTicks) < 20 && c.startTick == bar2;
        expect(take96Ok, "a 1 s take at 96 kHz is stored at 96 kHz and spans 1 s on the timeline");

        err = session.setSampleRate(44100);
        auto saved = ynot::loadProjectPackage(dir.getFullPathName().toStdString());
        expect(err.isEmpty() && near(session.sampleRate(), 44100) && near(saved.sampleRateHint, 44100),
               "switching to 44.1 kHz works and the project file remembers it");
        session.setSampleRate(48000);
    }

    {   // Time stretch through the session: Poly to 150%, Mono back to 75%, undo, reset.
        const ynot::AudioClip* clip = nullptr;
        for (auto& c : session.project().findTrack(vocalsId)->audioClips) clip = &c;
        const std::string clipId = clip ? clip->id : std::string();
        const int64_t len = clip ? clip->lengthTick : 0;
        auto find = [&]() -> const ynot::AudioClip* {
            for (auto& c : session.project().findTrack(vocalsId)->audioClips)
                if (c.id == clipId) return &c;
            return nullptr;
        };
        auto err = session.stretchClipNow(vocalsId, clipId, 1.5, ynot::StretchMode::Poly);
        const auto* c = find();
        expect(err.isEmpty() && c && std::abs(c->lengthTick - len * 3 / 2) <= 2 &&
                   ynot::clipStretchMode(*c) == ynot::StretchMode::Poly,
               "time stretch (Poly) makes the clip 150% long");
        err = session.stretchClipNow(vocalsId, clipId, 0.75, ynot::StretchMode::Mono);
        c = find();
        expect(err.isEmpty() && c && std::abs(c->lengthTick - len * 3 / 4) <= 2,
               "re-stretching (Mono) works from the original length");
        session.undo();
        c = find();
        expect(c && std::abs(c->lengthTick - len * 3 / 2) <= 2, "undo restores the previous stretch");
        err = session.stretchClipNow(vocalsId, clipId, 1.0, ynot::StretchMode::Poly);
        c = find();
        expect(err.isEmpty() && c && std::abs(c->lengthTick - len) <= 1 && !ynot::clipStretchMode(*c),
               "reset returns the clip to its original audio and length");
    }

    {   // Audio interface: inputs by channel, input meters, main output pair, buffer size.
        session.simulateInputsForTesting(4);
        session.simulateOutputsForTesting(4);
        const auto choices = session.inputChoices();
        expect(choices.size() == 6 && choices[2].label == "In 3" && choices[4].channels == std::vector<int>{0, 1} &&
                   choices[5].label == "In 3+4 (stereo)",
               "every interface input is offered, alone and as stereo pairs");
        session.setInput(vocalsId, {2});
        expect(session.inputFor(vocalsId) == std::vector<int>{2}, "a track's input can be chosen directly");
        const int block = 256;
        std::vector<float> quiet(block), loud(block, 0.5f), o0(block), o1(block), o2(block), o3(block);
        const float* ins[4] = {quiet.data(), quiet.data(), loud.data(), quiet.data()};
        float* outs[4] = {o0.data(), o1.data(), o2.data(), o3.data()};
        for (int c = 0; c < 4; ++c) session.takeInputPeak(c);
        session.processForTesting(ins, 4, outs, 4, block);
        expect(std::abs(session.takeInputPeak(2) - 0.5f) < 1e-6f && session.takeInputPeak(0) <= 0.f,
               "each input has its own level meter");
        session.setMainOutputPair(1);
        session.seekToTick(0);
        session.play();
        float pair0 = 0, pair1 = 0;
        for (int b = 0; b < 100; ++b) {
            session.processForTesting(ins, 4, outs, 4, block);
            for (int i = 0; i < block; ++i) {
                pair0 = std::max(pair0, std::abs(o0[static_cast<size_t>(i)]) + std::abs(o1[static_cast<size_t>(i)]));
                pair1 = std::max(pair1, std::abs(o2[static_cast<size_t>(i)]) + std::abs(o3[static_cast<size_t>(i)]));
            }
        }
        session.stop();
        expect(pair1 > 0.01f && pair0 <= 0.f, "the song plays through the chosen output pair (Out 3+4) only");
        session.setMainOutputPair(0);
        auto err = session.setBufferSize(128);
        expect(err.isEmpty() && session.bufferSize() == 128 && session.setBufferSize(100).isNotEmpty(),
               "buffer size can be changed; unsupported sizes are refused");
        session.setBufferSize(512);
        session.setInput(vocalsId, {0});
        session.simulateInputsForTesting(2);
        session.simulateOutputsForTesting(2);
    }

    {   // Mixer: a fader drag changes the level at once and is one undo step; meters follow.
        std::string keysId;
        for (auto& t : session.project().tracks)
            if (t.name == "Keys") keysId = t.id;
        session.edit([&](ynot::Project& p) {
            for (auto& t : p.tracks) t.armed = false;
        });
        session.seekToTick(0);
        session.play();
        std::vector<float> silence(512), outL(512), outR(512);
        const float* ins[2] = {silence.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        auto run = [&] {
            for (int b = 0; b < 40; ++b) session.processForTesting(ins, 2, outs, 2, 512);
            return session.takeMeter(keysId);
        };
        session.takeMeter(keysId);
        const auto before = run();
        session.beginMixChange();
        for (double db = -1; db >= -20; db -= 1) session.setTrackGain(keysId, db);  // a drag
        const auto after = run();
        session.setTrackMute(keysId, true);
        const auto muted = run();
        const auto master = session.takeMeter({});
        expect(before.left > 0.01f && after.left < before.left * 0.2f && muted.left <= 0.f,
               "mixer fader and mute change the level immediately; meters follow");
        session.undo();  // the mute
        session.undo();  // the whole drag
        expect(std::abs(session.project().findTrack(keysId)->gainDb) < 1e-9 && !session.project().findTrack(keysId)->mute,
               "a fader drag is one undo step");
        expect(master.left >= 0.f, "the master meter reads");
        session.stop();
        session.seekToTick(0);
    }

    {   // Loop + cycle recording: 2.5 passes over bar 2 -> one clip with 3 takes, last full pass active.
        const int64_t bar = session.project().barTicks();
        session.edit([&](ynot::Project& p) {
            for (auto& t : p.tracks) t.armed = t.id == vocalsId;
        });
        session.setPunchEnabled(false);
        session.setCountInBars(0);
        session.setLoopRange(bar, 2 * bar);
        session.setLoopEnabled(true);
        std::string err;
        const bool started = session.startRecording(err);
        const int block = 500;  // not a divisor of the loop length: wraps land mid-block
        std::vector<float> in(block, 0.25f), silence(block), outL(block), outR(block);
        const float* ins[2] = {in.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        bool stayedInLoop = true;
        for (int b = 0; b < 5 * 48000 / block; ++b) {  // 5 s = 2.5 passes of a 2 s bar
            session.processForTesting(ins, 2, outs, 2, block);
            // Give the disk-writer thread time, as real-time audio would (the test runs far faster).
            if (b % 4 == 0) juce::Thread::sleep(1);
            const int64_t t = session.positionTick();
            stayedInLoop &= t >= bar && t <= 2 * bar;
        }
        auto res = session.stopRecording();
        const ynot::AudioClip* take = nullptr;
        for (auto& c : session.project().findTrack(vocalsId)->audioClips)
            if (!res.newClips.empty() && c.id == res.newClips[0]) take = &c;
        std::cout << "  info cycle take: " << (take ? "clip at " + std::to_string(take->startTick) + " len " +
                                                          std::to_string(take->lengthTick) + " takes " +
                                                          std::to_string(take->takeOffsets.size()) + " active " +
                                                          std::to_string(take->activeTake)
                                                    : std::string("none"))
                  << ", " << res.newClips.size() << " new clip(s), dropped " << res.droppedFrames << " frames"
                  << std::endl;
        expect(started && stayedInLoop, "with the loop on, recording plays the loop over and over");
        expect(take && res.droppedFrames == 0 && take->startTick == bar && take->lengthTick == bar &&
                   take->takeOffsets.size() == 3 && take->activeTake == 1,
               "cycle recording keeps each pass as a take; the last complete pass plays");
        if (take) {
            const auto id = take->id;
            expect(session.selectTake(vocalsId, id, 0), "an earlier take can be chosen");
        }
        session.setLoopEnabled(false);
        session.seekToTick(0);
    }

    {   // Export: every format this platform offers, read back with the importer.
        const auto outDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ynot-export-test");
        outDir.deleteRecursively();
        const ynot::TempoMap tempo(session.project());
        const double songSeconds = tempo.tickToSeconds(static_cast<double>(session.project().endTick()));
        bool allOk = true;
        for (auto f : availableExportFormats()) {
            ExportSettings es;
            es.format = f;
            es.sampleRate = 48000;
            es.bitDepth = f == ExportFormat::Wav ? 16 : 24;
            es.normalize = true;
            const auto file = outDir.getChildFile("song" + formatExtension(f));
            auto job = session.prepareExport(es);
            const auto err = runExport(job, file);
            const auto back = decodeAudioFile(file);
            float exportPeak = 0;
            for (float v : back.audio.samples) exportPeak = std::max(exportPeak, std::abs(v));
            const double seconds = back.ok ? static_cast<double>(back.audio.frames()) / back.audio.sampleRate : 0;
            // Song plus a reverb/delay tail (at most 10 s); lossy encoders may pad a little.
            const bool good = err.isEmpty() && back.ok && std::abs(back.audio.sampleRate - exportRateFor(es)) < 1 &&
                              seconds >= songSeconds - 0.01 && seconds <= songSeconds + 10.2 && exportPeak > 0.8f &&
                              exportPeak < 1.0f;
            std::cout << "  info export " << formatName(f) << ": "
                      << (good ? juce::String("ok") : "FAILED " + err + " " + back.error) << " (" << seconds
                      << " s of " << songSeconds << ", peak " << exportPeak << ", " << file.getSize() << " bytes)" << std::endl;
            allOk &= good;
        }
        expect(allOk, "export writes every available format; files read back at the right length and level");

        ExportSettings loopOnly;
        loopOnly.format = ExportFormat::Wav;
        loopOnly.loopOnly = true;
        session.setLoopRange(0, session.project().barTicks());
        const auto loopFile = outDir.getChildFile("loop.wav");
        auto job = session.prepareExport(loopOnly);
        const auto err = runExport(job, loopFile);
        const auto back = decodeAudioFile(loopFile);
        const int64_t barFrames = tempo.tickToFrame(session.project().barTicks(), 48000);
        expect(err.isEmpty() && back.ok && back.audio.frames() == barFrames,
               "loop export is exactly the loop's length (seamless)");
        outDir.deleteRecursively();
    }

    {   // Drums: a step pattern plays through the session; auditioning works with the transport stopped.
        std::string drums, clip;
        session.edit([&](ynot::Project& p) {
            drums = ynot::addDrumTrack(p, 0, 1);
            clip = p.findTrack(drums)->midiClips[0].id;
            for (int s : {0, 4, 8, 12}) ynot::setStep(p, drums, clip, 36, s, 110);
            for (auto& t : p.tracks) t.mute = t.id != drums;
        });
        std::vector<float> silence(512), outL(512), outR(512);
        const float* ins[2] = {silence.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        auto peakOf = [&](int blocks) {
            float pk = 0;
            for (int b = 0; b < blocks; ++b) {
                session.processForTesting(ins, 2, outs, 2, 512);
                for (float v : outL) pk = std::max(pk, std::abs(v));
            }
            return pk;
        };
        session.seekToTick(0);
        session.play();
        const float playing = peakOf(60);
        session.stop();
        peakOf(100);  // let the tail die
        session.setAuditionTrack(drums);
        const float before = peakOf(10);
        session.auditionNote(38, 110);
        juce::Thread::sleep(5);
        const float audition = peakOf(20);
        session.setAuditionTrack({});
        expect(playing > 0.1f, "a drum pattern made in the step sequencer plays");
        expect(before < 1e-4f && audition > 0.05f, "clicking a drum name auditions it with the transport stopped");
        session.edit([&](ynot::Project& p) {
            for (auto& t : p.tracks) t.mute = false;
        });
    }

    {   // Ynot Tune loads as an insert and the track keeps playing (in time, via delay compensation).
        session.edit([&](ynot::Project& p) {
            auto fx = ynot::makeBuiltin(ynot::kBuiltinTune, "Ynot Tune");
            fx.params["key"] = static_cast<int64_t>(9);  // A minor
            fx.params["scale"] = static_cast<int64_t>(2);
            ynot::addInsert(p, vocalsId, fx);
        });
        bool loaded = true;
        for (const auto& i : session.engineIssues()) loaded &= i.message.find("Ynot Tune") == std::string::npos;
        std::vector<float> silence(512), outL(512), outR(512);
        const float* ins[2] = {silence.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        session.seekToTick(0);
        session.play();
        float pk = 0;
        for (int b = 0; b < 100; ++b) {
            session.processForTesting(ins, 2, outs, 2, 512);
            for (float v : outL) pk = std::max(pk, std::abs(v));
        }
        session.stop();
        expect(loaded && pk > 0.01f, "Ynot Tune loads on a vocal track and the track plays through it");
        session.undo();
        session.seekToTick(0);
    }

    {   // Tempo: a change at bar 3 moves the playhead's time but not its bar; undo restores it.
        const int64_t bar3 = 2 * session.project().barTicks();
        session.seekToTick(bar3 + ynot::kTicksPerQuarter);
        const double before = session.positionSeconds();
        session.edit([&](ynot::Project& p) { ynot::setTempoChange(p, bar3, 60); });
        session.seekToTick(bar3 + ynot::kTicksPerQuarter);
        const double after = session.positionSeconds();
        expect(std::abs(after - before - 0.5) < 1e-3 && session.positionTick() == bar3 + ynot::kTicksPerQuarter &&
                   std::abs(session.project().tempoAt(bar3) - 60) < 1e-9,
               "a tempo change slows the song from its bar on; the playhead keeps its bar and beat");
        session.undo();
        expect(session.project().tempoChanges.empty(), "undo removes the tempo change");
        session.seekToTick(0);
    }

    {   // MIDI controllers (e.g. Akai MPK25): bend/mod play live, MIDI Learn, MMC transport, clock out.
        auto& mc = session.midiControls();
        const auto settings = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ynot-midi-test.json");
        settings.deleteFile();
        mc.useSettingsFileForTesting(settings);
        std::string keysId;
        for (auto& t : session.project().tracks)
            if (t.name == "Keys") keysId = t.id;
        session.edit([&](ynot::Project& p) {
            for (auto& t : p.tracks) {
                t.armed = t.id == keysId;
                t.mute = t.id != keysId;
            }
        });
        const int block = 256;
        std::vector<float> silence(block), outL(block), outR(block), heard;
        const float* ins[2] = {silence.data(), silence.data()};
        float* outs[2] = {outL.data(), outR.data()};
        auto render = [&](int blocks) {
            heard.clear();
            for (int b = 0; b < blocks; ++b) {
                session.processForTesting(ins, 2, outs, 2, block);
                heard.insert(heard.end(), outL.begin(), outL.end());
                juce::Thread::sleep(1);
            }
        };
        auto pitch = [&] {
            const int n = static_cast<int>(heard.size()) / 2;
            return ynot::detectPitchHz(heard.data() + n, n, 48000);
        };
        session.injectMidiForTesting(juce::MidiMessage::noteOn(1, 57, static_cast<juce::uint8>(100)));
        render(60);
        const double plain = pitch();
        session.injectMidiForTesting(juce::MidiMessage::pitchWheel(1, 16383));
        render(60);
        const double bent = pitch();
        session.injectMidiForTesting(juce::MidiMessage::pitchWheel(1, 8192));
        session.injectMidiForTesting(juce::MidiMessage::controllerEvent(1, 64, 127));  // sustain down
        session.injectMidiForTesting(juce::MidiMessage::noteOff(1, 57));
        render(40);
        float sustained = 0;
        for (float v : heard) sustained = std::max(sustained, std::abs(v));
        session.injectMidiForTesting(juce::MidiMessage::controllerEvent(1, 64, 0));  // pedal up
        render(200);
        float released = 0;
        for (size_t i = heard.size() / 2; i < heard.size(); ++i) released = std::max(released, std::abs(heard[i]));
        const double semis = 12.0 * std::log2(bent / std::max(1.0, plain));
        expect(plain > 0 && std::abs(semis - 2.0) < 0.15, "pitch bend from a MIDI controller bends the synth 2 semitones");
        expect(sustained > 0.02f && released < 1e-3f, "the sustain pedal holds notes and releases them");

        // MIDI Learn: right-click > MIDI Learn on track 1's fader, then turn a knob.
        mc.startLearning(MidiTarget::TrackVolume, 0);
        session.injectMidiForTesting(juce::MidiMessage::controllerEvent(1, 22, 100));
        mc.flushForTesting();
        const auto learned = mc.mappingFor(MidiTarget::TrackVolume, 0);
        session.injectMidiForTesting(juce::MidiMessage::controllerEvent(1, 22, 50));
        mc.flushForTesting();
        const double gain = session.project().tracks[0].gainDb;
        mc.startLearning(MidiTarget::TrackPan, 0);
        session.injectMidiForTesting(juce::MidiMessage::controllerEvent(1, 23, 0));
        mc.flushForTesting();
        const double pan = session.project().tracks[0].pan;
        expect(learned && learned->cc == 22 && !mc.isLearning() && std::abs(gain - 20 * std::log10(0.25)) < 0.01 &&
                   std::abs(pan + 1.0) < 1e-9,
               "MIDI Learn assigns a knob to a track's volume and pan");
        session.undo();  // knob moves are undoable
        session.undo();
        mc.useSettingsFileForTesting(settings);  // saved and reloaded
        expect(mc.mappings().size() == 2, "MIDI Learn assignments are saved on this computer");

        // Transport buttons: MMC Play / Stop (the MPK25 default).
        std::vector<TransportCommand> got;
        session.onTransportCommand = [&](TransportCommand c) { got.push_back(c); };
        const juce::uint8 play[] = {0xF0, 0x7F, 0x7F, 0x06, 0x02, 0xF7}, stop[] = {0xF0, 0x7F, 0x7F, 0x06, 0x01, 0xF7};
        session.injectMidiForTesting(juce::MidiMessage(play, 6));
        session.injectMidiForTesting(juce::MidiMessage(stop, 6));
        mc.flushForTesting();
        session.onTransportCommand = nullptr;
        expect(got.size() == 2 && got[0] == TransportCommand::Play && got[1] == TransportCommand::Stop,
               "MMC transport buttons (Play/Stop) reach the transport");

        // MIDI clock out: 24 pulses per beat while playing, with Start and Stop.
        auto& clock = session.midiClockForTesting();
        clock.holdForTesting(true);
        session.setMidiClockOut(true);
        int pulses = 0, starts = 0, stops = 0;
        auto sink = [&](const juce::MidiMessage& m) {
            pulses += m.isMidiClock();
            starts += m.isMidiStart() || m.isMidiContinue();
            stops += m.isMidiStop();
        };
        clock.drainForTesting([](const juce::MidiMessage&) {});
        session.seekToTick(0);
        session.play();
        render(48000 * 2 / block);  // 2 s at 120 BPM = 4 beats
        session.stop();
        render(2);
        clock.drainForTesting(sink);
        session.setMidiClockOut(false);
        clock.holdForTesting(false);
        expect(starts == 1 && stops == 1 && std::abs(pulses - 96) <= 1, "MIDI clock out sends 24 pulses per beat with Start/Stop");
        mc.clear();
        settings.deleteFile();
        session.edit([&](ynot::Project& p) {
            for (auto& t : p.tracks) t.armed = t.mute = false;
        });
    }

    {   // Save As copies the song (with its recordings) and continues in the copy; edits mark it unsaved.
        session.edit([&](ynot::Project& p) { p.tracks[0].name = "Lead vocal"; });
        const bool markedEdited = session.isModified();
        const auto copyDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("ynot-saveas-test.ynot");
        copyDir.deleteRecursively();
        const auto original = session.packageDir();
        const auto err = session.saveAs(copyDir);
        auto copy = ynot::loadProjectPackage(copyDir.getFullPathName().toStdString());
        bool mediaCopied = !copy.media.empty();
        for (const auto& m : copy.media) mediaCopied &= copyDir.getChildFile(m.fileName).existsAsFile();
        expect(markedEdited && err.isEmpty() && !session.isModified() && session.packageDir() == copyDir &&
                   copy.tracks[0].name == "Lead vocal" && copy.name == "ynot-saveas-test" && mediaCopied &&
                   Session::recentProjects().contains(copyDir.getFullPathName()),
               "Save As copies the song and its recordings, continues there and lists it under recent songs");
        expect(session.saveAs(original).isNotEmpty(), "Save As refuses to overwrite an existing song");
        session.open(original);  // carry on with the original for the rest of the test
        copyDir.deleteRecursively();
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
    // `inputTone`: feed a varying test tone into the inputs (to see recording waveforms).
    SimulatedAudio(Session& s, bool inputTone) : session_(s), tone_(inputTone), in_(256, 0.f), outL_(256), outR_(256) {
        session_.simulateInputsForTesting(2);
        startTimer(5);  // ~256 frames at 48 kHz per 5.3 ms
    }
    ~SimulatedAudio() override { stopTimer(); }

private:
    void hiResTimerCallback() override {
        if (tone_)
            for (int i = 0; i < 240; ++i, ++t_) {
                const double env = 0.5 + 0.45 * std::sin(t_ / 48000.0 * 2.0);  // swells, like a voice
                in_[static_cast<size_t>(i)] = static_cast<float>(0.6 * env * std::sin(t_ * 2 * 3.14159265 * 220.0 / 48000.0));
            }
        const float* ins[2] = {in_.data(), in_.data()};
        float* outs[2] = {outL_.data(), outR_.data()};
        session_.processForTesting(ins, 2, outs, 2, 240);  // 240 frames = 5 ms at 48 kHz
    }
    Session& session_;
    bool tone_;
    int64_t t_ = 0;
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
        if (args.contains("--simulate-audio")) simulated_ = std::make_unique<SimulatedAudio>(*session_, args.contains("--simulate-input-tone"));
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
