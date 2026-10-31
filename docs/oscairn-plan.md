# Oscairn extension plan for Dexed

Status: proposal, 2026-10-30. Nothing in this document is implemented yet.

This branch (`oscairn`) of the [oscairn/dexed](https://github.com/oscairn/dexed) fork turns
Dexed into an 8-operator FM instrument for Oscairn. The target is everything a MONTAGE M /
MODX M FM-X Part can do as a sound engine, while keeping everything Dexed does today as a
DX7 instrument. It also covers what Oscairn asks of a plugin: the requirements and
recommendations in Oscairn's `docs/plugin-porting.md`, native sound contexts (RFC 0005), and
the test list.

The code facts below were read from the fork at commit `fea2f0f` ("fix(#48): mono-mode last
note priority"), which is the same commit as `asb2m10/dexed` `master` on 2026-10-30. Paths
are relative to the repository root. The FM-X facts come from the parameter tables transcribed
from Yamaha's published MONTAGE M Data List (in Jacob's research repository:
`docs/params/fmx-part.md`, `docs/params/performance-part.md`, `docs/params/awm2-element.md`,
`data/fmx_algorithms.json`, `docs/pfm-format.md`). Nothing here comes from Yamaha code,
binaries or disassembly.

## Contents

1. [Goal, scope and relation to RFC 0014](#1-goal-scope-and-relation-to-rfc-0014)
2. [How Dexed and MSFA are built today](#2-how-dexed-and-msfa-are-built-today)
3. [Gap table against an FM-X Part](#3-gap-table-against-an-fm-x-part)
4. [Oscairn integration](#4-oscairn-integration)
5. [DX7 compatibility](#5-dx7-compatibility)
6. [FM-X Part conversion](#6-fm-x-part-conversion)
7. [Milestones](#7-milestones)
8. [Upstreamability](#8-upstreamability)
9. [Licences and naming](#9-licences-and-naming)
10. [Risks and open questions](#10-risks-and-open-questions)

---

## 1. Goal, scope and relation to RFC 0014

**Goal.** One CLAP instrument, built from this fork, that:

- plays any FM-X Part converted from a MONTAGE M / MODX M Performance with no loss in the
  engine's parameters, matching Yamaha's behaviour within measured tolerances;
- plays any DX7 voice or 32-voice bank exactly as Dexed does today;
- meets every Oscairn host requirement and follows the recommended behaviour, including
  native sound contexts, CLAP notes with note IDs and `NOTE_END`, deterministic state, a Linux
  ARM64 build and the Oscairn test list.

**Out of scope for the engine** (as in RFC 0013 and RFC 0014): insertion, system and master
effects, Part EQ, volume, pan and sends at the mixer, zones and note/velocity limits, arpeggios,
scenes, Motion Sequencer, Super Knob, and controller assignment. Those are Performance-level in
Oscairn. The engine exposes parameters and per-operator switches that those systems target.

**What RFC 0014 said.** RFC 0014 recommended taking MSFA's operator, EG and scaling code as a
starting point and writing the voice structure, the operator graph and the spectral
oscillators inside Oscairn, as a new engine under Oscairn's AGPL licence. It noted that reusing
GPL-3.0 Dexed code would be licence-compatible but probably unnecessary.

**What forking Dexed changes.** Jacob has decided to fork Dexed instead. In practice:

- **The product is a separate plugin, not part of the Oscairn core.** It loads through CLAP,
  so under RFC 0001's plugin boundary it is a separate work and can stay GPL-3.0-or-later. It
  lives in its own repository, builds with its own CMake and JUCE, and Oscairn treats it like
  any third-party plugin. That also means it gets nothing from Oscairn's in-tree code for free:
  the RFC 0005 helper `plugins/common/SoundContexts.h`, Oscairn's allocation guard and test
  harness all sit in the Oscairn repository and must be copied or vendored (with licence care,
  see section 9).
- **We inherit a working plugin.** Parameters, state, DX7 SysEx import and export, cartridge
  management, the JUCE editor, MTS-ESP and Scala tuning, MPE, CLAP/VST3/AU builds and CI on
  three desktop platforms all exist on day one. RFC 0014's step 1 gate ("DX7 SysEx banks render
  like Dexed") becomes a regression gate against our own baseline instead of a reimplementation
  target.
- **We inherit Dexed's shape too.** The engine assumes six operators and the 155-byte DX7
  voice everywhere (section 2). RFC 0014's warning still stands: getting to FM-X means
  restructuring, not configuring.
- **The shared blocks need a licence decision.** RFC 0013 and RFC 0014 wanted the filter, FEG,
  LFO and key-on-delay blocks built once and shared between the sample engine and the FM
  engine. With the FM engine in a GPL plugin and the sample engine in the AGPL core, the
  shared code has to be licensed so both can use it (section 9).

**Recommendation: keep MSFA, restructure around it.**

- Keep MSFA's numeric core unchanged where it is DX7-accurate: `source/msfa/fm_op_kernel.*`,
  `sin.*`, `exp2.*`, `freqlut.*`, `env.*`, the scaling functions in `dx7note.cc`
  (`ScaleVelocity`, `ScaleRate`, `ScaleCurve`, `ScaleLevel`), `lfo.*`, `pitchenv.*`,
  `porta.*`. These carry Dexed's DX7 accuracy work and stay the "classic" behaviour.
- Replace the fixed voice (`Dx7Note`) and the fixed algorithm table (`FmCore::algorithms[32]`)
  with a data-driven voice: N operators (6 or 8), an operator graph compiled from data, and
  per-operator, per-voice and per-sound blocks added beside the MSFA ones.
- Separate the engine from JUCE. Today voice allocation, MIDI handling and rendering live in
  `source/PluginProcessor.cpp` inside the JUCE `AudioProcessor`. Move them into a headless
  engine library (plain C++, no JUCE) that the JUCE plugin, the CLAP direct-process path and
  the tests all link. This is what makes the Oscairn tests and sound contexts practical.
- Keep the JUCE plugin shell, editor and cartridge manager for desktop use, as in Dexed.
- Keep the engine core files Apache-2.0 where they derive from MSFA, so they stay reusable
  (section 9).

---

## 2. How Dexed and MSFA are built today

All of this was read from the code at `fea2f0f`.

### Build and formats

- `CMakeLists.txt` pulls in JUCE (`libs/JUCE`, pinned at `c352e24`, JUCE 8.0.11) and
  `libs/clap-juce-extensions` (pinned at `16e9d4c`).
- `source/CMakeLists.txt` builds Standalone, VST3 and (on Apple) AU with `juce_add_plugin`,
  and a CLAP with `clap_juce_extensions_plugin(... CLAP_ID "com.digital-suburban.dexed"
  CLAP_FEATURES instrument FM DX7)`. It sets `NEEDS_MIDI_OUTPUT TRUE`.
- There are no tests in the repository.
- `.github/workflows/project-pipeline.yml` builds Linux on `ubuntu-latest` (x86_64), Windows
  on `windows-2022` and macOS on `macos-14`. Release builds on macOS are universal
  (`CMAKE_OSX_ARCHITECTURES "x86_64;arm64"`). There is no Linux ARM64 job.

### The plugin wrapper (`source/PluginProcessor.*`)

- `DexedAudioProcessor` derives from `juce::AudioProcessor` and
  `clap_juce_extensions::clap_properties`. It does not use
  `clap_juce_audio_processor_capabilities`, so it gets none of that interface's CLAP
  features (CLAP note dialect, direct events, outbound events, direct process).
- Polyphony is fixed: `MAX_ACTIVE_NOTES = 16` `ProcessorVoice` entries, each owning a
  `Dx7Note*`. The voices are `new`ed in `prepareToPlay` and deleted in `releaseResources`.
- `keydown()` scores voices for stealing (+4 not playing, +2 key up, +1 same pitch, oldest
  `keydown_seq` on ties), handles portamento start (`initPortamento`), mono mode
  (`transferState` / `transferSignal`) and phase transfer for repeated pitches.
  `keyup()` finds the voice by pitch (or by channel in MPE mode).
- `processBlock()` renders in fixed chunks of `N = 64` samples (`source/msfa/synth.h`:
  `LG_N = 6`), carrying leftovers in `extra_buf`. MIDI events are applied once per chunk by
  `processMidiMessages(it, end, i)`, so event timing is quantised to the 64-sample grid.
- Each voice's int32 output is shifted and clipped to a 16-bit range per voice (`val >> 9`,
  then divided by `0x8000`) before being summed in float. The sum goes through `PluginFx`
  (one global filter on the mixed signal) and is copied to the right channel. The output is
  mono.
- `getTailLengthSeconds()` returns 0. `producesMidi()` returns true (SysEx output).
- One `Lfo lfo` is shared by all voices ("The original DX7 had one single LFO"). `lfo.keydown()`
  runs only when no key is held.
- Three render cores are selectable through `engineType`: `FmCore` (MSFA, "Modern"),
  `EngineMkI` (`source/engine/EngineMkI.*`) and `EngineOpl` (`source/engine/EngineOpl.*`). The
  constructor calls `setEngineType(DEXED_ENGINE_MARKI)`.

### The voice (`source/msfa/dx7note.*`)

- `Dx7Note` holds `Env env_[6]`, `FmOpParams params_[6]`, one `PitchEnv`, `basepitch_[6]`,
  a two-sample feedback buffer `fb_buf_[2]`, `ampmodsens_[6]`, `opMode[6]`, an operator
  on/off mask `opSwitch_`, and the algorithm number.
- `init()` and `update()` read the raw 156-byte voice directly by offset (`op * 21`,
  `patch[126..135]`, `patch[139]`, `patch[140]`, `patch[143]`, `patch[155]`). The six
  operators are stored in reverse order (operator 6 at offset 0), as in DX7 SysEx.
- `osc_freq()` computes log frequency: ratio mode uses `coarsemul[32]` plus
  `log(1 + 0.01 * fine)` and a measured detune curve; fixed mode uses
  `(coarse & 3) * 100 + fine` as a log-decade value. There is a `// TODO: pitch
  randomization` comment.
- `compute()` mixes LFO pitch modulation (`pitchmoddepth_`, `pitchmodsens_`) with controller
  modulation, the pitch EG, pitch bend (including MPE per-note bend and scale-aware bend),
  amplitude modulation (`ampmoddepth_`, per-operator `ampmodsens_`), EG bias, and portamento
  (`Porta::rates`), then calls `ctrls->core->render(...)`.
- `isPlaying()` is true while any carrier's envelope is active (`FmCore::isCarrier`).

### Operators and the 32 algorithms (`source/msfa/fm_core.*`, `fm_op_kernel.*`)

- `FmCore::algorithms[32]` encodes each algorithm as six flag bytes: output bus (none, bus 1,
  bus 2), add flag, input bus, and feedback in/out (`FmOperatorFlags`). Rendering walks the
  six operators in order through two 64-sample buses.
- `FmCore::render()` handles only single-operator feedback (`// todo: more than one op in a
  feedback loop`). `EngineMkI::render()` adds two- and three-operator feedback loops
  (`compute_fb2`, `compute_fb3`) for DX7 algorithms 6 and 4.
- `FmOpKernel::compute`, `compute_pure` and `compute_fb` render a sine operator with a gain
  ramp over the block. The NEON path is disabled (`#ifdef HAVE_NEONx` in
  `fm_op_kernel.cc`). The sine comes from `Sin::lookup` (1024-entry table with delta
  interpolation, `sin.h`).

### Envelopes, scaling, LFO, pitch EG

- `Env` (`source/msfa/env.*`): DX7 4-rate / 4-level EG. Rates 0–99 become qRate
  (`rates * 41 >> 6` plus rate scaling); `ACCURATE_ENVELOPE` adds measured static-segment
  times (`statics[]`, "I stopped measuring after R=76"). It works at 44.1 kHz and scales by a
  static `sr_multiplier`.
- Scaling (`dx7note.cc`): `ScaleLevel` (break point, left and right depth, the four curves
  −LIN, −EXP, +EXP, +LIN with `exp_scale_data`), `ScaleRate` (rate scaling 0–7),
  `ScaleVelocity` (sensitivity 0–7 with `velocity_data`).
- `Lfo` (`source/msfa/lfo.*`): six waves in this order: triangle, saw down, saw up, square,
  sine, S/H. Speed 0–99 through the measured `lfoSource[100]` table, delay, key sync.
- `PitchEnv` (`source/msfa/pitchenv.*`): one DX7 4-rate / 4-level pitch EG.
- `Porta` (`source/msfa/porta.*`): rate tables for normal and glissando portamento.

### Process-wide state

`Exp2::init()`, `Tanh::init()` and `Sin::init()` run in the processor constructor.
`Freqlut::init(sampleRate)`, `Lfo::init`, `PitchEnv::init`, `Env::init_sr` and
`Porta::init_sr` run in `prepareToPlay` and write **static, process-wide** tables and
multipliers. Two instances at different sample rates in one process share these.

### Parameters and state (`source/parameter/*`, `source/core/Program.*`)

- `DexedApvts.cpp` builds a JUCE `AudioProcessorValueTreeState`: one parameter per DX7 voice
  byte (via `MetaParameterID`, which carries the SysEx offset), operator parameters grouped
  as `Op1`…`Op6`, plus plugin-level parameters (output, cutoff, resonance, mono mode, the four
  controller routes, portamento, glissando, engine type, pitch bend). `ParameterDx` is an
  `AudioParameterInt`; waves and curves are `AudioParameterChoice`.
- `ParametersMapper.cpp` attaches listeners that write the value into `activeProgram` (the
  155+1 byte `Program`) through `setDxValue()`, which sets `refreshVoice`. The audio thread
  sees `refreshVoice` at the top of `processBlock` and calls `Dx7Note::update()` on live
  voices. `activeProgram` is written by the listener's thread and read by the audio thread
  without a lock. `setDxValue()` calls `panic()` when the transpose byte changes.
- `State.cpp` saves an XML `ValueTree` (`copyXmlToBinary`): the APVTS parameters, a
  `profile` with MIDI CC mappings, and the Scala and KBM text. Loading rejects any state whose
  `version` property differs from `DexedApvts::MODEL_VERSION` (it returns without loading).
  A legacy `dexedState` loader handles Dexed 1.0 states. The voice name and the cartridge are
  not among the saved parameters; whether the name survives a round trip has not been tested.
- `Cartridge` (`source/core/Cartridge.*`) and `SysexComm` (`source/midi/SysexComm.*`) handle
  DX7 32-voice banks and single-voice SysEx.

### CLAP (`libs/clap-juce-extensions` at `16e9d4c`)

Read from `src/wrapper/clap-juce-wrapper.cpp` and `include/clap-juce-extensions/
clap-juce-extensions.h` at that commit:

- Implements params, state, audio ports, note ports, latency, tail, render, timer, POSIX fd,
  track info, voice info and note name hooks, and GUI when the processor has an editor.
- One note input port when `acceptsMidi()`, one note output port when `producesMidi()`.
  Dialect is MIDI (plus MPE); CLAP dialect only if the processor opts in.
- `CLAP_EVENT_NOTE_ON` / `NOTE_OFF` are converted to JUCE MIDI messages; the note ID is
  dropped. Incoming `CLAP_EVENT_NOTE_END` is ignored. Nothing emits `NOTE_END` unless the
  processor uses outbound events.
- The capabilities interface offers `supportsDirectProcess()` / `clap_direct_process()`
  (process the raw `clap_process`, bypassing `processBlock`), `supportsDirectEvent`,
  `supportsOutboundEvents`, `supportsNoteDialectClap` and note expressions.
- We found no hook for a plugin to return its **own** CLAP extension from `get_extension`.
  That matters for Oscairn's `oscairn.sound-contexts/draft-4` (section 4).

---

## 3. Gap table against an FM-X Part

Legend: **Exists**: Dexed has the same parameter with the same range and meaning (behaviour
still to be verified by measurement). **Extend**: Dexed has the concept but the range,
resolution or meaning differs. **New**: Dexed has nothing.

Every row of `fmx-part.md` appears below, grouped. Part-level rows from
`performance-part.md` that act on the sound engine come at the end.

### 3.1 FM-X Part common (`3p 00 00`, `.pfm` block `fmx.common`)

| FM-X parameter | Range | Dexed today | Status | Plan |
| --- | --- | --- | --- | --- |
| Random Pan Depth | 0–127 | Output is mono | New | Per-voice stereo pan (shared block) |
| Alternate Pan Depth | L64–C–R63 | – | New | Same block |
| Scaling Pan Depth | −64–+63 | – | New | Same block |
| Key On Delay Time Length | 0–127 | – | New | Shared key-on delay block |
| Key On Delay Tempo Sync Switch | Off, On | – | New | Needs host transport (CLAP transport) |
| Key On Delay Note Length | 16th … 4th×8 (5–21) | – | New | Same |
| Pitch Velocity Sensitivity | −64–+63 | – | New | Per-voice pitch offset at note on |
| Random Pitch Depth | 0–127 | `// TODO: pitch randomization` in `osc_freq` | New | Per-voice; seeded deterministically (section 4) |
| Pitch Key Follow Sensitivity | −200%–+200% | Only Scala/KBM/MTS tuning | New | Applied before tuning lookup |
| Pitch Key Follow Center Note | C-2–G8 | – | New | |
| PEG Initial / Attack / Decay 1 / Decay 2 / Release Level | −50–+50 each | `PitchEnv`: 4 levels 0–99, L4 is both start and end | Extend | 5-level, 4-time pitch EG; classic `PitchEnv` kept for DX7 voices |
| PEG Attack / Decay 1 / Decay 2 / Release Time | 0–99 each | 4 rates 0–99 | Extend | Time table fitted by measurement |
| PEG Depth Velocity Sensitivity | 0–7 | – | New | |
| PEG Depth | 8, 2, 1, 0.5 oct | Fixed DX7 range (`pitchenv_tab`) | New | |
| PEG Time Key Follow Sensitivity | 0–7 | – | New | |
| 2nd LFO Wave | Triangle, Saw Down, Saw Up, Square, Sine, S/H | `Lfo` has the same six waves in the same order | Exists | Shapes verified by measurement |
| 2nd LFO Speed | 0–99 | `lfoSource[100]` (DX7, measured) | Extend | Same range; FM-X rate table to be measured |
| 2nd LFO Phase | 0°, 90°, 180°, 270° | – | New | |
| 2nd LFO Delay Time | 0–99 | `Lfo` delay 0–99 | Exists | Verify curve |
| 2nd LFO Key On Reset | Off, On | `lfoKeySync`; Dexed resets only when no key is held | Extend | Per-voice or shared to be determined by measurement |
| 2nd LFO Pitch Modulation Depth | 0–99 | `lfoPmDepth` | Exists | |
| 2nd LFO Amplitude Modulation Depth | 0–99 | `lfoAmpDepth` | Exists | |
| 2nd LFO Filter Modulation Depth | 0–99 | No filter | New | |
| Algorithm Number | 1–88 | 32 DX7 algorithms in `FmCore::algorithms` | Extend | Graph renderer, section 3.6 |
| Feedback Level | 0–7 | `fb_shift_`, 0–7 | Exists | Multi-operator loops: section 3.6 |
| LFO Speed Range | Normal, Extended | – | New | The Data List ties it to 2nd LFO Speed ("available only when LFO Speed Range is Normal") |
| LFO Extended Speed | 0–415 | – | New | |
| FM Depth, FM Harmonics, FM Attack, FM Decay, FM Sustain, FM Release, FM Texture | −99–+99 each | – | New | Macro offsets; semantics to be specified from the manual and measured |

### 3.2 FM-X filter (`3p 00 01`, `.pfm` block `fmx.filter`)

Dexed has one global post-mix filter (`source/PluginFx.*`: cutoff, resonance, gain, a 24 dB
and a 2-pole multimode stage). It is not per voice and has no envelope. Every row is **New**
and belongs to the shared AWM2-class filter / FEG block that RFC 0013 also needs.

| FM-X parameter | Range |
| --- | --- |
| Filter Type | LPF24D, LPF24A, LPF18, LPF18s, LPF12+HPF12, LPF6+HPF12, HPF24D, HPF12, BPF12D, BPFw, BPF6, BEF12, BEF6, DualLPF, DualHPF, DualBPF, DualBEF, LPF12+BPF6, Thru |
| Filter Cutoff Frequency | 0–1023 |
| Filter Cutoff Velocity Sensitivity | −64–+63 |
| Filter Resonance / Width | 0–127 (not for LPF6+HPF12) |
| Filter Resonance Velocity Sensitivity | −64–+63 |
| HPF Cutoff Frequency | 0–1023 |
| Distance (Dual types) | −128–+127 |
| Filter Gain | 0–255 |
| FEG Hold / Attack / Decay 1 / Decay 2 / Release Time | 0–127 each |
| FEG Hold / Attack / Decay 1 / Decay 2 / Release Level | −128–+127 (−9600–+9600 cents) each |
| FEG Depth | −64–+63 |
| FEG Time Velocity Sensitivity Segment | Attack, Atk+Dcy, Decay, Atk+Rls, All |
| FEG Time Velocity Sensitivity | −64–+63 |
| FEG Depth Velocity Sensitivity | −64–+63 |
| FEG Depth Velocity Sensitivity Curve | 0–4 |
| FEG Time Key Follow Sensitivity | −64–+63 |
| FEG Time Key Follow Center Note | C-2–G8 |
| Filter Cutoff Key Follow Sensitivity | −200%–+200% |
| Filter Cutoff Scaling Break Point 1–4 | C-2–E8 … D#-2–G8, BP1 < BP2 < BP3 < BP4 |
| Filter Cutoff Scaling Offset 1–4 | −128–+127 each |
| HPF Cutoff Key Follow Sensitivity | −200%–+200% |

The same list appears for AWM2 elements in `awm2-element.md` (`2p 04 ee`, `el.filter`), with
different defaults. One block serves both engines.

### 3.3 Operator controller switches (`3p 01 0o`, `.pfm` block `fmx.opsw`), per operator

| FM-X parameter | Range | Dexed today | Status | Plan |
| --- | --- | --- | --- | --- |
| Controller Set 1–32 Switch | Off, On each | Four fixed controller routes (wheel, foot, breath, aftertouch) with pitch, amp and EG-bias flags, applied to every operator (`Controllers` in `source/msfa/controllers.h`) | New | One 32-bit mask parameter per operator. Destinations are engine parameters; the assignments themselves are Performance-level (RFC 0013) |
| Operator Key Controller Set 1–4 Switch | Off, On each | – | New | 4-bit mask per operator |
| LFO Box 1–3 Switch | Off, On each | – | New | 3-bit mask per operator; gates the Part LFO destination boxes |

### 3.4 Operator (`3p 02 0o`, `.pfm` block `fmx.op`), per operator, 8 operators

| FM-X parameter | Range | Dexed today | Status | Plan |
| --- | --- | --- | --- | --- |
| Oscillator Key On Reset | Off, On | `oscKeySync` (voice byte 136), one switch for all operators | Extend | Per operator |
| Oscillator Frequency Mode | Ratio, Fixed | `mode` | Exists | |
| Tune Coarse | 0–31 | `frequencyCoarse` 0–31 (`coarsemul`); fixed mode uses `coarse & 3` | Extend | Ratio table to verify; fixed-mode meaning of 0–31 to measure |
| Tune Fine | 0–127 | `frequencyFine` 0–99 (`1 + 0.01 * fine`) | Extend | Different resolution; see section 5 on profiles |
| Detune | −15–0–+15 | `detune` 0–14 (centre 7) | Extend | Wider range; curve to measure |
| Pitch Key Follow Sensitivity (Fixed mode) | 0–99 | Fixed mode ignores the key | New | |
| Pitch Velocity Sensitivity | −7–0–+7 | – | New | |
| Spectral Form | Sine, All 1, All 2, Odd 1, Odd 2, Res 1, Res 2 | Sine only (`Sin::lookup`) | New | Section 3.7 |
| Spectral Skirt | 0–7 | – | New | |
| Spectral Resonance | 0–99 | – | New | |
| PEG Initial Level, PEG Attack Level | −50–0–+50 each | One voice-wide pitch EG | New | Per-operator 2-stage pitch EG |
| PEG Attack Time, PEG Decay Time | 0–99 each | – | New | |
| AEG Attack / Decay 1 / Decay 2 / Release (Hold) Level | 0–99 each | `egLevel` 1–4 | Exists | The release level is also the hold level |
| AEG Attack / Decay 1 / Decay 2 / Release Time | 0–99 each | `egRate` 1–4 (rates, 99 = fastest) | Extend | FM-X calls them times (init voice: attack time 0, release time 40). Table fitted by measurement; classic rate path kept |
| AEG Hold Time | 0–99 | – | New | Hold segment before attack, at the release (hold) level |
| AEG Time Key Follow Sensitivity | 0–7 | `rateScaling` 0–7 (`ScaleRate`) | Exists | Verify curve |
| Operator Level | 0–99 | `outputLevel` 0–99 | Exists | |
| Level Scaling Break Point | A-1–C8 | `breakpoint` 0–99, labelled from A-1 (`getBreakpointLabel`) | Exists | |
| Level Scaling Low / High Depth | 0–99 | `lScaleDepth` / `rScaleDepth` | Exists | |
| Level Scaling Low / High Curve | −Linear, −Exp, +Exp, +Linear | `lKeyScale` / `rKeyScale`, same four in the same order | Exists | Verify shapes |
| Level Velocity Sensitivity | −7–0–+7 | `keyVelocity` 0–7 | Extend | Negative values new |
| 2nd LFO Pitch Modulation Depth Offset | 0–7 | `pitchModSens` 0–7, one for the voice (byte 143) | Extend | Per operator |
| 2nd LFO Amplitude Modulation Depth Offset | 0–7 | `ampModeSens` 0–3 per operator (`ampmodsenstab[4]`) | Extend | Wider range, table to measure |
| Pitch Controller Sensitivity | −7–0–+7 | Controller routes apply equally to all operators | New | |
| Level Controller Sensitivity | −7–0–+7 | Same | New | |
| Part LFO Destination 1 / 2 / 3 Depth Ratio | 0–127 each | No Part LFO | New | |

### 3.5 Operator ratio and frequency (`7p 01 0o`)

| FM-X parameter | Range | Status | Plan |
| --- | --- | --- | --- |
| Operator Ratio | 00 00–18 7F | Not engine state (assumed) | This block is not in the `.pfm` block order in `pfm-format.md`. It looks like a read-out of the resulting ratio or frequency. The converter can use it as a cross-check when it is present in a SysEx dump. Meaning unverified |
| Operator Frequency | 00 00–15 7F | Same | Same |

### 3.6 Operators and algorithms

- **8 operators.** `Dx7Note` hard-codes 6 in array sizes and loops (`env_[6]`,
  `params_[6]`, `for (op < 6)`), `VoiceStatus` has `amp[6]`, `FmAlgorithm` has `ops[6]`,
  and operator parameters are addressed by DX7 byte offset (`op * 21`). The new voice holds
  8 operator slots. A DX7 voice uses 6 of them, mapped from the reversed DX7 order.
- **88 algorithms.** `fmx_algorithms.json` lists, per algorithm, the carriers, the
  modulator → targets map, and the feedback path. A check over the file (2026-10-30) found:
  every algorithm has a feedback path; 86 are self-feedback; two are multi-operator loops
  (algorithm 12: 5 → 3 around the chain 3 > 4 > 5; algorithm 14: 4 → 3 around 3 > 4);
  16 algorithms have a modulator with more than one target; the largest fan-in is 7; and all
  401 modulator → target edges go from a lower operator number to a higher one, so ascending
  operator order is a valid render order (only the two multi-operator feedback paths run the
  other way). Algorithm 65 has one ambiguous junction noted in the file.
- **Plan.** Replace the two-bus flag table with a compiled graph: at preparation time, each
  algorithm (DX7's 32 and the 88) becomes a topologically sorted operator list (for the 88,
  simply ascending operator number), each
  operator's list of input operators, a carrier mask and a feedback descriptor. Rendering
  gives each operator its own 64-sample output buffer and sums its inputs before calling the
  kernel. Self-feedback reuses `FmOpKernel::compute_fb`. Multi-operator loops render the
  loop's operators sample by sample, as `EngineMkI::compute_fb2` / `compute_fb3` do for DX7
  algorithms 6 and 4. The 32 DX7 algorithms become data in the same format, and the classic
  render must stay bit-identical to today's (milestone gate).
- `FmCore::isCarrier` becomes a lookup in the compiled graph; `isPlaying()` keeps its meaning.

### 3.7 Spectral forms

FM-X operators can produce six non-sine spectra (All 1, All 2, Odd 1, Odd 2, Res 1, Res 2),
shaped by Skirt (0–7) and, for the Res forms, Resonance (0–99). How these are generated is not
published. Plan, following RFC 0014's open question:

1. Measure first: render each form at fixed pitch for each Skirt value and a sweep of
   Resonance, with the operator as an unmodulated carrier, and fit harmonic amplitudes and
   phases.
2. Implement as band-limited single-cycle tables in MSFA's phase format (Q24 phase, like
   `Sin::lookup`), one set per form and Skirt, with Resonance as a parameterised family or
   interpolation between tables. Tables are built at preparation time, never in `process`.
3. Extend the kernel to take a table pointer instead of always calling `Sin::lookup`. Keep the
   sine path untouched for classic voices.
4. Check behaviour as a modulator as well as a carrier (FM of a non-sine operator), and
   measure aliasing at high pitch against renders.

### 3.8 Part-level parameters that act on the engine (`performance-part.md`)

FM-X Parts also use these Part parameters. They belong to the sound, so the engine has them.

| Part parameter | Range | Dexed today | Status |
| --- | --- | --- | --- |
| Mono/Poly Mode | Mono, Poly | `monoMode` | Exists (verify note priority and legato against measurement) |
| Key Assign Mode | Single, Multi | – | New |
| Portamento Switch | Off, On | CC 65 (`portamento_enable_cc`) | Extend (patch parameter) |
| Portamento Time | 0–127 | `portamentoTm` 0–99 | Extend |
| Portamento Mode | Fingered, Full-time | – | New |
| Portamento Time Mode | Rate 1, Time 1, Rate 2, Time 2 | Rate tables only (`Porta::rates`) | New |
| Pitch Bend Range Upper / Lower | −48–+24 each | `pitchBendUp` / `pitchBendDown` 0–48 | Extend (signed) |
| Detune | −12.8–+12.7 Hz | `masterTuneAdj` (global, ±1 semitone) | New (Hz offset) |
| Note Shift | −48–+48 semitones | `transpose` (DX7 byte 144, ±24) | Extend |
| Micro Tuning Scale / Root | ET, Pure Major … User, Library | Scala/KBM files, MTS-ESP | Extend (converter writes Scala data for built-in scales) |
| Velocity Sensitivity Depth / Offset | 0–127 each | `normalizeDxVelocity` preference only | New |
| Part LFO: Phase, Wave (13 incl. User), Speed 0–63, Tempo Sync and Tempo Speed, Delay, Fade In, Hold, Fade Out, Key On Reset (Off, Each-On, 1st-On), Destination 1–3 and Depth 1–3, User LFO Cycle, Slope and 16 Step Values | see `part.lfo` | – | New (shared LFO block; per-operator depth ratio and LFO Box switches from 3.3 and 3.4) |
| AEG Attack / Decay / Sustain / Release offsets, FEG Depth, Filter Cutoff and Resonance offsets | −64–+63 each | – | New (Part-level offsets; FEG A/D/S/R offsets are AWM2-only per the Data List) |
| Legato Slope | 0–7 | – | Not for FM-X (Data List) |

Volume, pan, sends, output select, EQ, insertion connection, note and velocity limits, arpeggio,
scene, motion sequencer and controller boxes stay at the Performance level.

---

## 4. Oscairn integration

Checked against every item in Oscairn's `docs/plugin-porting.md`. "Today" means the fork at
`fea2f0f`. Nothing in this section has been run through Oscairn's scanner or renderer yet.

### 4.1 Host requirements (the load checks)

| Requirement | Today | Work |
| --- | --- | --- |
| `clap_entry` with a plugin factory | Provided by clap-juce-extensions | Confirm with `oscairn-plugin-scan` |
| Complete plugin interface | Provided by the wrapper | Confirm with the scanner |
| Audio or note outputs | Stereo audio output (mono allowed by `isBusesLayoutSupported`) and a note output port, because `producesMidi()` returns true | Drop the note output in the Oscairn build (section 4.3) |
| ≤ 64 channels, ≤ 16 note ports | 2 channels, 1 in / 1 out | None |
| Complete params extension; unique IDs; finite min ≤ default ≤ max; ≤ 32,768 | JUCE parameters through the wrapper; how the wrapper assigns clap IDs was not checked | Check for duplicate IDs with the scanner after adding ~500 parameters |
| Complete state and audio-ports extensions | Provided | None |
| Activates at the host's rate and maximum block | `prepareToPlay` allocates voices and resets static tables | Per-instance tables (4.6) |
| Audio port layout constant while loaded | Mono or stereo chosen by the host | Fix the Oscairn build at stereo, since pan modes need it |
| macOS desktop and Linux ARM64 device builds | macOS universal; Linux x86_64 only | Add a Linux ARM64 CI job (4.8) |

### 4.2 Deterministic, architecture-independent state

Today: XML from JUCE `ValueTree` with floats written as text, MIDI CC mappings and Scala/KBM
text; strict version check that silently ignores other versions. No test proves the bytes are
identical across machines.

Plan:

- A new versioned state format written by the engine library, not by APVTS: a header with
  format name and version, then every engine parameter as (stable ID, value) in ID order,
  integers stored as integers, any float written with the classic locale and round-trip
  precision (as Oscairn's Basic Synth does), the voice name, the Scala/KBM text, and the
  sound-context snapshot (4.5). Same bytes on macOS and Linux ARM64 for the same patch.
- Loading accepts the current version, migrates older ones, and accepts Dexed 1.1 and 1.0
  states through the existing loaders. A state it cannot read is reported, not silently
  ignored.
- Ports, note dialect and parameter list never change with the state.
- MTS-ESP: when an MTS master is present, `osc_freq` takes tuning from it, outside the state.
  The Oscairn build turns MTS-ESP off by default so a saved Performance renders the same
  everywhere. (Option, not a removal; the desktop Dexed build keeps it on.)
- Random pan and random pitch use a per-instance generator with a fixed seed reset on
  activation, so offline renders are reproducible.

### 4.3 CLAP notes, note IDs and `NOTE_END`

Today the wrapper turns CLAP notes into MIDI and drops the note ID; `NOTE_END` is never sent;
the plugin offers MIDI and MPE only.

Plan: implement `supportsDirectProcess()` / `clap_direct_process()` from
`clap_juce_audio_processor_capabilities` in the Oscairn build, and drive the headless engine
from the raw CLAP events:

- Note on/off/choke with note ID, port, channel and key, honouring the CLAP `-1` wildcards
  (as RFC 0005 states for the Basic Synth). A voice keeps its note ID for its whole life.
- `NOTE_END` (with the original note ID) when a voice's carriers have finished, and when a
  voice is stolen.
- Per-note expressions (tuning, volume, pan, pressure, brightness) where the engine has a
  matching destination. MIDI and MPE input stay supported.
- Parameter events applied at their sample offset. Events still land on the 64-sample control
  grid of the MSFA EG and LFO; the plan is to split rendering at event offsets so note starts
  are sample accurate, and to verify that output is identical for 37- and 512-frame blocks.
- Oscairn build: `producesMidi()` returns false, so there is no note output port. SysEx output
  to DX7 hardware stays in the desktop build. Oscairn's instant sound switching for ordinary
  plugins only accepts instruments with no note output, so this matters even before native
  sound contexts.

### 4.4 Parameter metadata and module paths

Today: JUCE groups `Op1`…`Op6` with separator `|`; names generated from IDs ("OP1 EG RATE1");
ints, choices and bools give stepped parameters; break points have note-name text.

Plan:

- Stable CLAP parameter IDs that never change between versions (explicit numbers, not hashes
  of names), checked by a test.
- Module paths: `Common`, `Pitch`, `Pitch/EG`, `LFO 1`, `LFO 2`, `Filter`, `Filter/EG`,
  `Amp`, `Voice`, `Operator 1` … `Operator 8` with sub-modules `Frequency`, `Envelope`,
  `Pitch EG`, `Scaling`, `Wave`, `Modulation`. Confirm the wrapper turns JUCE group paths into
  CLAP module paths; if not, set them in the direct path.
- Value text for every parameter (note names, ratios such as "1.00", "0.50", fixed frequencies
  in Hz, times in ms where fitted, cents, wave names). Stepped flag on every enumerated or
  integer parameter. Read-only flag on any read-out (for example a computed ratio).
- Hide the classic-only parameters when a sound uses the extended profile and vice versa only
  through the editor description, never by changing the parameter list.

### 4.5 Native sound contexts (RFC 0005)

Today: none. Dexed has one `activeProgram`, one LFO and one voice pool.

Plan:

- **Engine.** A fixed set of eight context slots (the Basic Synth's number), each holding a
  complete patch, its Part-level state (Part LFO, 2nd LFO when shared, key-on-delay clock,
  portamento and mono state) and its parameter values. One shared, preallocated voice pool
  (size set by measurement on the target hardware, at least Dexed's 16). A voice keeps its
  owning context, key, channel and note ID through a sound change. Each context sums its
  voices; contexts sum to the output.
- **Contract.** Prepare on the control thread into a free preallocated slot (validate all
  values first; reject the whole preparation on any bad value; write a diagnostic into the
  message buffer). Select through a bounded queue applied at the start of the next process
  call, before that call's events, for one channel or all. Discard only when no channel,
  queued selection, voice or tail pins the context. IDs never reused. Parameter events edit
  the contexts selected for their channel. The optional exclusive `state` snapshot saves all
  slots and selections.
- **Patch text.** The draft-3 patch text can carry a native patch (the state format of 4.2 as
  text) or a DX7 single-voice SysEx in hex, so the host can prepare a DX7 voice directly.
- **Exposing the extension.** The pinned clap-juce-extensions has no hook for a plugin's own
  `get_extension` entries. Options, in order of preference: (a) add a small hook to
  clap-juce-extensions (upstreamable to free-audio); (b) wrap the plugin's `get_extension` in
  our CLAP entry; (c) for the Oscairn build only, a thin CLAP entry that skips the JUCE wrapper
  and drives the headless engine directly. Decide in milestone M3.
- Reuse the rules and tests of Oscairn's `plugins/common/SoundContexts.h` and
  `core/engine/tests/sound_contexts.cpp`; copy with licence care (section 9).

### 4.6 Real-time safety

Found in the code today:

| Issue | Where | Fix |
| --- | --- | --- |
| Static tables rewritten on activation, shared by every instance in the process | `Freqlut::init`, `Lfo::init`, `PitchEnv::init`, `Env::init_sr`, `Porta::init_sr` in `prepareToPlay`; `Exp2::init`, `Tanh::init`, `Sin::init` in the constructor | Sample-rate-independent tables built once (thread-safe static init); sample-rate-dependent values per instance |
| `std::vector::push_back` on the audio thread | `queueOutputMidiMessage` (`outputMidiMessageQueue`), called from commands run in `processBlock` | Preallocated ring; no MIDI output in the Oscairn build |
| Patch bytes written by parameter listeners and read by the audio thread without synchronisation | `setDxValue` writes `activeProgram`; `processBlock` reads it after `refreshVoice` | Parameter changes reach the engine as events or through a lock-free single-producer queue; the audio thread owns the live patch |
| `panic()` from a non-audio thread | `setDxValue` on transpose change | Queue a reset event instead |
| Debug logging in note handling | `TRACE` in `keyup` (compiled only with `DEBUG`) | Keep out of release; no logging in `process` |
| State read before it was ever written (found in M0; the renders of the same voice changed from run to run) | `Dx7Note::fb_buf_` (operator feedback history), `Lfo::phase_`, `randstate_` and `delaystate_` (free-running and S/H LFO before the first synced key-down), `Controllers::portamento_gliss_cc` (portamento rate table until the glissando parameter changed) | Fixed in M0: zero-initialised. The M0 baseline renders of unmodified Dexed that were reproducible are unchanged by the fix |
| Possible MTS-ESP calls per block | `MTS_HasMaster`, `updateBasePitches` in `processBlock` | Off by default in the Oscairn build (4.2) |

Rule for new code: preparation (tables for spectral forms, filter coefficients tables, compiled
algorithms, context patches) happens off the audio thread into preallocated memory;
`process` never allocates, locks, logs or touches files.

### 4.7 Shared filter, FEG and LFO blocks

Built once for RFC 0013 and RFC 0014 as a small standalone C++ library with no JUCE and no
Oscairn dependencies, so the fork and the Oscairn Sampler can both compile it. Licence: section
9. If the Sampler's blocks exist first, the fork vendors them at a pinned revision; if the fork
gets there first, the blocks are written in that library from the start, not inside
`source/msfa/`.

### 4.8 Linux ARM64 build

- Add a CI job on an ARM64 Linux runner (or cross-compile with a sysroot) building the CLAP
  with `-DDEXED_SKIP_VST3=ON` if needed, no JACK, no ALSA (`DEXED_NO_ALSA`), and the
  Standalone off. Run the tests there.
- The NEON kernel is disabled today (`HAVE_NEONx`). Measure CPU per voice on the device first;
  add NEON or auto-vectorised kernels only if needed, behind tests that compare with the scalar
  kernel.

### 4.9 Editor descriptions (planned in Oscairn)

Oscairn does not show plugin windows yet and generates knob pages from parameters. So the
parameter metadata (4.4) is the editor for now. When RFC 0011 defines editor descriptions, add
one for: the algorithm graph, operator envelopes (AEG with hold, operator PEG), the pitch and
filter EGs, the filter response, and the spectral form preview. The JUCE editor stays for the
desktop build; it needs new pages for operators 7–8 and the new sections, but that is not part
of the Oscairn gate.

### 4.10 Presets

Oscairn does not list CLAP presets. DX7 cartridges reach Oscairn through the converter or as
patch text in a sound context (4.5). The cartridge manager stays in the desktop editor.

### 4.11 Tests

None exist today. The plan adds a `tests/` tree built with CTest against the headless engine and
the CLAP binary:

| Test | What it checks |
| --- | --- |
| Offline render determinism with `oscairn-render` | Same output for the same input across runs, 37- and 512-frame blocks, 44.1/48/96 kHz, and two simultaneous instances at different rates |
| No allocation while rendering | An allocation guard (as in Oscairn's `prepared_instances.cpp`) around `process`, including note on/off, voice stealing, parameter edits, context selection |
| State round trip | Save → load → save gives identical bytes; render after load equals render before; Dexed 1.0/1.1 states load |
| Worker tests | The same renders with the plugin in an Oscairn worker process (`*-workers` pattern) |
| Sanitizers | ASan/UBSan on all tests; TSan on concurrent prepare/select/discard and parameter edits during rendering |
| CLAP notes | Note IDs, wildcards, repeated keys, `NOTE_END` timing, stolen voices, sustain |
| Sound contexts | Held notes keep their sound across a switch, queue exhaustion, stale IDs, discard rules |
| Algorithm topology | For each of the 32 + 88 graphs, solo each operator and check which operators are audible and which modulate which |
| DX7 regression | Renders of generated (not Yamaha) DX7 voices compared with the M0 baseline |
| Measurement suites | Section 7; run locally against the user's own ESP renders, never in CI with Yamaha audio |

---

## 5. DX7 compatibility

DX7 voices and banks stay first-class:

- **Profiles, not approximations.** Where DX7 and FM-X differ in resolution or table (fine tune
  0–99 vs 0–127, detune ±7 vs ±15, EG rates vs times, amp-mod sensitivity 0–3 vs depth offset
  0–7, the DX7 pitch EG, the LFO speed table, velocity 0–7 vs ±7), one value cannot hold both
  without loss. Each sound therefore has a **profile** parameter: *Classic* (6 operators, DX7
  tables, exactly today's MSFA code paths) or *Extended* (8 operators, FM-X ranges and
  measured tables). A DX7 voice imports as Classic and renders bit-identically to Dexed. An
  FM-X Part converts as Extended.
- **Classic is a strict subset in structure.** Classic sounds use the same voice, graph
  renderer, filter (set to Thru), pan (centre) and LFO blocks with the classic settings, so new
  features (filter, pan, Part LFO, key-on delay) are available to DX7 voices too without
  changing their default sound.
- **Keep:** `Cartridge` and `SysexComm` import/export, the 1.0 and 1.1 state loaders, Scala
  and KBM tuning, MPE, the Modern / Mark I / OPL render choices (Classic profile only), the
  per-voice output quantisation and other DX7 quirks inside Classic.
- **Open:** whether Extended should keep any DX7 quirk (RFC 0014's open question). Default:
  Extended uses fitted FM-X tables and no DX7 quantisation, unless measurement shows FM-X has
  the same behaviour.

---

## 6. FM-X Part conversion

The converter belongs to Oscairn's Performance converter (RFC 0008), not to this fork. This
section fixes what it produces for this engine. It reads the `.pfm` container as described in
`pfm-format.md` and the parameter tables of `fmx-part.md` and `performance-part.md`.

- **Finding the Part.** A Part record is `u32 1`, an engine byte (`2` = FM-X), then the Part's
  common blocks (`name`, `p1`, `p2`, `p3`, insertion A and B, `arp`, `lfo`, `zone`,
  `keyctrl`, scenes, knob names, controller boxes, lanes). After all Part records, each Part's
  engine data follows in Part order; for FM-X: `common`(82) `filter`(70) and eight
  { `ctrl-switch`(39) `operator`(76) }.
- **Values.** 2-byte parameters are little-endian u16 holding the 14-bit value
  (`MSB * 128 + LSB`). Bipolar values are offset: `−64–+63` with default `00 40` is `raw − 64`;
  `−128–+127` / `01 00` and `−99–+99` / `01 00` are `raw − 256`. The engine stores the
  displayed value (for example −64…+63), so conversion is offset removal, never rescaling.
- **Mapping.** The Extended profile uses the same ranges as the Data List, so most rows map one
  to one:

  | Source block | Engine target |
  | --- | --- |
  | `fmx.common` | Part common: pan modes, key-on delay, pitch, Part PEG, 2nd LFO, algorithm, feedback, LFO speed range, macros |
  | `fmx.filter` | Filter and FEG block |
  | `fmx.opsw` × 8 | Per-operator controller-set (32-bit), key-controller (4-bit) and LFO-box (3-bit) masks |
  | `fmx.op` × 8 | Operator *n* (FM-X operator numbers are kept; the algorithm graphs use the same numbering) |
  | `part.p1` | Mono/poly, key assign, portamento switch |
  | `part.p2` | Velocity sensitivity depth and offset; AEG and filter offsets |
  | `part.p3` | Pitch bend range, detune (Hz), note shift, portamento time, mode and time mode, micro tuning scale and root |
  | `part.lfo` | Part LFO block |

- **Not engine parameters.** Controller boxes, scenes, arpeggios, lanes, insertion effects, EQ,
  volume, pan, sends and zones go to the Performance-level parts of Oscairn's converter. The
  per-operator switches tell those systems which operators each controller set and LFO box
  reaches.
- **Report.** The conversion report lists any value the engine cannot represent, against
  section 3's table, so "no loss" is checked per Part.
- **SysEx.** A bulk dump carries the same blocks (`pfm-format.md`); the `7p 01 0o` ratio and
  frequency block, if present, is checked against the computed operator frequency, not stored.
- **DX7.** A DX7 single voice or 32-voice bank converts to a Classic sound through the existing
  `Cartridge` code.

---

## 7. Milestones

Each milestone ends with a test gate. Behaviour is specified as RFC 0013 says: documentation
first, then black-box measurement of renders from Jacob's legally obtained ESP plugin or
hardware, with per-feature tolerances. Reference renders and any Yamaha audio stay out of the
repository; fitted tables and the fitting scripts are committed. DX7 regression fixtures are
generated voices, not Yamaha ROM voices.

| # | Deliverable | Gate |
| --- | --- | --- |
| M0 | Fork hygiene: `oscairn` branch, headless engine library split out of `PluginProcessor.cpp` with no behaviour change, `tests/` with CTest, Linux ARM64 CI job, baseline renders of generated DX7 voices from unmodified Dexed | Builds on macOS, Linux x86_64 and Linux ARM64; `oscairn-plugin-scan` lists the CLAP; `oscairn-render` renders it; engine-library renders are bit-identical to the unmodified plugin |
| M1 | Oscairn integration on the unchanged DX7 engine: direct CLAP process, note IDs and `NOTE_END`, no note output in the Oscairn build, real-time fixes of 4.6, per-instance tables, new state format, stable parameter IDs, module paths, value text, neutral name and CLAP ID | Determinism (37/512 frames, three rates, two instances), no-allocation, state round trip, worker tests, ASan/UBSan and TSan all pass; DX7 renders bit-identical to M0 |
| M2 | Data-driven voice and compiled operator graph; DX7's 32 algorithms as data; multi-operator feedback in the graph renderer | DX7 renders bit-identical to M1 for every algorithm and each of Modern / Mark I / OPL; topology tests pass for all 32 |
| M3 | Native sound contexts (RFC 0005) and the extension hook decision | Oscairn's sound-context test cases (held notes across switches, exact-sample note-ID releases, channel parts, sustain, queue exhaustion, stale IDs, slot reuse) pass, plus TSan on concurrent prepare/select/discard |
| M4 | Extended profile core: 8 operators, the 88 algorithms, fine/detune/velocity ranges, fixed-mode key follow, per-operator key-on reset, AEG hold and FM-X time table, operator PEG, pitch velocity sensitivity, 2nd LFO extensions (phase, extended speed) | Topology tests for all 88 (algorithm 65 resolved by measurement); EG segment times within 2 %; pitch within 1 cent; LFO rate within 1 % |
| M5 | Spectral forms with Skirt and Resonance | Per form, Skirt and a Resonance sweep: first 16 harmonic levels within 1 dB of the fitted target, aliasing below a measured bound at the top of the keyboard |
| M6 | Shared blocks: 19-type filter with FEG and cutoff scaling, Part PEG, Part LFO with user steps and tempo sync, key-on delay, stereo output with random, alternate and scaling pan, random pitch, pitch key follow | Filter response within 1 dB up to cutoff per type; FEG times within 2 %; LFO shapes and rates; pan law; tempo-sync timing exact to the sample at a fixed tempo |
| M7 | Voicing and macros: mono/poly with legato behaviour, key assign, portamento modes and time modes, signed pitch bend ranges, Part offsets, the seven macros, controller-set, key-controller and LFO-box masks | Per-feature measurement tests; macro response curves fitted and within tolerance |
| M8 | Converter coverage (in Oscairn's converter) and editor metadata | The factory FM-X set converts with no engine-parameter loss in the report; Jacob's listening tests |

Polyphony and CPU are measured on the target ARM64 hardware at M4 and M6 before any voice-count
claim.

---

## 8. Upstreamability

Jacob's rule for other people's projects: architectural changes start as issues, and pull
requests are reserved for targeted fixes whose scope and behaviour are fully understood.

**Could go to asb2m10/dexed as targeted pull requests**

- Real-time fixes: no `push_back` on the audio thread, no `panic()` from the parameter thread,
  per-instance sample-rate state instead of static tables.
- A Linux ARM64 CI job.
- CLAP note-ID handling and `NOTE_END` through clap-juce-extensions' capabilities (if done
  without the headless split, or after it is accepted).
- State fixes found by the round-trip tests (for example the voice name), if confirmed.

**Should go to asb2m10/dexed as issues first**

- The headless engine split and the test suite.
- The compiled operator graph (behaviour-identical for DX7).
- 8 operators, the extended profile and spectral forms: possibly welcome as a "DX7 and beyond"
  feature, but large and outside Dexed's DX7 focus.

**Could go to free-audio/clap-juce-extensions**

- A hook letting a JUCE plugin return its own CLAP extensions from `get_extension`.

**Stays Oscairn-only**

- The `oscairn.sound-contexts` implementation, Oscairn naming and CLAP ID, the Oscairn build
  options (no MIDI out, MTS-ESP off), FM-X measured tables if upstream does not want them, and
  the converter (which lives in Oscairn).

Keeping the branch rebased on upstream `master` is part of each milestone, while the diff is
still small enough.

---

## 9. Licences and naming

### Licences

| Part | Licence (as found in the files) |
| --- | --- |
| Dexed plugin, editor, `source/engine/EngineMkI.*`, `EngineOpl.*`, `source/PluginFx.*` | GPL-3.0-or-later (headers say "version 3 of the License, or (at your option) any later version"; `LICENSE` is GPL v3) |
| MSFA, `source/msfa/*` | Apache-2.0 (headers checked for every file except `tuning.cc` / `tuning.h`, which have no header; provenance to confirm) |
| JUCE at `c352e24` (8.0.11) | AGPLv3 or commercial (`LICENSE.md` at that commit) |
| clap-juce-extensions, tuning-library, MTS-ESP, surgesynthteam_tuningui | Not checked in this plan; check before release |

- **Plugin boundary (RFC 0001).** Oscairn loads the plugin through CLAP, so it is a separate
  work and may keep its own licence. The fork stays GPL-3.0-or-later. Built with JUCE under
  AGPLv3, the distributed binary combines GPLv3 and AGPLv3 code, which GPLv3 §13 permits; the
  JUCE part keeps AGPL terms. That is compatible with Oscairn's AGPL but should be stated in the
  fork's README and in Oscairn's CREDITS.
- **New files.** Engine-core files derived from MSFA stay Apache-2.0 with their notices.
  New engine-core files: Apache-2.0 as well, so the core stays reusable by MSFA, Dexed and (if
  wanted) the Oscairn core. Plugin-shell files: GPL-3.0-or-later, as Dexed.
- **Shared blocks.** The filter/FEG/LFO library is used by the AGPL Sampler and this GPL plugin.
  Jacob owns that code, so it can be licensed permissively (Apache-2.0 or MIT) or dual-licensed.
  AGPL-only blocks would also combine with GPLv3 under §13, but would make the fork's binary
  partly AGPL in a second place. Decision needed (section 10).
- **Copying Oscairn code into the fork.** `SoundContexts.h`, the allocation guard and test
  helpers are AGPL in Oscairn. Copying them into the GPL fork is allowed under §13 but brings
  AGPL terms with them; or Jacob relicenses those helpers. Same decision.
- Keep all notices; credit Dexed, MSFA and their authors in Oscairn's CREDITS.

### IP rules (RFC 0013)

- Rule 1 (clean room): engine work uses public documentation (Data List, manuals),
  black-box measurements and Dexed/MSFA source only. People writing engine code do not work
  from reverse-engineering notes.
- Dexed's DX7 tables carry comments saying they were measured on a DX7 (`osc_freq`'s detune
  curve, `env.cc`'s `statics[]`), and `EngineMkI.cpp` cites ppplay and an OPL3 maths document.
  These are inherited, not new work, but counsel should see them in the freedom-to-operate
  review (rule 6).
- Rule 2: no Yamaha content in the repository or packages. No DX7 ROM cartridges, factory
  Performances or ESP renders as fixtures. Dexed embeds `assets/builtin_pgm.zip` (33 banks of
  4104 bytes: `Dexed_01.syx` and `SynprezFM_01.syx` onward, loaded by `source/PluginData.cpp`).
  Check their origin and redistribution terms before the Oscairn package includes them.
- Rule 4: ESP renders are used only to fit tables and are never committed.

### Naming (rule 5)

No Yamaha names in the plugin, parameter or UI names. "DX7" is a Yamaha product name too.

| Today / Yamaha term | Proposed neutral name |
| --- | --- |
| Plugin "Dexed", CLAP ID `com.digital-suburban.dexed`, features `instrument FM DX7` | Oscairn build: "Oscairn FM", CLAP ID `org.oscairn.fm` (matches `org.oscairn.basic-synth` etc.), features `instrument synthesizer` and the CLAP `fm` feature if defined. The desktop Dexed build keeps its own name and ID |
| FM-X (engine) | "Extended" profile |
| DX7 (mode) | "Classic" profile |
| Spectral Form / Skirt / Resonance | Operator Wave / Spread / Formant |
| 2nd LFO | Voice LFO |
| Part LFO | Common LFO |
| FM Depth, Harmonics, Attack, Decay, Sustain, Release, Texture | Mod Depth, Ratio Shift, Attack, Decay, Sustain, Release, Texture (generic terms; confirm in review) |
| Key On Delay | Note Delay |
| Controller Set / Operator Key Controller / LFO Box switches | Mod Route n / Key Route n / LFO Route n (per operator) |
| AEG / PEG / FEG | Amp Envelope / Pitch Envelope / Filter Envelope |
| Filter types LPF24D, BPFw, … | Descriptive names: "Low-pass 24 dB (digital)", "Band-pass wide", … |

Compatibility statements describe facts ("imports MODX M Performances", "loads DX7 SysEx
banks") and are reviewed before release. Algorithm numbering for the Extended profile follows
the published chart so converted Parts keep their number; whether to keep that numbering in the
UI or show the graph only is for review.

---

## 10. Risks and open questions

**Risks**

- **Restructuring cost.** Six operators and the 155-byte voice are everywhere (voice,
  parameters, editor, SysEx, state). M0–M2 must keep DX7 output bit-identical while moving all
  of it; that is the main schedule risk.
- **CPU on the device.** 8 operators, per-voice 24 dB filters, two LFOs and three EGs per voice
  is several times a DX7 voice. The NEON path is disabled today. Measure at M4 before promising
  polyphony.
- **Spectral forms.** Unpublished; may not fit a table model cleanly, and band-limiting under
  heavy FM is hard. Measurement may take longer than the code.
- **Macros.** The Data List gives only ranges for FM Depth, Harmonics and Texture; their
  behaviour must come from the manual and measurement.
- **clap-juce-extensions limits.** No hook for custom extensions at the pinned revision; the
  direct-process path bypasses much of JUCE's processor. Either may push us to a thin native
  CLAP entry for the Oscairn build.
- **Upstream drift.** Dexed is actively maintained (the fork head is a commit from 2026-10-14).
  A deep restructure makes rebasing harder each month.
- **Algorithm table accuracy.** `fmx_algorithms.json` was transcribed by eye; edges are not
  verified against hardware and algorithm 65 is ambiguous. The topology tests in M4 must be
  measured against renders.
- **Licence mixing.** AGPL helpers copied into a GPL plugin, and AGPL JUCE in the binary,
  need clear notices.

**Open questions for Jacob**

1. Licence of the shared filter/FEG/LFO library and of the Oscairn test helpers copied into
   the fork (permissive, dual, or AGPL).
2. One plugin with two profiles (proposed), or a separate Classic plugin.
3. Plugin name and CLAP ID (proposed "Oscairn FM", `org.oscairn.fm`).
4. Voice pool size and context count on the device (after measurement).
5. Where the measurement harness lives: shared with RFC 0013 in Oscairn (proposed) or in the
   fork.
6. Whether the desktop JUCE editor gets the new features at all, or Oscairn's generated and
   described editors are enough.
7. Whether to keep MTS-ESP in the Oscairn build at all.

**Unverified in this plan**

- Nothing was built or run. Scanner, renderer and test results are all future work.
- Whether the voice name survives Dexed's state round trip today.
- Whether event timing today is identical for different block sizes.
- Whether clap-juce-extensions turns JUCE parameter groups into CLAP module paths, and whether
  its derived parameter IDs collide.
- The meaning of the `7p 01 0o` ratio and frequency block.
- Whether 2nd LFO key-on reset and the Part LFO are per voice or shared in FM-X.
- Licences of the submodules other than JUCE, and the origin of `source/msfa/tuning.*`.
- The origin and licence of the banks in `assets/builtin_pgm.zip`.
