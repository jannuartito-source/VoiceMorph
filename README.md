# VoiceMorph

VST3 voice changer with independent pitch and formant control, plus an optional
neural voice-conversion stage.

Two things are going on here, and they are not the same thing:

**The DSP stage** separates your voice into an excitation (which carries pitch)
and a spectral envelope (which carries the formants — the resonances that tell
a listener how big your vocal tract is). It shifts them independently. This is
what makes a male↔female transformation sound like a person rather than a
chipmunk. It works out of the box, runs in about 30 ms, and needs no model.

**The neural stage** does what Vocoflex does: it listens to a few seconds of
any voice, reduces that voice to a single vector, and wears it. Zero-shot — no
training per target. Load two references and morph between them. This needs
three general-purpose ONNX models that you supply. See below.

---

## Building

Requirements: CMake 3.22+, a C++17 compiler, and git. JUCE 8 is fetched
automatically.

```bash
git clone <this repo> VoiceMorph && cd VoiceMorph
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

On Linux you also need the usual JUCE dependencies:

```bash
sudo apt install libasound2-dev libx11-dev libxext-dev libxrandr-dev \
                 libxinerama-dev libxcursor-dev libfreetype6-dev libcurl4-openssl-dev
```

`COPY_PLUGIN_AFTER_BUILD` installs the VST3 to the system plugin folder. A
standalone app is also built, which is the easiest way to test with a mic.

### Enabling the neural stage

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DVM_ENABLE_ONNX=ON \
      -DVM_ONNXRUNTIME_ROOT=/path/to/onnxruntime-1.17.0
```

Download a prebuilt ONNX Runtime from the
[onnxruntime releases page](https://github.com/microsoft/onnxruntime/releases)
and point `VM_ONNXRUNTIME_ROOT` at the extracted folder. Ship the shared
library next to the plugin binary.

---

## Getting a Windows .exe

Three routes, easiest first.

**1. Let GitHub build it.** Push this repo to GitHub and open the Actions tab.
`.github/workflows/windows.yml` builds on a Windows runner and produces
`VoiceMorph-Windows-Setup.exe` as a downloadable artifact. Nothing needs to be
installed locally. Tag a commit `v0.1.0` and the installer is attached to the
release automatically.

**2. Build locally.** Install Visual Studio 2022 with the "Desktop development
with C++" workload, then:

```bat
build-windows.bat            :: DSP only
build-windows.bat neural     :: also downloads ONNX Runtime and builds the AI stage
```

This produces the standalone exe, the VST3, and — if Inno Setup 6 is installed
— `dist\VoiceMorph-0.1.0-Windows-Setup.exe`.

**3. Just the installer,** from an existing build:

```bat
iscc installer\VoiceMorph.iss
```

### What the installer does

Standalone app to Program Files, VST3 to `Common Files\VST3`, `onnxruntime.dll`
alongside both, Start menu shortcut, optional desktop icon. Components are
selectable, so someone who only wants the plugin does not get the app.

The build is unsigned. Windows SmartScreen will show a blue warning on first
run — More info, then Run anyway. Getting rid of that means buying an EV code
signing certificate, several hundred dollars a year; it is not worth it until
you have users who are not you.

### The standalone app is probably what you want

`VoiceMorph.exe` runs on its own with no DAW at all. Launch it, click the
settings gear, pick your microphone as input and a virtual cable as output.
That is the whole setup. The VST3 only matters if you are already working
inside a DAW.

---

## Using it as a microphone

A VST3 is not a driver — it needs a host, and the host needs a virtual output
device for other apps to pick up.

| Platform | Virtual device | Host |
|---|---|---|
| Windows | VB-Cable or VoiceMeeter | Cantabile Lite, Reaper, Element |
| macOS | BlackHole | Hosting AU, Reaper, Element |
| Linux | PulseAudio null sink | Carla, Reaper |

Route: mic → host → VoiceMorph → virtual cable → Discord/OBS selects the cable
as its input.

The standalone app shortcuts most of this. Once VB-Cable is installed and the
machine restarted, **Send output to VB-Cable** finds the device and switches to
it; if it is not installed the same button opens the download page. In OBS,
pick **CABLE Output** as the device for Audio Input Capture.

VB-Cable is not bundled and cannot be. It is a signed kernel-mode driver, and
that is also why VoiceMorph cannot register itself as a microphone the way
Voicemod does: doing so needs a driver signed with an EV certificate plus
Microsoft attestation, which is an administrative barrier rather than a coding
one.

---

## Controls

**Pitch** transposes the excitation, ±24 semitones. The formants stay put, so
the speaker's apparent size does not change.

**Formant** transposes the vocal tract resonances, ±12 semitones. Up shortens
the tract; down lengthens it. This is the one that changes who you sound like.

**Gender** is a macro over both: +7 semitones of pitch and +4 of formant at
full travel. The formant range is deliberately smaller than the pitch range —
the average male/female vocal tract length differs by roughly 15%, which is
about four semitones, and overshooting it is what produces the cartoon voice.

**Link formants to pitch** turns the source/filter separation off and gives you
the old tape-speed effect. Useful for monsters, wrong for people.

**Detail** is the cepstral lifter length. Lower is a smoother envelope; higher
tracks narrower resonances but starts picking up the pitch harmonics, which
sounds buzzy. Raise it for low voices, lower it for high ones.

**CPU saver** drops the neural overlap from 50 % to 25 %. Each input sample is
then reprocessed 1.33 times instead of twice, so inference cost falls by about
a third with no change to latency. Block seams get slightly harder, which shows
up as faint periodic texture on sustained vowels.

If the load figure sits above 100 %, the fix is counterintuitive: **make the
neural block bigger, not smaller.** Each run has a fixed overhead, so short
blocks pay it more often. Load falls roughly as that overhead is amortised
across a longer window, and 320 ms costs far less CPU per second of audio than
80 ms does.

**Vocoder window** and **Neural block** trade latency against quality, and both
recompute the engine when changed, so expect a brief gap in the audio.

Total latency is the two added together, shown bottom right. The floor is about
96 ms with both at their fastest, plus whatever the audio driver adds. Going
below that needs a different neural architecture, not a smaller number: the
content encoder has to hear enough speech to know what sound is being made, and
starving it produces slurred, unstable output rather than fast output.

**Gate** runs before the vocoder. Spectral processing divides by small numbers
in quiet bins, so untreated room tone comes back as a metallic wash. Set the
threshold just above your noise floor.

The display shows the measured tract envelope in grey and the shifted one in
cyan. If the cyan curve is not moving when you turn Formant, no signal is
reaching the plugin.

---

## Supplying neural models

The plugin does not ship with models, and it cannot: the useful ones are
hundreds of megabytes and carry licences that forbid redistribution.

### The part that matters: zero-shot, not trained-per-voice

There are two families of voice conversion, and confusing them wastes weeks.

**Trained per voice** — RVC, so-vits-svc. One model *is* one target voice. To
sound like a specific person you collect ten-plus minutes of clean recordings
of them, rent a GPU, and train for hours. Excellent quality. No way to point
it at an arbitrary audio file and have it imitate that.

**Zero-shot** — FreeVC, OpenVoice, kNN-VC, Seed-VC. Three general models,
trained once by someone else, that work on voices they have never heard. A
speaker encoder listens to a few seconds of anybody and produces one vector,
typically 256 numbers, describing the identity. The decoder wears that vector
like a mask.

Vocoflex is the second kind. Its 2D map is a space of those vectors; dragging
the ball interpolates between the ones you loaded. That is why it can imitate
a file you drop in, instantly, with no training.

This plugin now implements the second kind.

### The three graphs

Put all three in one folder with exactly these names:

| File | Job | Shape |
|---|---|---|
| `content.onnx` | Speech to phonetic frames, identity stripped | `[1, samples]` at 16 kHz in, `[1, frames, D]` out |
| `speaker.onnx` | A few seconds of any voice to one identity vector | `[1, samples]` at 16 kHz in, `[1, 256]` out |
| `decoder.onnx` | Frames plus identity back to speech | frames + `[1, 256]` in, audio out |

Then click **Load models folder**, then **Voice A**, and point it at any
recording of the person you want to sound like. Five to fifteen seconds of
clean speech is the sweet spot — a whole song wastes time, and anything with
two people talking blurs the identity into an average of both.

Load a second recording into **Voice B** and the **MORPH** slider comes alive.
That slider is doing plain arithmetic on the two vectors: interpolate, then
renormalise back onto the unit sphere. The renormalise step matters. A blunt
average of two points on a sphere lands inside it, and voices from the
interior sound washed out and characterless — present but nobody in
particular.

### Where to get them

**FreeVC** is the most direct fit; its architecture is already content encoder
plus speaker encoder plus decoder, so the three exports map one to one.

**OpenVoice v2** separates tone colour from content even more cleanly, and its
tone-colour converter is close to a drop-in for `decoder.onnx`.

**Seed-VC** is newer, sounds better, and has a real-time mode, but is heavier.

All three publish PyTorch checkpoints. Getting from a checkpoint to
`something.onnx` means writing a short export script with
`torch.onnx.export`, marking the time axis dynamic. This is the part of the
project that still needs doing, and it is a real afternoon of work, not a
download.

### Then fix the tensor names

Export scripts disagree about what to call things. Open each `.onnx` in
[Netron](https://netron.app), read the actual input and output names off the
diagram, and edit the six lines near the top of `Impl` in
`Source/ai/NeuralVoiceConverter.cpp`. Names that do not match produce an
immediate ONNX Runtime exception, logged, with the audio passing through
unconverted — so a mismatch sounds like the neural stage doing nothing rather
than like a crash.

### Honest limits

- **Latency.** 200 ms blocks with 50 % overlap, on top of the vocoder's 32 ms.
  Fine for streaming and voice chat. Unusable for singing to a monitor.
- **CPU.** Watch the load figure in the footer. Above 100 % the worker cannot
  keep up, the FIFO drains, and the plugin silently falls back to the vocoder
  output — audible as a sudden change of character, not a dropout.
- **Block independence.** Consecutive blocks are generated without knowledge
  of each other. A neural vocoder invents phase, so two calls covering the same
  audio agree on content and disagree on phase — which is why the stage keeps
  only the newest hop from each call and crossfades a 5 ms seam, rather than
  overlap-adding. Overlap-adding two independently phased renders sums two
  uncorrelated signals and collapses correlation with the intended waveform to
  around 0.58: audibly, noise. Conditioning the decoder on the previous block's
  state would remove the seam entirely, and is a substantial piece of work.
- **The encoder's tail hole.** WavLM's convolution stack has a 400-sample
  receptive field at 16 kHz, so it produces no output for the last 400 samples
  of any window — a fixed 20 ms gap at the end of every decode, regardless of
  window length. The emitted region is backed off 30 ms from the edge so it
  never reads into it. Reading right up to the edge puts a 20 ms silence at the
  end of every hop, which at a 120 ms hop is an 8 Hz stutter.

- **No f0 tracking yet.** Pitch comes from the vocoder stage upstream rather
  than being handed to the decoder. For models that accept an f0 input this
  leaves quality on the table.

## Architecture

```
input ─┬──────────────────────────── dry delay ─────────┐
       │                                                 │
       └─ gate ─┬─ phase vocoder ─ align delay ──┐       ├─ mix ─ output
                │                                ├─ amt ─┘
                └─ neural (worker thread) ───────┘
```

The vocoder and the neural stage run **in parallel off the same clean signal**,
never in series. A content encoder is trained on human speech; handing it a
phase-vocoded chipmunk produces meaningless features, and the decoder
synthesises the meaninglessness perfectly. Running them in parallel also means
total latency is the longer of the two rather than their sum.

The audio thread never blocks. It pushes to and pops from lock-free FIFOs; all
inference happens on a worker thread. Delay lines keep the three paths sample-
aligned so the mix control does not comb.

| File | What it does |
|---|---|
| `dsp/CepstralEnvelope.h` | Formant estimation and warping |
| `dsp/PhaseVocoderEngine.*` | STFT, source/filter split, pitch shift, resynthesis |
| `dsp/NoiseGate.h` | Pre-vocoder gate with hysteresis |
| `ai/NeuralVoiceConverter.*` | Threading, FIFOs, resampling, speaker embeddings, ONNX inference |
| `PluginProcessor.*` | Parameters, signal chain, latency reporting |
| `PluginEditor.*` | UI and the envelope display |

---

## Licensing

JUCE is dual-licensed: GPLv3, or a paid licence for closed-source products.
RVC and so-vits-svc models carry their own terms, and many published voice
models are trained on material the trainer had no right to use. Check before
you ship anything, and be careful about cloning a real person's voice without
their consent — several jurisdictions now treat that as actionable regardless
of what the model licence says.
