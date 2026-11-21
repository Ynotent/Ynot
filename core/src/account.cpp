#include "ynot/account.hpp"

namespace ynot {

Entitlement resolveEntitlement(const std::vector<PurchaseRecord>& records, int64_t nowMs) {
    Entitlement e;
    for (auto& r : records) {
        if (r.revoked) continue;
        bool live = r.expiresAtMs == 0 || r.expiresAtMs > nowMs || r.inGracePeriod;
        if (!live) continue;
        bool better = static_cast<int>(r.tier) > static_cast<int>(e.tier);
        bool sameTierLonger = r.tier == e.tier && e.tier != Tier::Free &&
                              (r.expiresAtMs == 0 || (e.expiresAtMs != 0 && r.expiresAtMs > e.expiresAtMs));
        if (better || sameTierLonger) {
            e.tier = r.tier;
            e.expiresAtMs = r.expiresAtMs;
            e.managedBy = r.store;
        }
    }
    return e;
}

PurchaseChannel purchaseChannelFor(Platform p, bool macAppStoreBuild) {
    switch (p) {
        case Platform::IOS: return PurchaseChannel::AppleInAppPurchase;
        case Platform::Android: return PurchaseChannel::GooglePlayBilling;
        case Platform::MacOS:
            return macAppStoreBuild ? PurchaseChannel::AppleInAppPurchase : PurchaseChannel::WebCheckout;
        case Platform::Windows:
        case Platform::Linux: return PurchaseChannel::WebCheckout;
    }
    return PurchaseChannel::WebCheckout;
}

namespace {
struct AiRule {
    Tier minTier;
    bool onDeviceDesktop;
    bool onDeviceMobile;      // without NPU
    bool onDeviceMobileNpu;
    bool cloudFallback;
};

AiRule ruleFor(AiFeature f) {
    switch (f) {
        case AiFeature::ChordAndKeyDetection: return {Tier::Free, true, true, true, false};
        case AiFeature::VocalTuning: return {Tier::Creator, true, true, true, false};
        case AiFeature::TranscribeAudioToMidi: return {Tier::Creator, true, false, true, true};
        case AiFeature::StemSeparation: return {Tier::Pro, true, false, true, true};
        case AiFeature::AutoMixAssist: return {Tier::Pro, true, true, true, true};
        case AiFeature::MasteringAssist: return {Tier::Pro, true, true, true, true};
        case AiFeature::TextToMidiIdeas: return {Tier::Creator, false, false, false, true};
    }
    return {Tier::Pro, false, false, false, false};
}
}  // namespace

AiAvailability aiAvailability(AiFeature f, Tier tier, Platform p, bool online, bool deviceHasNpu) {
    AiRule r = ruleFor(f);
    if (static_cast<int>(tier) < static_cast<int>(r.minTier))
        return {false, AiRuntime::OnDevice, "Requires a higher subscription tier."};
    bool mobile = p == Platform::IOS || p == Platform::Android;
    bool local = mobile ? (deviceHasNpu ? r.onDeviceMobileNpu : r.onDeviceMobile) : r.onDeviceDesktop;
    if (local) return {true, AiRuntime::OnDevice, ""};
    if (r.cloudFallback && online) return {true, AiRuntime::Cloud, "Runs in the cloud on this device."};
    if (r.cloudFallback) return {false, AiRuntime::Cloud, "Needs an internet connection on this device."};
    return {false, AiRuntime::OnDevice, "Not supported on this device."};
}

std::string toString(AiFeature f) {
    switch (f) {
        case AiFeature::StemSeparation: return "Stem separation";
        case AiFeature::AutoMixAssist: return "Mix assistant";
        case AiFeature::MasteringAssist: return "Mastering assistant";
        case AiFeature::VocalTuning: return "Vocal tuning";
        case AiFeature::ChordAndKeyDetection: return "Chord & key detection";
        case AiFeature::TextToMidiIdeas: return "Text-to-MIDI ideas";
        case AiFeature::TranscribeAudioToMidi: return "Audio-to-MIDI";
    }
    return "";
}

}  // namespace ynot
