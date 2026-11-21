// Ynot application entry point (all platforms).
//
//   Ynot                        normal GUI launch
//   Ynot --smoke-test <dir>     headless check of the JUCE <-> core integration (CI)
//   Ynot --open <dir>           open a project at launch
//   Ynot --screenshot <png> <w> <h> [--open <dir>] [--layout-as ios|android|macos|windows]
//                               render the UI at a size and exit (layout checks)
#include <juce_gui_extra/juce_gui_extra.h>

#include <iostream>

#include "MainComponent.h"

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

    session.save();
    auto reopened = ynot::loadProjectPackage(dir.getFullPathName().toStdString());
    expect(ynot::projectToJson(reopened) == ynot::projectToJson(session.project()), "save/reopen is lossless");
    std::cout << (failures ? "SMOKE TEST FAILED" : "smoke test passed") << std::endl;
    return failures ? 1 : 0;
}

class MainWindow : public juce::DocumentWindow {
public:
    MainWindow(const juce::String& name, Session& session, JucePluginHost& host)
        : DocumentWindow(name, juce::Colours::black, DocumentWindow::allButtons) {
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
    }
    void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
};

class YnotApplication : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Ynot"; }
    const juce::String getApplicationVersion() override { return ynot::kCoreVersion; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override {
        auto args = juce::StringArray::fromTokens(commandLine, true);
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
        session_ = std::make_unique<Session>(*host_);
        if (args.contains("--open")) {
            try {
                session_->open(juce::File(args[args.indexOf("--open") + 1].unquoted()));
            } catch (const std::exception& e) {
                std::cerr << "could not open project: " << e.what() << std::endl;
            }
        }
        window_ = std::make_unique<MainWindow>(getApplicationName(), *session_, *host_);
        if (args.contains("--screenshot")) {
            int i = args.indexOf("--screenshot");
            juce::File png(args[i + 1].unquoted());
            int w = args[i + 2].getIntValue(), h = args[i + 3].getIntValue();
            auto* content = window_->getContentComponent();
            content->setSize(w, h);
            juce::Timer::callAfterDelay(300, [this, content, png] {
                auto img = content->createComponentSnapshot(content->getLocalBounds(), true, 2.0f);
                png.deleteFile();
                juce::FileOutputStream out(png);
                juce::PNGImageFormat().writeImageToStream(img, out);
                out.flush();
                quit();
            });
            return;
        }
        session_->startAudio();  // default device; the user can change it in Audio settings
    }

    void shutdown() override {
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
};

}  // namespace ynotapp

START_JUCE_APPLICATION(ynotapp::YnotApplication)
