# Ynot for Android: native shell (planned, milestone M5)

**Status: planned. Nothing in this folder runs yet.** See `docs/ROADMAP.md`.

What this shell will contain (everything else comes from `ynot_core`, built with the NDK and
exposed to Kotlin through JNI):
- `OboeDevice`: an `ynot::AudioDevice` built on AAudio, falling back to OpenSL ES. It supports
  USB class-compliant interfaces and measures round-trip latency per device.
- A plugin host for the built-in processors. AAP (Audio Plugins for Android) stays behind a
  feature flag while it is evaluated. VST3, AU and AUv3 do not exist on Android.
- Touch-first UI: Compact layout on phones and Expanded on tablets, with 48 dp targets.
- Google Play Billing and Android Keystore for tokens.

> In code, the audio and plugin pieces above are `app/src/JuceAudioDevice` and
> `app/src/JucePluginHost`, shared by all platforms and configured for each by JUCE.
