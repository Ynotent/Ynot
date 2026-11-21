# Testing Ynot on a Mac (M1 real-device testing)

This is the first real-device testing for **M1 (macOS alpha)**. It works on any Apple Silicon
Mac. It has two parts: an automated script (about 10 minutes, mostly waiting), then about
20 minutes of hands-on checks that need your ears and hands.

## 1. Automated checks

You need **Xcode** (App Store) or the Command Line Tools (`xcode-select --install`), plus
**CMake** (`brew install cmake`).

```sh
git clone -b claude/universal-cross-platform-music-mufnfc https://github.com/Ynotent/Ynot.git ~/Ynot
cd ~/Ynot
./tools/macos-test.sh
```

The script builds everything in `build-macos/`, runs all automated tests on your Mac, and
writes **`ynot-macos-report.txt`**. Paste that file's contents back to Claude, whether it
passes or fails.

## 2. Hands-on checks

Launch the app with the `open ...` command printed at the end of the script. For each step,
note **OK** or what went wrong. Short notes are fine, e.g. "4: clip landed about half a beat
late".

Use **headphones** for steps 5 and 6. Speakers plus the built-in mic will feed back.

| # | Do this | Expected |
|---|---|---|
| 1 | Look at the status line (top right). | Shows `CoreAudio:` and your output device. If it warns that the output is a virtual device (BlackHole, Pro Tools Audio Bridge, Zoom, Teams...), click **Audio** and pick your speakers or headphones. Ynot remembers the choice next time. |
| 2 | **More/New Demo**, then **Play**. | You hear a steady tone (Vocals) and chords (Keys). Strings is silent because its plugin doesn't exist. The playhead moves; **Play** turns to **Stop**. |
| 3 | **Audio** → pick another output (e.g. headphones vs speakers) and buffer sizes 64 / 256 / 1024. Play each. | Sound follows the device with no crackles. Note any buffer size that crackles. |
| 4 | Turn on **Click** and play. | A click on every beat, accented on beat 1. |
| 5 | Arm **Vocals (R)**, turn on **Count-in**, press **Record**, clap on beats 1–4 of bar 2, press **Stop Rec**. | macOS asks for microphone permission the first time; allow it. A new clip appears. On playback, the claps line up with the clicks. *Write down "early", "late" or "on time".* |
| 6 | With Vocals armed, turn on **Monitor** and speak. | You hear yourself in the headphones. The status line shows a latency figure in ms; note it. |
| 7 | Arm **Keys (R)**, press **Keys**, then click notes on the on-screen piano **and** type on your computer keyboard (A W S E D F T G Y H U J K = one octave from middle C). | The synth plays both ways. If you have a USB/Bluetooth MIDI keyboard, plug it in **before** launching the app and play it too. |
| 8 | With Keys armed, **Record**, play a few notes, **Stop Rec**. Then press **Return**. | While recording, the Keys track shows red **● REC** and a red region grows. Afterwards a MIDI clip with your notes appears, in time. Return jumps back to the start. |
| 8b | **Quantize → While recording MIDI → MPC-style 16th swing 62%**, then record a few even 16th notes on Keys. Then right-click the clip → **Quantize → 1/16 straight**. | The recording lands on the swung grid; re-quantizing straightens it. Cmd+Z undoes it. |
| 8c | **Shift+drag** in the ruler over bar 2, arm a track, press **Record**, play through bars 1–3, **Stop Rec**. | Recording starts a bar early (pre-roll) but only bar 2 is replaced; bars 1 and 3 keep the old material. |
| 8d | **4/4 → 6/8**, turn on **Click** and play. | The ruler shows 6 beats per bar; the click is on every eighth with an accent on beat 1. |
| 9 | Drag a clip; drag its edges; double-click to split; select and press Delete; then **Cmd+Z** a few times. | Everything works, and undo steps back through it. Also try a trackpad pinch to zoom and a two-finger swipe to scroll. |
| 10 | **Scan Plugins**, if you have third-party AU/VST3 plugins installed. | The button counts "Scanning 12/140..." while the window stays usable, then a summary lists how many were found and any that failed. Ynot should **never** crash here: failing plugins are caught in a helper process. Note the counts and any failures listed. |
| 11 | **Save**, quit, relaunch and **Open** the project (`~/Documents/Ynot/Demo Song.ynot`). | Everything comes back exactly as saved. |
| 12 | Change the **Open this project on** menu between iPhone/iPad, Android and Windows. | The report explains which plugins won't work where, and what to do. |

Also note anything that feels wrong, looks wrong, or is confusing. That's as useful as the
checklist.

## What this does and doesn't prove

Passing this makes the M2 MacBook the first **real-device** result for macOS. M1's full gate
(see `docs/ROADMAP.md`) also needs:
- an Intel Mac;
- three audio interfaces;
- a one-hour stability run;
- a 20-plugin load and freeze suite.

So one MacBook moves macOS forward but doesn't complete M1 on its own.
