// One account, one subscription, every device.
//
// Purchases can come from the Apple App Store (iOS + Mac App Store), Google
// Play, or direct web checkout (Windows / direct-download macOS). Each store's
// receipt is validated *server-side*; the server returns PurchaseRecords and
// this resolver turns them into a single entitlement every client agrees on.
// Clients must only show the purchase UI permitted on their platform (see
// docs/architecture/ACCOUNTS_AND_BILLING.md) — that policy lives in
// purchaseChannelFor().
//
// AI feature gating also lives here so every platform makes the same decision.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ynot/platform.hpp"

namespace ynot {

enum class Tier { Free = 0, Creator = 1, Pro = 2 };
enum class Store { AppleAppStore, GooglePlay, Web };

struct PurchaseRecord {
    Store store;
    Tier tier;
    int64_t expiresAtMs;  // 0 = non-expiring
    bool revoked = false; // refunded / chargeback
    bool inGracePeriod = false;  // billing retry: keep access per store rules
};

struct Entitlement {
    Tier tier = Tier::Free;
    int64_t expiresAtMs = 0;
    Store managedBy = Store::Web;  // where the user must go to manage/cancel
    bool active() const { return tier != Tier::Free; }
};

Entitlement resolveEntitlement(const std::vector<PurchaseRecord>& records, int64_t nowMs);

enum class PurchaseChannel {
    AppleInAppPurchase,   // StoreKit
    GooglePlayBilling,    // Play Billing Library
    WebCheckout,          // direct sale (Windows, direct-download macOS)
};
// The channel a client on `p` uses for *new* purchases. Distribution matters
// on macOS: Mac App Store builds must use StoreKit.
PurchaseChannel purchaseChannelFor(Platform p, bool macAppStoreBuild);

// ---------------------------------------------------------------- AI features

enum class AiFeature {
    StemSeparation,
    AutoMixAssist,
    MasteringAssist,
    VocalTuning,
    ChordAndKeyDetection,
    TextToMidiIdeas,
    TranscribeAudioToMidi,
};

enum class AiRuntime { OnDevice, Cloud };

struct AiAvailability {
    bool available;
    AiRuntime runtime;
    std::string reason;  // why unavailable / degraded
};

// Decides where (or whether) a feature runs given tier, platform, connectivity.
AiAvailability aiAvailability(AiFeature f, Tier tier, Platform p, bool online, bool deviceHasNpu);
std::string toString(AiFeature f);

}  // namespace ynot
