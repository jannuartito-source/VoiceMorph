# VoiceMorph

VST3 voice changer with independent pitch and formant control, plus an optional
neural voice-conversion stage.

Two things are going on here, and they are not the same thing:

**The DSP stage** separates your voice into an excitation (which carries pitch)
and a spectral envelope (which carries the formants — the resonances that tell
a listener how big your vocal tract is). It shifts them independently. This is
what makes a male↔female transformation sound like a person rather than a
chipmunk. It works out of the box, runs in about 30 ms, and needs no model.

**The neural stage** does what Vocoflex does: encode speech into a
speaker-independent representation, then resynthesise it with a different
identity. This one needs trained models that you supply. See below.

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

**Gate** runs before the vocoder. Spectral processing divides by small numbers
in quiet bins, so untreated room tone comes back as a metallic wash. Set the
threshold just above your noise floor.

The display shows the measured tract envelope in grey and the shifted one in
cyan. If the cyan curve is not moving when you turn Formant, no signal is
reaching the plugin.

---

## Supplying neural models

The plugin loads two ONNX graphs.

**1. Content encoder.** ContentVec or HuBERT, 16 kHz mono in, frame embeddings
out. Export from the RVC repo, or convert from a fairseq checkpoint. Input
shape `[1, 1, samples]`, output `[1, frames, 768]`.

**2. Decoder.** An RVC or so-vits-svc generator taking content frames, an f0
contour, and a speaker index. Both projects have ONNX export scripts.

Tensor names differ between export scripts. Open your files in
[Netron](https://netron.app), read the actual input and output names, and edit
`encoderInputNames` / `decoderInputNames` in
`Source/ai/NeuralVoiceConverter.cpp` to match.

### What is honestly missing

`runModel()` currently feeds the decoder a **flat f0 contour**. That produces
intelligible but monotone speech. A usable build needs a pitch tracker — RVC
uses RMVPE, which exports to ONNX cleanly — run over the 16 kHz block and
scaled by the pitch offset before it reaches the decoder. That is the single
highest-value thing to add next.

Also worth knowing:

- **Latency.** The neural stage works in 200 ms blocks with 50% Hann overlap,
  so it adds 200 ms on top of the vocoder's 32 ms. That is fine for streaming
  and unusable for singing along to a monitor. Shorter blocks cut latency but
  give the encoder less context and the quality falls off quickly below ~100 ms.
- **CPU.** Watch the load figure in the footer. Above 100% the worker cannot
  keep up, the output FIFO runs dry, and the plugin falls back to the vocoder
  output — audible as a sudden change in character rather than a dropout.
- **Overlap-add on neural output** is a compromise. Consecutive blocks are
  generated independently, so their phase does not necessarily agree, and the
  crossfade can comb slightly. Conditioning the decoder on the previous block's
  state is the proper fix and a substantial piece of work.

---

## Architecture

```
input ─┬─────────────────────── dry delay ──────────────┐
       │                                                 │
       └─ gate ─ phase vocoder ─┬─ dsp delay ────────────┤
                                │                        ├─ mix ─ output
                                └─ neural (worker) ──────┘
```

The audio thread never blocks. It pushes to and pops from lock-free FIFOs; all
inference happens on a worker thread. Delay lines keep the three paths sample-
aligned so the mix control does not comb.

| File | What it does |
|---|---|
| `dsp/CepstralEnvelope.h` | Formant estimation and warping |
| `dsp/PhaseVocoderEngine.*` | STFT, source/filter split, pitch shift, resynthesis |
| `dsp/NoiseGate.h` | Pre-vocoder gate with hysteresis |
| `ai/NeuralVoiceConverter.*` | Threading, FIFOs, resampling, ONNX inference |
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
