#include "PluginSandbox.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <chrono>
#include <cstring>
#include <thread>

#include "JucePluginHost.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <semaphore.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace ynotapp {

// ===================================================================== OS primitives

namespace {

// Named shared memory region.
class SharedRegion {
public:
    ~SharedRegion() { close(); }

    bool create(const std::string& name, size_t size) { return openImpl(name, size, true); }
    bool open(const std::string& name, size_t size) { return openImpl(name, size, false); }
    void* data() const { return data_; }

    void close() {
#if JUCE_WINDOWS
        if (data_) UnmapViewOfFile(data_);
        if (handle_) CloseHandle(handle_);
        handle_ = nullptr;
#else
        if (data_) munmap(data_, size_);
        if (owner_ && !name_.empty()) shm_unlink(name_.c_str());
#endif
        data_ = nullptr;
    }

private:
    bool openImpl(const std::string& name, size_t size, bool create) {
        name_ = name;
        size_ = size;
        owner_ = create;
#if JUCE_WINDOWS
        const auto wname = "Local\\" + name;
        handle_ = create ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                              static_cast<DWORD>(size), wname.c_str())
                         : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, wname.c_str());
        if (!handle_) return false;
        data_ = MapViewOfFile(handle_, FILE_MAP_ALL_ACCESS, 0, 0, size);
        return data_ != nullptr;
#else
        const auto pname = "/" + name;
        name_ = pname;
        int fd = shm_open(pname.c_str(), create ? (O_CREAT | O_EXCL | O_RDWR) : O_RDWR, 0600);
        if (fd < 0) return false;
        if (create && ftruncate(fd, static_cast<off_t>(size)) != 0) {
            ::close(fd);
            return false;
        }
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) return false;
        data_ = p;
        return true;
#endif
    }

    std::string name_;
    size_t size_ = 0;
    bool owner_ = false;
    void* data_ = nullptr;
#if JUCE_WINDOWS
    HANDLE handle_ = nullptr;
#endif
};

// Named counting semaphore used to wake the helper (post is cheap and realtime-tolerable).
class WakeSignal {
public:
    ~WakeSignal() { close(); }

    bool create(const std::string& name) { return openImpl(name, true); }
    bool open(const std::string& name) { return openImpl(name, false); }

    void post() {
#if JUCE_WINDOWS
        if (h_) ReleaseSemaphore(h_, 1, nullptr);
#else
        if (s_) sem_post(s_);
#endif
    }
    void wait() {
#if JUCE_WINDOWS
        if (h_) WaitForSingleObject(h_, INFINITE);
#else
        if (s_) while (sem_wait(s_) != 0 && errno == EINTR) {}
#endif
    }
    void close() {
#if JUCE_WINDOWS
        if (h_) CloseHandle(h_);
        h_ = nullptr;
#else
        if (s_) sem_close(s_);
        if (owner_ && !name_.empty()) sem_unlink(name_.c_str());
        s_ = nullptr;
#endif
    }

private:
    bool openImpl(const std::string& name, bool create) {
        owner_ = create;
#if JUCE_WINDOWS
        const auto wname = "Local\\" + name;
        h_ = create ? CreateSemaphoreA(nullptr, 0, 1 << 20, wname.c_str())
                    : OpenSemaphoreA(SEMAPHORE_ALL_ACCESS, FALSE, wname.c_str());
        return h_ != nullptr;
#else
        name_ = "/" + name;
        s_ = create ? sem_open(name_.c_str(), O_CREAT | O_EXCL, 0600, 0) : sem_open(name_.c_str(), 0);
        if (s_ == SEM_FAILED) s_ = nullptr;
        return s_ != nullptr;
#endif
    }

    bool owner_ = false;
    std::string name_;
#if JUCE_WINDOWS
    HANDLE h_ = nullptr;
#else
    sem_t* s_ = nullptr;
#endif
};

bool processAlive(int64_t pid) {
#if JUCE_WINDOWS
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return pid > 1 && kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

int64_t currentPid() {
#if JUCE_WINDOWS
    return static_cast<int64_t>(GetCurrentProcessId());
#else
    return static_cast<int64_t>(getpid());
#endif
}

std::string uniqueName(const char* suffix) {
    // Short: macOS limits POSIX shm/semaphore names to 31 characters.
    return "ynot" + juce::String::toHexString(juce::Random::getSystemRandom().nextInt()).toStdString() + suffix;
}

}  // namespace

// ===================================================================== host side

struct SandboxedProcessor::Impl : private juce::Timer {
    explicit Impl(SandboxedProcessor& o) : owner(o) {}
    ~Impl() override { stopTimer(); }

    void timerCallback() override {
        if (shared) shared->heartbeat.fetch_add(1);
        owner.pollCrashed();
    }

    SandboxedProcessor& owner;
    SharedRegion region;
    WakeSignal wake;
    juce::ChildProcess child;
    sandbox::Shared* shared = nullptr;
    juce::File stateFile;
    bool quitting = false;
    void start() { startTimer(250); }
};

SandboxedProcessor::SandboxedProcessor(const Spec& spec, int loadTimeoutMs) : impl_(std::make_unique<Impl>(*this)), spec_(spec) {
    spec_.maxBlock = std::min(spec_.maxBlock, sandbox::kMaxBlock);
    const auto shmName = uniqueName("m"), semName = uniqueName("s");
    if (!impl_->region.create(shmName, sizeof(sandbox::Shared)) || !impl_->wake.create(semName)) {
        error_ = "couldn't create shared memory for the plugin sandbox";
        return;
    }
    impl_->shared = new (impl_->region.data()) sandbox::Shared();
    impl_->shared->magic = sandbox::kMagic;
    impl_->shared->version = sandbox::kVersion;
    impl_->shared->isInstrument = spec_.isInstrument ? 1 : 0;

    impl_->stateFile = juce::File::createTempFile(".ynotstate");
    impl_->stateFile.replaceWithData(spec_.state.getData(), spec_.state.getSize());

    juce::StringArray cmd{spec_.executable.getFullPathName(),
                          "--plugin-sandbox",
                          juce::String(shmName),
                          juce::String(semName),
                          spec_.formatName,
                          spec_.identifier,
                          impl_->stateFile.getFullPathName(),
                          juce::String(spec_.sampleRate),
                          juce::String(spec_.maxBlock),
                          juce::String(spec_.isInstrument ? 1 : 0),
                          juce::String(currentPid()),
                          juce::String(spec_.uid.empty() ? "-" : spec_.uid)};
    if (!impl_->child.start(cmd, 0)) {
        error_ = "couldn't start the plugin sandbox process";
        return;
    }
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(loadTimeoutMs);
    while (juce::Time::getMillisecondCounter() < deadline) {
        if (impl_->shared->ready.load()) break;
        if (impl_->shared->failed.load() || !impl_->child.isRunning()) break;
        juce::Thread::sleep(5);
    }
    if (!impl_->shared->ready.load()) {
        error_ = impl_->shared->failed.load() ? "the plugin couldn't be loaded"
                 : impl_->child.isRunning()   ? "the plugin took too long to load"
                                              : "the plugin crashed while loading";
        impl_->child.kill();
        return;
    }
    ok_ = true;
    impl_->start();
}

SandboxedProcessor::~SandboxedProcessor() {
    if (impl_->shared) {
        impl_->quitting = true;
        impl_->shared->quit.store(1);
        impl_->wake.post();
        if (!impl_->child.waitForProcessToFinish(2000)) impl_->child.kill();
    }
    impl_->stateFile.deleteFile();
    impl_->stateFile.withFileExtension(".out").deleteFile();
}

void SandboxedProcessor::process(float* left, float* right, int frames, const ynot::MidiEvent* events, int numEvents) {
    auto* sh = impl_->shared;
    if (!ok_ || dead_.load(std::memory_order_relaxed) || sh == nullptr) return;  // bypass: audio passes through
    frames = std::min(frames, sandbox::kMaxBlock);
    // Previous block still being processed (helper fell behind): skip this block rather
    // than overwrite buffers the helper is reading.
    if (sh->doneSeq.load(std::memory_order_acquire) < seq_) {
        late_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    std::memcpy(sh->in[0], left, sizeof(float) * static_cast<size_t>(frames));
    std::memcpy(sh->in[1], right, sizeof(float) * static_cast<size_t>(frames));
    const int nMidi = std::min(numEvents, sandbox::kMaxMidi);
    if (nMidi > 0) std::memcpy(sh->midi, events, sizeof(ynot::MidiEvent) * static_cast<size_t>(nMidi));
    sh->numMidi = nMidi;
    sh->frames = frames;
    const uint64_t n = ++seq_;
    sh->heartbeat.fetch_add(1, std::memory_order_relaxed);
    sh->requestSeq.store(n, std::memory_order_release);
    impl_->wake.post();

    // Wait at most ~60% of the block's duration (at least 1.5 ms) for the result.
    const double budgetSec = std::max(0.0015, 0.6 * frames / std::max(1.0, spec_.sampleRate));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(budgetSec);
    int spins = 0;
    while (sh->doneSeq.load(std::memory_order_acquire) < n) {
        if (std::chrono::steady_clock::now() > deadline) {
            late_.fetch_add(1, std::memory_order_relaxed);
            return;  // late: this plugin is skipped for this block
        }
        if (++spins > 64) std::this_thread::yield();
    }
    std::memcpy(left, sh->out[0], sizeof(float) * static_cast<size_t>(frames));
    std::memcpy(right, sh->out[1], sizeof(float) * static_cast<size_t>(frames));
}

void SandboxedProcessor::setParameter(const std::string& id, double v) {
    auto* sh = impl_->shared;
    if (!ok_ || sh == nullptr) return;
    const uint64_t w = sh->paramWrite.load(std::memory_order_relaxed);
    if (w - sh->paramRead.load(std::memory_order_acquire) >= sandbox::kMaxParams) return;  // full: drop
    auto& slot = sh->params[w % sandbox::kMaxParams];
    std::strncpy(slot.id, id.c_str(), sizeof(slot.id) - 1);
    slot.id[sizeof(slot.id) - 1] = 0;
    slot.value = static_cast<float>(v);
    sh->paramWrite.store(w + 1, std::memory_order_release);
}

void SandboxedProcessor::reset() {
    if (impl_->shared) impl_->shared->resetRequest.fetch_add(1);
}

bool SandboxedProcessor::getState(juce::MemoryBlock& out, int timeoutMs) {
    auto* sh = impl_->shared;
    if (!ok_ || dead_.load() || sh == nullptr) return false;
    const auto want = sh->stateRequest.load() + 1;
    sh->stateRequest.store(want);
    impl_->wake.post();
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (sh->stateDone.load() < want) {
        if (juce::Time::getMillisecondCounter() > deadline) return false;
        juce::Thread::sleep(2);
    }
    return impl_->stateFile.withFileExtension(".out").loadFileAsData(out);
}

void SandboxedProcessor::showEditor() {
    if (!ok_ || dead_.load() || !impl_->shared) return;
    impl_->shared->editorRequest.fetch_add(1);
    impl_->wake.post();
}

bool SandboxedProcessor::pollCrashed() {
    if (!ok_ || impl_->quitting || dead_.load()) {
        if (dead_.load() && !crashReported_) {
            crashReported_ = true;
            return true;
        }
        return false;
    }
    if (!impl_->child.isRunning()) {
        dead_.store(true);
        crashReported_ = true;
        return true;
    }
    return false;
}

// ===================================================================== helper side

namespace {

// A tiny plugin used by tests: "gain2" doubles the signal, "crash-after:N" crashes after
// N blocks, "slow" takes 30 ms per block; state is echoed back unchanged.
class SandboxTestPlugin : public ynot::PluginProcessor {
public:
    explicit SandboxTestPlugin(juce::String id) : id_(std::move(id)) {}
    void process(float* l, float* r, int frames, const ynot::MidiEvent*, int) override {
        ++blocks_;
        if (id_.startsWith("crash-after:") && blocks_ > id_.fromFirstOccurrenceOf(":", false, false).getIntValue())
            std::abort();
        if (id_ == "slow") std::this_thread::sleep_for(std::chrono::milliseconds(30));
        for (int i = 0; i < frames; ++i) {
            l[i] *= gain_;
            r[i] *= gain_;
        }
    }
    void setParameter(const std::string& id, double v) override {
        if (id == "gain") gain_ = static_cast<float>(v * 4.0);
    }
    void reset() override {}

private:
    juce::String id_;
    int blocks_ = 0;
    float gain_ = 2.0f;
};

class SandboxChild : private juce::Thread {
public:
    SandboxChild() : juce::Thread("Ynot plugin sandbox") {}
    ~SandboxChild() override {
        editorWindow.reset();
        if (shared) shared->quit.store(1);
        wake.post();
        stopThread(2000);
        if (watchdog.joinable()) watchdog.join();
    }

    int start(const juce::StringArray& a) {
        // --plugin-sandbox <shm> <sem> <format> <identifier> <stateFile> <sr> <maxBlock> <isInstrument> <parentPid>
        const int i = a.indexOf("--plugin-sandbox");
        if (i < 0 || i + 9 >= a.size()) return 2;
        if (!region.open(a[i + 1].toStdString(), sizeof(sandbox::Shared)) || !wake.open(a[i + 2].toStdString())) return 3;
        shared = static_cast<sandbox::Shared*>(region.data());
        if (shared->magic != sandbox::kMagic || shared->version != sandbox::kVersion) return 4;
        const juce::String format = a[i + 3], identifier = a[i + 4];
        stateFile = juce::File(a[i + 5]);
        const double sr = a[i + 6].getDoubleValue();
        const int maxBlock = std::min(a[i + 7].getIntValue(), sandbox::kMaxBlock);
        parentPid = a[i + 9].getLargeIntValue();
        wantedUid = i + 10 < a.size() ? a[i + 10].toStdString() : std::string("-");

        juce::MemoryBlock state;
        stateFile.loadFileAsData(state);
        initialState = state;
        plugin = load(format, identifier, state, sr, maxBlock);
        if (!plugin) {
            shared->failed.store(1);
            return 5;
        }
        l.assign(static_cast<size_t>(maxBlock), 0.f);
        r.assign(static_cast<size_t>(maxBlock), 0.f);
        startThread(juce::Thread::Priority::highest);
        watchdog = std::thread([this] {
            // Exit if the host disappears (e.g. it crashed), so no orphaned helpers remain.
            while (!shared->quit.load()) {
                if (!processAlive(parentPid)) std::_Exit(0);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
        });
        shared->ready.store(1);
        return 0;
    }

private:
    std::unique_ptr<ynot::PluginProcessor> load(const juce::String& format, const juce::String& id,
                                                const juce::MemoryBlock& state, double sr, int maxBlock) {
        if (format == "YnotSandboxTest") return std::make_unique<SandboxTestPlugin>(id);
        if (format == "Builtin") {
            ynot::PluginInstance p;
            p.format = ynot::PluginFormat::Builtin;
            p.uid = id.toStdString();
            if (state.getSize() > 0) {
                try {
                    p.params = ynot::Json::parse(state.toString().toStdString());
                } catch (...) {
                }
            }
            return ynot::BuiltinPluginHost{}.load(p, sr, maxBlock);
        }
        formats = std::make_unique<juce::AudioPluginFormatManager>();
        addPluginFormats(*formats);
        for (auto* f : formats->getFormats()) {
            if (f->getName() != format) continue;
            juce::OwnedArray<juce::PluginDescription> found;
            f->findAllTypesForFile(found, id);
            if (found.isEmpty()) return nullptr;
            const juce::PluginDescription* pick = found[0];
            for (auto* d : found)
                if (portableUid(*d) == wantedUid) pick = d;  // multi-plugin files (VST3 shells)
            juce::String err;
            auto inst = formats->createPluginInstance(*pick, sr, maxBlock, err);
            if (!inst) return nullptr;
            if (state.getSize() > 0) inst->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
            juceProcessor = inst.get();
            return std::make_unique<JuceProcessorAdapter>(std::move(inst), sr, maxBlock);
        }
        return nullptr;
    }

    // The plugin's own editor, in a window owned by this helper process.
    class EditorWindow : public juce::DocumentWindow {
    public:
        EditorWindow(juce::AudioProcessor& p)
            : juce::DocumentWindow(p.getName(), juce::Colours::black, juce::DocumentWindow::closeButton) {
            setUsingNativeTitleBar(true);
            juce::AudioProcessorEditor* ed = p.hasEditor() ? p.createEditorIfNeeded() : nullptr;
            if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor(p);
            setContentOwned(ed, true);
            setResizable(ed->isResizable(), false);
            centreWithSize(getWidth(), getHeight());
            setVisible(true);
            toFront(true);
        }
        void closeButtonPressed() override { setVisible(false); }
    };

    void showEditorOnMessageThread() {
        if (!juceProcessor) return;  // builtin/test plugins have no window
#if JUCE_MAC
        juce::Process::makeForegroundProcess();
#endif
        if (editorWindow) {
            editorWindow->setVisible(true);
            editorWindow->toFront(true);
            return;
        }
        editorWindow = std::make_unique<EditorWindow>(*juceProcessor);
    }

    void run() override {
        uint64_t resets = 0;
        uint32_t editors = 0;
        while (!threadShouldExit()) {
            wake.wait();
            if (shared->quit.load()) break;
            // Parameter changes queued by the host.
            for (uint64_t rd = shared->paramRead.load(); rd < shared->paramWrite.load(std::memory_order_acquire); ++rd) {
                const auto& pc = shared->params[rd % sandbox::kMaxParams];
                plugin->setParameter(pc.id, pc.value);
                shared->paramRead.store(rd + 1, std::memory_order_release);
            }
            if (const auto er = shared->editorRequest.load(); er != editors) {
                editors = er;
                juce::MessageManager::callAsync([this] { showEditorOnMessageThread(); });
            }
            if (const auto rq = shared->resetRequest.load(); rq != resets) {
                resets = rq;
                plugin->reset();
            }
            if (const auto sq = shared->stateRequest.load(); sq != shared->stateDone.load()) {
                juce::MemoryBlock mb;
                if (juceProcessor) juceProcessor->getStateInformation(mb);
                else mb = initialState;
                stateFile.withFileExtension(".out").replaceWithData(mb.getData(), mb.getSize());
                shared->stateDone.store(sq);
            }
            const uint64_t req = shared->requestSeq.load(std::memory_order_acquire);
            if (req <= shared->doneSeq.load()) continue;
            const int frames = std::clamp(shared->frames, 0, static_cast<int>(l.size()));
            std::memcpy(l.data(), shared->in[0], sizeof(float) * static_cast<size_t>(frames));
            std::memcpy(r.data(), shared->in[1], sizeof(float) * static_cast<size_t>(frames));
            plugin->process(l.data(), r.data(), frames, shared->midi, std::clamp(shared->numMidi, 0, sandbox::kMaxMidi));
            std::memcpy(shared->out[0], l.data(), sizeof(float) * static_cast<size_t>(frames));
            std::memcpy(shared->out[1], r.data(), sizeof(float) * static_cast<size_t>(frames));
            shared->doneSeq.store(req, std::memory_order_release);
        }
        juce::MessageManager::callAsync([] { juce::JUCEApplicationBase::quit(); });
    }

    SharedRegion region;
    WakeSignal wake;
    sandbox::Shared* shared = nullptr;
    juce::File stateFile;
    juce::MemoryBlock initialState;
    int64_t parentPid = 0;
    std::string wantedUid;
    std::unique_ptr<juce::AudioPluginFormatManager> formats;
    juce::AudioProcessor* juceProcessor = nullptr;
    std::unique_ptr<ynot::PluginProcessor> plugin;
    std::vector<float> l, r;
    std::thread watchdog;
    std::unique_ptr<juce::DocumentWindow> editorWindow;
};

std::unique_ptr<SandboxChild> gChild;

}  // namespace

int runSandboxChild(const juce::StringArray& args) {
    gChild = std::make_unique<SandboxChild>();
    const int rc = gChild->start(args);
    if (rc != 0) gChild.reset();
    return rc;
}

void shutdownSandboxChild() { gChild.reset(); }

}  // namespace ynotapp
