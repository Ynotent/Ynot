# One account and one subscription across devices

## Identity

- One Ynot account works on all four platforms.
- Sign-in options:
  - email and passkey;
  - Sign in with Apple, which is required on iOS when other social logins are offered;
  - Google.
- Identity is platform-neutral, so the same account works on a Windows PC and an iPhone.

## Purchases come from three channels

| Platform | Channel for **new** purchases | Code |
|---|---|---|
| iOS / iPadOS | Apple In-App Purchase (StoreKit 2) | `PurchaseChannel::AppleInAppPurchase` |
| Android | Google Play Billing | `PurchaseChannel::GooglePlayBilling` |
| macOS (Mac App Store build) | Apple In-App Purchase | `purchaseChannelFor(MacOS, true)` |
| macOS (direct download) | Web checkout | `purchaseChannelFor(MacOS, false)` |
| Windows | Web checkout (or Microsoft Store commerce if distributed there) | `PurchaseChannel::WebCheckout` |

## One entitlement

1. Each client sends its store receipt or purchase token to the **entitlement service**.
2. The service validates it **server-side**:
   - App Store Server API and App Store Server Notifications v2;
   - Google Play Developer API and Real-time Developer Notifications;
   - web payment provider webhooks.
3. The service stores `PurchaseRecord`s on the account.
4. `resolveEntitlement()` picks the highest live tier and takes these into account:
   - expiry;
   - refunds and revocations;
   - billing grace periods.

   It also records which store manages the subscription.

Every device then shows the same tier. A subscription bought on Android unlocks the Mac app.

## Store rules (must be checked by legal before each launch)

These rules vary by region and change over time. The code centralizes the policy in
`purchaseChannelFor()` so it can be updated in one place.

- On iOS and Android, digital subscriptions are sold through the platform's billing system
  unless a regional exception applies and the product opts into it.
- The app may honor a subscription bought elsewhere. Which "buy elsewhere" links or messages
  are allowed differs by store and country.
- Managing or cancelling a subscription sends the user to the store in
  `Entitlement::managedBy`.
- Prices can differ by channel because store fees differ. Entitlements are identical.

## AI features

`aiAvailability()` decides whether each feature can run and where (on-device or cloud), based on:
- subscription tier;
- platform;
- whether the device has an NPU;
- connectivity.

Every platform makes the same decision. Users get an honest reason when a feature is
unavailable, for example "needs an internet connection on this device". Cloud AI jobs process
audio only with the user's consent, and inputs are deleted after the job.
