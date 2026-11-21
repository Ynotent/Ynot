#include "ynot/quantize.hpp"

#include <algorithm>
#include <cmath>

namespace ynot {

bool isTripletGrid(int64_t g) { return g == kGrid1_4T || g == kGrid1_8T || g == kGrid1_16T; }

std::string gridName(int64_t g) {
    switch (g) {
        case kGrid1_1: return "1/1";
        case kGrid1_2: return "1/2";
        case kGrid1_4: return "1/4";
        case kGrid1_8: return "1/8";
        case kGrid1_16: return "1/16";
        case kGrid1_32: return "1/32";
        case kGrid1_4T: return "1/4T";
        case kGrid1_8T: return "1/8T";
        case kGrid1_16T: return "1/16T";
        default: return std::to_string(g) + " ticks";
    }
}

int64_t quantizeTick(int64_t tick, const QuantizeSettings& q) {
    if (q.gridTicks <= 0 || q.strength <= 0.0) return tick;
    const int64_t g = q.gridTicks;
    const double swing = isTripletGrid(g) ? 50.0 : std::clamp(q.swingPercent, 50.0, 75.0);
    // Candidate targets: grid steps, with every odd step moved by swing.
    auto target = [&](int64_t step) {
        const int64_t pairStart = (step >= 0 ? step / 2 : (step - 1) / 2) * 2 * g;
        const bool offbeat = (step % 2) != 0;
        return offbeat ? pairStart + static_cast<int64_t>(std::llround(2.0 * static_cast<double>(g) * swing / 100.0))
                       : step * g;
    };
    const int64_t base = static_cast<int64_t>(std::floor(static_cast<double>(tick) / static_cast<double>(g)));
    int64_t best = target(base);
    for (int64_t s = base - 1; s <= base + 2; ++s) {
        const int64_t t = target(s);
        if (std::llabs(t - tick) < std::llabs(best - tick)) best = t;
    }
    const double moved = static_cast<double>(tick) + static_cast<double>(best - tick) * std::clamp(q.strength, 0.0, 1.0);
    return static_cast<int64_t>(std::llround(moved));
}

int quantizeClip(MidiClip& clip, const QuantizeSettings& q) {
    int moved = 0;
    for (auto& n : clip.notes) {
        if (n.tick < 0 || n.tick >= clip.lengthTick) continue;  // hidden by a trim: leave alone
        const int64_t abs = clip.startTick + n.tick;
        const int64_t start = std::max<int64_t>(clip.startTick, quantizeTick(abs, q));
        int64_t length = n.lengthTick;
        if (q.quantizeLengths) {
            const int64_t end = std::max(start + q.gridTicks / 2, quantizeTick(abs + n.lengthTick, q));
            length = std::max<int64_t>(1, end - start);
        }
        if (start - clip.startTick != n.tick || length != n.lengthTick) ++moved;
        n.tick = start - clip.startTick;
        n.lengthTick = length;
    }
    std::sort(clip.notes.begin(), clip.notes.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.tick != b.tick ? a.tick < b.tick : a.pitch < b.pitch;
    });
    return moved;
}

const std::vector<QuantizePreset>& quantizePresets() {
    static const std::vector<QuantizePreset> presets = [] {
        std::vector<QuantizePreset> v;
        auto add = [&](std::string id, std::string name, std::string desc, int64_t grid, double swing) {
            QuantizeSettings s;
            s.gridTicks = grid;
            s.swingPercent = swing;
            v.push_back({std::move(id), std::move(name), std::move(desc), s});
        };
        // Straight grids.
        add("straight-1-4", "1/4 straight", "Quarter notes.", kGrid1_4, 50);
        add("straight-1-8", "1/8 straight", "Eighth notes.", kGrid1_8, 50);
        add("straight-1-16", "1/16 straight", "Sixteenth notes: the classic step-sequencer grid.", kGrid1_16, 50);
        add("straight-1-32", "1/32 straight", "Thirty-second notes, for fast rolls and hats.", kGrid1_32, 50);
        // Triplets (and the natural grids for 6/8, 9/8, 12/8 feels).
        add("triplet-1-4", "1/4 triplet", "Three per half note.", kGrid1_4T, 50);
        add("triplet-1-8", "1/8 triplet", "Three per beat: shuffle, 12/8 blues, swing-era feel.", kGrid1_8T, 50);
        add("triplet-1-16", "1/16 triplet", "Six per beat: trap hats and rolls.", kGrid1_16T, 50);
        // Sampler-style 16th swing: the well-known settings of classic hardware samplers.
        for (int sw : {54, 58, 62, 66, 71, 75})
            add("mpc-16-" + std::to_string(sw), "MPC-style 16th swing " + std::to_string(sw) + "%",
                "Sampler-style swing on 16ths (" + std::to_string(sw) + "%), the classic hip-hop groove grid.",
                kGrid1_16, sw);
        // Sampler-style 8th swing.
        for (int sw : {58, 66})
            add("mpc-8-" + std::to_string(sw), "MPC-style 8th swing " + std::to_string(sw) + "%",
                "Sampler-style swing on 8ths (" + std::to_string(sw) + "%).", kGrid1_8, sw);
        // Analog drum-machine shuffle (step-sequencer shuffle knob, approximated).
        add("analog-shuffle-light", "Analog drum machine shuffle: light", "Step-sequencer shuffle on 16ths, light (56%).",
            kGrid1_16, 56);
        add("analog-shuffle-medium", "Analog drum machine shuffle: medium",
            "Step-sequencer shuffle on 16ths, medium (62%).", kGrid1_16, 62);
        add("analog-shuffle-heavy", "Analog drum machine shuffle: heavy",
            "Step-sequencer shuffle on 16ths, heavy (70%).", kGrid1_16, 70);
        // 12-bit sampler drum machine feel: swung 16ths at a tighter setting.
        add("vintage-sampler-16-60", "Vintage 12-bit sampler 16th swing 60%",
            "Late-80s sampler/drum machine 16th swing (60%).", kGrid1_16, 60);
        // Early digital drum machine shuffle on 8ths/16ths.
        add("early-digital-16-58", "Early digital drum machine 16th shuffle 58%",
            "Early-80s digital drum machine shuffle on 16ths (58%).", kGrid1_16, 58);
        add("early-digital-8-62", "Early digital drum machine 8th shuffle 62%",
            "Early-80s digital drum machine shuffle on 8ths (62%).", kGrid1_8, 62);
        return v;
    }();
    return presets;
}

const QuantizePreset* findQuantizePreset(const std::string& id) {
    for (auto& p : quantizePresets())
        if (p.id == id) return &p;
    return nullptr;
}

const std::vector<std::pair<int, int>>& commonTimeSignatures() {
    static const std::vector<std::pair<int, int>> v = {{4, 4}, {3, 4}, {2, 4}, {5, 4}, {7, 4},
                                                       {6, 8}, {7, 8}, {9, 8}, {12, 8}};
    return v;
}

}  // namespace ynot
