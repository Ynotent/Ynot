#include "test.hpp"
#include "ynot/ynot.hpp"

using namespace ynot;

TEST("platform: plugin format matrix") {
    CHECK(platformLoadsFormat(Platform::MacOS, PluginFormat::VST3));
    CHECK(platformLoadsFormat(Platform::MacOS, PluginFormat::AudioUnit));
    CHECK(platformLoadsFormat(Platform::MacOS, PluginFormat::AUv3));
    CHECK(platformLoadsFormat(Platform::Windows, PluginFormat::VST3));
    CHECK(!platformLoadsFormat(Platform::Windows, PluginFormat::AudioUnit));
    CHECK(platformLoadsFormat(Platform::IOS, PluginFormat::AUv3));
    CHECK(!platformLoadsFormat(Platform::IOS, PluginFormat::VST3));
    CHECK(!platformLoadsFormat(Platform::IOS, PluginFormat::AudioUnit));
    CHECK(!platformLoadsFormat(Platform::Android, PluginFormat::VST3));
    CHECK(!platformLoadsFormat(Platform::Android, PluginFormat::AUv3));
    for (Platform p : allTargetPlatforms()) CHECK(platformLoadsFormat(p, PluginFormat::Builtin));
    CHECK_EQ(allTargetPlatforms().size(), size_t(4));
    CHECK(profileFor(Platform::Windows).audioApis.front() == AudioApi::ASIO);
    CHECK(profileFor(Platform::Android).audioApis.front() == AudioApi::AAudio);
}

TEST("layout: adapts to phone, tablet and desktop") {
    auto phone = decideLayout({Platform::IOS, 390, 844, false, false});
    CHECK(phone.layout == LayoutClass::Compact);
    CHECK(phone.navigation == Navigation::BottomTabs);
    CHECK_EQ(phone.minTouchTargetDp, 44.0);
    CHECK_EQ(phone.primaryWorkspaces.front(), std::string("record"));

    auto androidPhone = decideLayout({Platform::Android, 412, 915, false, false});
    CHECK_EQ(androidPhone.minTouchTargetDp, 48.0);

    auto ipadPro = decideLayout({Platform::IOS, 1366, 1024, true, true});
    CHECK(ipadPro.layout == LayoutClass::Expanded);  // never "Studio" chrome on a tablet
    CHECK_EQ(ipadPro.minTouchTargetDp, 24.0);         // trackpad attached
    CHECK(ipadPro.showKeyboardShortcutHints);

    auto narrowMacWindow = decideLayout({Platform::MacOS, 500, 700, true, true});
    CHECK(narrowMacWindow.layout == LayoutClass::Medium);
    auto studio = decideLayout({Platform::Windows, 1920, 1080, true, true});
    CHECK(studio.layout == LayoutClass::Studio);
    CHECK(studio.visibleMixerStrips > phone.visibleMixerStrips);
}

TEST("account: one entitlement from purchases across stores") {
    int64_t now = 1'000'000;
    std::vector<PurchaseRecord> recs{
        {Store::GooglePlay, Tier::Creator, now + 10, false, false},
        {Store::AppleAppStore, Tier::Pro, now - 10, false, false},  // expired
        {Store::Web, Tier::Pro, now + 5, true, false},              // refunded
    };
    auto e = resolveEntitlement(recs, now);
    CHECK(e.tier == Tier::Creator);
    CHECK(e.managedBy == Store::GooglePlay);
    recs[1].inGracePeriod = true;
    CHECK(resolveEntitlement(recs, now).tier == Tier::Pro);
    CHECK(!resolveEntitlement({}, now).active());

    CHECK(purchaseChannelFor(Platform::IOS, false) == PurchaseChannel::AppleInAppPurchase);
    CHECK(purchaseChannelFor(Platform::Android, false) == PurchaseChannel::GooglePlayBilling);
    CHECK(purchaseChannelFor(Platform::MacOS, true) == PurchaseChannel::AppleInAppPurchase);
    CHECK(purchaseChannelFor(Platform::MacOS, false) == PurchaseChannel::WebCheckout);
    CHECK(purchaseChannelFor(Platform::Windows, false) == PurchaseChannel::WebCheckout);
}

TEST("ai: features run where they can and degrade honestly") {
    auto desk = aiAvailability(AiFeature::StemSeparation, Tier::Pro, Platform::Windows, false, false);
    CHECK(desk.available && desk.runtime == AiRuntime::OnDevice);
    auto oldPhone = aiAvailability(AiFeature::StemSeparation, Tier::Pro, Platform::Android, true, false);
    CHECK(oldPhone.available && oldPhone.runtime == AiRuntime::Cloud);
    auto offline = aiAvailability(AiFeature::StemSeparation, Tier::Pro, Platform::Android, false, false);
    CHECK(!offline.available);
    auto tier = aiAvailability(AiFeature::StemSeparation, Tier::Creator, Platform::MacOS, true, true);
    CHECK(!tier.available);
    CHECK(aiAvailability(AiFeature::ChordAndKeyDetection, Tier::Free, Platform::IOS, false, false).available);
}
