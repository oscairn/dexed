# Oscairn extension plan for Dexed

Status: proposal, 2026-10-30; M0 and M1 implemented on 2026-10-31 (see [section 11](#11-progress)).
Sections 2-4 describe the fork as it was at `fea2f0f`; where the implementation changed the plan,
the text says so.

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
11. [Progress](#11-progress)

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

**Changed in M1:** the Oscairn build is a native CLAP entry over the headless engine
(`source/oscairn/`, target `oscairn_fm`, display name "Oscairn FM", `org.oscairn.fm`), not the JUCE
processor's direct-process path. Why: with direct process, parameters, state, ports and note
ports would still go through the JUCE processor (APVTS listeners on other threads, parameter IDs
derived by the wrapper, a note output port fixed by `producesMidi()` on the shared class, no
`get_extension` hook for section 4.5), so each M1 item would be a workaround; the M0 engine
split made the native entry small (about 800 lines with the parameter table, no JUCE), and M3
needs it anyway. The JUCE
plugin stays as the desktop Dexed, unchanged. The original plan was:

Plan (original): implement `supportsDirectProcess()` / `clap_direct_process()` from
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
  **M1:** not split. The engine applies every event at the first 64-sample boundary at or
  after it, counted from the start of the stream, so output is already identical for any
  block size (tested), and splitting would change DX7 output, which M1 must keep
  bit-identical. Sample-accurate starts are left for the Extended profile.
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
  **Decided in M1: (c)**, see section 4.3.
- Reuse the rules and tests of Oscairn's `plugins/common/SoundContexts.h` and
  `core/engine/tests/sound_contexts.cpp`; copy with licence care (section 9).
- **Instant Performance changes (seventeenth session, decision 82):** Oscairn FM offers
  `oscairn.sound-snapshots/draft-3`. Its `prepare` reads a whole `octet-contexts 1` snapshot (the
  reading is split out of the snapshot load and touches nothing of the plugin, so it is safe on
  the main thread while rendering), then makes every sound in it, the Default included, a new
  context through the same free-slot path as `prepare`, all or nothing, selecting nothing. A
  sound's engine state is loaded at its selection, as for any prepared sound; the engine's tables
  are process-wide already, so there is nothing per sound to build or share. Discard refuses while
  a sound is selected, queued, has a voice or is not yet silent (its release), so outgoing sounds
  ring out. Eight contexts, as before. Given a ring-out buffer (`ring_out`, just before a
  `process()`), the engine adds every context no channel selects into it, after that context's own
  output filter, instead of the main output (`SynthEngine::render`'s optional `ring` argument; the
  desktop Dexed passes none), so Oscairn rings outgoing sounds out through the outgoing chain.
  Draft 3 (Octet 0.2.2): the buffer has four channels, and `choke_retiring`, queued like a
  selection, fades every sound retiring at that boundary out over the choke time (its voices as a
  choke, its filter's tail with them) into the last two, so only one change's tail carries over.
  Octet 0.2.3: `process()` returns `CLAP_PROCESS_SLEEP` once no voice is live, no context's output
  filter rings (every context's tail is tracked now, the default's too, and a tail ends below a
  24-bit step, since the engaged filter can settle on a tiny constant instead of 0) and no parameter
  feedback waits. The feedback for a new sound on channel 1 carries only the values that differ
  from the sound shown before, and what a full output queue refuses goes at the next process or
  flush.
- **Voice budget (nineteenth session, decision 85):** Octet 0.3.0 offers
  `oscairn.voice-budget/draft-1` (Oscairn RFC 0018 step 4). The engine is not the SDK's
  sound-contexts Engine, so Oscairn FM keeps its own pool and uses the SDK's `voices::Limits`,
  `voices::Candidate`, `voices::victim()` and `voices::Report` (`voice-budget-plugin.hpp`, vendored),
  through one hook in the engine (`SynthEngine::VoiceGiver`; without one, as in the desktop Dexed,
  nothing changes). At the start of each block, voices past a lowered limit (the instance's, or a
  sound's own, a sound found by the ID of the preparation its slot holds or as one a channel selects)
  fade out over the choke time, 3 ms; a new note past a limit makes room the same way first. The
  order is the SDK's: never a held voice of a mono sound (Oscairn FM's own Mono, or an Extended sound's
  Mono/Poly at mono) or of a host entry marked mono or legato, nor a held voice within its sound's
  reserve unless the new voice is that sound's; released before held (a key up counts as released,
  pedal or not); the new voice's own sound first; lower priority, quieter, older. `NOTE_END` comes
  once a given-up voice's fade is over (each note has one voice). `active_voices` counts the voices
  sounding, fading ones included, with no cost estimate (0; the host measures the instance);
  `notes` gives each sounding note by the address the host gave it, with its loudness: the loudest
  carrier's gain now (envelope, operator level and velocity: about 1 at level 99 without velocity
  sensitivity, more with it; an Extended voice times its mix gain), through any fade. A released voice holding an audible
  release level (decision 84) counts as released and is as quiet as that level (level 20 reads
  about 0.001). `honors_volume` is false: Oscairn FM has no note expressions yet. With every voice
  sounding, a new note takes a voice in the same order (one already fading first) while the host
  has set any limit; without limits it keeps Dexed's stealing while one sound plays, and with several
  each sound in use keeps an equal share of the pool as its reserve (twenty-seventh session; known
  differences register). Oscairn carries the extension only in-process so far: a plugin in a worker
  is budgeted as one without it.
- **Parts sharing one instance (twentieth session, decisions 86-88):** Octet 0.4.0 has what Oscairn's
  RFC 0021 slice 3 asks of a plugin parts can share: native sound contexts and their snapshot, no audio
  input or note output, eight stereo sound buses beside Main (`Bus 1` to `Bus 8`, one for each of its
  eight sounds, as the Basic Synth has), and the Output Bus (parameter 134, `Amp`, 0 for Main to 8),
  a per-sound setting tagged `OSCAIRN_ROLE_OUTPUT_BUS` through `oscairn.ui/draft-3`, which the host
  sets on each part's channel. A change applies at the start of the next `process()`
  (`SynthEngine::applyOutputBuses`). A voice keeps the bus it started on: each sound has a second lane
  (`SynthContext::previous`, with an output filter of its own) for the voices still on its bus before
  a change, so the held notes play on untouched and the new ones start on the new bus. A third bus
  while both lanes sound fades the older lane's voices out as a choke (3 ms) and waits until they and
  their filter's tail are gone (a shortcut in the code). Only the main output rings out into the
  ring-out buffer; a retiring sound on a bus plays on through its port, as draft 3 says. Unused buses
  are written silent and render nothing; the desktop Dexed passes no buses and is unchanged. The
  per-part voice budget entries (one per part, its sound, reserve and priority, limit 0) needed no
  change. State and snapshot formats are 2 (`oscairn-fm-state 2`, `oscairn-fm-contexts 2`); format 1 is
  refused (pre-release). MPE is now scoped to the channels playing channel 1's sound (decision 87).
  Octet 0.4.1 moves to `oscairn.sound-snapshots/draft-4` (Oscairn `0a5c14f`, which shares instances
  between parts on its own; draft 3 is no longer read): `save_sound` writes a snapshot holding one
  sound alone, as its Default on every channel and on Main, and `sound_values` gives the values of a
  sound no channel selects (as prepared until it is first loaded, then as the engine has them). A new
  instance now reports its Default selected on every channel before its first `process()`, which
  Oscairn checks as it adds the plugin.

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

From 0.4.6 `oscairn.ui/draft-4` describes the algorithm graph, the operators' envelopes and key
scaling, both pitch EGs, the operators' pitch EGs and the filter EG as rich views, which Oscairn
draws from the parameters (section 11, twenty-fifth session; decisions 90-92). The filter response
and the spectral form preview have no view in draft 4.

From 0.4.7 (`oscairn.ui/draft-5`) `seconds()` gives the Classic operators' rates and the Extended
filter EG's times, so the host draws those envelopes by time; the Extended operators' envelopes and
the pitch EGs are drawn by their parameters' ranges (twenty-sixth session; decision 93).

### 4.10 Presets

Oscairn lists CLAP presets and prepares native contexts from them (RFC 0005 "Presets",
`oscairn.sound-contexts/draft-5` `prepare_preset`). The cartridge manager stays in the desktop editor.

**Done (seventeenth session, decision 83):** cartridge voices are presets. Oscairn FM's entry offers
CLAP preset discovery (the SDK's factory, provider `org.oscairn.fm.cartridges`) over two folders of
`.syx` files: the built-in cartridges, which the build unpacks into `Oscairn FM/Cartridges` beside the
`.clap`, and the desktop Dexed's cartridge folder (user content). Each cartridge lists its 32 voices,
load keys `0` to `31`, named from the cartridge; `prepare_preset` makes a voice a new context (Classic,
the values loading it as parameters gives) and `clap.preset-load` loads it into the sound shown. A
Sound saves values, not the preset. Written in the sixteenth session (local commits `3e3d0bb`,
`4b3d866`), carried onto this branch as `cb8d3b6` and `2b967b7`. Oscairn's plugin store installs
only the `.clap`, so from 0.4.4 the built-in cartridges come from Oscairn's asset library
`org.oscairn.fm-cartridges` when the indexer has it (`oscairn.assets/draft-2` `library_root`), and
from the folder beside the `.clap` otherwise (section 11, twenty-third session). From 0.4.5 the
provider no longer declares the desktop Dexed's cartridge folder, and under Oscairn it declares only
the library's folder (section 11, twenty-fourth session).

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

The converter lives in its own private repository, `oscairn/performance-converter`
(https://github.com/oscairn/performance-converter; Jacob's decision, 2026-10-31). It turns
FM-X Parts into Extended sounds and a Performance's other parts into sounds for other engines
(RFC 0008). It is not part of this fork and is not built in M4. This section fixes what it
produces for this engine. It reads the `.pfm` container as described in
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
  volume, pan, sends and zones go to the Performance-level parts of the converter. The
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
| M8 | Converter coverage (in `oscairn/performance-converter`) and editor metadata | The factory FM-X set converts with no engine-parameter loss in the report; Jacob's listening tests |

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
- All Notes Off with a transpose: upstream's CC 123 passes each held voice's `midi_note`, which
  already includes the transpose, to `keyup`, which adds it again, so with any transpose but 0 it
  releases nothing. The fork's fix (`0325a55`) sits on the headless split and per-channel
  controllers (it releases by channel) and re-records the baselines, so it does not apply to
  upstream as it is; the local branch `upstream-cc123` (`74f8d1a`, on upstream `fea2f0f`) holds
  the transpose part of it as a one-line change to upstream's `source/midi/PluginMidi.cpp` (undo
  the shift before `keyup`), ready to offer later. Jacob wants it
  upstreamed eventually, not yet: no pull request has been opened and the branch is not pushed.

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
  the converter (which lives in `oscairn/performance-converter`).

Keeping the branch rebased on upstream `master` is part of each milestone, while the diff is
still small enough.

**Upstream merge test (twelfth session, 2026-11-01).** Upstream's default branch is `master` at
`https://github.com/asb2m10/dexed`; it was fetched into the local ref `refs/remotes/upstream/master`
only (no remote added, nothing pushed, the fork's `master` untouched). Results:

- `oscairn` is **0 commits behind**: upstream `master` is still `fea2f0f`, the fork's base, so
  merging it changes nothing. Upstream's `master` is a single squashed commit (its history has one
  commit; the older branches `refactor_events`, `feature/adsr-mode` and the operator-toggle branch
  share no history with it, `refactor_events`' tree differs from it by one line). If upstream
  squashes again, a merge will report unrelated histories and need `--allow-unrelated-histories`
  or a rebase by hand.
- The next upstream change in sight is `monoMode_lastNote` (`58af0cf`, also the `Nightly` tag),
  one commit on top of `fea2f0f`: true last-note priority in mono (the newest key always takes
  over, and a released key falls back to the most recently pressed one, with portamento). A test
  merge into `oscairn` (in a scratch worktree, never committed) **conflicts in one file,
  `source/PluginProcessor.cpp`**: the fork moved `keydown`/`keyup` out of it into
  `source/engine/SynthEngine.cpp` (the headless split, M1), so upstream's edit to those functions
  meets a deletion. Resolving it means keeping the deletion and porting the change into
  `SynthEngine::keydown`/`keyupVoice`. It would also change Classic mono behaviour, so the Classic
  baseline would have to be re-recorded from upstream at that commit (it is upstream's behaviour,
  so "Classic sounds like stock Dexed" still holds after the port).
- Size of the fork's diff against upstream `master`: 42 new files, +15,411 lines (Oscairn FM's
  engine, plugin, tests, tables, plan), and 31 upstream files changed, +634 / -859 lines. Most of
  the upstream-file change is the headless split (`PluginProcessor.cpp` -339, `PluginMidi.cpp`
  -110, `PluginProcessor.h` -48, moved into `SynthEngine`); the rest is real-time fixes in MSFA
  (per-instance rate tables, initialised state), the compiled operator graph in `fm_core.*` (M2,
  which Classic voices use too), Extended hooks in `dx7note.*` and `lfo.*` (an early return to
  `ExtNote` at the top of each `Dx7Note` method, the 2nd LFO's Extended reset), plus build lines in
  two `CMakeLists.txt`.
- Where upstream changes most (its history from 2024 on, in the older branches): the editor and
  parameter code (not touched by the fork), `PluginProcessor.cpp/.h` (20 and 16 commits),
  `source/CMakeLists.txt` (13), `PluginMidi.cpp` (9); MSFA rarely (`controllers.h` 6, `dx7note.*`
  4 since 2022). So the conflicts to expect are the ones the test merge shows: voice and MIDI
  changes in `PluginProcessor.cpp`/`PluginMidi.cpp`, which have to be ported into `SynthEngine` by
  hand, and occasionally the source lists in `source/CMakeLists.txt`.
- **Not refactored** (decision 68): moving the Extended hooks out of `dx7note.*` into a voice
  wrapper would shrink that file's diff, but `dx7note.*` changes upstream about once a year and its
  hooks are early returns that merge cleanly unless upstream edits the same lines; it would not
  touch the conflict that actually occurs (the headless split). What helps instead: keep
  `SynthEngine::keydown`/`keyupVoice`/`processMidiMessage` text close to upstream's `PluginProcessor`
  versions so a port is a copy, merge upstream soon after each upstream change, and re-record the
  Classic baseline from upstream whenever upstream changes Classic behaviour.

---

## 9. Licences and naming

### Licences

| Part | Licence (as found in the files) |
| --- | --- |
| Dexed plugin, editor, `source/engine/EngineMkI.*`, `EngineOpl.*`, `source/PluginFx.*` | GPL-3.0-or-later (headers say "version 3 of the License, or (at your option) any later version"; `LICENSE` is GPL v3) |
| MSFA, `source/msfa/*` | Apache-2.0 (`source/msfa/LICENSE`; headers checked for every file except `tuning.cc` / `tuning.h`, which have no header; provenance to confirm), except the Extended files `ext_*.cc` / `ext_*.h`, GPL-3.0-or-later by their headers; `tuning_standard.cc` (split from `tuning.cc`) and the generated `ext_*_data.h` have no header |
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
  partly AGPL in a second place. Decided: GPL-3.0-or-later (section 10).
- **Copying Oscairn code into the fork.** `SoundContexts.h`, the allocation guard and test
  helpers are AGPL in Oscairn. Copying them into the GPL fork is allowed under §13 but brings
  AGPL terms with them; or Jacob relicenses those helpers. Decided: test helpers are MIT
  (section 10); none were copied, the fork's own were written fresh.
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
| Plugin "Dexed", CLAP ID `com.digital-suburban.dexed`, features `instrument FM DX7` | Oscairn build: "Oscairn FM" (decided 2026-11-07; "Octet" from 2026-10-31), CLAP ID `org.oscairn.fm` (matches `org.oscairn.basic-synth` etc.), features `instrument synthesizer` and the CLAP `fm` feature if defined. The desktop Dexed build keeps its own name and ID |
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

**The name Oscairn FM (2026-11-07, Jacob's decision).** The plugin was called "Octet" from
2026-10-31 to 0.4.2; from 0.4.3 its display name is "Oscairn FM", its preset provider "Oscairn FM
cartridges" (ID `org.oscairn.fm.cartridges`, unchanged), the built-in cartridges sit in
`Oscairn FM/Cartridges` beside the `.clap`, the snapshot format is `oscairn-fm-contexts 2` and the
init sounds are `assets/oscairn-fm/*.oscairn-fm`. The CLAP ID `org.oscairn.fm` and the file
`oscairn-fm.clap` keep their names. Nothing reads the old folder or snapshot name (pre-release). The
progress entries in section 11 keep the name Octet as written then, as do the version numbers in
section 4.

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
- **Licence mixing.** AGPL JUCE in the desktop binary needs clear notices.

**Decisions (Jacob, 2026-10-31)**

These answer the seven questions this section used to list.

1. **Licences.** The shared DSP blocks (filter, FEG, LFOs) are GPL-3.0-or-later. Test helpers
   (test host, allocation guard, render-comparison tools) are MIT. No Oscairn AGPL code has
   been copied into the fork.
2. **One plugin, two profiles**, Classic and Extended, as section 5 describes, with one
   interface. DX7 imports come in as Classic; Extended-only controls are hidden or disabled
   through the editor description, never by changing the parameter list. FM-X conversions
   come in as Extended. Two init sounds ship: the Dexed/DX7 init voice (Classic) and an FM-X
   init (Extended) built only from the Data List's documented defaults, with no Yamaha preset
   content. A "convert to Extended" action maps a Classic sound's values and is labelled
   approximate, because where the tables differ the result may sound slightly different.
   Converting back to Classic only works when the sound stays within the Classic subset.
3. **Name and ID.** The CLAP ID stays `org.oscairn.fm`; the display name is "Oscairn FM" (decided 2026-11-07; it was "Octet"
   from 2026-10-31 to 2026-11-07).
4. **Voice pool size and context count: on hold.** Voice stealing is being discussed with
   Oscairn. Voice allocation and stealing policy do not change beyond what bit-exactness
   needs.
   Notes for that decision: with the placeholder policy (Dexed's, across the shared pool), a
   mono or portamento sound whose voice another sound steals loses its legato state; the
   portamento source is now only taken from the sound's own last voice, but mono hand-over
   still assumes the voice is still the sound's own.
5. **Measurement harness:** its own repository, `oscairn/measurement` (private, MIT). It holds
   only analysis code (pitch, level, spectrum and envelope comparison against reference
   renders) and the pass/fail tolerances: `common/` for shared analysis, `fm/` for FM checks
   (another project adds `analog/`). It never contains renders, test performances, the
   recorder, Yamaha names in identifiers, or anything read from a Yamaha binary. Pull and
   rebase before pushing; never rewrite others' history.
6. **Desktop JUCE editor:** it should eventually show every feature, but that is careful UI
   work, deferred and not part of these milestones.
7. **Tuning (decided 2026-10-31, Oscairn RFC 0017).** Oscairn is the MTS-ESP master and also
   offers CLAP's draft `clap.tuning`; the Performance owns the tuning. Plugins use host tuning
   first, then their own stored tuning (FM-X micro tuning as Scala data in the state), then
   MTS-ESP, then 12-TET. The desktop Dexed keeps MTS-ESP. Not part of M4.

**Answers on the implementation decisions (Jacob, 2026-11-01)**

The decisions taken while implementing are listed under M4 in section 11; each answer is marked
there. Confirmed as unmeasured choices: 5, 19's negative time key follow mirroring, 31, 43, 45-47,
49, 50, 52, 54, 56, 58, 61, 66 and 67's unmeasured parts. Confirmed as measured matches: 11, 12, 22,
24, 36, 38, 42, 48, 51, 55, 59, 62, 63, 66 and the measured parts of 44-67. 14: keep macOS 15 for
CI. 17 (resonance overload not modelled) and 20 (FEG segments straight in cents): known differences
to look at later. 40 waits on the voice-stealing decision (Oscairn RFC 0018, Q4). Acted on in the
thirteenth session: 2, 3, 10, 18 and 19's time velocity (decisions 69-75); listening examples for 7,
17 and 20 are in `forks/measurements/listen/` (renders, not committed). The others stay open.

**Answer (Jacob, 2026-11-02):** 70 is decided: output clipping is left to Oscairn's mixer, not done
inside Oscairn FM, as the reference clips after part volume and pan, at the mix. No engine change.

**Notes from implementation**

- The desktop Dexed, when loaded (also by `oscairn-render`), creates
  `~/Library/Application Support/DigitalSuburban/Dexed` (or the XDG equivalent) with the
  built-in cartridges and reads `Dexed.xml` from it; the engine type in `Dexed.xml` overrides
  the default, so that plugin is not deterministic across machines. The tests give it a data
  folder in the build tree. The native CLAP touches no files.

- **The baseline differs from unmodified Dexed in exactly one respect: CC 123.** Dexed
  transposed each held voice's already transposed pitch a second time before matching, so with
  a voice transpose other than 0 All Notes Off released nothing; a DX7 releases every note, and
  the engine now does too (only the addressed channel's notes). Fixing it changed 116 of the 164
  cases (the generated voices use transposes 18-30). For every changed case, on arm64 and on
  x86_64, the output was bit-identical up to the CC 123 event's frame (first difference 0 to
  4,955 samples after it); the other 48 cases did not change. The last baseline that was pure
  Dexed is `tests/baseline/` as of commit `bee75c1` (files last changed in `a2ba0c0`).

**Unverified in this plan**

- Nothing was built or run. Scanner, renderer and test results are all future work.
- Whether the voice name survives Dexed's state round trip today.
- Whether event timing today is identical for different block sizes.
- Whether clap-juce-extensions turns JUCE parameter groups into CLAP module paths, and whether
  its derived parameter IDs collide.
- The meaning of the `7p 01 0o` ratio and frequency block.
- Whether 2nd LFO key-on reset and the Part LFO are per voice or shared in FM-X. (Measured in M7:
  the 2nd LFO runs per voice, the part LFO per part.)
- Licences of the submodules other than JUCE, and the origin of `source/msfa/tuning.*`.
- The origin and licence of the banks in `assets/builtin_pgm.zip`. Decided (Jacob, 2026-11-07): the 33
  banks ship unchanged, as in upstream Dexed, with `assets/SynprezFM-NOTICE` crediting Jean-Marc
  Desprez's compilation (upstream README line 85); the build copies it into `Oscairn FM/Cartridges`.

### Known differences from the reference (2026-11-01)

Every difference between Extended sounds and the reference known at `2b35e18` (updated in the fifteenth session): what differs, how
much and where, the cause if known, whether it is deliberate (with its decision) or unresolved, how
to reproduce it, and where it sits in the code. Renders live in `forks/measurements/` (never
committed); scripts are oscairn/measurement's `fm/` unless a local folder is named. Every
comparison against the reference is at 44.1 kHz.

**The whole-sound comparison.** Before it, every check was per parameter. 50 factory performances
with FM-X Parts (bass, keys, piano, organ, mallets and bells, leads, comping and plucks, pads, brass,
strings, woodwind, guitar, effects and percussion; 44 with one FM-X Part, 6 with two or three) were
copied with every effect, EQ, arpeggio and motion sequence off and only the FM-X Parts sounding,
and recorded playing one 29 s sequence: keys 36, 60 and 84 at velocities 40, 90 and 127, a
four-note chord, and a held note with 4 s of release. Oscairn FM played the converter's states
(performance-converter `7a71d75`, unchanged) through `oscairn-fm-test render-seq`, which gives the
same audio as loading the state (checked bit for bit on 15 Parts), with each copy's Part Volume,
Performance Volume and centre pan applied as Output (decision 69). Compared with
`fm/compare_sound.py` (measurement `ce52ec5`): each segment aligned on its own, then level,
envelope, third-octave spectrum, pitch, attack and release differences and one ranking score
(envelope + spectrum + |cents| / 5 + 6 |log2| of the release and attack ratios, each capped at 10;
level left out). Scores: median 11.5, quartiles 4.7 and 16.9, range 0.4-28.8; 14 below 5, 19 at 15
or more. 23 of 50 within 1 dB in median level, 16 at 3 dB or more (8 of those fall under 1 dB with
entry 1's parameter at 0). 425 of 458 single-note pitch readings within 5 cents, 346 within 1.
Reproduce: `forks/measurements/fmx-compare/` (`gen.py`, the recorder job file `jobs.txt`,
`octet_render.py`, `score.py`, `results.json`); 20 listening pairs with a README in
`forks/measurements/listen-fmx/`.

Found by the whole-sound comparison:

**Re-run after the fifteenth session** (Octet `5b0c5d3`, the same 50 recordings, converter states,
scripts and scoring; `octet-final/`, `results-final.json`, `score-final.txt`): median score 11.5 ->
3.9, quartiles 4.7 / 16.9 -> 2.4 / 8.9, range 0.4-28.8 -> 0.4-26.7; below 5 ("close"): 14 -> 31; 15 or
more: 19 -> 7. Median level within 1 dB: 23 -> 37 sounds, 3 dB or more: 16 -> 7. Median envelope term
2.2 -> 0.6 dB, spectrum term 4.2 -> 1.6 dB. Single-note pitch readings (finite ones) within 5 cents:
382 of 413 -> 398 of 429, within 1 cent 311 -> 331. 32 sounds improved, 17 did not move (by 0.05 or
less), one got worse: 3F0913 12.7 -> 15.3 (its level and envelope terms
improved, 2.5 -> 0.2 dB and 10.0 -> 2.7; the score rose through the held note's release reading, 54 ms
against 203 ms, which could not be read before; not looked into further). The per-Part isolation
renders (`m4/m15b`, `m4/m15p`: each FM-X Part rebuilt from its decoded values without controller sets
or knobs, keys 48 and 72, both sides) show which of the remaining sounds are engine differences:
3F003B, 3F0300 and 3F1A31's Parts match within 1 dB median per third-octave band and 0.3 dB in level
(their whole-sound scores come from what the converter drops, see "Not engine differences"); 3F0A41's
and 3F0B05's match in spectrum (0.0-0.6 dB median) but one Part of each is 2.4-5.2 dB louder in Oscairn FM
at one key (3F0B05's at key 72, 3F0A41's second Part at both keys; not looked into); 3F0231 (4.4-4.9 dB
median), 3F0B37's third Part (1.8-3.7, up to 19 at one band) and 3F0573 at key 48 (5.6) differ in
spectrum.

1. **AEG time key follow** (resolved, decision 77): Oscairn FM used the Classic rate-scaling curve on
   every segment and the hold, far too strong at high keys and the wrong way at low ones (3F0118's
   held note 43 dB low at 2.9 s; 3F076E attacks 56 instead of 253 ms at key 36). Measured in `m15k`
   (230 renders) and modelled as the reference's rate steps; 24 of the 50 sounds use it, and their
   scores fell the most (3F076E 28.8 -> 1.4, 3F025C 19.6 -> 1.5, 3F0925 18.8 -> 1.3, 3F0118 9.9 ->
   2.4). Left: fast segments (faster than about 1.4 ms/dB) read up to 15 % slower in the reference
   than Oscairn FM's tables give, with or without key follow (`m15k` t20s0: 6 %), an EG difference at fast
   rates, not key follow; the slowest attack checked is 7 % slow. Code `extTimeKeyFollowSteps`,
   `extTimeKeyFollowScale` (`msfa/ext_tables.cc`), `ExtEnv` (`msfa/ext_note.cc`). Reproduce:
   `m4/m15gen.py m15k`, `m4/ana15.py`, `m4/cmp15.py`.
2. **Formant waves in whole sounds** (3F0231, 3F0B37, 3F0573; time-boxed, no change): of the four
   sounds listed first, 3F003B, 3F0300 and 3F0B05 match the reference's spectrum Part by Part (`m15b`,
   `m15p`; 3F0B05's Part is 5.2 dB louder at key 72), so their spectral deviation in the whole sound is
   the converter's (controller sets, knobs). 3F0231 (formant 1 at
   spread 7 and formant 2) still differs by 4-5 dB median per band on the Part alone, 3F0B37's Part 3
   (formant 1 at spread 7, resonance 60) by up to 19 dB at one band, 3F0573 at key 48 by 5.6 dB: the
   open M5 cases (entries 9, 10, 12). Unresolved; code `msfa/ext_waves.*`, `ExtNote::chooseWave`.
3. **Feedback on a modulator** (resolved, decision 78): Oscairn FM used a carrier's fitted amount (0.18 of
   the DX7's) on every operator; a modulator in the reference feeds back the DX7's amount. 3F0579's
   spectrum term 10.0 -> 2.6 (its pitch reading at key 84, an operator above Nyquist, remains: score
   10.6 -> 9.6), 3F0443 10.2 -> 3.3, 3F044C 9.7 -> 3.4, 3F0526 8.7 -> 5.0. Carrier feedback is unchanged
   and matches (`m15f`: feedback 4-7 at levels 50-99 and an EG level on a carrier, every harmonic
   above -70 dB within 0.4 dB). Code `ExtNote::setFeedback` (`msfa/ext_note.cc`). Reproduce:
   `m4/m15gen.py m15g`, `m4/fbcmp.py`.
4. **Strong macros with feedback 7** (3F1A31): not the macros. Its FM-X Part uses algorithm 1, where
   every operator is a carrier; the reference's macros do nothing to a carrier that feeds back
   (`m15h`: depth -99 to +50 and ratio shift -99 at feedback 4 and 7, all within 0.1 dB), as in Oscairn FM,
   and the Part alone matches the reference within 0.4 dB median per band (`m15p`). The whole-sound
   difference (Oscairn FM 9 dB quieter, 20 dB brighter above 500 Hz) comes from the Performance (controller
   sets, a part LFO to an effect), which the converter drops. No engine difference known.
5. **Attacks that read slower in Oscairn FM**: 3F0443's (702 against 9 ms) was a missing early peak, fixed
   with the modulator feedback (now 9-10 ms on every note). 3F003B's (63 against 9 ms) is not the
   engine's: the Part alone matches (`m15b`). 3F0221 (56 against 9 ms at key 36, 166 against 21 ms on
   the chord) is left: its operators 1 and 2 play at ratio 0.5 (33 Hz at key 36) and operator 2 has
   key-on reset off, so the attack reading depends on the free-running phase (decision 46).
   Unresolved for 3F0221.
6. **3F0A3E's held release** (56 against 510 ms): the two releases have the same shape (both fall
   about 5 dB per 100 ms after the note-off at 25 s); the reading differs because the level was still
   falling before the note-off and the reference has a 1 dB bump there. A measurement artefact, not an
   engine difference.
7. **2nd LFO pitch modulation in whole sounds**: 3F0526 (depth 8, delay 10, key-on reset) reads
   -0.6, -26.6 and -6.5 cents at keys 36, 60 and 84, the same at every velocity, averaged over
   250-750 ms after the onset, so the vibrato's phase, depth or delay differs there. 3F067F (depth 4,
   free-running phase) scatters +-50 cents from note to note, which is decision 46's phase choice.
   Unresolved for 3F0526.
8. **Clicks in the reference that Oscairn FM does not make**: 12 recordings have clicks Oscairn FM's renders
   do not have there; in the 11 recorded twice they recur at the same places (3F055C 91 and 92,
   3F0349 39 and 38, 3F0231 23 and 24). Found by `fmx-compare/dropouts.py`, which also found and
   replaced one real dropout. Source not known. Unresolved.

Deliberate (decided):

- **Band-limited waves, formants above Nyquist silent** (decision 7): the reference aliases up to
  -14 dB at note 108 and plays a non-harmonic tone near Nyquist where a formant's centre is above it.
  No whole-sound case was traced to this alone. Code `ExtNote::chooseWave`, `msfa/ext_waves.*`;
  listening examples in `forks/measurements/listen/`.
- **No resonance overload** (decision 17): LPF24D, HPF24D and BPF12D above resonance 80 overload in
  the reference (20 filter cases); Oscairn FM only bounds its states. `msfa/ext_filter.cc` (shortcut
  comment); `filt*`, `fm/compare_filter.py`.
- **FEG segments straight in cents** (decision 20): the reference's bend by up to about 90 cents
  along 1200. FEG in `msfa/ext_note.cc`; `feg2`, `fm/compare_feg.py`.
- **No output clip** (decision 70, decided by Jacob 2026-11-02): the reference clips at full scale
  after Part and Performance Volume (6 of the 50 copies clipped at Performance Volume 127 and were
  recorded at 64), that is at the mix; clipping is left to Oscairn's mixer, not done inside Oscairn FM.
- **Filter and FEG once per 64 samples** (decisions 18, 75): instant FEG segments (times 0-20) take
  1.45 ms. `msfa/ext_note.cc`.
- **Random pitch and pan** (decisions 26, 44): uniform approximations; the reference's extra step
  down at depths 32-127, narrower pan spread at 127 and left lean are not modelled. `m7f`,
  `fm/compare_seq.py`.
- **Key-on delay** (decision 27): a table in 64-sample blocks at 44.1 kHz, the same time at every rate.
- **Ratio past coarse 31** (decision 76 and Jacob's rule of no ceiling on pitch): the reference's ratio
  at coarse 31 with fine above 100 is not stable (13.0 in one campaign, 6.658 in another; c31f100
  3660 cents from the documented ratio in `m14s`); Oscairn FM plays the documented ratio. `extRatio`,
  `extRatioSteps` (`msfa/ext_tables.cc`); `pitch2`, `m14s`, `fm/compare.py`.

- **Voice stealing under a voice budget** (decision 85, confirmed by Jacob 2026-11-07): while Oscairn has set voice
  limits (`oscairn.voice-budget`), a full pool and voices past a limit go in the SDK's order
  (released before held, the new voice's own sound, lower priority, quieter, older; never a held
  mono key or a sound's reserve), where Dexed takes, among voices that all sound, a released one
  before a held one, then the same key, then the oldest. Without limits Dexed's stealing is
  unchanged (the 164 Classic cases are bit-identical). The reference's own stealing order is not
  measured. Why: Oscairn RFC 0018's order, so the host and Oscairn FM agree on what is least audible.
  Reproduce: `oscairn-fm-test voice-budget`, last case (16 held notes, the sixth at velocity 0.3:
  with a budget it is the one given up; the same case without limits gives up the oldest, checked
  once by hand in the nineteenth session, not by a test). Code `Plugin::steal`, `Plugin::victim` (`oscairn/OscairnFm.cpp`), the hook in
  `SynthEngine::keydown`.
- **Polyphony** (twenty-seventh session): one pool of 128 voices shared by every sound of an instance
  (the reference lists 128 FM voices for the instrument), each note one voice whatever its operator
  count, a mono sound's held keys and a key played again with key assign Single (decision 41, while
  its fade lasts) a voice each. Whether the reference's 8-operator voices, its parts or its mono
  and Single fades count against its 128 the same way is not measured, nor how it steals at 128.
  Within 16 sounding voices every sound plays as on Dexed's 16 (the 164 Classic cases, sample for
  sample, up to Dexed's first steal); past 16 the extra notes play where Dexed would steal. Reproduce:
  `dexed-render-engine --pool-identity`, `oscairn-fm-test polyphony`. Code `SynthEngine::keydown`,
  `setPolyphony`; `polyphony()` in `oscairn/OscairnFm.cpp` (`OSCAIRN_FM_POLYPHONY` sets 1 to 128).
- **Stealing between parts without a budget** (twenty-seventh session): with every voice sounding,
  no limits from the host and more than one sound in use, each sound in use (sounding, or the new
  note's) keeps an equal share of the pool as its reserve and the SDK's order picks the voice, so
  layered parts each keep their share (at 16 voices, Dexed's stealing left the first of two layered
  parts 4 of 16 and Oscairn's kit measured 0). One sound alone keeps Dexed's stealing. The
  reference's stealing between parts is not measured. Reproduce: `oscairn-fm-test polyphony` with
  `OSCAIRN_FM_POLYPHONY=16` (CTest `oscairn-fm-polyphony-16`). Code `Plugin::steal`.

Pitch ranges past the reference's (Jacob: no ceiling on pitch shift or transpose, even where the
reference has one; deliberate):

- **Note Shift and Pitch Bend Upper and Lower -127..+127** (decision 79; the reference: -48..+48 and
  -48..+24): values in the reference's ranges play as before; a key shifted past 0-127 plays the rest
  as equal-tempered semitones with the end key's key-scaled tables. Parameters 244, 253, 254
  (`oscairn/Parameters.cpp`), `SynthEngine::keyBeyond`, `ExtStart::keyBeyond`, `ExtNote`'s setup.
- **Fixed frequencies on every coarse and fine value** (decision 80): the reference plays coarse 21
  fine 64 at 19,920 Hz and holds coarse 21 fine 127 and coarse 22-31 at 22,043 Hz (`m15f`); Oscairn FM
  plays the 1/128-octave grid without a ceiling (above Nyquist as ratio-mode operators do).
  `extFixedSteps`, `extFixedHz` (`msfa/ext_tables.cc`).

Unresolved from the milestones (measured, not matched):

9. **Spread 7 formant waves** (M5): the formant sweep at spread 7 meets in 10 of 24 (formant 1 off by
   3.1-5.7 dB at formants 9, 27, 45, 54; formant 2 by 3.3 to over 100 dB at 9 and 27-99); as
   modulators formant 2 at formants 9 and 27 (1.6-8.7 dB; `spec5` m6s7r27l75 37.8 dB) and formant 1
   at formant 9 (1.4-5.4 dB). The window is wider than the low harmonics' distance from the centre
   and the reference's spectrum is not the window model there. `spec`, `spec5`, `m9a`;
   `fm/compare_waves.py`, `fm/compare_mod.py`; tables from `fm/make_waves.py`, code
   `msfa/ext_waves.*`.
10. **Formant 2 at formant 0, spreads 4-5** (2.8 dB at 4, 1.98 dB at 5 after decision 63) and the
    reference's flat floor under formant 2 at spreads 5-7. Same campaigns and code.
11. **Feedback on a modulator** (resolved, decision 78): df7 within 0.09 dB at harmonics 2-6 (was 17.7
    dB). Left in `m15g`: feedback 7 at level 50 (2.5 dB) and 99 (3.8 dB), feedback 4 at level 80 (20 dB
    at one harmonic). `ExtNote::setFeedback`; `m4/fbcmp.py`.
12. **Waves 1-2 at spread 7 under a strong modulator** (`m9a` level 75: 4.2 and 17.1 dB); wave 2 at
    spread 2 under it (2.1 dB, at a harmonic 58 dB down).
13. **Texture extremes**: +51 to +66 on the level 70 modulator (`m8c` tp70 rows, 16-25 dB), -99 on the
    wave probe (`m7a`, 22.6 dB), `m8f` tc10 (-89, 95.5 dB); unmeasured: the wrap past +-99 and the
    places of off-path formant waves (decision 67). `extTextureWave` (`msfa/ext_tables.cc`);
    `fm/compare_m7.py`.
14. **Ratio Shift between its measured values**: up to 5.7 cents (decision 52); fixed-frequency
    modulators not shifted (unmeasured).
15. **The depth macro** with feedback during a rising level (probe D: 1.7 and 3.7 dB at +25 and +50,
    harmonics 4-6 in the attack), at feedback 1 and 2 (+50: 1.9-2.6 dB) and on a stack of three (+25:
    8.8 dB), measured before decision 78 and not re-run since. With the new feedback amount the rule
    holds on `m15h` (feedback 4 and 7 at depth -50 and +25: 0.02-1.25 dB). `m7b`, `fm/compare_m7.py`.
16. **Part AEG offsets**: attack +16 to +63 (up to 5.6 dB), sustain -64, -32, -16, +16, +32, +63
    (1.2-2.3 dB), release -8 (1.0 dB), attack +10 (1.05 dB, in the decay after it). `m7b`, `m7h`.
17. **Level velocity**: velocities 2 and 3 at sensitivity +7 (0.8-1.5 dB), part velocity 36 (0.8 dB).
    `m7d`.
18. **Portamento at fast times**: `m7f` 22 of 29 and `m7g` 27 of 28 within 2 % (slower slopes at times
    1-24 for 12- and 24-semitone glides); the reference's glide clock phase against a note is not
    known (decision 66). `fm/compare_seq.py`, `fm/glide_grid.py`.
19. **Pitch tables the step-grid rule left** (decision 76): the operator detune between its measured
    keys (5 of 20 values one step off, up to 1.18 cents), the part detune between measured values
    (up to 0.15 cent worse), the common pitch velocity (cpv32v1 1.12 cents, pv127o14v127 11.22), fixed
    key follow (kf99n50 1.77). `m14s`, `pitch`, `fm/compare.py`; the tables in `msfa/ext_tables.cc`.
20. **2nd LFO**: pitch depth 80 (+1.3 cents); the square's start (61 cents, 95th percentile); delay
    99's fade in 2.5 % early; LFO delay values 9 and 23 11 ms early; key-on reset off (phases not
    modelled, decision 46). `m7l`, `m7l2`, `m6o`; `fm/compare_lfo2.py`.
21. **Filter**: 62 cases miss by 1.3 dB at one harmonic near 14.9 kHz (the two sources differ there,
    not the filters); unmeasured or unresolved: LPF12+BPF6's structure, BPFw's lower edge at low
    cutoffs, the dual types' second section above F = 1, FEG holds above 96, FEG depth velocity
    curves 0, 1, 3, 4 away from velocities 32 and 96, negative FEG time key follow (mirrored,
    decision 19). `filt*`, `fm/compare_filter.py`; `msfa/ext_filter.cc`.
22. **Pan**: scaling pan +63 at note 30 (one case); the alternation of alternate pan.
23. **Topology**: `topo` f72, one intermodulation line with feedback 5.
24. **The retrigger fade** runs about 1 ms ahead in its first 10 ms (decision 54); above attack 60 it
    is taken as at 60.

Checked only against the renders they were fitted from (an implementation check, not an independent
one): the LFO rate table (speeds 2-63), the part detune table (37 of 49 cases), the portamento rate
table, the fractional velocity curve below velocity 8, the 2nd LFO amplitude tables, the carrier EG
level table (`m9b`), the texture places (`m8c`, `m8f`) and the Ratio Shift table. Not measured at
all: fixed frequencies below 20 Hz, LFO speed 0, operator pitch EG attack at times 90-99, level
scaling (decision 5, Classic curves), the mod wheel's depths in Extended sounds.

Verified against the reference only at 44.1 kHz (decision 71): everything above. Oscairn FM's own
cross-rate check (`oscairn-fm-test rates`, decision 81) holds 14 measurements (level, pitch, decay,
attack, hold, both LFOs, key-on delay, two filters, the FEG and portamento) at 48, 88.2, 96 and
192 kHz to the 44.1 kHz values within the comparison tolerances. Behaviours that change with the rate
by design: band-limiting and formant silence follow the running Nyquist (at 88.2 kHz and up, harmonics
and formants between 22 kHz and the running Nyquist sound where the reference's 44.1 kHz rendering
cannot have them; all above hearing); the filter's cutoff limit stays 7.35 kHz and its sections warp
differently near it; the FEG, filter and portamento steps follow 64-sample blocks of the running rate;
the retrigger fade and the pitch grid's mantissa were measured at 44.1 kHz.

Not engine differences (the converter's, found by the whole-sound comparison and listed in its
report): controller sets and knob positions are not converted, so a Part that a knob fades in at its
stored position plays at full level (one factory performance's FM-X Part is silent in the reference
for this reason and was replaced in the comparison); Performance-level AEG, FEG, cutoff and resonance
offsets are dropped; Part and Performance Volume and pan are not applied yet; a Part that plays only
from the arpeggiator is converted as a normal Part. After the fifteenth session these account for most
of what is left in 3F003B (26.7), 3F1A31 (14.2) and 3F0300 (16.2): each of their FM-X Parts, rebuilt
without controller sets, matches the reference within 1 dB median per band and 0.3 dB in level
(`m4/m15b`, `m4/m15p`), and of the spectral part of 3F0A41's (13.5) and 3F0B05's (17.3).

---

## 11. Progress

### M0 (done 2026-10-31)

Commits on `oscairn`: `f1aaf84` (copy-after-build option), `12ad187` (state read uninitialised,
section 4.6), `7f0204b` (render tests and baseline), `a915d9c` (engine split), `e1bbe8d`
(Oscairn scanner/renderer tests, CI), `a2ba0c0` (x86_64 baseline).

- `source/engine/SynthEngine.*` is the headless engine; it builds as `dexed_engine` without
  JUCE. The JUCE processor delegates to it.
- `tests/`: 164 render cases of generated DX7 voices (all 32 algorithms, three render
  engines, poly/mono/MPE, 44.1/48/96 kHz, blocks 37 to 1024). `dexed-render-plugin` (the JUCE
  plugin as a black box) and `dexed-render-engine` (the engine alone) both match
  `tests/baseline/dx7-renders-<cpu>.txt`, recorded from unmodified Dexed (`7f0204b`).
  arm64 and x86_64 differ in the last bits of the float output filter (fused multiply-add on
  arm64), hence two files; Linux ARM64 matches macOS exactly.
- CI (`.github/workflows/oscairn-tests.yml`): Linux x86_64, Linux ARM64 and macOS build the
  engine, both CLAPs and the tests and run CTest. Windows is not built by this workflow.
- With `-DDEXED_OSCAIRN_BUILD_DIR=<an Oscairn build>`, CTest runs `oscairn-plugin-scan` on
  both CLAPs and renders them with `oscairn-render` (512 and 37 frames, twice, and in
  Oscairn's plugin worker) and compares the files. These ran on macOS only; CI does not build
  Oscairn.
  TODO: run these in CI too. That needs Oscairn built in the workflow (a checkout of the
  Oscairn repository and its tools); not done yet.
- CTest checks that `oscairn-fm.clap` exports nothing but `clap_entry` (`oscairn-fm-exports`),
  on every CI platform.

### M1 (done 2026-10-31, except where listed)

Commits: `9919464` (per-instance tables), `aa84166` (`-fwrapv`, hidden symbols), `a195449` (note
tracking in the engine), `8d0f288` (native CLAP and its tests).

- Per-instance sample-rate tables (`msfa/synth_rate.h`); shared tables built once.
- `source/oscairn/`: the native CLAP (section 4.3) with CLAP note IDs, wildcards, choke and
  `NOTE_END` (stolen, choked, or released and silent; reported at the end of the block, or at
  the event for steals and chokes); no note output; MIDI and MPE input; fixed parameter IDs
  (voice 1-99, settings 100-199, operator n at 1000 n + k) with module paths, value text and
  stepped/enum flags, listed in `tests/baseline/oscairn-fm-params.txt`; a versioned text state
  (`oscairn-fm-state 1`, parameters by ID, voice name), refused whole if anything is wrong;
  state loaded while active is handed to the audio thread without locks.
- Tests (`oscairn-fm-test`): the 164 cases through the CLAP binary are bit-identical to the
  baseline; two instances at different rates; parameter list and text round trips; notes;
  state round trip and render after load; no allocation while processing; main-thread calls
  during processing. ASan+UBSan and TSan builds pass on macOS; CI runs them on Linux.
- Not done in M1: CLAP note expressions (the DX7 engine has no per-voice destinations except
  MPE pitch bend); the real-time fixes of section 4.6 in the desktop JUCE processor (the
  `push_back`, the unsynchronised `activeProgram`, `panic()` from the parameter thread), which
  the native CLAP does not use; Dexed 1.0/1.1 state loading (not applicable: the native CLAP
  has its own ID, so hosts never give it Dexed states); a Windows build of the native CLAP.

### M2 (done 2026-10-31)

- `source/msfa/algorithms.h`: the 32 DX7 algorithms as operator graphs (inputs per
  operator, carriers, feedback from/to), derived from the old bus flags of
  `FmCore::algorithms` (a script check found no difference), and `compileAlgorithm()`, which
  turns a graph of 6 or 8 operators into a render order, per-operator input masks, the
  output mask and the feedback loop. The bus flag table is gone.
- `renderGraph()` (`msfa/fm_core.h`) renders every operator into its own buffer from the sum
  of its audible modulators; Modern, Mark I and OPL supply only their gain mapping, level
  threshold and kernels. Multi-operator feedback is part of the graph: Mark I renders the
  loops of algorithms 4 and 6 sample by sample with its existing `compute_fb3`/`compute_fb2`;
  Modern and OPL keep feeding back only the loop's first operator, as before.
- Kept for identical classic output: `FmCore::isCarrier` (used by `isPlaying()`) still counts
  operators that used to add into bus 1 or 2 (`dx7ExtraActive`), so voice stealing is
  unchanged.
- Gate: the 164 baseline cases (every algorithm, each of Modern, Mark I and OPL) are
  bit-identical through the engine, the JUCE plugin and the native CLAP; the topology test
  passes for all 32 algorithms and three engines; ASan+UBSan pass. Disabling the Mark I loop
  path makes five baseline cases (algorithms 4 and 6 on Mark I) differ, so the baseline
  covers it.

### M3 (done 2026-10-31)

- The engine holds eight sound contexts (`SynthContext`: voice data, controllers, LFO, output
  filter, mono and portamento state) over one shared pool of 16 voices; each voice keeps its
  context, key, channel and note ID. Every context renders on the same 64-sample grid, goes
  through its own output filter, and the contexts are summed. Context 0 is the default sound;
  with only it in use, rendering is unchanged (the 164 baseline cases are bit-identical through
  the engine, the JUCE plugin and the native CLAP).
- Voice choice is in one place, `VoiceStealing::choose` (Dexed's policy, unchanged), marked as
  a placeholder pending the voice-stealing decision (section 10, question 4).
- Octet implements `oscairn.sound-contexts/draft-4` (MIT header vendored in
  `libs/oscairn-clap/`): prepare from defaults plus values, or from patch text (the
  `oscairn-fm-state 1` format) plus values, refused whole on any bad value, with a diagnostic;
  a 32-entry selection queue applied at the start of the next process call, before its
  events; discard only when not selected, queued, sounding or ringing; IDs never reused; the
  default sound (ID 0) cannot be discarded; parameter events edit the sounds selected for
  their channel; `get_value` and CLAP state show the sound selected on channel 1, with
  parameter feedback when that changes; the exclusive snapshot (`octet-contexts 1`) saves
  every sound and each channel's selection.
- Tests (`oscairn-fm-test contexts`, plus the no-allocation and threads tests): held notes keep
  their sound across a switch and equal standalone instances sample for sample; old and new
  sounds play together; edits reach only the selected sound; note-ID release at an exact frame;
  raw MIDI repeated keys; sustain holding an outgoing sound; channel parts equal independent
  instances; capacity, queue exhaustion and pins; stale IDs; refused preparations; patch text;
  identical output for 37- and 512-frame blocks; snapshot round trip; no allocation while
  selections are applied; concurrent prepare/select/discard while processing under TSan.
  ASan+UBSan pass on macOS.
- Controllers per channel (Octet only; the engine's per-channel mode): mod wheel, breath,
  foot, CC 5 and CC 65 portamento, sustain, channel pressure and pitch bend act only on voices
  of the channel that sent them, also after a sound switch; with MPE, channel 1 is the master
  for all channels and member channels keep per-note bend. CC 120 stops only the addressed
  channel's voices and sends `NOTE_END` for each at its frame; CC 123 releases only that
  channel's held notes, matching voices as Dexed does (including Dexed's second transpose of
  the held pitch, kept so single-channel output stays identical). The desktop Dexed keeps
  instrument-wide controllers. Tested in `oscairn-fm-test contexts`.
- Where Octet still differs from RFC 0005's description of the Basic Synth (revisit with
  question 4): a raw MIDI note-off releases the first matching voice slot, which is not always
  the oldest press; no note expressions.

### M4 (done 2026-10-31, gates met where measurable; see the table)

Commits: `02c6811` (Extended voice, 88 algorithms, parameters, conversion), `c2ad09a` (fitted
tables, per-sample envelopes, init sounds, Extended test), `1a26b42` (attack and hold refit), the extended LFO table commit below. Measurement code: oscairn/measurement
`f12fb53` (`fm/eg.py`, `fit_eg.py`, `lfo.py`, `fit_tables.py`, `make_tables.py`, `calibrate.py`,
`compare.py`, `topology.py`). Second session: `7cc0925` (per-sample Extended LFO), `9a0aa9a` (algorithm 70),
`33ae421` (rate-based operator pitch EG, per time value), `abc697b` (modulation index and
feedback), `2976b05` (odd holds, attack 95, decay 85), `a14e132` (holds 94-99, attacks 96-99), `312febe`
(output level by algorithm);
measurement `a126d92`, `6b83d7b`.

- **Profile.** Parameter 5 (Classic, Extended) per sound. Extended values have their own IDs:
  40-51 common, 1000 n + 30..59 for operators 1-8; the Classic IDs are unchanged and the
  parameter list only grew (`tests/baseline/oscairn-fm-params.txt`, 431 parameters). The
  engine reads an `ExtendedVoice` beside the 156-byte DX7 voice (`msfa/ext_voice.h`); without
  one (the desktop Dexed) every sound is Classic. Classic stays bit-identical: the 164
  baseline cases pass through the engine, the JUCE plugin and the native CLAP.
- **Extended voice** (`msfa/ext_note.*`): 8 operators through the compiled graph with
  `extAlgorithms[88]`; multi-operator feedback (12, 14) rendered sample by sample;
  per-sample envelopes (hold at the release level, attack, decay 1, decay 2, release) with
  each operator's per-sample gains passed to its own kernels; rate-based operator pitch EG stepped per sample; per-sample LFO and per-sample phase increments; a modulator level curve of its own and the measured feedback amount;
  fixed-mode key follow; per-operator key-on reset; common pitch velocity depth; 2nd LFO
  start phase and extended speed range (`Lfo::reset(ExtendedVoice)`). Tables in
  `msfa/ext_tables_data.h`, written by `fm/make_tables.py`.
- **Init sounds:** `assets/octet/Classic Init.octet` (the DX7 init voice) and
  `assets/octet/Extended Init.octet` (the documented defaults, profile Extended: operator 8
  at level 85 in algorithm 69). **Conversion:** `convertToExtended` / `convertToClassic` in
  `oscairn/Parameters.cpp` (DX7 algorithm n maps to an Extended graph found by search, mostly
  n + 8 with operators 8..3); back to Classic only within the subset listed below.
- **Measurements** (renders of the reference plugin in `forks/measurements/m4/`, never
  committed): about 1,500 single-note renders in campaigns for envelopes (decay, release and
  decay 2 at every time value, attack and hold timed by a marker carrier at 31 times the
  fundamental, fast segments on a 4.19 kHz carrier), pitch (coarse, every fine value, detune
  at 8 notes, fixed mode, key follow, pitch velocity), operator pitch EG, LFO rates and
  topology (each algorithm with eight unrelated operator frequencies, with and without
  feedback). `fm/compare.py` runs the same analysis on reference and Octet renders of every
  case and checks `fm/tolerances.json`. Every pitch the reference plays is +0.91 cents sharp
  (ratio 1 at several notes; fixed mode +0.66); the comparison renders add +0.91 cents of
  master tune, the engine keeps standard tuning (replaced by decision 74: the offset depends on the pitch class and is now in the engine).

**Gate status** (as measured on 2026-10-31; "pending" means the reference renders were still
queued on the shared recorder when this was written):

| Gate | Result |
| --- | --- |
| Topology, all 88 | **Met for the graphs.** In-fork test (graph against Octet's render, solo and pairwise) passes for all 88 and the 32 DX7 graphs. Against the reference, 172 of 177 cases show the same spectral peak pattern and the other five differ only in weak lines. **Algorithm 65 resolved:** the transcribed reading (operators 3 and 4 both modulate 5-8) matches; the alternative (4 modulates 5 only) differs at many strong peaks. **Algorithm 70 resolved:** operators 1 and 2 both modulate 3 (the chart reading had 1 > 2 > 3); found by trying single-edge changes with `fm/topology.py` against Octet renders of an edited table; with it, algorithm 70 matches with and without feedback (the other one-edge changes tried, 1 > 5, 6, 7 or 8 added or moved, 1 > 4 moved, each left or added strong lines). The weak engine-only lines at algorithms 2, 69 and 79 (-37 to -43 dB) were modulation depth: Octet's index was 12 % (1 dB) higher than the reference's at operator level 75; with the modulator level curve and feedback amount fitted (below) they are gone. Now 176 of 177 cases match; the one left, f72 (algorithm 72 with feedback), has a reference line at -40 dB (Octet: -56 dB) whose frequency is only reachable by mixing the two separate carrier chains (f1 + f4 + f6 + 2 f8, or similar), so it is intermodulation in the reference's output, not a graph edge |
| Modulation depth (not a gate; found while checking topology) | Modulation index by modulator level, from the sideband pattern of a two-operator pair at every third level: Octet within 0.01 dB of the reference at all 32 measured levels (6-99). A modulator's level curve is its own (0.75 dB per step, -1.13 dB at 99, audible down to level 6, where a carrier measures silent below 12). Self feedback 1-7: 0.1845 times the DX7's amount at every value; harmonic 2 within 0.1 dB and harmonics 3-5 within 1.2 dB wherever they are above -75 dB (0.5 dB or better from feedback 3 up) (at feedback 1 and 2 harmonics 3-5 lie at -78 to -110 dB, near the renders' noise, and differ by up to 6 dB) |
| EG segment times 2 % | **Met.** Decay 1 at all 100 time values, release and decay 2 (every even value 0-30, and 55-99): pass (fast values on the 4.19 kHz carrier; the 262 Hz carrier cannot resolve decays faster than about 1 ms per dB). Decay time 85 was a measurement error (its 12-second note decayed only 14 dB, 467 ms/dB); it now takes the release measurement at 85 (877 ms/dB, the same as its pair 86); a 30-second render of decay 85 confirms it (876 ms/dB). Attack: pass at 0-40 (even), 40-90, the extra values 56-64 and 95-97 (95 in a 70-second note, 96-97 in 75-second notes), including attack to a lower EG level and lower operator levels, and 98-99 (66.5 s to 1 dB below the target, 75-second notes). Hold: pass at 0-40 (every value; the odd values now on the 4.19 kHz carrier) and 40-99; hold 0 is 0.011 ms (half a sample) short; holds 94-99 in 75-second notes (hold 99: 46.1 s) pass. **Operator pitch EG: met.** Every segment moves straight in cents at its time value's rate whatever its distance, so a segment of no distance takes no time; the rate table now has one value per time value (measured from full-range decays at all 100 values, slope of the instantaneous pitch, three calibration passes), the EG steps per sample with 16 extra fraction bits. Decay at all 100 values and attack at 0-80 (every tenth) and 5-85 (every tenth from 5) pass within 2 %; attack at 90, 95 and 99 does not move far enough in the 10-12 s reference notes to measure (it shares the decay's table, which passes there). This also models the slow drift towards 0 with initial and attack level equal (it is the decay, 12.6 cents per second at decay time 99, at every level) |
| Pitch 1 cent | **Met where measurable.** Coarse 0-31, fine 0-127 at several coarse values, detune -15..+15 at 8 notes, fixed mode 20 Hz-14 kHz with fine, key follow 0-99 at 5 notes, common pitch velocity depth -64..+63 at 9 velocities. Exceptions: coarse 31 with fine above 100 (the reference gave 13.0 in one campaign and 6.658 in another, so its behaviour there is not stable), fixed frequencies below about 20 Hz (not measurable in short notes), common pitch velocity +32 at velocity 1 (1.11 cents). Operator pitch EG levels -50..+50: pass at all 101 levels, refitted by extrapolating the pitch back to the note-on (the decay towards 0 starts at once, see below; the old table, read from a 0.05-0.5 s window, was about 3.5 cents towards 0) |
| LFO rate 1 % | **Met where measurable.** Normal speeds 0-99: all 100 pass (0-20 re-rendered with 5-13 cycles per note; the first renders held only 1.7-5 cycles and measured up to 4 % apart). Extended speeds (measured every fifth value): pass from 1 to 415, including 355-415 (377-1357 Hz) since Extended sounds run the LFO once per sample (pitch and amplitude modulation per sample, per-sample phase increments in the Extended kernels); Classic sounds keep the per-block LFO and stay bit-identical. 15 and 25 measured 1.1-1.2 % apart in 2-second notes (2.5 cycles); see the re-render note below. Speed 0 plays about 0.002 Hz, too slow to measure in 40 s. Values between the measured ones are interpolated; the table is piecewise linear, so speeds next to a slope change are unverified. In-fork test: the LFO, stepped per sample, matches its tables within 1 % at every normal speed and every fifth extended speed |

**Tests run locally** (second session, on the commits through `6f3ac57`): Release CTest 36/36
(164 Classic cases bit-identical through engine, JUCE plugin and native CLAP; topology 32 + 88;
Extended test, now with the per-sample LFO at every speed, the operator pitch EG and the wave
harmonics; Oscairn scanner and renderer), ASan/UBSan 19/19 and TSan 19/19 (those trees build
without the Oscairn tools). CI (`oscairn-tests`) for `f1a1678`: green on Linux x86_64, Linux
ARM64, macOS, ASan+UBSan and TSan.

**What is left of M4.** Polyphony and CPU on the target ARM64 hardware are not measured (no
access to it here); the per-sample LFO, pitch EG and wave lookups add work per operator, so
this matters before any voice-count claim. Unmeasured values: LFO speed 0 (about 0.002 Hz),
pitch EG attack at time 90-99 (the decay at those times, sharing the table, passes),
fixed frequencies below 20 Hz.

**CPU benchmark (eleventh session; desktop and CI numbers, not the target hardware).**
`oscairn-fm-test bench [seconds]` (`f0198b4`) holds notes at 48 kHz in 64-sample blocks on one thread:
a Classic voice with all six operators sounding (algorithm 1, feedback 6, LFO pitch modulation), an
Extended voice with eight sine operators (algorithm 1, feedback 5, the per-voice 2nd LFO on pitch and
amplitude), and the same with operator waves (formant 2 at spreads 5 and 7, odd and all harmonics,
formant 1), texture +20 and a 24 dB lowpass moved by its EG. An instance plays 16 voices (the engine's
polyphony); more voices take more instances, each with its own tables, so the large counts include
that memory traffic. RTF is render time over audio time; "fits" is the most voices, in steps of 16,
measured under 100 % and 50 % of one core (the scan stops at the first count over one core). It is not
a gate (decision 65).

| Where | Sound | RTF at 16 voices | RTF at 64 voices | Per voice per block | Fits in one core / half |
| --- | --- | --- | --- | --- | --- |
| macOS arm64 (this Mac, 1 s per measurement, machine loaded: load average 35 from other jobs) | Classic | 0.0119 | 0.0470 | 0.98 us | 1344 / 704 |
| | Extended, sines | 0.0545 | 0.2369 | 4.94 us | 288 / 144 |
| | Extended, waves, filter | 0.0705 | 0.2733 | 5.69 us | 224 / 112 |
| CI Linux ARM64 (`ubuntu-24.04-arm`, run 41291604861, 0.5 s) | Classic | 0.0179 | 0.0712 | 1.48 us | 880 / 432 |
| | Extended, sines | 0.0958 | 0.3881 | 8.08 us | 160 / 80 |
| | Extended, waves, filter | 0.1099 | 0.4463 | 9.30 us | 128 / 64 |
| CI Linux x86_64 (`ubuntu-24.04`, same run) | Classic | 0.0218 | 0.0824 | 1.72 us | 816 / 384 |
| | Extended, sines | 0.1111 | 0.4362 | 9.09 us | 144 / 64 |
| | Extended, waves, filter | 0.1319 | 0.5177 | 10.79 us | 112 / 48 |
| CI macOS (`macos-15`, same run) | Classic | 0.0220 | 0.0965 | 2.01 us | 736 / 400 |
| | Extended, sines | 0.1104 | 0.4305 | 8.97 us | 160 / 64 |
| | Extended, waves, filter | 0.1282 | 0.4436 | 9.24 us | 144 / 64 |

An Extended voice costs about 5-6.5 times a Classic one here (per-sample envelopes, LFOs and pitch, and
floating point; the NEON path stays off). A first run on this Mac with 0.5 s measurements gave 6.0-8.0
us per Extended voice per block; shared CI runners vary by more than that between runs. The target
ARM64 hardware is still not measured.

**Decisions taken while implementing (for Jacob to confirm)**

1. Extended values are separate parameters (IDs above), not shared with Classic ones even where
   the range matches; Classic Transpose and the settings (100-199) apply to both profiles.
2. The operator Pitch Velocity Sensitivity had no audible effect in the reference (-7..+7 at
   depths -64 and +63); it is kept as a parameter and not applied.
    *Resolved in the thirteenth session: the sensitivity moves fixed-frequency operators only (decision 73).*
3. The reference's +0.91 cent tuning is not copied into the engine (standard tuning in M4).
    *Resolved in the thirteenth session: the reference's tuning depends on the pitch class; Extended sounds now play it (decision 74).*
4. Back to Classic only when: the algorithm is the image of a DX7 algorithm with the other two
   operators silent; one key-on reset and one LFO pitch depth offset for all operators; no
   hold, operator pitch EG, pitch velocity, key follow or negative velocity; amplitude depth
   offsets 0, 2, 4 or 7; LFO phase 0 and the normal speed range. Rates, times, fine and detune
   convert to the nearest value (approximate both ways).
5. Unmeasured, using the Classic curves: level scaling, level velocity (mirrored for negative
   values; measured and replaced in the fifth session, see M7), time key follow, LFO depth offsets, LFO delay; the LFO start phase is taken as the
   start of the drawn wave. (The 2nd LFO's depths, offsets, delay and start phase were measured
   and replaced in the seventh session, see M7 and decisions 46-48.)
    *Confirmed (Jacob, 2026-11-01).*
6. The init sounds are named "Classic Init" and "Extended Init" (no product names).
7. Measured differences Octet keeps on purpose (second session): the operator waves are
   band-limited (the reference's are not, and alias audibly at the top of the keyboard), and a
   formant wave whose centre is above Nyquist is silent (the reference plays a non-harmonic
   tone near Nyquist there). Both are departures from the reference's behaviour; the gate's
   aliasing bound asks for the first, the second follows from it.
    *Listening examples for Jacob in `forks/measurements/listen/` (thirteenth session); still to confirm.*
8. Operator wave names: Wave (Sine, All harmonics 1, All harmonics 2, Odd harmonics 1, Odd
   harmonics 2, Formant 1, Formant 2), Spread, Formant (parameters 1000 n + 60..62).
9. The wave tables take 7.7 MB (10.6 MB with decision 57), built once per process in SynthEngine's constructor (the
   desktop Dexed builds them too, since it shares the engine).
10. Extended carriers play the reference's output level per algorithm (measured, 0 to
    +13.55 dB above algorithm 1) and are 6.57 dB lower overall than a Classic carrier, the
    level of the reference's probes; the absolute level may also include the probes' Part and
    master volume, so Jacob may prefer another overall level (normalising to the loudest
    algorithm instead would make algorithm 1 7 dB quieter than the reference).
    *Replaced in the thirteenth session (decision 69): the 6.57 dB was the probes' Part Volume and pan.*
11. A modulator's level follows its own measured curve (0.75 dB per step, audible down to
    level 6), different from a carrier's; an operator is a modulator when its algorithm does
    not route it to the output (no algorithm does both).
    *Confirmed as a measured match (Jacob, 2026-11-01).*
12. Operator waves 1 and 2 carry a constant (they run from 0 to their peak) because the
    reference's modulators show it; as carriers the constant reaches the output only as a DC
    step that Dexed's output DC filter removes (the reference's own output highpass does the
    same). A carrier-only offset-free version would be indistinguishable after that filter.
    *Confirmed as a measured match (Jacob, 2026-11-01).*
13. The reference's output highpass (about 10.3 Hz, first order) is not copied; Dexed's DC
    filter (about 20 Hz) stays for Extended sounds too, 1 dB lower at 33 Hz (note 24).
    Reversed in the fifth session (decision 38).
14. CI: the macOS job of `oscairn-tests` now runs on macos-15 (GitHub's macos-14 brownout);
    `dexed-pipeline` is left as upstream has it.
    *Jacob, 2026-11-01: keep macOS 15 for CI.*
15. Filter type values (M6): Octet's Type parameter (60) numbers the 19 types 0-18 in the
    documented order (18 Thru, the default); the reference's own values skip 9, 18 and 20
    (0-8, 10-17, 19, 21), so the converter maps them. The choices are shown with the
    documented abbreviations (LPF24D ... LPF12+BPF6, Thru), which are generic filter names.
16. Cutoff and HPF key follow are shown as signed steps (-64..+63, 3.125 % each; +63 plays
    +200 %) rather than percent.
17. The reference overloads at high resonance (LPF24D, HPF24D, BPF12D above resonance 80 give
    an output full of harmonics); Octet does not model that, it only bounds the filter's
    states. Its filter runs in floating point after the operators; Extended voices are no
    longer rounded to 16 bits (decision 32).
    *Jacob, 2026-11-01: recorded as a known difference, to look at later (listening examples in `forks/measurements/listen/`).*
18. Filter coefficients and the filter EG are updated once per 64-sample block, ramping the
    coefficients linearly across it (the FEG's fastest measured segments, 0-20, are instant
    in the reference and take one block in Octet).
    *Explained and kept in the thirteenth session (decision 75).*
19. FEG time velocity sensitivity is kept as a parameter and not applied (the reference made
    a segment instant at velocity 127 with both -64 and +63; unresolved); negative FEG time
    key follow mirrors the positive rule, though the reference measured shorter there too.
    *Negative time key follow mirroring the positive rule: confirmed (Jacob, 2026-11-01). Time velocity sensitivity: resolved and applied in the thirteenth session (decision 72).*
20. Octet's FEG segments run straight in cents; the reference's bend slightly (up to about 90
    cents along a 1200-cent segment).
    *Jacob, 2026-11-01: recorded as a known difference, to look at later (listening examples in `forks/measurements/listen/`).*
21. A sound whose filter is not Thru at gain 255 does not convert back to Classic (Dexed's
    output filter is a plugin setting, not part of the sound).
22. Carrier feedback (found in M6): the feedback amount fitted in M4 now acts on a carrier's
    signal before the reference's output level, which `312febe` added later and which had
    halved the feedback (decision 10's level applies to the output only).
    *Confirmed as a measured match (Jacob, 2026-11-01).*
23. Common pitch EG (fourth session): parameters 28-39 under Extended/Pitch EG, depth shown as
    "8 octaves" ... "0.5 octave". A sound whose common pitch EG levels are not all 0 does not
    convert to Classic, and "convert to Extended" does not map the DX7 pitch EG onto it (it
    could, approximately; not done).
24. Depth velocity sensitivity scales the common pitch EG's rates as well as its levels, so
    segment times stay the same at every velocity (measured at one point: 1.35 % off).
    *Confirmed as a measured match (Jacob, 2026-11-01).*
25. Pitch key follow (56) stores the documented value 0-127 and shows the percent each value
    plays, unlike cutoff key follow's signed steps (decision 16), because its percent steps
    are uneven. Centre (57) is a MIDI note, C3 = 60 by default.
26. Random pitch and random pan draw from one xorshift generator per sound (two values per
    Extended note), reset by prepare, so renders are deterministic; both spreads are uniform
    approximations (random pan's left bias in the reference is not modelled). The distributions
    were refitted in the sixth session (decision 44).
27. Key-on delay: the table is in 64-sample blocks at 44.1 kHz and plays the same time at every
    sample rate; the note starts at its exact sample (its output then runs late within the
    64-sample grid); tempo sync takes the CLAP transport's tempo, 120 bpm without one; a note in
    its delay holds its voice.
28. Stereo: the engine renders a right channel when given one, through a second copy of each
    context's output filter (about twice the output filter's CPU in Octet); unpanned voices go
    to both channels alike, so Classic sounds are unchanged in both channels. The desktop Dexed
    stays mono.
29. Pan positions (alternate, scaling, random) add and clamp to -64/+63; alternate pan counts
    the sound's Extended notes since prepare (per sound, not per key or channel).
30. Part LFO: one per sound context, shared by its voices. Its destinations are Octet's own list
    (Off, Pan, LFO 2 Speed, Filter Cutoff, Filter Resonance, Feedback, Operator Frequency,
    Operator Wave, Operator Detune, Operator Level: the order of the reference's numbers
    70-78), so the converter maps the reference's numbers and drops the insertion-effect ones;
    all nine act.
    Parameters 200-234 (voice parameters now also use 200-299; Parameters.h says so) and
    1000 n + 63..68 for each operator's three switches and depth ratios.
31. The part LFO's envelope (delay, fade in, hold, fade out) restarts whenever its phase resets,
    and on a first key-on when key-on reset is off (unmeasured); a sound with a part LFO
    destination that has depth does not convert to Classic.
    *Confirmed (Jacob, 2026-11-01).*
32. Extended voices skip Dexed's 16-bit rounding of each voice (fifth session): each voice's
    output goes to the mix in floating point (full scale unchanged, clipped at +-1); no longer clipped since decision 70. Classic
    voices keep the rounding, so Classic output is bit-identical. Reason and numbers: the
    filter gate below.
33. Part values (M7) are Extended parameters 235-245 under Extended/Part (velocity sensitivity
    depth and offset, the AEG, FEG depth, cutoff and resonance offsets, note shift, detune) and
    the macros 246-252 under Extended/Macros, named Mod Depth, Ratio Shift, Attack, Decay,
    Sustain, Release, Texture (section 9's generic names). A sound with any of them away from
    its default does not convert to Classic.
34. Ratio Shift and Texture are stored and not applied (their behaviour is not resolved);
    detune is shown as signed steps (-128..+127) rather than Hz or cents.
35. A note whose part velocity comes out at 0 or below plays nothing and gets its NOTE_END at
    once; fractional part velocities are rounded (for operator level velocity no longer: decision 45).
36. Mod Depth cannot raise a modulator past level 99, in Octet as in the reference (`m7e`: a
    static pair at levels 60, 80 and 99 with the macro taking it to 99-147 plays exactly as
    level 99, 20 cases within 0.16 dB rms).
    *Confirmed as a measured match (Jacob, 2026-11-01).*
37. Extended sounds bend by the part's signed ranges, Pitch Bend Upper and Lower (253, 254,
    -48..+24 semitones, defaults +2 and -2, the documented values), not by the plugin's Pitch
    Bend Up/Down settings; the plugin's bend step setting, when on, still quantises as in
    Dexed. Implemented from the Data List only (the recorder cannot bend).
38. Extended sounds' output filter uses the reference's highpass (first order, 10.3 Hz) in place
    of Dexed's DC filter; Classic sounds keep Dexed's exactly (the contexts switch it with their
    profile).
    *Confirmed as a measured match (Jacob, 2026-11-01).*

39. Mono/poly, key assign and portamento of Extended sounds are part parameters 255-260 under
    Extended/Part (Mono/Poly, Key Assign, Portamento, Portamento Time, Portamento Mode, Portamento
    Time Mode; defaults Poly, Multi, Off, 64, Full-time, Rate 1). The reference glides only when
    both the performance's portamento switch and the part's are on (the part's documented default
    is On, the performance's Off, so a new performance does not glide); Octet has one switch, Off
    by default, which the converter should set only when both are on. Extended sounds ignore the
    plugin's Mono, Portamento Time and Glissando settings (101-103), which Classic sounds keep. A
    sound with any of 255-257 away from its default does not convert to Classic.
40. Mono keeps Dexed's structure: each held key keeps a pool voice and only one is live, so voice
    stealing across the pool is unchanged (question 4). For that decision: a held mono key whose
    voice another sound steals cannot be returned to when the sounding key is released.
    *Jacob, 2026-11-01: waits on the voice-stealing decision (Oscairn RFC 0018, Q4).*
41. A mono note without portamento, and a key played again with key assign Single, fade the
    sounding note out and start the new note from silence when the fade ends. The fade's length
    follows the carriers' slowest attack time: none at 0, 2, 5 and 13 ms at 20, 40 and 60 (`m7g`,
    from the sustain level), 13 ms above 60 (unmeasured), straight in amplitude. The reference
    seems to drop the same envelope quickly and attack from where it got (at attack 20 it dips
    only about 6 dB before attacking); Octet's fade-then-start reproduces 40 and 60 but dips to
    silence at 20. (Replaced by decision 54 in the eighth session.)
42. A MIDI key pressed n times while held needs n note-offs, then releases every voice it holds
    (measured, Single and Multi). CLAP note-offs keep their matching: by note ID per note, and a
    note-off for a key without an ID releases every held voice of that key at once.
    *Confirmed as a measured match (Jacob, 2026-11-01).*
43. Unmeasured, chosen: returning to a held key in mono draws new random pitch and pan values and
    counts as a new note for alternate pan; portamento moves ratio-mode operators only, like the
    common pitch EG (fixed-frequency operators keep their frequency); the band-limited wave is not
    chosen again during a glide (the existing shortcut). (Seventh session, `m7k`: a fixed-frequency
    carrier keeps its frequency through a legato glide in the reference too.)
    *Confirmed (Jacob, 2026-11-01).*
44. Random pitch and random pan (replaces the distribution part of decision 26, from `m7f`, 32-128
    notes per depth): random pitch is a uniform value within depth / 4 steps of 18.75 cents either
    way, cut towards 0 (depth 4 never moves); random pan is uniform within depth / 2 positions
    either way. Not modelled: the reference's one extra step down at depths 32-127, its narrower
    spread at pan depth 127 (standard deviation 28 positions against uniform's 36) and its left lean.
    *Measured parts confirmed (Jacob, 2026-11-01); the unmeasured choices stay open.*
45. Fractional part velocities (revises decision 35's rounding): the operator level velocity
    takes the velocity with its fraction (the reference plays it between its neighbours, and below 1
    down to 1/4 by +7's measured curve; at 1/4 and below +7 is silent); every other velocity-dependent
    value still takes the rounded velocity (unmeasured). Positive sensitivities follow +7's curve,
    measured at velocities 1-6 and 8 in this campaign, in proportion between velocities 1 and 8.
    *Confirmed (Jacob, 2026-11-01).*
46. The 2nd LFO runs per voice in Extended sounds (seventh session, measured): each note runs its
    own copy of the sound's LFO, stepped per sample in the note, with its own delay; Classic sounds
    keep Dexed's one LFO per sound. Chosen where unmeasured: a note without key-on reset takes the
    sound's free-running phase (the reference's phases with key-on reset off followed no pattern
    over three notes at irregular times, `m7l` `r0i`, and a note overlapping a held one did not take
    the held note's phase); a mono legato note (portamento on) carries the sounding note's LFO on; a
    returned-to mono key starts its own. CPU: each Extended voice now steps an LFO per sample, as
    each sound did before (not measured on the target hardware). The part LFO stays one per sound
    (measured as such in the sixth session).
    *Confirmed (Jacob, 2026-11-01).*
47. The 2nd LFO depths are the reference's (seventh session): pitch twice the DX7's, depth 99 as
    256, offsets 4-7 deeper than the DX7's sensitivities; amplitude straight in dB from the LFO's
    top, 0.375 dB steps by depth from a measured table and eighths by offset. The mod wheel's
    amplitude and pitch modulation in Extended sounds keep the DX7 path (unmeasured); an operator
    takes the larger of the LFO's and the wheel's level loss. "Convert to Extended" (and back) picks
    the pitch depth that swings as far with the same offset; the amplitude depth and its offsets
    still convert value for value (approximate: the curves differ).
    *Confirmed (Jacob, 2026-11-01).*
48. The 2nd LFO's phase 0 starts half a cycle into the waves as Dexed's LFO draws them (measured for
    every wave and phase), and its delay fades in at the hold's rate (measured). The filter depth
    keeps its measured direction (the reference's triangle starts at the cutoff's highest point).
    *Confirmed as a measured match (Jacob, 2026-11-01).*
49. The part AEG attack offset multiplies every attack time by 2^(offset / 4), held within attack
    times 0-99 (a positive offset on carriers only, as before), and the sustain offset follows a
    table of measured moves at even offsets from -22 to +32 (odd offsets halfway between, 1.5 per
    step beyond -22, unmeasured); decay and release offsets keep 1.5 values per step (seventh
    session, `m7h`).
    *Confirmed (Jacob, 2026-11-01).*
50. Key-on delay in mono legato with portamento (seventh session, `m7k`, measured): the legato note
    takes over the sounding note's key-on delay, so it sounds at once when that note has started and
    when that note would have started while it still waits (Octet used to give the legato note a
    delay of its own, with silence in between). The common pitch EG carries on through a legato
    note in the reference as Octet already did. Mono without portamento, which starts the note
    again, keeps a delay of its own (unmeasured).
    *Confirmed (Jacob, 2026-11-01).*
51. A modulator's EG levels (eighth session, `m8a`, measured): they step as its operator level does
    on the modulator curve (0.75 dB per step), so operator level and EG level add as values (level 70
    at EG 85 plays as level 56 at EG 99), in steps of two (an even EG level plays as the odd one
    above it). Carriers keep the measured carrier EG curve (whether carriers' EG levels also step in
    twos was not checked).
    *Confirmed as a measured match (Jacob, 2026-11-01).*
52. Ratio Shift (eighth session, `m8b`, `m8d`): a table of the ratio the reference plays at nine
    ratios (0.5, 1, 1.5, 2, 3, 4, 5, 8, 16) and 79 macro values (every third and every eleventh),
    straight between them; above ratio 16 the offset at 16. Carriers are not shifted (measured);
    fixed-frequency modulators are not shifted either (unmeasured: the probe's fixed frequency was too
    low to read). The reference's curve is not smooth between the measured values (up to 5.7 cents
    off at independent values in between, see M7).
    *Confirmed (Jacob, 2026-11-01).*
53. Texture (eighth session, `m8c`, `m8f`): a wave by the macro's size, in steps of two (listed at
    `extTextureWave`); negative values reach carriers, positive ones modulators; carriers take the
    second all and odd harmonics waves and modulators the first. Chosen where unmeasured: the texture
    replaces the operator's own wave (only sine operators were measured); above 85 the formant rises
    by five per value to silence (the reference's renders there match no wave of Octet's; decision
    7); values 32-33 play a sine 6.2 dB down on modulators as measured on carriers.
    *Measured parts confirmed (Jacob, 2026-11-01); the unmeasured choices stay open.*
54. The retrigger (replaces decision 41; eighth session, `m8e`, measured with the two notes two octaves
    apart so each note's level reads on its own): the new note starts at once, attacking from
    silence, while the sounding note fades out over 13.3 ms with a gain of the square of the time
    left's share. The fade is the same at every attack time 0-60 (at 70 the first note had not
    finished its attack; at 80 and 90 it was still near silent, so nothing could be read there), so
    the fade above attack 60 is taken as the same. A key played again with key assign Single and a
    mono return to a held key use the same fade (unmeasured). Octet's fade runs about 1 ms ahead of
    the reference's in its first 10 ms (-13 against -9 dB 6 ms after each falls 1 dB).
    *Confirmed (Jacob, 2026-11-01).*
55. Waves 1 and 2 start half a cycle on, at their minimum (ninth session, `m9a`, measured): as
    modulators of a sine `spec5` could not tell this from the third session's start at the peak; as
    carriers of a sine the two differ and the reference's is this one.
    *Confirmed as a measured match (Jacob, 2026-11-01).*
56. A modulated formant wave plays window(t + m) sin(centre t + m) (ninth session, `m9a`, measured):
    the centre's sine takes the modulation once. The kernel hands each operator's wave its phase and
    its modulation (modulators plus feedback) apart; feedback on a formant or odd wave operator takes
    the same path as a modulator's input (unmeasured).
    *Confirmed (Jacob, 2026-11-01).*
57. A modulated odd-harmonic wave (3, 4) plays E(2t + m) sin(t + m), the wave being E(2t) sin(t)
    (ninth session, `m9a`, measured). The engine builds E for each odd wave and spread in the same
    band-limited versions as the wave tables, 2.9 MB more (10.6 MB in all, shared by every
    instance); the wave tables of waves 3-4 are kept (the test of where the waves sit reads them),
    though the kernel no longer plays them.
    *Measured parts confirmed (Jacob, 2026-11-01); the unmeasured choices stay open.*
58. Rising envelope segments (ninth session, `m7a`, measured on decay 2 rising to 99 on a modulator
    and a carrier): every rising segment, not only the attack, approaches 1.52 dB above full level
    exponentially in dB at its own rate. Unmeasured, taken the same: a rise to a level below 99 and
    a rising release. Only the attack starts at most 56.3 dB below full level (unmeasured for the
    others).
    *Confirmed (Jacob, 2026-11-01).*
59. A carrier's EG levels go in steps of two from 20 up, like a modulator's (ninth session, `m9b`,
    measured, replaces the carrier half of decision 51): the m9b table at the odd level (98 plays as
    99). Below 20 the earlier carrier table (its 12-19 values were measured one by one and do not
    pair cleanly).
    *Confirmed as a measured match (Jacob, 2026-11-01).*
60. The texture macro starts from an operator's own wave (tenth session, `m7a` wave probe and
    `m10t`, measured): a wave on the macro's path has a place there (waves 3-4 at +-(2 + 2 spread),
    waves 1-2 at +-(32 - 2 spread), the formant path's places for formant 2; the plus sign for
    waves 1 and 3), the value moves it, and the place's sign picks the family; a sine sits at 0,
    so sine operators play as before. Chosen where unmeasured: formant 1 and formant 2 off the path
    are replaced as a sine's are, and formant 2's places on the path are taken from the table
    without a check.
    *Measured parts confirmed (Jacob, 2026-11-01); the unmeasured choices stay open.*
61. The 2nd LFO's amplitude loss bends below about -45 dB as a carrier's EG levels do (tenth
    session, `m7l`, measured at depth 99 offsets 3 and 4; the curve is `m9b`'s): the offset 7 table
    stays the swing as played (its inverse gives the straight loss), offsets share the straight
    loss in eighths, and the result goes through the bend. Unmeasured, chosen: the bend acts on the
    LFO's loss alone (not on the sum with the EG level) and on modulators as on carriers.
    *Confirmed (Jacob, 2026-11-01).*
62. Detune on fixed-frequency operators (tenth session, `m10d2`, measured): the operator's own
    detune moves them by the same cents as a ratio-mode operator at that key; the part's detune
    (Hz) does not reach them.
    *Confirmed as a measured match (Jacob, 2026-11-01).*
63. Formant 2's window components past the main lobe's null are negative (eleventh session, from
    the phases of the `spec2` and `spec4` renders, which agree on where the sign turns within one
    component; checked on `spec`, `spec5`, `m9a`, `m8c`, `m8f`, `m7a`). The tables keep each
    component's real part, so components near the null, whose phase lies between, are scaled down.
    *Confirmed as a measured match (Jacob, 2026-11-01).*
64. Portamento moves in steps (eleventh session, `m7f`, `m7g`, measured with
    `fm/glide_steps.py`): one every 440 samples at 44.1 kHz (9.977 ms), each the time value's rate
    times that period, the first at the glide's start. Unmeasured, chosen: the step clock starts with
    each glide (the sequence recorder's timing cannot show whether the reference's runs freely), and
    the period is the same time at every sample rate. The rate table is unchanged (its values at
    times 1-8 are 0.5-0.9 % above the step readings; within the 2 % tolerance).
    *Measured parts confirmed (Jacob, 2026-11-01); the unmeasured choices stay open.*
65. The CPU benchmark (`oscairn-fm-test bench`) is informational: the plan sets no CPU budget, so it
    is not a gate. CI prints it on the three Release jobs.
66. Portamento's step clock runs freely (twelfth session, `m7f`, measured with `fm/glide_grid.py`;
    replaces decision 64's unmeasured choice): one clock for every sound in the engine, counted from
    prepare, ticking at the start of each 64-sample block in which another 440 samples at 44.1 kHz
    end, so steps come 6 or 7 blocks apart; every gliding note moves one step in a ticking block. At
    other sample rates the period stays 9.977 ms on Octet's own 64-sample blocks (the reference was
    only measured at 44.1 kHz). The reference's clock phase against a note is not known for a render,
    so glide comparisons of few steps depend on where the ticks fall.
    *Confirmed, measured and unmeasured parts (Jacob, 2026-11-01).*
67. The texture macro's path (twelfth session; replaces decision 53's choice of formant 2 on both
    sides, decision 53's value-by-value formants above 63 and the shortcut above 85, and decision 60's
    "formant waves off the path are replaced as a sine's"): the plus side (modulators' values from a
    sine) has formant 1, the minus side formant 2, as for the other kinds; above place 63 the formant
    rises one per place to 30 at place 86, then by 69 / 13 per place to 99; every wave has a place,
    a formant wave by its spread and formant also off the path (formant 1 positive, formant 2
    negative, whether the operator is a carrier or a modulator); a place moved past +99 or -99 comes
    round from the other end. Checked on renders: `m8c` +51 to +66 on the level 70 modulator met
    (1.2-7.6 dB before), and in the 33 lone-carrier renders from -64 to -96 (`m8c`, `m8f`; the centre
    below Nyquist) the reference's strongest harmonic is the new formant's centre in 32 (one harmonic
    above it at -64), the earlier formants' in 11; Octet's own strongest harmonic now agrees in 32
    (was 14). Unmeasured: the wrap, and places of off-path formant waves.
    *Confirmed, measured and unmeasured parts (Jacob, 2026-11-01).*
68. No refactor for upstream merges (twelfth session, section 8): the Extended hooks stay in
    `dx7note.*` and `lfo.*`; the conflict that occurs is the headless split's, ported by hand.

69. Extended voices play at the reference's own level (thirteenth session, `m13v`, measured): the
    6.57 dB of decision 10 was the probes' Part Volume 100 (4.13 dB under 127) and centred Part Pan
    (3.01 dB under a hard pan on its side). Both belong to the performance, so the converter maps
    them and the engine does not: a full-level carrier in algorithm 1 is now 0.42 dB above a Classic
    carrier (level 99 sine at note 60: -24.63 -> -17.64 dBFS peak; Classic -18.08). Inside the voice
    the old level stays (the int32 buffer and the filter's bounds keep their headroom); the mix
    raises Extended voices by 6.99 dB (`kExtendedVoiceGain`). The reference's level stages, for the
    converter (dB against 127, sine probe; velocity 1-127, Part Velocity Sensitivity Depth 0-127 and
    the master EQ switch change nothing at operator level velocity 0):

    | value | Part Volume | Performance Volume | 40 log10(v / 127) |
    | ---: | ---: | ---: | ---: |
    | 1 | -84.30 | silent | -84.08 |
    | 8 | -48.03 | -48.80 | -48.01 |
    | 16 | -36.03 | -36.76 | -35.99 |
    | 24 | -28.89 | | -28.94 |
    | 32 | -24.01 | -24.01 | -23.97 |
    | 40 | -20.26 | | -20.07 |
    | 48 | -16.88 | -17.25 | -16.90 |
    | 64 | -12.01 | -12.00 | -11.91 |
    | 80 | -7.88 | -8.25 | -8.03 |
    | 96 | -4.88 | -4.87 | -4.86 |
    | 100 | -4.13 | -4.50 | -4.15 |
    | 104 | -3.38 | | -3.47 |
    | 112 | -2.25 | -2.62 | -2.18 |
    | 120 | -1.13 | -1.13 | -0.99 |

    The two volumes add (Part 127 with Performance 64: -7.88 against 100/127, as -12.00 + 4.13). Part
    Volume's measured values all lie on 0.375 dB steps. Dry Level: 0 silent, 64 -6.02, 100 -2.26 dB. Part Pan is
    equal-power: centre -3.01 dB on each side against hard pan, 0 left +3.01 / right silent, 32 left
    +2.34 / right -5.47 against centre (96 the mirror). The engine's own pans (alternate, scaling,
    random) keep their measured law with no centre dip (decision 29).
70. Extended voices are no longer clipped at full scale on their way to the mix (thirteenth session):
    at the new level the loudest algorithms pass it (the topology test failed with the clip). The
    reference's output does clip at full scale, after part volume and pan: `m7a` fr96 and fr127 (a
    resonant peak 26 dB over thru) peak at exactly 0 dBFS in its renders and Octet's voice used to
    clip at the same point by Dexed's per-voice clip; they now differ by 13.86 dB. Decided by Jacob
    (2026-11-02): output clipping belongs to Oscairn's mixer, not to Octet, since the reference clips
    at the mix (after part volume and pan). No engine change.
71. Sample rates (thirteenth session): `oscairn-fm-test rates` (CTest `oscairn-fm-rates`) checks
    Extended level, pitch, EG times, the 2nd LFO's rate and delay, the part LFO's rate, key-on delay,
    two filter types, an FEG decay and portamento at 44.1, 48 and 96 kHz against each other within
    the reference comparisons' tolerances; all agree and nothing was found still counted in 44.1 kHz
    samples. Verified against the reference only at 44.1 kHz (the recorder runs there, as the
    hardware does internally): everything in the comparison tables. Behaviours that change with the
    rate by design: the waves' band-limiting and the silence of formants above Nyquist (decision 7)
    follow the running rate's Nyquist, so above 44.1 kHz the top notes keep harmonics the reference
    cannot play; the filter's cutoff limit is the reference's 7.35 kHz (a sixth of 44.1 kHz) at every
    rate, and its Chamberlin sections, matched in Hz, warp differently near that limit at other rates
    (checked at cutoffs 300 and 512 only); the FEG and filter coefficients follow 64-sample blocks of
    the running rate, and portamento steps land on them; the retrigger fade and the reference's
    tuning offsets were measured at 44.1 kHz only.
72. FEG time velocity sensitivity (thirteenth session, `m13f`, measured, replaces the time velocity
    half of decision 19): a positive sensitivity moves the time value by sensitivity x (velocity - 1)
    / 127; any negative sensitivity makes the chosen segments instant above velocity 1. The segment
    value picks attack, attack and decay, decay, attack and release, or all (measured for the attack
    and decay 1). Unmeasured, chosen: velocities 2-15 with negative values, decay 2 going with decay 1,
    the release with "Atk+Rls" and "All", and the hold never moving.
73. The operator Pitch Velocity Sensitivity (thirteenth session, `m13q`, measured, replaces decision
    2) moves fixed-frequency operators only, straight in velocity from 64: 73.75, 147.7, 304.05,
    904.15 and 1504.05 cents at velocity 127 for 1, 2, 3, 5 and 7 (negative values mirror); 4 and 6
    halfway (unmeasured). The earlier probes used ratio-mode operators, which it does not move.
74. Tuning (thirteenth session, `m13t`, `m13u`, measured, replaces decision 3): the reference plays
    each pitch class slightly sharp of equal temperament (C +0.909, C# +1.671, D +1.107, D# +0.763,
    E +1.627, F +1.317, F# +0.896, G +1.380, G# +1.263, A +0.665, A# +1.548, B +1.300 cents), the same
    in every octave; fixed frequencies 0.775 cents above the 440 Hz grid on average (+0.59 to +0.97
    by fine value). Extended sounds now play these (engine behaviour: constant across velocity and
    ratio, not a master tune; the analog engine's renders are exact); the comparison renders no
    longer add +0.91 cents of master tune. Not modelled: about +-0.2 cent left between the measured
    points (ratio 3 at A plays 0.11 cent above ratio 1). Whether the offsets change with the host's
    sample rate could not be measured (the recorder runs at 44.1 kHz).
75. The filter and its EG stay updated once per 64-sample block (thirteenth session, decision 18
    explained). Why: each block's coefficients take the cutoff's exponential, a sine and the damping
    limits per section (and the FEG's step); doing that every sample instead of once per 64 adds it
    to every sample of every voice (not measured), while straight ramps across the block cost one
    addition per coefficient. The cost in sound: a segment the reference plays
    instantly (times 0-20) takes one block (1.45 ms at 44.1 kHz) in Octet. Tried in a local build
    (not committed): stepping the coefficients at once when the FEG takes an instant segment. The
    reference's own instant segments arrive about 2 ms after Octet's, so the step made Octet's holds
    earlier still: fm/compare_feg.py on `feg2` went from 91 to 82 of 92 (ht4-ht36 about 2.2 ms early
    against the 2 ms floor). Kept as it is.
76. The pitch grid (fourteenth session, `m14s`, `m14b`, with `m13u`, `m13t`, `fine`, `detune`,
    `m10b`, `pitch` and `pitch2`, measured; replaces decision 74). Jacob's hypothesis holds, with the
    second rounding found: every operator plays a whole step of 1/2048 octave, at
    44100 x m x 2^(octave - 25) Hz, where m is 4096 x 2^(step / 2048) rounded to an integer. That
    rounding is the residual (+-0.2 cent at most, by step). A4 is step 13012: on the grid 0.773 cent
    above 440 Hz, played at 440.169 Hz (+0.665) after the mantissa's rounding. So the constant is
    neither a reference-pitch offset of its own nor a clock ratio: it is the same in cents in every
    octave, and fixed coarse 16 fine 0 is A4's step. Where each term is quantised:
    - the key: rounded up to the 1/1024-octave grid counted from A (85 1/3 steps of 1/1024 octave a
      semitone), so every third key (C, D#, F#, A) lies on it and the others play 0.39 or 0.78 cent
      above it before the mantissa (Note Shift moves the key before the rounding: `m14s`);
    - ratios: rounded to the nearest 1/1024-octave step (all 128 fine values of coarse 1 and 15 other
      pairs); the operator detune table lies on the 1/1024-octave grid (105 measured values);
    - fixed frequencies: whole 1/128-octave fine steps from A4's step, up to coarse 21 fine 0 (the
      highest measured, 14085.4 Hz);
    - the part detune: whole steps, odd ones too, the same at every key (`m14s`);
    - the pitch bend: its cents truncated towards 0 to whole cents, then to whole steps (`m14b` in
      steps of 0.1 and 0.3 cent at keys 60 and 61, `m10b`; the same steps at both keys, so it adds
      after the key's rounding).
    The sum of the steps decides the frequency. Every steady render of a single carrier above
    100 Hz lies on this grid within 0.01 cent, but for the ratio past coarse 31 (0.15 cent off it)
    and the common pitch velocity at velocity 127 (0.03 cent off). Octet now plays the rule for
    Extended sounds (`extNoteSteps`, `ExtFreqlut`), Classic bit-identical. Chosen and not measured:
    the other static offsets (part detune, key follow, random pitch, pitch velocity) go through their
    tables and are rounded to the nearest step; pitch EG, LFO, portamento and master tune are added
    before the lookup, which takes the step below. The tables the rule leaves as they were now miss
    by whole steps where they missed before: the detune between its measured keys (5 of 20 new
    values one 1/1024-octave step off, up to 1.18 cent), the part detune between its measured values,
    the common pitch velocity (`pitch` cpv32v1 now 1.12 cent off) and the ratio past coarse 31. At
    48 and 96 kHz Octet plays the same frequencies in Hz: the residual is the mantissa's, not a
    44.1 kHz phase-increment effect (each increment is a whole number at 44.1 kHz), and the hardware
    and the recorder run at 44.1 kHz; the reference plugin at other host rates was not measured.
    Octet's increments are rounded, so 48 kHz plays within 0.01 cent of 44.1 kHz from 55 Hz up and
    96 kHz within 0.09 cent at 55 Hz (its 24-bit phase).
77. AEG time key follow (fifteenth session, `m15k`, measured; replaces decision 5's shortcut for
    it). The reference moves every segment's rate, not its time, by whole steps of its EG rate scale:
    eight steps to a doubling of the rate, each doubling split into eight equal steps, with the decay
    table's times on it (time 0 step 119, times 93-99 step 0; every value within 0.2 %). The move is
    sensitivity x keys / 4 steps, truncated towards 0, where keys counts groups of three notes from
    note 1 away from notes 64-69 (which do not move), down to -15 (notes 21 and below) and up to +15
    (112 and above); the moved step is held within 0-127, so fast segments stop at the fastest rate
    and slow ones at the slowest. Low notes play slower, high notes faster; at sensitivity 7 the
    range is 26 steps each way (about 9.5 times). The hold does not follow the key; ratio mode, a
    note shift (the shifted key counts), decay 2 and release follow the same rule. Octet:
    `extTimeKeyFollowSteps`, `extTimeKeyFollowScale` (`msfa/ext_tables.cc`), a time factor per
    segment in `ExtEnv`. Measured against Octet on the same 230 renders: 90 of 151 decay readings
    within 3 % (10 before); the rest are fast segments (faster than about 1.4 ms/dB) that read up
    to 15 % slower in the reference, by the same amount at sensitivity 0 (time 20: 6 %), so they are
    the fast-segment reading, not key follow. The slowest attack checked (time 50, sensitivity 7,
    note 12) is 7 % slow in Octet.
78. Feedback on a modulator (fifteenth session, `m7c` df7, `m15g`, `m15h`, measured): a modulator that
    feeds back uses the DX7's amount of its own output (scale 1 against the DX7's shift); a carrier
    keeps the amount fitted before (0.18 of the DX7's, its signal taken before the output level).
    df7 (feedback 7, level 70) falls a straight 6.8 dB per harmonic in both now (harmonics 2-6 within
    0.09 dB, 26 dB off before); `m15g` (14 renders, feedback 4-7, levels 50-99, an EG level, ratio 3):
    11 within 1 dB on the harmonics above -70 dB (1 before); feedback 7 at level 50 misses by 2.5 dB,
    at 99 by 3.8, feedback 4 at level 80 by 20 dB at one harmonic. The depth macro's rule (divided by
    1 + feedback on the operator that feeds back) holds with the new amount (`m15h`: 0.02-1.25 dB);
    the macros do nothing to a carrier that feeds back, in the reference as in Octet (`m15h`: depth
    -99 to +50 and ratio shift -99 at feedback 4 and 7, within 0.1 dB). The topology check with
    feedback 5 now matches all 88 algorithms (f72 included).
79. Note Shift and Pitch Bend Upper and Lower widened to -127..+127 (fifteenth session, Jacob's rule of
    no ceiling on pitch; a deliberate difference from the reference's -48..+48 and -48..+24). Values in
    the old ranges play as before. A key shifted past 0-127 keeps the end key for everything scaled by
    key (key follow, level scaling, time key follow, pan) and plays the distance beyond as
    equal-tempered semitones before the pitch grid's rounding (`ExtStart::keyBeyond`); voices keep
    that distance so note-offs, single key assign and repeated presses still find their key. Pitch
    increments above any Nyquist are held at the lookup's largest value; the Extended test renders
    the extremes (note shift and bend ranges at +-127 at keys 0 and 127, fixed coarse 31 fine 127)
    on four waves and checks the output is finite (under ASan/UBSan too).
80. Fixed frequencies past coarse 21 fine 0 (fifteenth session, `m15f`): the reference plays coarse 21
    fine 64 at 19,920 Hz and holds coarse 21 fine 127 and coarse 22-31 at 22,043 Hz (just under its
    Nyquist); Octet now plays every coarse and fine value on the 1/128-octave grid without a ceiling
    (a deliberate difference above 22 kHz). The table's old ceiling `kFixedMaxHz` stays in
    `ext_tables_data.h` (generated by the measurement repository) unused.
81. The `rates` check runs at 44.1, 48, 88.2, 96 and 192 kHz (fifteenth session); all 14 measurements
    met their tolerances at the new rates without an engine change.
82. Instant Performance changes (seventeenth session; proposed, for Jacob to confirm): Octet offers
    `oscairn.sound-snapshots/draft-3` with the ring-out buffer (section 4.5), so Oscairn prepares the next Performance's Octet
    sounds beside the playing ones in the same instance instead of loading another. The context
    count stays eight (decision 4 holds the pool and count on hold): a part's snapshot is its
    Default and its prepared sounds (two for a part playing one prepared sound); after a switch the
    instance holds its first Default, the outgoing sounds while they ring and the incoming ones
    (five for such a part), so one more switch fits while those still ring. With too few free
    contexts Oscairn loads the snapshot into an instance, as before.
83. Cartridge voices as presets (seventeenth session; proposed): as Oscairn's porting guide
    recommends for Octet, files of the cartridge folders, type `syx`, load key the voice `0` to `31`,
    named from the cartridge (section 4.10). A file that is not a 32-voice bulk dump lists nothing;
    a checksum that does not match is accepted, as Dexed does.
84. Audible release levels (eighteenth session, Jacob's decision): an Extended note whose carrier's
    release (hold) level is audible (12 or more) holds its voice after the key is up, as the
    reference does; nothing changes (levels 1-11 play as silence and end the note, `7f59025`).
    Oscairn's ring-out cut-offs and its voice stealing, still to come, deal with such voices; to be
    revisited if they turn out not to.
85. Voice budget (nineteenth session; confirmed by Jacob 2026-11-07): Octet offers
    `oscairn.voice-budget/draft-1` with the SDK's order (section 4.5). Choices: the full pool uses
    that order only while Oscairn has set a limit, because the Classic baseline's poly scenario
    steals (18 notes on 16 voices) and the SDK's order (quietest before oldest among released
    voices) changed 106 of its 164 cases; without limits Dexed's stealing is unchanged. Loudness is
    the loudest carrier's gain, no cost estimate is given, and `honors_volume` is false. Decision
    84's voices go first once a budget is set (released, and quiet at their hold level); without
    one Dexed's +2 for a key up already prefers them to held voices. Questions 4 and 40 stay open
    for the pool size, the context count and mono hand-over: a held mono key is now kept from
    another sound's new voice under a budget, but without one Dexed's stealing can still take it.

86. Parts sharing one instance (twentieth session; confirmed by Jacob 2026-11-07): eight sound buses,
    the Basic Synth's count, since Octet holds eight sounds (the Default and seven) and a fresh instance
    can then take a part for every sound. A voice keeps its bus through a change by a second lane per
    sound, not by a lane per bus (nine filters per sound), since the host sets a sound's bus when it
    joins and rarely again; a third bus while both lanes sound chokes the older lane instead. The bus
    applies at a process boundary, so a note in the same block as the change still starts on the old
    bus. No output clipping per bus: as the main output (decision 70), Octet does not clip.
87. MPE scoped to channel 1's sound (twentieth session; confirmed by Jacob 2026-11-07): while MPE detection is on (until
    two notes share a channel), a channel that selects another sound than channel 1 is a part of its
    own: its bend is a channel bend, and channel 1's controllers reach only the channels playing
    channel 1's sound. Before, two parts playing one note each on channels 1 and 2 made channel 2's
    bend an MPE per-note bend (range 24) and channel 1's bend reach both parts. One sound on every
    channel, and the desktop Dexed, behave as before.
88. `oscairn.ui` roles (twentieth session; confirmed by Jacob 2026-11-07): a plugin declaring any role gets none from its
    parameter names in Oscairn, so with the Output Bus's role Octet now also declares the roles
    Oscairn took from its names until then (both cutoffs and resonances, Part LFO Hold, the four
    envelope macros, and every operator's Level as the instrument's level). These are Oscairn's
    guesses kept as they were, not chosen roles; choosing them is left for later.
89. Reset ends notes without `NOTE_END` (twenty-first session; confirmed by Jacob 2026-11-07): the notes a CLAP reset
    ends are not reported, as Oscairn Analog and Oscairn sfizz do; reported at the next render,
    they counted as playing their sound for that render and made a sound's second lane render
    (twenty-first session).
90. Rich views past the porting guide (twenty-fifth session; proposed, for Jacob to confirm): the
    guide names views 1-3 (the Classic voice, the Extended voice, the Classic pitch EG) and leaves
    the Extended operators' pitch EGs optional. Oscairn FM also declares those (view 5, one envelope
    per operator), the Extended pitch EG (view 4) and the Extended filter EG (view 6), since the
    reference's pitch EG, filter EG and operator screens draw these envelopes. Without them an
    Extended sound would show its pitch and filter EGs as tiles only.
91. How the Extended envelopes read (twenty-fifth session; proposed): the filter EG starts at its
    Hold Level and holds for its Hold Time (a segment without a level), then Attack, Decay 1,
    Decay 2 and Release, each a time and a level; the Extended pitch EG starts at its Initial Level,
    then the same four; an operator's pitch EG starts at its Initial Level, the Attack Time reaches
    the Attack Level and the Decay Time is its last segment. The host draws that segment as a
    release falling to the bottom of the -50 to +50 range, after a hold until the note ends; the
    engine's operator pitch EG decays at once, without waiting for the key, back to 0, the middle
    (draft 4 has no field for an envelope that does not hold). As the guide says, the host draws
    pitch levels from the bottom, not about a centre, for now.
92. No widgets, priorities or groups, and the roles unchanged (twenty-fifth session; proposed): the
    views need no role, so decision 88's roles stay as they were. An operator's Rate Scaling and
    Amp Mod Sensitivity, an Extended operator's Time Key Follow, LFO depth offsets and wave, and the
    envelopes' depth and velocity settings are in no view (draft 4 has no field for them) and stay
    tiles. Whether the filter EG's stages should also take the filter envelope roles is left open.
93. Which envelopes give their times (twenty-sixth session; proposed, for Jacob to confirm): the
    porting guide has the Classic rates give theirs through `seconds()` and the Extended voice's
    times and the pitch EGs return false. Oscairn FM also gives the Extended filter EG's (68-72):
    its hold and segments take their time value's duration whatever their distance, so the
    engine's own tables give each one exactly (without time key follow or velocity). The Extended
    operators' times (ms per dB over the distance, an attack towards an asymptote) and both pitch
    EGs' times (cents per ms over the distance) have no time apart from their levels, and stay
    false, so those envelopes are drawn by their ranges.

### M5 (in progress, 2026-10-31; third session below; a modulated wave in the ninth; formant 2's sign in the eleventh; nothing new in the twelfth)

- **Operator waves** (the spectral forms, section 3.7; neutral names: Wave, Spread, Formant;
  parameters 1000 n + 60..62; Classic conversion refused unless every operator in the DX7
  graph is a sine). Measured first: every wave and spread as an unmodulated carrier at note 48,
  the formant waves over a formant sweep at three spreads, every wave at notes 24-108, and the
  formant waves at every spread with their centre at harmonics 10 and 46 (`fm/spectral.py`:
  least-squares harmonic amplitudes and phases, and the power off the harmonics).
- What the renders show: the all- and odd-harmonic waves have the same spectrum at every note
  (no band-limiting in the reference: at note 108 its odd-harmonic wave 2 has aliases only 14 dB
  below its harmonics); spread 0 is a sine for the all-harmonic waves, a fundamental with the
  third harmonic at -10 dB for the odd ones; each spread step widens the spectrum. The formant
  waves are a symmetric sideband pattern centred on harmonic formant + 1 (the strongest line
  checked at formant 0, 9, ..., 99 at note 48 and at formant 50 at notes 24 and 36), with the
  same shape at centres 10 and 19 (spreads 3 and 7), so Octet plays them
  as a window table times a sine at the centre harmonic; when the centre is above Nyquist the
  reference plays a non-harmonic tone near Nyquist (an artefact Octet does not copy: it is
  silent there).
- **Engine** (`msfa/ext_waves.*`, data `msfa/ext_waves_data.h` written by
  `fm/make_waves.py`): single-cycle tables of 2048 samples built once in SynthEngine's
  constructor from the measured amplitudes and phases, in versions holding the first 1-16
  harmonics exactly and more in quarter-octave steps (every harmonic below Nyquist up to the
  16th is kept; nothing above Nyquist is), chosen per operator at note-on from its frequency;
  7.7 MB (940 tables), shared by every instance. The Extended kernels call a per-operator wave function;
  the sine path is unchanged and Classic voices never reach it.
- **Where a wave starts in its cycle** (third session, `ae280e4`; measurement `5f3c330`,
  `46d2ecc`). A carrier's spectrum does not show it, a modulator's sidebands do. Renders of
  every wave at every spread modulating a sine of the same frequency (levels 60 and 75, 85 for
  some; formant waves at formants 0, 9 and 27, spreads 0, 3, 7; `spec5`, 111 cases) fit this
  model, with no free parameter: the modulator's depth is the same as for a sine (the earlier
  guess of a different scaling was wrong); waves 1 and 2 run from 0 to their peak (a constant
  of minus their minimum, 0.05-0.50) and start at their peak; waves 3 and 4 have no constant and
  peak a quarter cycle in, as a sine does; the formant windows are centred half a cycle in
  (formant 2's is symmetric); formant 1's window is not symmetric: each component has its own
  phase, measured from the two sidebands' phases up to a time shift, which is fitted (modulo
  half a cycle) to formant 1's formant-0 carrier, where the sidebands below harmonic 1 fold back.
  The measured carrier phases first lose the reference's output highpass (first order, about
  10.3 Hz, fitted to waves 2 and 4, whose harmonics are then in phase within 0.01 rad at every
  spread and note). In-fork test: every wave 1-4 table peaks where the model says (within
  1/256 cycle) and waves 1-2 have minimum 0; formant 2's window peaks half a cycle in.

- **Gate** (`fm/compare_waves.py`: first 16 harmonic levels relative to a sine at the same
  operator level, within 1 dB, harmonics more than 60 dB below the case's strongest not
  checked; aliasing at most the reference's). In-fork test: harmonics 1-8 of waves 1-4 at
  spread 3 within 0.5 dB of their table data.

| Gate | Result |
| --- | --- |
Results below as of the third session (`ae280e4` plus the analysis fix `46d2ecc`; all 161
carrier cases of `spec` re-rendered and compared: 144 met, was 133 before this session).

| Gate | Result |
| --- | --- |
| Waves 1-4, every spread | **Met:** 32 of 32 cases, worst 0.82 dB (was 0.22): all-harmonic wave 2 at spreads 3-5 moved 0.6 dB at one weak harmonic each (harmonics 7, 10 and 14) when the waves got their constant; unexplained, the tables' levels for them are unchanged |
| Waves 1-4 and sine at notes 24-108 | **Met:** 35 of 35 (all harmonics below Nyquist kept up to the 16th); aliasing -64 dB or lower (the renders' floor), where the reference's reaches -14 dB at note 108. Worst 0.82 dB, at note 24's fundamental (32.7 Hz) for every wave including the sine: Octet's output DC filter (Dexed's, about 20 Hz) takes 1 dB more there than the reference's 10.3 Hz highpass |
| Formant waves, every spread (formant 0) | Formant 1: **met** at every spread (worst 0.35 dB; was not met at 3-7). Formant 2: met at 0-3, 6 and 7 (worst 0.31 dB); **not met** at 4 (2.8 dB) and 5 (13.1 dB at harmonic 16, -53 dB), as before |
| Formant sweep (0-99 every ninth, spreads 0, 3, 7) | Spread 0: 24 of 24. Spread 3: 23 of 24 (f6s3r18 1.07 dB). **Spread 7: met in 10 of 24** (formant 1: 8 of 12, fails at formants 9, 27, 45 and 54 by 3.1-5.7 dB; formant 2: 2 of 12, 3.3-38 dB off at formants 9 and 27-99): the windows at spread 7 are wider than the low harmonics' distance from the centre, and the reference's spectrum there is not the window model (unresolved). Aliasing now no worse than the reference's in every case: the "-28 dB" at f6s3r54 was the analysis, not the engine (the fundamental refinement found the neighbouring sideband and fell back to the nominal pitch; Octet's render has no off-harmonic line above -64 dB), fixed in `fm/spectral.py` |
| Formant waves at notes 24-84 (formant 50, spread 3) | Met where the centre is below Nyquist (6 cases); above it the reference plays a non-harmonic tone near Nyquist and Octet is silent (not compared) |
| Waves as modulators (`spec5`, 111 cases; `fm/compare_mod.py`: first 12 harmonics relative to the strongest, 1 dB, those within 60 dB) | **100 of 111 met** (was 33 of 111 with the earlier tables). Waves 1-4: 49 of 51 (m4s3l85: harmonic 11, a null 52 dB down, 5.9 dB; m4s7l75: harmonic 2, 59 dB down, 2.3 dB). Formant 2: 22 of 26, fails at spread 7 (formants 9, 27: 1.6-8.7 dB) and m6s3r9l85 (2.7 dB at harmonic 7). Formant 1: 21 of 26, fails at formant 9 (spread 3: 1.4-2.3 dB; spread 7: 1.7-5.4 dB) and m5s7r27l75 (5.6 dB). `spec3` (the earlier campaign): 12 of 14, the two left are formant 1 at formant 9 |

**Where M5 stopped.** M5 is not complete. Not met: spread 7 formant waves (as carriers above
formant 9 and as modulators), formant 2 at formant 0 with spreads 4-5, formant 1 at formant 9 as
a modulator (1.4-5.4 dB). Next steps: renders of the spread 6-7 formant waves with the centre at
harmonic 100 and at low centres, to see what the reference does where the window reaches past
harmonic 1 (the fold model holds at spreads 0-3 only); the wave choice follows the note-on
frequency only (pitch EG, LFO and bend moves are not followed; marked as a shortcut in
`ext_note.cc`); Extended sounds could use the reference's 10.3 Hz output highpass instead of
Dexed's DC filter (1 dB at 33 Hz; done in the fifth session, decision 38).

**Fifth session: the `spec` carrier campaign again, after Extended voices lost the 16-bit
rounding (decision 32) and took the reference's 10.3 Hz output highpass (decision 38).** All 161
cases re-rendered and compared with `fm/compare_waves.py` (unchanged): **146 met** (was 144; the
comparison renders before were from the third session's engine). Waves 1-4 at every spread 32 of
32, worst 0.06 dB (was 0.82: the unexplained 0.6 dB at all-harmonic wave 2, spreads 3-5, is
gone); waves 0-4 at notes 24-108 worst 0.12 dB (note 24 was 0.82, the DC filter); formant 2 at
formant 0 now met at spread 4 (0.14 dB, was 2.8) but not at 5 (7.2 dB, was 13.1); formant 1 at
formant 0 worst 0.2 dB; the sweep at spreads 0 and 3: 48 of 48, worst 0.58 dB (f6s3r18 was 1.07).
Spread 7: 10 of 24 as before, but several of formant 2's misses grew (formants 36, 45, 90, 99: 26-111
dB, were 21-38): those harmonics are formant 2's flat floor in the reference (about 52-60 dB below
its strongest, fourth session), which Octet's rounding noise used to approach and its floating
point output no longer does. Aliasing no worse than the reference's in any case.

**Fourth session (M5 follow-up, measurements only, no engine change).** `spec6`: formant waves
5 and 6 at spreads 4-7 as carriers at note 24, with the centre at harmonic 100 and 61 and at
formants 0-20 (every second). Compared harmonic by harmonic relative to each render's
strongest (no sine in this campaign, so not the gate's normalisation): with the centre at
harmonic 100 Octet's window matches the reference within about 1 dB down to 50 dB below the
peak at every spread 4-7, both waves (formant 1 at every harmonic shown; formant 2 also, except
its deep nulls), so the window tables are right where the window does not reach harmonic 1.
At low centres the first 16 harmonics differ by up to 2.9 dB (formant 1, spreads 4-6), 6.6 dB
(formant 1, spread 7, formant 10), 0.9-13 dB (formant 2, spreads 4-7; worst at spread 5,
formants 14-18 and 0). Formant 2 at spreads 5-7 with the centre at 61 and 100 also shows a flat
floor of harmonics about 52-60 dB below the peak in the reference that Octet does not have
(23-44 dB differences there), as from a small click once per cycle. Not resolved: how the
reference folds the window below harmonic 1, and where formant 2's floor comes from.

**Ninth session: a modulated non-sine carrier (decisions 55-57).** Campaign `m9a` (79 renders,
`forks/measurements/m4/m9a`, generator `m9gen.py`): each wave as the carrier of a sine modulator
(algorithm 8, note 48): waves 1-4 at spreads 0, 2, 4, 7 at modulator levels 55, 65, 75; formant waves
1 and 2 at spreads 0, 3, 7 and formants 0, 5, 20 at level 65; waves 0-4 at spread 3 with the
modulator at twice the carrier's frequency and with the carrier at twice the modulator's; the sine
pair at each level. No render came back empty (`dropouts.py`: three zero runs, the formant waves'
own silent stretches). Analysis: `fm/spectral.py` and `fm/compare_mod.py` (first 12 harmonics
relative to the strongest, 1 dB, those within 60 dB; cases whose first 12 harmonics are all below
that are now reported as not checked, measurement `64d41f3`); models simulated with the index
read from the sine pair at the same level (local `m9fit.py`, `wavemod.py`).

What the reference does, each with no free parameter (Octet does the same since `07ec388`,
`202348f`, `4550932`):

- **Waves 1-2 start half a cycle on** (at their minimum, 0). As modulators of a sine at ratio 1 or
  as the modulator of a carrier at twice its frequency (`spec5`) the two starts give the same
  spectrum, so the third session's fit could not tell them apart; as a carrier at ratio 1 and 2
  they differ by up to 44 dB.
- **A modulated formant wave plays window(t + m) sin(centre t + m)**: the centre's sine takes the
  modulation once, not centre times as in Octet's window(t + m) sin(centre (t + m)).
- **A modulated odd wave plays E(2t + m) sin(t + m)**, where the wave is E(2t) sin(t): E is a wave
  of its own at twice the rate (built from the odd harmonics' amplitudes and phases; the
  construction needs the wave to be 0 at t = 0, which holds within 0.0006 for the three checked).
  Modulated as one table at t + m it missed by 7-35 dB.

| Gate (`m9a`, `fm/compare_mod.py`, 1 dB) | Before | After |
| --- | --- | --- |
| Waves 1-2 (28 cases) | 8 met | 25 met (after `07ec388`); not met: all harmonics 1 spread 7 at level 75 (4.2 dB), all harmonics 2 spread 7 at level 75 (17.1 dB) and spread 2 at level 75 (2.1 dB) |
| Odd waves 3-4 (28) | 0 met | 28 met, worst 0.28 dB |
| Formant waves (14 checked, 4 not checked: formant 20 at spreads 0 and 3 has nothing within 60 dB in harmonics 1-12) | 5 met | 11 met; not met: spread 7, formant 1 at formant 20 (5.4 dB), formant 2 at formants 0 (16.9 dB) and 20 (18.8 dB) |
| All 75 checked | 18 met | 69 met |

The same renders with `fm/compare_m7.py` (harmonics 1-8 over time): 22 → 74 of 79. Other campaigns,
like for like before and after the three commits: `m8c` 99 → 121 of 149 (the pair now meets every
negative texture value from -59 to -4 at both modulator levels, and -74 at level 40; still not met:
-94 to -64 otherwise, the spread 7 formant region, and +51 to +66 at level 70, a formant wave on a
modulator, unchanged), `m8f` 48 of 66 either way (lone carriers, unmodulated), `m7a` 180 → 205 of
242 (the wave probe's base case 3.23 → 0.28 dB, and with it every macro case on that probe except
depth +25 to +99 (1.1-4.3 dB), ratio +99 (1.6 dB) and texture; texture on the wave probe at -50 and
+50 got worse, 30.8 → 53.5 and 3.0 → 5.9 dB: formant waves at spread 1, a region still unresolved),
`m7b` 85 → 89 of 101; `spec` (lone carriers, `fm/compare_waves.py`) and `spec5` (waves as
modulators) unchanged: 146 of 161 and 100 of 111 met as before, each case's worst difference within
0.01 dB of the earlier renders'.

Still open in M5: the spread 7 formant windows (as carriers at formants above 9, as modulators, and
now modulated), formant 2 at formant 0 with spread 5, the reference's flat floor under formant 2 at
spreads 5-7, and the all-harmonic waves at spread 7 under a strong modulator (level 75). Not looked
at again this session. The wave choice still follows the note-on frequency only.

**Tenth session (2026-11-01): the open M5 cases, analysis only (no engine change for them).** Local
scripts in the scratchpad (`trunc.py`, `phfit.py`, `split.py`, `nk.py`, `modfit.py`), models
simulated with the index read from `m9a`'s sine pair as before.

- **Waves 1-2 at spread 7 under a strong modulator** (`m9a` level 75: 4.2 and 17.1 dB). Even the
  simulation of the current model (table(t + half a cycle + m)) misses these renders (2.2 and 4.5 dB
  rms over 16 harmonics) while it fits spreads 2 and 4 within 0.2 dB. Ruled out: the tables'
  -45 dB floor of high harmonics (truncating the tables at 8-168 harmonics, or dropping everything
  below -40 or -50 dB, changes nothing at level 75), a different start or index (a free phase and
  index scale fit best at exactly half a cycle and 1.0), each harmonic k taking a k + b times the
  modulation (best a = 1, b = 0), and splitting the wave into an even part at twice the rate and
  an odd part E(2t) sin(t) as the odd waves are (worse in every case, by 2-30 dB). The renders' spectra are steady
  over 0.05-0.4 s. Not resolved.
- **Wave 2 spread 2 at level 75** (2.1 dB): the miss is harmonic 5 at -58 dB, next to the 60 dB
  floor; the stored table has only 4 harmonics. Not changed.
- **Texture +51 to +66 on the level 70 modulator** (formant 2 at spreads 1-7, formant 7): searching
  Octet's modulator level 30-99 with the same wave finds no level that matches (best 2.7-5.0 dB rms
  over 12 harmonics, at levels 70-81), so it is the window's shape as a modulator, not its depth.
  Not resolved; like the spread 7 cases above, a wide window under strong modulation.
- **Texture -50 and -25 on the wave probe** turned out to be the macro, not the waves: the
  reference starts from the carrier's own wave (decision 60, below under M7). Formant 2 at spread 5
  and the reference's flat floor were not looked at again.

**Eleventh session (2026-11-01): formant 2's sign (decision 63).** Each sideband's phase against the
centre's, with the time shift (the main lobe's slope) taken out, turns by half a cycle just past formant
2's main-lobe null at spreads 3-7 (components 8, 12, 16, 20, 26 in `spec4`'s centre-100 renders; 12, 16,
21, 27 at spreads 4-7 in `spec2`'s centre-46 renders) and stays there: the flat floor 52-60 dB down is
made of negative components, a narrow dip at the window's peak. Octet already played the floor's levels
where the window does not fold (`spec4`, centre 100: within about 3 dB at spreads 5-7); the tables had every
component positive, which matters where the window folds below harmonic 1 and under modulation.
`fm/make_waves.py` now keeps each component's real part (measurement `f3861d9`; formant 1 is
unchanged). Like for like, Octet renders before and after (`ceb5d82` and the new tables):

| Campaign | Before | After |
| --- | --- | --- |
| `spec` (`fm/compare_waves.py`) | 146 of 161 | 146 of 161; formant 2 at formant 0 spread 5 7.16 -> 1.98 dB (still not met), spread 7 sweep misses move both ways (f6s7r36 55.1 -> 59.6, f6s7r99 111.1 -> 102.5) |
| `spec5` (`fm/compare_mod.py`) | 100 of 111 | 100 of 111; m6s7r27l75 8.7 -> 37.8 dB (a harmonic 57 dB down, 10, falls into a null; both fail), m6s3r9l85 2.76 -> 2.97 |
| `m9a` | 69 of 75 | 69 of 75; formant 2 spread 7 at formants 0 and 20 16.9 -> 3.7 and 18.8 -> 13.8 dB |
| `m8c` (`fm/compare_m7.py`) | 121 of 149 | 122 of 149 (tp40v20 11.8 -> 0.7 met; tp70v15-35 22-35 -> 16-25, tc18 28.9 -> 1.2 dB) |
| `m8f` | 48 of 66 | 49 of 66 (tc20 2.1 -> 0.3 met; tc14, tc16 13.6, 17.5 -> 3.0, 5.1 dB; tc10 102.9 -> 113.2) |
| `m7a` | 215 of 242 | 215 of 242 (texture -99 on the wave probe 21.9 -> 22.6 dB) |
| `m10t` | 27 of 28 | 27 of 28 |

The renders were not re-recorded: the reference renders are those of earlier sessions. Not looked at
again (time-boxed): texture +51 to +66 on the level 70 modulator (improved, see `m8c`'s tp70 rows, not
met), texture -99 on the wave probe, waves 1-2 at spread 7 under level 75 (no new idea).

**Twelfth session (2026-11-01): no engine change for the open M5 cases.** Feedback 7's straight
spectrum was looked at again without a model that could be checked against the renders. Waves 1-2
at spread 7 under the level 75 modulator (4.2, 17.1 dB), formant 2 at formant 0 spread 5 (1.98 dB)
and the cases that got worse with formant 2's sign (`spec5` m6s7r27l75 37.8 dB) were not looked
into (time-boxed). The texture path change (M7, decision 67) moved several spread 7 formant 2
cases, see there; `m8f` tc10 (-89) is now 95.5 dB (was 113.2).

### M6 (in progress, 2026-10-31: filter, filter EG, common pitch EG, pitch key follow, random pitch, key-on delay, stereo and pan are in the engine; the part LFO with all nine of its destinations)

Commits: `57b5e1a` (the filter, its EG and their parameters), `70ef883` (MSVC), `10d59f1` (carrier
feedback after the output level change, found while measuring the filter). Measurement code:
oscairn/measurement `fm/filter.py` (`be859e5`, analog prototypes, superseded),
`fm/fit_filter.py`, `fm/compare_filter.py`, `fm/compare_feg.py` (`8f9ff9a`). Renders in
`forks/measurements/m4/`: `filt`, `filt2`, `filt3` (earlier sessions), `filt4` (every cutoff
value near each segment boundary and above 819, key follow and velocity with a thru per note,
LPF18s, BPFw, LPF12+BPF6, the HPF of types 4/5, Distance, Filter Gain), `feg` and `feg2` (the
filter EG through a sine at note 84 and HPF12 near 1 kHz: the sine's level against thru gives
the cutoff at every sample; decay times at every second value, holds every fourth, release,
depth, depth velocity per curve, time velocity and key follow, resonance and cutoff velocity,
cutoff scaling).

**What the filter is.** Every type is built from two digital sections, which the reference's
responses fit with no free shape parameter: a Chamberlin state-variable filter at 44.1 kHz
(coefficient F = 2 sin(pi fc / fs), damping 1/Q; low-pass, band-pass, high-pass and notch =
low-pass + high-pass outputs) and a one-pole low-pass with the same F. HPF12 at resonance 0
fits it with Q 0.500 and a gain offset of 0.00 dB at every cutoff (the analog prototypes of
the last session needed a gain offset and stopped fitting near 2.2 kHz: those were the
Chamberlin filter's own shape near a sixth of the rate). Per type, with Q = 0.5 x 2^(res/16)
(confirmed to 0.01 at 16-127 on HPF12):

| Type | Structure |
| --- | --- |
| LPF24D | LP(Q) then LP(Q) |
| LPF24A | LP(Q 1) then LP(Q) |
| LPF18 | LP(Q) then one-pole |
| LPF18s | one-pole at the cutoff, then LP(Q) with F about 10 times the one-pole's (9.5-10.4, per cutoff value, no trend), F up to 1.75 |
| LPF12+HPF12, LPF6+HPF12 | LP(Q) or one-pole, then HP(Q 1) at the HPF cutoff (same table) |
| HPF24D, HPF12 | HP(Q) twice, HP(Q) |
| BPF12D, BPF6 | BP(Q) twice, BP(Q) |
| BPFw | HP(Q 1) at the low edge then LP(Q 1) at the high edge; the edges move apart by 3.207 cutoff values per width step (the upper one 31.5 values late, the lower 19.7 early) |
| BEF12, BEF6 | notch(Q) twice, notch(Q) |
| DualLPF, DualHPF, DualBPF | the sum of two sections, the second's F times 2^(Distance / 16) |
| DualBEF | two notches in series, the second's F times 2^(Distance / 16) |
| LPF12+BPF6 | approximately LP(Q) + BP(1.13 Q) (0.5-1 dB residual; not resolved) |

- **Cutoff:** a table of every value 0-1023 (`kFilterCutoffHz`, Hz of the 44.1 kHz filter),
  linear in Hz between measured values: the reference's cutoff is piecewise linear in Hz with
  the slope changing at every value but jumping about 3.7 times once per octave (near 203,
  305, 408, 511, 614, 716, 819; every value within 10 of each measured). From 819 (3.57 kHz)
  the reference raises the minimum Q (doubling over about 102 values) and stops the cutoff at
  7350.2 Hz (F = 1) with Q 1 from about 921: the damping is limited by cutoff frequency, which
  also keeps the section stable (at F 0.84, Q 0.5 would be unstable).
- **Key follow:** centre note 60; steps of 3.125 % (+63 = +200 %); the move is straight in
  frequency within each octave (+31.25 % one octave up: x 1.3125, not 2^0.3125), exact at whole
  octaves; the HPF's key follow the same. Residual up to 16 cents at fractional octaves.
- **Velocity:** cutoff velocity sensitivity +63 moves the cutoff by a measured curve (-9609
  cents at velocity 1, 0 near 114, +1201 at 127; negative sensitivities mirror the velocity),
  downward in cents in proportion, upward in frequency ratio in proportion. Resonance
  velocity divides the Q by 1 + (sensitivity share) h(127 - velocity), h measured (0.25 at 7,
  1 at 31, 3 at 63). -64 and +63 are both full scale.
- **Scaling:** 37.5 cents per offset step, straight between the break points by note,
  constant outside.
- **Filter Gain:** 0.375 dB per step below 255 (also through Thru); 0 is silent.
- **2nd LFO filter depth:** 68 cents per step at the LFO's peak, the triangle starting at its
  top (measured at depth 25 only; the reference's modulation shrank by about 15 % per second
  in that render, unexplained).
- **Filter EG:** levels 75 cents per step; depth -64..+63 as a share (-1, -0.497, 0, 0.274,
  0.505, 0.704, 1.004 at -64, -32, 0, 16, 32, 48, 63); each segment takes its time value's
  duration whatever its distance (decay 1 at time 64 from +600, +1200, +2400 cents: 725, 758,
  768 ms to 50 cents); attack, decay 1, decay 2 and release share one table (release measured
  within 2 % of decay plus about 15 ms note-off latency); durations measured at every second
  value 22-92 and 100/108 (0-20 instant, 96 and up all 11.85 s), holds every fourth value
  0-96; depth velocity sensitivity per curve (curve 2 at 5 velocities, curves 0, 1, 3, 4 at
  2). The segments are straight in cents in Octet; the reference's bend slightly (on a
  1200-cent decay it is a quarter of the way at 0.19 of the time, half at 0.51, three quarters
  at 0.78; up to about 90 cents from Octet's path), apparently following the cutoff segments.
- **Engine** (`msfa/ext_filter.*`, data `msfa/ext_filter_data.h` written by
  `fm/fit_filter.py`; parameters 60-95, `ExtendedVoice` common values): per voice, Extended
  sounds only, after the operators, in floating point (Extended voices are not rounded to 16 bits, decision 32);
  coefficients and the FEG per 64-sample block with linear ramps of F, damping and gain.
  Thru at gain 255 (the default) skips the filter, so existing Extended sounds render
  unchanged. Classic sounds never reach it; a sound with a filter does not convert to Classic.
- **CPU:** the filter section costs about 7-10 ns per sample per voice on this Mac (arm64,
  `-O2`, coefficients moving every block; Thru 4 ns of the benchmark's own overhead), so 16
  voices at 48 kHz are under 1 % of a core here. Not measured on the target hardware.
- **RFC 0014/0015 (same filter as the analog engine?):** they appear to be different models.
  The analog engine's cutoff curve has its kinks every 85.33 values (1024/12) with steep rises
  after each (`forks/surge/doc/oscairn-plan.md`); the FM filter's segments are about 103
  values long. The FM types fit Chamberlin sections exactly with no gain offset; the analog
  side's best fits were Surge's analog-style filters with gain offsets (-3 to +6 dB). Not
  checked: whether the analog renders fit Chamberlin sections too.

**Gate** (`fm/compare_filter.py`: each side's response against its own thru, every harmonic
the reference passes within 40 dB, below 16 kHz, and at most 60 dB below the strongest
harmonic in either render and in Octet's thru, within 1 dB; `fm/compare_feg.py`: segment
durations within 2 % or 2 ms, static levels within 15 cents). The level limit was added because
each Octet voice was rounded to 16 bits (Dexed's design) and on a periodic tone the rounding
lands on the harmonics, about 70 dB below this source's strongest; Extended voices no longer
are (decision 32), and the gate is reported both with and without the limit below.

| Gate | Result |
| --- | --- |
| Filter response within 1 dB, per type (`filt`, `filt3`, `filt4`, `feg2` filter cases, with the level limit: 670 of 697 met with 16-bit voices, 676 without the rounding; per-type figures below are the 16-bit ones) | **Met:** LPF24A 6/6, LPF18 6/6, LPF18s 24/24, LPF12+HPF12 126/126, LPF6+HPF12 7/7, HPF12 397/397 (every cutoff, resonance 0-127, key follow, velocity, scaling), BPF6 6/6, BEF12 6/6, BEF6 6/6, DualLPF 12/12, DualHPF 6/6, DualBEF 12/12; worst 0.88 dB. **Not met:** LPF24D, HPF24D and BPF12D at resonance 96-127 (8-51 dB: the reference overloads, its output full of harmonics, which Octet does not model; at 0-80 they meet, worst 0.67), BPF12D at cutoff 512 resonance 0 in `filt` (2.9 dB in the deep stop band; the same case in `filt3` meets), BPFw 14/16 (1.1-1.3 dB at cutoff 256, widths 64 and 127), DualBPF at Distance +127 (8.1 dB; the second section's limit above F = 1 is a guess), LPF12+BPF6 13/21 (1.0-4.0 dB, the model is approximate) |
| FEG times within 2 % | **Met** in `feg2` (the campaign the tables were fitted from) for 91 of 92 cases: decay 1 at every second value 0-92 and at 100 and 108, holds 0-96, depth velocity; decay 42 misses by 2.0 ms. In `feg` (an earlier campaign, same probe) 43 of 49: attack, decay 2 and hold at 0-16 and 64-80 meet; 6 cases at 24-48 do not (4-18 %): the reference itself differs between campaigns by up to 7.3 ms at the same setting (decay 48: 132.5 and 139.8 ms; hold 48: 192.4 and 185.1 ms), so short segments cannot be gated at 2 % |
| FEG levels, depth and depth velocity (15 cents) | **Met:** all `feg` and `feg2` static cases |

**The filter gate without its level limit (for Jacob to judge).** The limit (harmonics more than
60 dB below the strongest in either render or in Octet's thru are not checked) changes how the
gate is evaluated and has not been approved. Without it (every other condition unchanged:
within 40 dB of the reference's strongest passed harmonic, below 16 kHz, 1 dB), on the same
campaigns: **129 of 697 met** with Octet's voices rounded to 16 bits as in Dexed (fourth
session), **590 of 697 met** without that rounding for Extended voices (fifth session,
decision 32); with the limit, 670 and **676 of 697**.

Why (fifth session, measured on these renders): the failures without the limit were at
harmonics 110-130 dB below full scale (median -113 dBFS in the reference, -133 in Octet), and
Octet's harmonics there were the rounding error of its 16-bit voice output, which on a
periodic tone lands on the harmonics (Octet's floor between the harmonics was 20 dB lower, at
-141). The reference's own floor at the harmonics is lower than that rounding error. With the
voice output left in floating point (a local experiment first, then the change), Octet's floor
between harmonics is -182 dBFS against the reference's -124, and 461 more cases meet. Per type,
met without the limit / with it, of the cases, and the median and largest worst-harmonic
difference without it: LPF24D 7/9 of 14 (16.4, 81.8 dB), LPF24A 6/6 of 6 (0.0, 0.7), LPF18 6/6
of 6 (0.0, 0.9), LPF18s 22/24 of 24 (0.0, 14.7), LPF12+HPF12 117/126 of 126 (0.0, 2.9),
LPF6+HPF12 7/7 of 7 (0.2, 0.7), HPF24D 2/4 of 6 (17.4, 45.1), HPF12 338/397 of 397 (0.0, 9.4),
BPF12D 6/8 of 14 (16.8, 50.4), BPFw 13/14 of 16 (0.1, 1.4), BPF6 5/6 of 6 (0.3, 2.9), BEF12 5/6
of 6 (0.1, 9.1), BEF6 5/6 of 6 (0.1, 4.5), DualLPF 11/12 of 12 (0.0, 4.6), DualHPF 6/6 of 6 (0.1,
0.7), DualBPF 10/11 of 12 (0.4, 8.7), DualBEF 10/12 of 12 (0.1, 40.1), LPF12+BPF6 14/16 of 21
(1.0, 5.1). Of the 107 cases still not met without the limit, 62 miss by 1.3 dB at one harmonic
near 14.9 kHz, where the reference's own source (its thru render) dips to 96-98 dB below its
strongest harmonic (Octet's source is 33 dB lower than the reference's around 14 kHz, so the
two sources differ there, not the filters); 20 are the resonance 96-127 overload cases
(decision 17); about 25 others are spread over the types.

Unmeasured or unresolved: FEG time velocity sensitivity (the reference made the segment
instant at velocity 127 with both -64 and +63 and left it at velocity 1; not applied), FEG
time key follow below zero (the reference was shorter for -64 too; the positive rule is
mirrored), FEG holds above 96, the FEG segment bend, the 2nd LFO filter depth other than 25,
resonance overload, LPF12+BPF6's exact structure, BPFw's lower edge at low cutoffs, the dual
types' second section above F = 1, FEG depth velocity curves 0, 1, 3, 4 at velocities other
than 32 and 96.

**Carrier feedback** (`10d59f1`). Measuring the filter showed Octet's feedback-7 sine far
weaker in its upper harmonics than the reference's (harmonic 20 about -100 dB against -35).
The feedback amount was fitted (`abc697b`) before carriers got the reference's output level
(`312febe`: algorithm gain and -6.57 dB overall), which also lowered the signal a carrier
feeds back. A carrier's feedback now takes its signal before that level: against the reference's
`modidx` renders, feedback 7 is within 0.1 dB at harmonics 2-8, feedback 5 within 0.3 dB at
2-6 (1.4 dB at harmonic 7, 75 dB down) and feedback 3 within 0.3 dB at 2-4 (the rest lie near
-90 dB); harmonic 2 was 4 dB (feedback 7) and 7 dB (feedback 5) low before. The M4 topology comparison with feedback 5 (`topo` f1-f88) was re-run after this: 87 of 88 match, the one left is f72, the same intermodulation line as before.

**Tests run locally** (on `10d59f1`, and on `57b5e1a` before it): Release CTest 36/36 (the
164 Classic cases bit-identical through engine, JUCE plugin and native CLAP on arm64; the
Extended test now checks every filter type against its model within 0.1 dB, the cutoff and
gain tables, the damping limit, key follow, FEG segment durations, and HPF12 through the
plugin), the x86_64 baseline under Rosetta (engine, plugin, CLAP: 164 of 164 identical),
ASan/UBSan 19/19, TSan 19/19.

**Fourth session (2026-10-31): the common pitch EG, pitch key follow, random pitch, key-on delay,
stereo and pan.** Commits `3049366` (common pitch EG), `b408553` (pitch key follow, random
pitch), `8db4d81` (key-on delay), `e0639e8` (stereo rendering, pan modes). Measurement code:
oscairn/measurement `fm/fit_peg.py` (`ef88fd1`: the level, rate and key-on delay tables),
`fm/pitch_track.py`, `fm/segment_rate.py`, `fm/marker_delay.py`, `fm/dropouts.py` (`89b40d2`). Renders
in `forks/measurements/m4/`: `m6a` (pilot: every block below at a few values), `m6b` (common
pitch EG level at every value at depth 2 octaves; decay 1 from +50 to -50 at every time value),
`m6c` (rate at depths 8 octaves and 0.5 octave, depth velocity sensitivity 1-7, time key follow
0-7 at notes 24-108 every octave, fixed-frequency operators, key-on delay without a marker),
`m6d` (pitch key follow at every value and at fractional octaves, random pitch at 5 depths, the
pan law every 4 positions, scaling pan at notes 24-108 every 6, random pan), `m6e`/`m6f`
(key-on delay at every value against a second part without delay). Pitch from the analytic
signal's instantaneous frequency (`fm/pitch_track.py`, the same method as the M4 operator pitch
EG fit); rates by a straight-line fit between 15 % and 85 % of a segment's move. Dropouts were
checked in every campaign (runs of near-zero samples or jumps inside the held part); one
render was re-recorded (`m6f` key-on delay 109; `m6b` decay 99 was replaced by `m6a`'s, the
same setting at another depth). The formant renders of `spec6` flag zero runs without jumps,
which is the narrow formant windows' own silence within each cycle, not dropouts.

What the reference does, and Octet now does in Extended sounds only:

- **Common pitch EG.** Levels are steps of 37.5 cents at depth 8 octaves (one step per level to
  +-32, then wider: +50 is 128 steps, -50 is -128), scaled by depth (8, 2, 1, 0.5 octaves:
  x1, 1/4, 1/8, 1/16). Every segment (attack, decay 1, decay 2, release from wherever it is)
  moves straight in cents at its time value's rate whatever its distance, the same rate at
  every depth; every rate is a whole multiple of 0.05619077 cents per ms (254 at time 1 down to
  1 at 99; time 0 is instant). Decay 2's level holds while the key is down. Depth velocity
  sensitivity removes up to 17, 35, 53, 70, 88, 106, 126 (sensitivity 1-7) of +50's 128 steps at
  velocity 1, in proportion to 127 - velocity, rounded down; it scales the rates too (the
  segment times do not change with velocity: 1.35 % off at velocity 64, sensitivity 7). Time
  key follow multiplies the rates by 1 + sensitivity (note - 3) / 33. Fixed-frequency
  operators do not follow the common pitch EG, pitch key follow or random pitch.
- **Pitch key follow.** Each value plays a whole percent: 1 % steps within 10 of 0, 100 and 200
  % (down to -191 % at the bottom, from 192 % at the top), 5 % steps between; straight in
  cents from the centre note (fractional octaves within 1 cent).
- **Random pitch.** The reference's offsets lay on a grid of 18.75 cents (within 1.4 cents),
  up to +-600 cents at depth 127; the reach grows with the depth (about depth / 4 steps).
  Octet draws a uniform step within round(depth / 4) either way from a per-sound xorshift
  generator that prepare resets (deterministic renders); the distribution is not measured.
  (Sixth session: refitted from 32-128 notes per depth, decision 44.)
- **Key-on delay.** Whole 64-sample blocks at 44.1 kHz (0 to 5570 blocks, 8.08 s, each value
  within 0.07 blocks of the grid). Tempo sync plays the note length (1/16 to 8 beats) at the
  host's tempo: the recorder's renders at 120 bpm matched the lengths within the onset jitter
  (about +-40 ms, no marker), and changing the performance's own tempo did not change them.
  Octet takes the CLAP transport's tempo (120 without one) and starts the note at its exact
  sample. A note released before its delay ends never sounds (measured at 16 and 32).
- **Pan.** The near side keeps full level (no dip at the centre); the far side falls by a
  measured table (-0.39 dB at 8, -6.41 at 32, -17.70 at 48, -45.55 at 60, silent from 63; the
  same both ways within 0.02 dB). Scaling pan: depth / 32 positions per key below note 60
  and above note 64, none between, +63 playing as 64. Alternate pan puts the first note at its
  side; the alternation itself could not be measured (the recorder plays one note per
  performance load); Octet alternates per note of the sound. Random pan: 16 notes at 127
  reached position 50 and 8 notes at 64 reached 14, 13 of 16 (and 8 of 8 in `m6a`) to the
  left: Octet spreads uniformly within 64 (depth / 127)^2 either way (approximation; the
  left bias is not modelled). (Sixth session: depth / 2 positions, decision 44; the alternation
  measured, M7.) The positions add up and clamp at -64/+63.
- **Stereo.** The engine renders a right channel when given one: unpanned voices (every
  Classic voice) go to both channels alike and the right channel has its own copy of the
  output filter with the left one's settings, so the left channel is exactly the old mono
  output and unpanned sounds are identical in both channels (the 164 Classic cases hash both
  channels and stay bit-identical). The desktop Dexed still renders mono.

| Gate | Result |
| --- | --- |
| Common pitch EG levels, 1 cent | **Met:** all 101 levels at depth 2 octaves (worst 0.31 cents); the pilot's 16 static levels at the four depths (each within 0.5 cents); depth velocity sensitivity 1-7 at velocity 1 (worst 0.22 cents) and 3 and 7 at 3-5 velocities (worst 0.2 cents; 2.5 cents before the velocity table was refitted from the sensitivity sweep) |
| Common pitch EG times (rates), 2 % | **Met:** decay 1 at all 99 time values 1-99 (worst 0.33 %), rate at depths 8, 2, 1 and 0.5 octaves (0.3 %), attack, decay 2 and release at 30-70 (0.3 %), time key follow 0-7 at notes 24-108 (64 cases), depth velocity 7 at velocity 64 (1.35 %). Not measurable with the pitch tracker: segments shorter than about 20 ms (attack to +10 at times 40 and 60, release at time 0), which share the measured rate table |
| Pitch key follow, 1 cent | **Met:** 157 of 157 (every value at +2 octaves, 12 fractional-octave cases, the pilot's 17 at two centres; worst 0.97 cents) |
| Random pitch | Range and grid only (above); the reference's sequence and distribution are not reproducible from one note per render |
| Key-on delay, 2 % or 2 ms | **Met:** 128 of 128 values (worst 0.29 ms) |
| Pan law and scaling pan, 0.5 dB on the far side | **Met:** 75 of 76 (alternate pan every 4 positions, scaling pan at depths +63, +32, -32 and -64 at notes 24-108); not met: scaling +63 at note 30 (the reference's far side is at -64.7 dB, Octet's at -45.6 dB: position -60; the reference behaves as if 61-62 there) |
| Tempo-synced key-on delay exact to the sample | **Met in Octet's own test** (1/4 at 120 bpm starts at sample 24000 at 48 kHz); the reference's exact sync timing was not measurable without a marker (only 120 bpm, onset jitter about 40 ms) |

Unmeasured or unresolved: the alternation of alternate pan; random pan's spread and its left
bias; random pitch's distribution; the common pitch EG's time key follow below note 3 (left
unchanged), its velocity scaling of rates (1.35 % at the one measured point), and whether it
continues or restarts in mono legato (Octet carries it over like the filter EG); key-on delay
in mono and legato playing; positions 61-62 of the pan law.

**Part LFO (fourth session, later): `84b1819`, `0433952`, `ef31f00`, `56e3a58`, `4f83501`, `996ee32`, `ae58014`.** Measurement code: oscairn/measurement `fm/lfo_rate.py`, `fm/lfo_envelope.py` (`546e4f7`) with
the pitch tracker above. Renders: `m6g` (pilot at depth 127),
`m6h` (every wave, phase, delay, fade in, hold, fade out, tempo sync and the user wave at depth
8), `m6i`/`m6k` (speed at every value), `m6j` (the envelope times every eighth value, depth at
17 values, depth ratio at 7, sample and hold, the user slopes, and the operator level and pan
destinations). Destination 1 was the operator frequency (its reference number 75) with
operator 1's switch on; pitch tracked as above; a triangle fitted to each speed case (whole
cycles above speed 50).

- **What it is.** One LFO per sound, shared by its voices (Octet: one per sound context, like
  the DX7 LFO). Waves, all measured: triangle (from 0 up, bipolar), unipolar triangle (0 to
  +1), saw up (0 to +1), saw down (0 to -1), squares with the top for 1/4, 1/3, 1/2, 2/3, 3/4
  of the cycle (bipolar, starting at the top), trapezoid (the triangle doubled and clipped),
  sample and hold 1 (a uniform value per cycle), sample and hold 2 (Octet: the same; the
  reference's held values include many zeros, not resolved), and the user wave: 2-16 steps per
  cycle, +63 playing as full scale, slope Up ramping to a higher next step, Down to a lower
  one, Up and Down to either. Phase 0, 90, 120, 180, 240, 270 degrees. Speed: a table of
  every value (0 stands still, 1 is 0.0019 Hz, 2 is 0.062 Hz, then about a doubling every 8
  values to 11.86 Hz at 63). Tempo sync: 1/16 to 64 beats per cycle at the host's tempo
  (1/4, 1/2 and 4 beats matched at 120 bpm). Delay, fade in, hold and fade out share one time
  table (the four, measured every eighth value, agree within the measurement's resolution from
  value 48 up): 0 to 11.5 s, read from the delay's onset at every value (`m6o`: an LFO at
  11.9 Hz), made non-decreasing where values 1-30 scatter by about 10 ms; hold 127 holds for
  ever, any other hold is followed by the fade out. After a reset the reference's LFO starts
  about 12 ms late (its onset with no delay; without modelling it, Octet's waves led the
  reference's by 3-18 ms); Octet holds its LFO still for 12 ms.
- **Operator frequency:** 3600/127 cents per depth step at the LFO's peak (17 depths, within
  0.3 cents); the operator's depth ratio applies as integer arithmetic: depth x ratio / 128
  rounded down, ratio 127 as 128 (7 ratios, within 0.4 cents). **Operator level:** at the
  LFO's bottom 0.589 dB lower per depth step, unchanged at its top (square wave at depths 8,
  32, 64: within 0.2 dB of the reference's 4.56, 18.86, 37.80 dB; depth 127 is silent in the
  reference, -74.8 dB in Octet). **Pan:** up to 64 (1 - e^(-depth/20)) positions at the LFO's top, the other
  way at its bottom, through the pan law (fitted to a square wave at depths 8, 32, 64: the far
  side within 0.15 dB of the reference at 8 and 32, 3.6 dB at 64, both silent at 127); the LFO's
  value scales the positions linearly (unmeasured between top and bottom) and the pan moves
  once per 64-sample block. **Filter cutoff:** the LFO moves the cutoff value itself (not cents): 8
  values per depth step at its top (`m6l`: the filter EG probe, HPF12 at 628, square wave at
  depths 2-32: 7.93-8.13 values per step, symmetric; Octet's cutoff moves within 1.5 % (18 cents) of the
  reference's in cents for every square case, and its triangle cases follow the same path,
  shifted by the LFO's lead). **Operator detune:** depth / 4 detune steps at the LFO's top (after the
  ratio rule), the move rounded towards 0, the detune held within +-15 (`m6m`: depths 8-127
  at notes 36, 60, 96; Octet within 0.08 cents). **Feedback:** depth / 16 feedback steps,
  rounded towards 0, held within 0-7 (`m6m`: depths 8, 32, 127 from feedback 0, 4, 7;
  harmonics 2-4 within 0.2 dB of the reference's in each half). **LFO 2 speed:** depth x 100 / 128 speed steps (range Normal), rounded
  towards 0 (`m6n`: Octet's 2nd LFO rate within 0.3 % in each half at depths 8, 32, 127).
  **Resonance:** one resonance step per depth step, rounded towards 0, within 0-127 (`m6n`,
  the filter EG probe at resonance 40: the level in each half within 0.5 dB at depths 8 and
  32, the same 0.5 dB as without the LFO; at depth 127 the top half is 19.0 dB in Octet and
  27.4 dB in the reference, where the reference's high resonance overloads, decision 17).
  **Operator wave:** depth / 16 wave steps (sine ... formant 2) after the ratio rule, rounded
  towards 0, within 0-6 (`m6n`: from all harmonics 1 every half within 0.7 dB at the first 26
  harmonics within 60 dB; from formant 1 within 1.5 dB except depth 32's bottom half, odd
  harmonics 1 at spread 3: 7.6 dB at one harmonic).

| Gate | Result |
| --- | --- |
| LFO rates, 1 % | **Met** for speeds 2-63 (53 of 53 compared, worst 0.23 %; the table comes from the same renders, so this checks the implementation and analysis, not an independent sample) |
| LFO shapes | Octet's pitch against the reference's, 95th percentile of the difference at depth 8 (peak 227 cents), with the start latency modelled and no time shift: every wave 0.1-10.1 cents, the phases 4.8-8.0, the user wave 0.5-10.5 (16 steps: 137 at the step edges, 29.8 after the best shift of 18 ms), tempo sync at 1/4 and 1/2 9.3-17.6, 4 beats 17.8, 1/16 and 1/8 41.7-76.9 (the tracker's smoothing at 5-8 Hz); the best shifts now lie between -26 and +26 ms around 0 |
| LFO delay at every value, 2 % or 10 ms | **Met** for 109 of 111 (`m6o`, worst 27 ms at value 125, 0.3 %); not met: values 9 and 23 (11 ms early: the non-decreasing table) |
| Depth and ratio | **Met** within 0.4 cents (24 cases) |

Not done or not measured: every destination was measured with a square wave only (the
values between the LFO's top and bottom are taken as straight, except where a step count is
rounded); the LFO 2 speed destination in the Extended speed range; BPFw's width under the
resonance destination. Random speed depth, the reference's key-on reset Off and
1st-On behaviour (one note per render), the envelope's behaviour when the LFO does not reset,
sample and hold 2, the fades and hold below about 100 ms (taken from the delay), whether the
LFO reaches fixed-frequency operators, and the operator level at values between the LFO's top
and bottom.

**Tests run locally (fourth session)**, for each pushed commit's tree (`3049366`, `b408553`,
`8db4d81`, `e0639e8`, `84b1819`, `0433952`, `ef31f00`, `56e3a58`, `4f83501`, `996ee32`,
`ae58014`): Release CTest 36/36 (the 164 Classic cases bit-identical through
engine, JUCE plugin and native CLAP on arm64; the Extended test now also checks the common
pitch EG levels per depth, velocity, decay and release rates and time key follow through the
plugin, fixed operators unaffected, pitch key follow, random pitch's grid and determinism, the
key-on delay's exact start, tempo sync and early release, the pan law, scaling and alternate
pan, centred sounds identical in both channels, and the part LFO's rate, wave shapes, phase,
delay, user steps and slope, and all nine destinations through the plugin or, for LFO 2
speed, through the part LFO's own step count), the x86_64 baseline under Rosetta
(engine, plugin, CLAP: 164 of 164 identical), ASan/UBSan 19/19, TSan 19/19.

### M7 (in progress: part values and four of the seven macros in the fifth session; mono, key assign, portamento, random pitch and pan, alternate pan, part LFO key-on reset and fractional velocities in the sixth; the 2nd LFO per voice with its measured depths, start and delay, and the AEG attack and sustain offsets in the seventh; modulator EG levels, Ratio Shift, Texture and the retrigger fade in the eighth, 2026-10-31; rising segments and carrier EG levels in the ninth, 2026-11-01; texture from an operator's own wave, the 2nd LFO's deep amplitude, bend ranges and fixed-frequency detune in the tenth, 2026-11-01; portamento in steps in the eleventh; a free-running glide clock and the texture path in the twelfth)

Measurement code: oscairn/measurement `fm/compare_m7.py` (tolerances `m7`). Renders in
`forks/measurements/m4/`: `m7a` (every macro at -99, -50, -25, +25, +50, +99 on three probes:
a modulator/carrier pair with slow envelopes, the same pair static with feedback 4, and the
pair with operator waves; the part AEG offsets at +-32 and +-64 on a lone carrier and on the
pair; the filter offsets through the filter EG probe; velocity depth and offset at 5
velocities; note shift; detune at notes 36 and 72), `m7b` (the depth macro on four probes
that separate feedback, EG levels and operator level; the ratio macro every 11 steps at
modulator ratios 1 and 4; the texture macro every 11 steps; every macro on a lone carrier; the
AEG offsets at +-4, 8, 16), `m7c` (detune every eighth value, the attack offset on the pair at
6 values, 24 more velocity cases, the depth macro at feedback 1, 2 and 7 and on a stack of three
modulators), `m7d` (operator level velocity sensitivity, below), `m7e` (the depth macro
taking a modulator past level 99). The first `m7a` macro renders used the wrong raw values (the Data List's two-byte
values are 7-bit byte pairs: 0 is raw 128, not 256) and were re-recorded. How each value acts
was found by rendering Octet over every candidate value of one operator or filter parameter
and taking the one closest to the reference render (`m7fit.py`, local), then checked by
rendering the cases with the new parameters (`fm/compare_m7.py`).

What the reference does (Octet does the same in Extended sounds, parameters 235-252, module
Extended/Part and Extended/Macros):

- **Velocity sensitivity depth and offset:** the velocity becomes velocity x depth / 64 + 2
  (offset - 64), at most 127; at 0 or below no note sounds (Octet sends the note's NOTE_END at
  once). The reference plays fractional results between the neighbouring velocities' levels;
  Octet rounds them (1.9 dB at the extreme measured: 0.98 played as 1).
- **AEG offsets** (attack, decay, sustain, release): 1.5 EG values per step added to every
  operator's attack time, both decay times, decay 2 level and release time (each fits within
  0.4 dB rms of the equivalent values on a lone carrier). (Seventh session: the attack and sustain
  offsets measured every second step, decision 49.) A positive attack offset reaches
  carriers only: with +4 to +32 the modulator's attack stays as set (within 0.8 dB); negative
  attack offsets reach every operator. The sustain offset reaches decay 2 level only (adding
  it to decay 1 level too is 1.3-7.9 dB worse).
- **Filter offsets:** cutoff 8 cutoff values per step, resonance 2 values per step, FEG depth
  2 values per step (clamped to the parameters' ranges; each equal to the reference within 0.3
  dB at 6 offsets).
- **Note shift:** whole semitones on the played key (with the voice transpose; held within
  0-127).
- **Detune:** documented as -12.8..+12.7 Hz, it plays cents: the same cents at notes 36 and 72,
  -49.95 cents at the bottom, +49.63 at the top, nothing within 2 values of the centre, measured
  every eighth value (table `extPartDetuneCents`, straight between).
- **Macros act on modulators only** (operators the algorithm does not route to the output; a
  lone carrier does not change with any macro except texture). **Mod depth** (the depth macro)
  adds its value to every modulator's level, divided by 1 + the feedback level for the
  operator that feeds back, rounded towards 0: at feedback 0 the level moves one step per step
  (three probes, within 0.3 dB rms), at feedback 1, 2, 4 by 1/2, 1/3, 1/5 of a step (fits at
  every measured value; feedback 7 does not fit any single level). **Attack, decay, sustain and
  release** add their value to the modulators' attack time, both decay times, decay 2 level and
  release time (one value per step; each found as the equivalent single value at +-25, +-50).
- **Ratio shift and texture: not modelled.** The ratio macro moves the modulators' frequency
  (at ratio 1: x0.5 at -99, x0.82 at -50, x0.92 at -22, x1.30 at +22, x3.0 at +44, x4.0 at
  +55, x12.0 at +88, x17.95 at +99; at ratio 4 the factors differ: x0.46 at -99, x5.34 at
  +99), neither a fixed factor nor a fixed offset in ratio; the texture macro changes the
  carrier's spectrum as well (at -99 the fundamental vanishes and the energy sits near
  Nyquist, at -50 a formant-like peak near harmonic 7-8). Both are kept as parameters with no
  effect. (Both modelled in the eighth session: decisions 52 and 53.)

| Gate (`fm/compare_m7.py`) | Result |
| --- | --- |
| Pitch, 1 cent | **Met:** note shift 4 of 4, detune 49 of 49 (the table comes from `m7c`'s own renders for 37 of them; the 12 `m7a` cases at notes 36 and 72 are independent) |
| Filter offsets, level within 0.5 dB | **Met:** 18 of 18 |
| AEG offsets, harmonics within 1 dB | 44 of 62: decay met at every offset tested, release at all but -8 (1.0 dB); attack misses at +16 (3.1 dB on the carrier, 3.6 on the pair), +24 (2.0, pair) and +32/+63 (1.9-5.6 dB, both probes); sustain misses at -64, -32, -16, +16, +32 and +63 (1.2-2.3 dB: the reference moves the level by 26 values at -16 where 1.5 per step gives 24, and stops at 98 at +16) (fifth session). **Seventh session** (decision 49), like for like with `fm/compare_m7.py`: `m7b` 22 of 24 (was 20: attack +16 0.5 dB, was 3.1; sustain -16 0.4, was 1.9), `m7c`'s attack offsets on the pair 6 of 6 (were misses at +16 and +24), `m7h` (every second attack and sustain offset from -32 to +32, the renders the new rules come from) 53 of 64 (was 43; not met: attack +10 1.1 dB, newly, and +26 to +32 1.1-5.4 dB, where the reference reaches attack 99 sooner than 2^(offset / 4) and then holds; sustain -32 to -20 1.1-6.8 dB, where its level goes silent), `m7a` unchanged at 20 of 32 of its AEG cases (its offsets of +-32 and +-64 saturate either way) |
| Mod depth, harmonics within 1 dB | Without feedback (probes A, B): 12 of 12. Static EG at level 85 (C) and the pair with feedback 4 (D): 8 of 12 (+25 and +50 miss by 1.7-3.7 dB). Feedback 1, 2, 7 (`df`): 7 of 12 (+50 misses at feedback 1 and 2 by 1.9-2.6 dB; at feedback 7 the base case misses by 17.7 dB, so a modulator with feedback 7 is not reproduced, an M4 matter, and -50 and +50 miss too). Stack of three: 3 of 4 (+25: 8.8 dB). `m7a`: 8 of 18 (pair: -25 and +25 by 1.0-1.1 dB, +50 and +99 by 8.8 dB; static pair with feedback 4: +25 to +99 by 1.8-8.4 dB, unexplained: in a static pair without feedback the reference stops at level 99 as Octet does (decision 36), so the misses come from the envelope levels or the feedback, not from a level past 99; the wave probe's base case already misses by 3.2 dB, an M5 matter) |
| Envelope macros, harmonics within 1 dB | Pair and static probes: 44 of 48 (attack +25 1.0 dB on the pair, sustain +25, +50, +99 1.5 dB); wave probe 4 of 24 (its base case misses by 3.2 dB) |
| Velocity, level within 0.5 dB | **56 of 64** with the measured level velocity table below (29 of 64 with M4's, which used Dexed's curve: up to 14 dB off). Not met: velocities 2 and 3 at sensitivity +7 (0.8-1.5 dB: the table is straight between velocities 1 and 8 where the curve bends), fractional part velocities (0.5 plays 12 dB quieter in the reference than Octet's rounding to 1; 0.98 1.9 dB), and part velocity 36 (0.8 dB, between table points) |
| Ratio shift and texture | Not modelled (17 of 25 and 2 of 19 cases happen to meet). Eighth session: both modelled, 24 of 25 and 8 of 19 (see below) |

**Operator level velocity sensitivity (an M4 value, measured here because the velocity cases
exposed it).** `m7d`: a lone carrier at every sensitivity -7..+7 and 17 velocities (102 of the 255
renders came back empty from the recorder and were re-recorded). Against sensitivity 0, positive
sensitivities lower the level at low velocities and raise it slightly at high ones (+7: -59.1 dB
at velocity 1, 0 near 96, +5.3 dB at 127; every value about sensitivity times the +1 curve, in
0.375 dB steps); negative ones mirror that about velocity 64, and -7 at velocity 127 is silent.
Octet now plays this table (straight between the measured velocities) in place of Dexed's curve
(M4 decision 5), and lets the velocity raise an operator above its level (the clip at 0 dB still
applies to level and scaling). The table is checked against the renders it came from only in
Octet's own test; the 64 velocity cases of `m7a`/`m7c` above are independent renders.

**Not done in M7.** The controller-set and key-controller masks only matter with
Performance-level controller assignments (not part of the engine renders). Pitch bend ranges are
implemented from the Data List (decision 37) and checked only in Octet's own test (the sequence
recorder can bend; not measured yet). Not measured either: how the macros treat multi-operator
feedback loops (Octet divides the depth on the loop's last operator only), the macros' rounding at
odd values, the AEG offsets' rounding, detune on fixed-frequency operators (Octet applies it to
every operator).

**Sixth session (2026-10-31): sequences.** A second recorder plays several notes and controller
events per render, so what happens between notes could be measured. Campaigns in
`forks/measurements/m4/`: `m7p` (pilot), `m7f` (83 renders: mono and poly with both key assign
modes, portamento every eighth time value, the four time modes, fingered and full-time on legato and
detached notes, random pitch and pan with 32 notes per render, alternate pan, the part LFO's key-on
reset modes in poly and mono, the 2nd LFO's key-on reset, fractional part velocities), `m7g` (41:
time modes over 5 and 19 semitones, the other fourth time values, the retrigger at five attack
times, a key pressed twice with two note-offs, mono priority among three keys, new notes during a
release). No render came back empty. Measurement code: oscairn/measurement `fm/compare_seq.py`
(tolerances `seq`); local: `m7seq.py` (campaigns), `seq_octet.py` (Octet renders through the
test tool's new `render-seq`, the same events as MIDI). Event times in the reference move by the
recorder's buffer and run-loop jitter, so everything is measured from each render's own onsets
and pitch changes.

What the reference does (Octet does the same in Extended sounds, parameters 255-260):

- **Mono:** the last key pressed sounds; releasing it returns to the last key still held (three
  held keys in two orders). Without portamento every new note, legato or not, and every return to
  a held key starts again: the sounding note fades out (decision 41) and the new one attacks from
  silence; also when the old note is in its release. With portamento on (either mode), a legato
  note carries the envelopes on and glides; key assign makes no difference in mono.
- **Key assign** (poly): Single fades out the key's sounding note (also in its release) and starts
  it again; Multi adds a voice. In both a key pressed twice needs two note-offs (decision 42).
- **Portamento:** only when the performance's switch and the part's are both on (decision 39).
  Every glide is straight in semitones. Rate 1 keeps the time value's rate (12, 24 semitones, up and
  down); time 1 takes as long for any distance as rate 1 for an octave (5, 12, 19, 24 semitones);
  rate 2 and time 2 jump to an octave from the target when farther, then glide as rate 1 and time 1.
  Fingered glides only on legato notes, in poly as well (a key held); full-time also from a
  released note's pitch, in mono and poly. The rate by time value (`kPortaRate`, every fourth value
  measured: 434 semitones per second at 1, 36.6 at 64, 0.12 at 127; 0 is instant), straight in log
  rate between.
- **Random pitch, random pan:** decision 44.
- **Alternate pan:** alternates on every note of the sound: the same key, other keys, with a note
  held, and in mono on each legato note (as Octet already did).
- **Part LFO key-on reset:** Each-On resets on every note, 1st-On only when no key of the sound is
  held, in poly and mono (Octet's existing behaviour; now measured); Off runs free (its phase
  depends on the time since the performance loaded, not compared).
- **Fractional part velocities:** decision 45.
- **The 2nd LFO is per voice in the reference** (found here, changed in the seventh session). With
  its key-on reset on, a note overlapping a held one starts its own LFO from the start while the
  held note's LFO goes on without a jump (`l2r1`: zero crossings of the held note's vibrato every
  0.198 s straight through the second note-on); Octet's 2nd LFO was one per sound (as in Dexed)
  and reset only on a first key. Its pitch depth at depth 30 with offset 7 was also about twice
  Octet's (+-722 cents against +-361).

| Gate (`fm/compare_seq.py`) | Result |
| --- | --- |
| Portamento rate 2 %, start jump 0.5 semitone (where both glides last 0.25 s) | `m7f` 22 of 29, `m7g` 27 of 28 (the rate table comes from these renders: every fourth value from `m7g`'s 48- and 12-semitone glides and `m7f`'s multiples of 8, so this checks the implementation and the time modes, not an independent sample). Every glide longer than 0.11 s meets the rate. **Not met:** the 12-semitone glides at times 1-24 and the time-32 glides of time 1, rate 2 and time 2 (25-100 ms long), 2-6 % off: at times 1-24 the reference glides 12 and 24 semitones at a lower rate than 48 (time 1: 415 against 434 semitones per second), which a rate per time value cannot follow; pq28's rate meets (0.08 %) and its jump misses by 0.52 semitone, at the 0.31 s glide's analysis resolution |
| Mono, legato, key assign: the same notes start again, the same pitches (0.05 semitone) | `m7f` 18 of 18, `m7g` 12 of 13; not met: the retrigger at attack time 20 (decision 41: Octet dips to silence, the reference about 6 dB) |
| Alternate pan sides | 5 of 5 (same key, other keys, a held note; the mono case is in the row above) |
| Part LFO key-on reset, 95th percentile 15 cents | 4 of 4 (Each-On and 1st-On, poly and mono; worst 9.6 cents) |
| Fractional part velocities, level 0.5 dB | 5 of 5 renders, 71 notes, worst 0.46 dB (the curve below velocity 8 comes from these renders) |
| Random pitch, random pan | Statistics (32-128 notes per depth): random pitch range and standard deviation within one step of the reference's at every depth (depth 127: reference -31..+30, sd 19.3 steps; Octet -31..+30, 19.2), depth 4 never moves in either; random pan range within 4 positions (depth 127: reference -58..+60, Octet -62..+62), standard deviation 28.2 against 38.0 at 127, 16.8 against 19.2 at 64 |

Unmeasured or unresolved: the retrigger fade above attack time 60 and its shape (decision 41);
portamento from a held key returned to; the
reference's lower rates for short glides at times 1-24; random pan's narrower spread and left
lean, random pitch's extra step down; fractional velocities' effect on other velocity-dependent
values; key-on delay when a mono note starts again without portamento. (Seventh session, `m7k`:
portamento leaves fixed-frequency operators alone, the common pitch EG carries on through a legato
note, and a legato note takes over the key-on delay; decision 50.)

**Seventh session (2026-10-31): the 2nd LFO.** Campaigns in `forks/measurements/m4/`: `m7l` (61
sequence renders: pitch depth at 17 depths with offset 7 and offsets 0-6 at depths 30 and 99,
amplitude depth at 8 depths and offsets 0-6 at depth 99, every wave and phase from a key-on reset,
two notes four octaves apart with key-on reset off and on, with and without delay, and three notes
at irregular times without key-on reset) and `m7l2` (48: amplitude depth at 18 more depths and
offsets 1-6 at depths 16, 30 and 70, delay every tenth value). No render came back empty; the zero
runs flagged were amplitude modulation reaching silence and the gaps between notes. Measurement
code: oscairn/measurement `fm/compare_lfo2.py` (tolerances `lfo2`); local: `lfo2*.py`.

What the reference does (Octet does the same in Extended sounds; decisions 46-48):

- **Per voice:** each note runs its own 2nd LFO and its own delay; a note started during another
  leaves the held note's vibrato unchanged (both sides within 0.004 cycle across the second
  note-on, with key-on reset on and off, with and without delay).
- **Start:** phase 0 starts the triangle and saw down at their top, saw up and the square low (half
  a cycle into Dexed's LFO); the other phases follow. Octet's LFO now starts about 1.4 ms behind the
  reference's (not modelled) and runs 0.3 % slow at the probes' speed (the Extended speed table's
  interpolation, within the 1 % rate gate).
- **Pitch depth:** twice the DX7's; the DX7's depth steps (depth x 165 / 64) except that 99 plays as
  256; offsets 1-7 as 10, 20, 33, 56, 96, 160, 256 (the DX7's sensitivities at 1-3). Depth 99 with
  offset 7 swings +-2400 cents.
- **Amplitude depth:** straight in dB from the LFO's top (a triangle LFO gives a triangle in dB), in
  steps of 0.375 dB: offset 7 loses 1, 3, 8, 16, 32, 56, 111 and 256 steps at depths 3, 8, 16, 30,
  50, 70, 90 and 99 (table of 26 depths); offsets 1-6 lose 1, 2, 4, 5, 6, 7 eighths of offset 7's
  (the same at depths 16, 30, 70 and 99).
- **Delay:** the hold as the DX7's, then a fade in as long as the hold (the DX7's is quantised and at
  most about half a second).
- **Key-on reset off:** not modelled. Each note took a phase of its own: three notes on the same key
  at irregular times did not share one free-running phase, and a note overlapping a held one (`l2r0`)
  started in the opposite phase to it in one render and about 15 ms ahead of it in another.

| Gate (`fm/compare_lfo2.py`) | Result |
| --- | --- |
| Pitch depth, 1 cent | 30 of 31 (`m7l`: 17 depths, offsets 0-6 at depths 30 and 99; worst +0.5 cent elsewhere); **not met:** depth 80, +1.3 cents (0.07 %) |
| Amplitude depth, 0.5 dB where the reference is above its floor | `m7l` and `m7l2`: every depth from 3 up at offset 7 and offsets 1-6 at depths 16, 30 and 70 within 0.16 dB (the renders the tables come from); at depth 99 offsets 1-2 within 0.07 dB; **not met:** depth 99 offsets 3 and 4 (Octet's swing 0.8 and 2.6 dB less than the reference's 48.7 and 62.8 dB); deeper swings are silent at the bottom in both (not compared) |
| Wave start and shape, 95th percentile 15 cents over 1 s | Triangle, saws and the three other phases 6.3-14.2 cents; **not met:** the square (61 cents: at its edges, 1.4 ms of start offset and the tracker's smoothing) and the sine (18.9 cents); sample and hold reported only (its values are random) |
| Delay fade in, 2 % or 50 ms (the depth is read per 0.2 s cycle) | 11 of 12 (`m7l2`, delays 0-90); **not met:** delay 99, whose fade in reaches 10 % 75 ms (2.5 %) earlier in Octet (Octet against the reference: 2.89 against 2.97 s to 10 %, 3.96 against 4.02 s to 50 %, 5.02 against 5.12 s to 90 %). The delay commit's message (`d7cf201`) quotes a first local analysis that sampled both sides at one fixed period; the reference's period differs by 0.3 %, which biased its long cases, so these numbers replace it |
| Per voice: the held note's phase across another note's start, 0.05 cycle | 4 of 4 (key-on reset off and on, delay 0 and 50) |

Before these changes, like for like: the pitch depth was half the reference's at every depth (e.g.
+-361 against +-722 cents at depth 30); the amplitude swing at depth 30 was 2.8 dB against 6.0; the
fade in at delay 99 took 0.54 s against about 2.2 s; the start was half a cycle off for every wave.

**Mono legato (`m7k`, 7 sequence renders, seventh session).** A fixed-frequency carrier under a
legato glide (time 64) stays at its 440 Hz in both (within 0.01 semitone); the common pitch EG
(attack from -50 at depth 1 octave, time 70) carries on through a legato note in both (the pitch
after the note change continues the same ramp, within 0.3 semitone at each 0.1 s point, the rest
being the recorder's event timing); key-on delay 20 (0.52 s) with portamento: a legato note
during the sounding note takes over at once in the reference, where Octet left 0.5 s of silence and
started again, and a legato note during the first note's delay sounds when the first would have,
where Octet waited its own delay. After decision 50 Octet does the same in both cases (checked
against these renders by eye in the level and pitch timelines, and in Octet's own test).

Unmeasured or unresolved: the 2nd LFO in mono legato and on a returned-to mono key (decision 46);
the reason for depth 99's smaller swings at offsets 3-4; the mod wheel's pitch and amplitude
modulation in Extended sounds (the DX7 path); the speed range Normal's table was measured in M4 and
not re-checked per voice.

**Eighth session (2026-10-31): modulator EG levels, Ratio Shift, Texture, the retrigger.** Campaigns
in `forks/measurements/m4/`: `m8a` (38: a pair, the modulator's EG levels every third value at
operator level 99 and four at level 70), `m8b` (174: Ratio Shift every 11 values at modulator ratios
0.5, 1, 1.5, 2, 3, 4, 5, 8, 16, and a fixed-frequency modulator), `m8c` (149: Texture every third
value on a lone carrier, every fifth on the pair at modulator levels 70 and 40), `m8d` (594: Ratio
Shift every third value at the same ratios), `m8f` (66: Texture on a lone carrier at the other
negative values), `m8e` (10 sequences: the mono retrigger two octaves apart at attack times 0-90, so
each note's level can be read on its own). No render came back empty; the zero runs `dropouts.py`
flagged in `m8c` are the formant waves' own silent stretches. Local tools: `m8gen.py`, `fitegl.py`,
`ratiofit2.py` (the modulator ratio from the sideband spacing, refined on the first upper sideband),
`texfit.py`, `texfit2.py`, `retrig3.py` (each note's level by demodulation).

- **Modulator EG levels (decision 51).** The equivalent modulator level (Octet's renders searched
  over operator level at EG 99) was E or E + 1 for EG level E, alternating (E + 1 for even E); at
  operator level 70 the sum minus 99. Octet used the carrier EG curve, 0.6 dB louder at EG 85, which
  was the depth macro's miss on probe C. Like for like with `fm/compare_m7.py`: `m8a` 29 → 38 of 38
  (the renders the rule comes from), `m7b` 69 → 71 of 101 (depth macro probe C at +25 and +50: 2.45
  and 2.64 → 0.14 and 0.15 dB), `m7a` 169 → 170 of 242, `m7c` 78 of 85 either way (the attack
  offset cases on the pair 0.7-0.9 → 0.1-0.5 dB). Worse: `m7a`'s five cases where the sustain macro
  or offset takes the modulator's decay 2 level from 75 to 99 (1.52 → 3.42 dB, not met before
  either): the modulator rises in decay 2 sooner in the reference (Octet's rising decay segments
  move straight in dB at the decay rate; not measured).
- **Ratio Shift (decision 52).** At table values, independent renders (`m7b`, note 60 against the
  table's note 48) agree within 0.6 cents; between table values, `m7a` at ratio 1 (-50, -25, +25,
  +50) within 0.2, 0.4, 4.5 and 3.6 cents and `m7b` at ratio 4 (-50, +25, +50) within 4.7, 1.9 and
  5.7 cents (with the first table, every 11 values only: 0.8, 2.1, 1.8, 3.2, 2.2, 4.9, 17.7). The value at
  ratio 1, +55 comes from `m7b`'s own render (the scan read half the frequency in `m8b`). Timbre
  (`fm/compare_m7.py`): `m7b` Ratio Shift cases 24 of 25 (r4 at +50 misses, 7.95 dB on harmonic 8:
  the inharmonic sidebands move 5.7 cents), `m7a` 16 of 18 (the two misses on the wave probe,
  whose base case already misses).
- **Texture (decision 53).** On a lone carrier every negative value from -63 to -1 matches one of
  Octet's waves within 0.3 dB rms (the renders the table comes from); -64 to -85 within 0.7-2.6 dB
  rms on the closest formant; beyond, nothing matches. On the pair, positive values reach the
  modulator: `m8c` level 40 every case met, level 70 met except +51 to +66 (1.2-7.6 dB: the formant
  wave at spread 1-7 on a modulator, an M5 matter). Negative values on the pair miss almost
  everywhere (up to 60 dB) though the carrier's wave is right on its own: a modulated carrier with a
  non-sine wave does not play as the reference's, the M5 issue the wave probe's base case shows.
  `fm/compare_m7.py` like for like: `m8c` 60 → 99 of 149, `m8f` 4 → 48 of 66, `m7b` texture cases
  2 → 8 of 19, `m7a` 1 → 5 of 18.

- **The retrigger (decision 54).** The old note's level (demodulated at 130.8 Hz) falls the same way
  at attack times 0-60: -1, -3, -5, -9, -15, -24, -40 dB at 2 ms steps, (1 - t / 13.3 ms) squared
  within 1.5 dB; the new note's level (at 523 Hz) rises from its key-on at each attack time's rate.
  Octet waited for a 2-13 ms straight fade before starting the new note. `fm/compare_seq.py` like for
  like: `m7g` notes 12 → 13 of 13 (the retrigger at attack 20 now dips as little as the reference's),
  `m7f` 18 of 18 either way, the glides unchanged (`m7f` 22 of 29, `m7g` 27 of 28).

Found and not changed:

- **Feedback 7 on a modulator** (`m7c` `df7`, 17.7 dB): the reference's harmonics fall a straight
  6.8 dB per harmonic up to the 12th; Octet's fall away, and no single feedback amount (tried 1.5,
  2.3 and 3 times the DX7's at feedback 7) gives a straight line: a stronger feedback turns Octet's
  spectrum into a saw's (1/n) instead. Not resolved.
- **Probe D** (the pair with envelopes and feedback 4, depth macro +25 and +50: 1.67 and 3.70 dB):
  steady state matches; the misses are harmonics 4-6 during the attack (0.02-0.3 s), 2-6 dB fewer in
  Octet. Not resolved (feedback during a rising level).
- **Random pan's spread:** in `m7f`'s four depth-127 renders, two pairs share long runs of the same
  positions (23 values in a row in one pair), so the reference's 128 notes hold about 70 independent
  values, and its positions near the centre read in steps of 8-12 (the pan law is flat there). Its
  standard deviation (28.2 against uniform's 36.4) is therefore weaker evidence than it looked; not
  changed.
- **Portamento at fast times:** the slope is read between 20 % and 80 % of each glide, so a fixed
  start delay would not lower it; the lower slope of 12- and 24-semitone glides at times 1-24 needs a
  finer look than the 25-100 ms glides give. Not changed.
- **The attack offset +10** (`m7h` `oA74`, 1.05 dB): the attack itself is within 0.3 dB; the miss is
  in the decay after it (2-3 s), where Octet's level runs about 1 dB above the reference's. Its
  neighbours (+8, +12) meet. Not changed.

Still open after this session: the 2nd LFO's amplitude depth 99 at offsets 3 and 4 and the M5 cases
(spread 7 formants, formant 2 at spread 5, the reference's flat floor) were not looked at; a modulated
carrier with a non-sine wave (the wave probe's base case and every negative Texture value on the pair)
is the largest remaining timbre miss and belongs with them; the modulator's rising decay 2 segment.

**Ninth session (2026-11-01): rising envelope segments and carrier EG levels (decisions 58, 59).**
The M5 part of this session (a modulated non-sine carrier) is under M5.

- **Rising segments.** In `m7a`'s sustain cases (the pair; the sustain macro +25 to +99 or offset
  +32/+63 takes both decay 2 levels to 99) the modulation index read from harmonics 1-3 (local
  `idx.py`) rises in the reference from -10.8 to -1.0 dB towards a point 1.52 dB above its final
  value, the distance falling as e^(-6.06 t): the attack's curve at decay time 55's rate in
  doublings per second (within 0.06 dB at every 50 ms step); Octet's straight rise got there about
  50 ms sooner. A lone carrier's decay 2 rising to 99 (sustain offset +63) does the same.
  `fm/compare_m7.py` like for like (`4102745`): `m7a` 205 → 212 of 242 (the five sustain cases on
  the pair 3.42 → 0.74-0.99 dB, the carrier's +32 and +63 1.18 → 0.25), `m7b` 89 → 90 of 101; `m7c`,
  `m7h`, `m8a` and the M4 `eg` and `decay` campaigns unchanged.
- **Carrier EG levels** (`m9b`, 52 renders: a lone carrier at every EG level 60-99 and every fifth
  below, levels read 0.15-0.35 s in). They go in steps of two as a modulator's do: 2k plays as 2k + 1,
  1.51 dB a step, linear from 25 up (0.75 dB a value); 10 and below silent, 15 at -68.7 dB. Octet's
  table (every tenth level) was 0.15 dB low throughout and 0.68 dB low at 98. After `a217a5b`: 48 of
  49 audible levels within 0.1 dB of `m9b` (the renders the table comes from; was 1 of 49, worst 1.3
  dB), `m7a` 212 → 213, `m7b` 90 → 91, the rest unchanged; nothing went from met to not met, the
  largest change for the worse 0.16 dB.

| Gate (`fm/compare_m7.py`) | Eighth session's numbers | Now (every campaign re-rendered from `a217a5b`'s tree) |
| --- | --- | --- |
| `m7a` | 170 of 242 | 213 of 242 |
| `m7b` | 71 of 101 | 91 of 101 |
| `m7c` | 78 of 85 | 78 of 85 |
| `m7h` | (not listed) | 55 of 66 |
| `m8a` | 38 of 38 | 38 of 38 |
| `m8c` | 99 of 149 | 121 of 149 |
| `m8f` | 48 of 66 | 48 of 66 |

The eighth session's `m7a` and `m7b` numbers were from its own renders; re-rendered from the eighth
session's head `b909855` with the same script this session they read 180 and 85 (why they differ was
not looked into). Like for like from `b909855`: `m7a` 180 → 213, `m7b` 85 → 91.

Not done this session (time-boxed, no change): the 2nd LFO's amplitude depth 99 at offsets 3 and 4
(the reference's swings imply 130 and 167 steps where eighths of offset 7's 256 give 128 and 160,
while offsets 1-2 fit 256; not resolved); portamento at fast times; feedback 7's straight 6.8 dB per
harmonic (no model found).

**Tenth session (2026-11-01): the count discrepancy, texture, the 2nd LFO's deep amplitude, bend
ranges and fixed-frequency detune (decisions 60-62).** Campaigns in `forks/measurements/m4/`:
`m10b` (8 sequences: pitch bend ranges, below), `m10d` (12 renders, wrong probe: the fixed carrier
at coarse 3 is 0.84 Hz; not used), `m10d2` (12: a 220 Hz fixed-frequency carrier at notes 48 and 72
with part detune 0, +-5, +12.7 Hz and operator detune +-15), `m10t` (28: texture on lone carriers with
waves 1-4 and on the m7 pair's modulator with waves 1, 3 and 4 at level 50). No render came back
empty; one `m10t` render had a 12 ms gap in its held part and was recorded again (`m10t2`); the
others have no run of 16 or more near-zero samples in the held part (the formant window at spread 7
excepted, whose silent stretches are its own). Local generators
`m10gen.py`, `m10seq.py`; `octet_render.py` and `seq_octet.py` now take `OCTET_OUT` so a build's
renders can go to their own directory.

- **Why the eighth session's `m7a`/`m7b` counts (170, 71) differ from the ninth's re-render of
  `b909855` (180, 85).** Neither the renders nor the comparison are nondeterministic: the eighth
  session's own last renders (`m7a/octet_prev`, `m7b/octet_prev`, made at 22:44 with the ratio and
  texture changes built) are bit-identical to the ninth session's re-render of `b909855`
  (`octet_old`), and `fm/compare_m7.py` (unchanged since `d14c301`) gives 180 and 85 on both. Re-
  rendering `c59a932` (the modulator EG commit) this session gives 170 and 71: the eighth session's
  table quoted the counts measured after its first commit, before Ratio Shift and Texture (which
  together added 10 met cases to `m7a` and 14 to `m7b`). No fix to the measurement was needed.
- **Texture from the operator's own wave (decision 60, `ebe7c42`).** On the wave probe the reference
  played a sine at -25 and all harmonics 2 at spread 4 at -50 (fits 0.22 and 2.48 dB rms over every
  candidate wave), which is the carrier's own place (+26: all harmonics 1 at spread 3) moved by the
  value. `fm/compare_m7.py` like for like: `m7a` 213 -> 215 of 242 (texture -25 and -50 on the wave
  probe 12.0 and 53.5 -> 0.15 and 0.35 dB; -99 183.7 -> 21.9 dB, still not met); `m7b` 91, `m8c` 121
  and `m8f` 48 unchanged (sine operators play as before, nothing else moved by more than 0.01 dB).
  Independent check, `m10t`: 11 -> 27 of 28 (not met: all harmonics 2 at spread 3 at -40, 1.9 dB).
- **The 2nd LFO's amplitude depth 99 at offsets 3 and 4 (decision 61, `73d3b18`).** The reference's
  48.7 and 62.8 dB swings are half and five eighths of the straight 96 dB (48 and 60 dB) played
  through the same bend as a carrier's EG levels that far down (`m9b`: 48.67 dB at level 35 against
  48.16 straight; 63.02 at level 19 against 60.2). `fm/compare_lfo2.py`: `m7l` 54 -> 56 of 59
  (offsets 3 and 4: 0.83 and 2.64 -> 0.33 and 0.13 dB), `m7l2` 47 of 48 either way (offset 7 at
  depths 80-90 unchanged, 95 +0.08 -> +0.02 dB: the table goes through the bend's inverse first).
- **Pitch bend ranges** (decision 37, now measured; no engine change): `m10b`, a sine at note 60
  with ranges +2/-2, +12/-12, +24/-48, +7/-5, +1/-1, 0/0, -12/+24, -48/+24, bent to the top, the
  bottom, a quarter up and down and back. New `fm/compare_bend.py` (tolerances `bend`, measurement
  `92f1c44`, `75c6079`): 38 of 38 checked bend values within 1 cent of the reference (worst 0.55
  cents); the two bends to -48 semitones (16 Hz) read about 50 cents high in both renders and are reported only.
- **Detune on fixed-frequency operators (decision 62, `d766d55`).** `m10d2`: the part's detune left
  the fixed carrier at 220.08 Hz in every case, and the operator's detune +-15 moved it by +31.8 /
  -31.4 cents at note 48 and +20.1 / -20.0 at note 72, the ratio-mode table's values at those keys
  within 0.4 cents. Octet did the opposite (up to 49.6 cents off); now within 0.4 cents in all 12
  cases; ratio-mode operators unchanged (`m7a` bit-identical).

Not done this session (time-boxed): portamento at fast times (the 25-100 ms glides need a finer
pitch reading than the tracker gives; no new renders); feedback 7's straight spectrum (no model).
M8 (the converter) is in `oscairn/performance-converter` and editor metadata in Oscairn, both outside
this fork.

**Eleventh session (2026-11-01): portamento in steps (decision 64).** No new renders. Read with 1 ms
smoothing (new `fm/glide_steps.py`, measurement `63558f7`, with a self-test on synthetic stepped glides),
every glide in `m7f` and `m7g` moves in steps: the period is 9.977 ms (440.0 samples at 44.1 kHz,
read from glides of 0.3-1.7 s at times 32-100: 9.970-9.979 ms), the same at every time value, and
the step is the time value's rate times the period whatever the distance (time 1: 4.29 semitones on
12-, 24- and 48-semitone glides; time 8: 3.38-3.42; time 16: 2.48). So the fast 12- and 24-semitone
glides were not slower than the 48-semitone ones: the tracker's 20-80 % slope on a staircase of 3-9
steps depends on where the steps fall. Octet now moves its glides the same way (`bd83f35`).
`fm/compare_seq.py` like for like: `m7f` glides 22 -> 23 of 29, `m7g` 27 -> 28 of 28. Still not met:
pt1, pt8, pt16 (2-6 % on the slope; step sizes within 1.7 % of the reference's: time 1 4.33 against
4.29, time 8 3.36 against 3.42, time 16 2.47 against 2.48) and the time-mode glides at time 32
(pm1t32, pm2t32, pm3t32: 1.7-8 % on the slope, not looked into). Feedback 7's straight spectrum: no
model, not looked at again.

**Twelfth session (2026-11-01): the glide clock and the texture path (decisions 66, 67).** No new
renders.

- **Portamento's clock runs freely.** Read edge by edge (new `fm/glide_grid.py`, measurement
  `f105288`, with a self-test), the steps in `m7f`'s renders at times 32-64 fall on 64-sample block
  boundaries, 6 or 7 blocks apart (pt64: seven of 448 samples, then one of 384: 440 on average, which
  is why `fm/glide_steps.py` read 440.0), and both glides of each render, 2-4 s apart, sit on one such
  grid: 100 % of the second glide's edges within 8 samples of the grid fitted to the first (94-100 %
  for the first itself), against 6-33 % for a 440-sample grid restarted at each glide's first edge.
  Octet now keeps one clock per engine, from prepare (`174b8fd`); its renders show the same structure
  (the second glide 94-98 % on the first's grid; before, 0 % on it and 100 % on its own restarted
  grid). `fm/compare_seq.py` like for like: `m7f` glides 23 -> 24 of 29 (pt16 met), `m7g` 28 of 28
  either way. Still not met: pt1 (glide 0 -5.6 -> -7.6 %), pt8 (glide 1 -3.5 -> -5.3 %), pm1t32
  (-6.5 -> -5.6 %), pm2t32 (+8.3 -> +6.0 %), pm3t32 (+6.2 -> +3.9 %): glides of 3-10 steps whose
  tracker slope depends on where the ticks fall against the note, and the reference's clock phase is
  not known for a render (it would have to be fitted per render, as `fm/glide_grid.py` does, before
  such a comparison means much).
- **The texture path** (`998251d`): formant 1 on the plus side, the formant above place 63 by rule,
  places for every wave and the wrap (decision 67). `fm/compare_m7.py` like for like: `m8c` 122 ->
  126 of 149 (+51 to +66 on the level 70 modulator met: 1.22-7.57 -> 0.10-0.31 dB), `m8f` 49 of 66
  either way, `m7a` 215 -> 219 of 242 (texture -99 on the wave probe 22.65 -> 7.44 dB, still not met;
  +50 on it and on the other probes met), `m7b` 91 -> 94 of 101, `m10t` 27 of 28 either way. Worse,
  all already failing: formant 2 at spread 7 with formants 9-62 (`m8c` tc12, -87, 6.67 -> 56.49 dB;
  tp70v15 23.71 -> 35.66; `m8f` tc7 104.4 -> 119.7; `m7b` t11 29.1 -> 44.0; others 0.1-3.9 dB) and
  +99 on the wave probe (1.85 -> 5.11, a formant centred above Nyquist either way). In those renders
  the formant numbers now put Octet's strongest harmonic where the reference's is (32 of 33, was 14),
  so what is left there is the spread 7 window's shape (M5).
- **Not looked into again, time-boxed:** the fast glides above; the 2nd LFO and other M7 open items.

### Thirteenth session (2026-11-01): Jacob's answers, levels, tuning, velocity, sample rates

Jacob's answers on decisions 1-68 are marked in the list above and summarised in section 10. New
decisions 69-75. Renders in `forks/measurements/m4/` (never committed): `m13v` (42: level stages),
`m13t` (28) and `m13u` (185: tuning), `m13p` (29) and `m13q` (71: pitch velocity), `m13f` (95: FEG
time velocity), `m13l` (40: listening material); listening examples in `forks/measurements/listen/`
(decisions 7, 17, 20; README there; renders, not committed). Comparison renders
(`m4/octet_render.py`) now set the probes' part volume and pan as Output (-7.136 dB) and no longer
add master tune.

- **Level** (decision 69): Extended voices play 6.99 dB louder at the mix, the reference's level
  without its part volume and pan; the measured volume, dry level and pan curves are recorded for
  the converter. Extended voices are no longer clipped per voice (decision 70).
- **FEG time velocity** (decision 72): against `m13f`, 65 of 82 durations within 2 % or 2 ms (18
  before).
- **Operator pitch velocity** (decision 73): fixed-frequency operators only; against `m13q`, 71 of
  71 within 1 cent (9 before).
- **Tuning** (decision 74): per pitch class; `m13u` 155 -> 185 of 185 within 0.25 cent (mean 0.155
  -> 0.064 cent), `m13t` mean 0.185 -> 0.067 cent; fm/compare.py pitch and pitch2 unchanged (37 and
  4 outside tolerance, the same cases; 35 of pitch's are renders its analysis cannot read).
- **Sample rates** (decision 71): `oscairn-fm-test rates` passes at 44.1, 48 and 96 kHz.
- **Filter blocks** (decision 75): kept; the instant-segment experiment made the FEG gate worse
  (91 -> 82 of 92).
- Like for like on the level change (fm/compare_m7.py, Octet before = 5a11334): m7a 219 -> 217 of
  242 (fr96 and fr127, the reference's output clip, decision 70; egm0v74 and egm2v124 swap sides of
  the limit), m8c 126 -> 126 of 149, m9a 74 -> 74 of 79.

**Thirteenth session: tests run.** Code commits `cb9b0d7` (Extended level), `68c1d29` (FEG time
velocity), `946d1b6` (fixed-frequency pitch velocity), `ef5a1d8` (tuning), `43be587` (the
sample-rate check). Each was tested on its committed tree with nothing else of the engine's in the
work tree, locally: Release CTest 36/36 (37/37 with the rate check; the 164 Classic cases
bit-identical through engine, JUCE plugin and native CLAP on arm64), the x86_64 baseline under
Rosetta (engine, plugin, CLAP: 164 of 164 identical) and the Extended test under Rosetta,
ASan/UBSan 19/19 (20/20), TSan 19/19 (20/20); `oscairn-fm-test rates` also passes under Rosetta.
Each new check was confirmed to fail with its change undone (the level, FEG time velocity, pitch
velocity and tuning checks; the rate check fails when the key-on delay is counted in 44.1 kHz
samples). The level commit was amended three times before it was pushed (the voice clip and the topology
failure it caused, the gain moved out of the voice after the filter resonance cases clipped, the
modulator check's carrier level after an x86_64 failure). CI: the macOS job of `oscairn-tests` was
not picked up by GitHub's hosted runners on some heads ("not acquired ... after multiple
attempts"); results per head are in the session report.

### Fourteenth session (2026-11-01): the pitch grid

Decision 76. Renders in `forks/measurements/m4/` (never committed): `m14s` (79: part detune at keys
45, 69 and 93, note shift, ten coarse/fine pairs, detune at keys 54, 66, 90 and 101, common pitch
velocity, fixed key follow) and `m14b` (3 sequences: pitch bend in steps of 4 and 2 values at ranges
2 and 12, keys 60 and 61). Pitches by the slope of the analytic signal's phase over 0.65-0.85 s of
a steady note (the reference's renders sit on the grid within 0.003 cent above 200 Hz with it, 0.006
above 100 Hz; below that the estimate wanders by up to 0.13 cent). Octet before = `e57145d`, both
at 44.1 kHz:

| renders | before | after |
| --- | --- | --- |
| `m13u` (185) | mean 0.064, worst 0.198 cent | mean 0.0000, worst 0.0004 |
| `m13t` (26) | mean 0.067, worst 0.410 | mean 0.001, worst 0.012 |
| `m14s` (79) within 0.05 cent | 23 | 55 |
| `m10b` bends (fm/compare_bend.py, 38 checked) | 38 met, worst 0.55 | 38 met, worst 0.01 |
| `m14b` steps | not the reference's | the same steps |
| fm/compare.py `pitch` outside tolerance; readable cases within 0.05 cent | 37; 108 of 135 | 38; 125 of 135 |
| fm/compare.py `pitch2` | 4; 62 of 120 | 4; 117 of 120 |

Worse in `m14s`: 6 part detune values (by up to 0.15 cent), 5 detunes between the table's keys (up
to 1.18 cent), pitch velocity (pv127o14v127, 11.06 -> 11.22 cent), fixed key follow (kf99n50, 1.71
-> 1.77) and the ratio past coarse 31 (c31f127, 24.4 -> 24.7; c31f100 is 3830 and 3660 cent off before and
after, the reference playing ratio 6.565 there). In `pitch`, cpv32v1 went from -0.85 to
-1.12 cent. All are tables the rule leaves as they were.

**Fourteenth session: tests run.** Code commit `b903c93`, tested on its committed tree with nothing
else of the engine's in the work tree, locally: Release CTest 37/37 (the 164 Classic cases
bit-identical through engine, JUCE plugin and native CLAP on arm64), the x86_64 baseline under
Rosetta (engine, plugin, CLAP: 164 of 164 identical), the Extended test and `oscairn-fm-test rates`
under Rosetta, ASan/UBSan 20/20, TSan 20/20. The new tuning and bend checks were confirmed to fail
with the change undone (`ext_note.cc` and `dx7note.cc` as at `e57145d`, with the old table): three
of the four fixed fine values, both ratios and all 14 bend values fail; the 12 keys, fine 100 and
the two detunes pass, as the table played them. CI on `b903c93`: `oscairn-tests` all 5 jobs
passed; `dexed-pipeline` Linux Build and Windows Build passed, macOS Build failed at its signing
certificate import (the fork has no signing secrets).

### Fifteenth session (2026-11-01): time key follow, modulator feedback, wider ranges

Decisions 77-81; the register in section 10 has the per-sound results. Renders in
`forks/measurements/m4/` (never committed): `m15k` (230: AEG time key follow), `m15f` (18: carrier
feedback, fixed frequencies past coarse 21), `m15g` (14: modulator feedback), `m15h` (20: macros on a
carrier that feeds back, the depth macro on a modulator that does), `m15b` (4) and `m15p` (24): FM-X
Parts of eight factory performances rebuilt from their decoded values without controller sets, both
sides; the whole-sound comparison re-run in `fmx-compare/` (`octet-final/`, `results-final.json`) and
the listening pairs refreshed in `listen-fmx/` (old Octet files as `-octet-before`).

**Fifteenth session: tests run.** Each code commit was tested on its committed tree (the plan's
edits stashed), locally: Release CTest 37/37 (the 164 Classic cases bit-identical through engine,
JUCE plugin and native CLAP on arm64), the x86_64 baseline under Rosetta (engine, plugin, CLAP: 164 of
164 identical), the Extended test and `oscairn-fm-test rates` under Rosetta, ASan/UBSan 20/20, TSan
20/20: `166aa13`, `e381059`, `1163ef4`, `5b0c5d3` and `2b35e18` (the last one's `rates` at all five
rates, also under Rosetta). Each new check was confirmed to fail with its
change undone: the time key follow slopes at notes 30, 66 and 105 and sensitivity 3 (with the Classic
curve back in `ExtNote`; the step-rule checks are table checks and pass either way), df7's harmonics
(26.2 dB off with the old feedback amount), the three wide-range pitches (2000, -1200 and 5200 cents
off with the old ranges) and fixed coarse 21 fine 64 (600 cents low with the old ceiling). The
extremes check (note shift, bend ranges and fixed frequency at their limits stay finite) is a guard
and passes either way; the rates commit adds rates only. CI on `166aa13`, `e381059`, `1163ef4`,
`5b0c5d3` and `2b35e18`: `oscairn-tests` all 5 jobs passed on each; `dexed-pipeline` Linux Build and
Windows Build passed on each, macOS Build failed at its signing certificate import (the fork has no
signing secrets).

### Sixteenth session (2026-11-02): presets (stopped early)

Pushed with this record: `9c4ae45`, the Oscairn SDK headers (`sound-contexts.h` draft 5, `assets.h` draft 2, new
`preset-discovery-plugin.hpp`) copied unchanged at Oscairn `3b58efe`; Octet advertises draft 5 with
`prepare_preset` null and behaves as before. Decision 70 recorded as decided (section 10).

Not pushed, stopped when the session's budget ran out: two commits on the local branch
`oscairn-presets` (`3e3d0bb` "List cartridge voices as CLAP presets", `4b3d866` "Prepare sound contexts
from cartridge voices"): `source/oscairn/Presets.*`, the discovery factory in the entry, the built-in
cartridges unpacked into `Octet/Cartridges` beside the `.clap` by the build, `prepare_preset`,
`clap.preset-load`, and `oscairn-fm-test presets` (a generated 32-voice cartridge listed with names and
keys; each voice prepared, preset-loaded and loaded as parameter events renders bit-identically; a
snapshot reopens with the cartridge deleted; six malformed cartridges and bad keys refused). On a local
Release arm64 build of `4b3d866` only: the presets test passed and failed with each change undone
(no factory; no `prepare_preset`; no preset-load; offset dropped; fine frequency unpacked wrongly;
format byte unchecked); `oscairn-plugin-check` passed in the engine and with `OSCAIRN_PLUGIN_WORKER`
set; a scratch probe on Oscairn's host library indexed 2,112 presets (the built-in 33 cartridges and the
same 33 from the desktop Dexed's folder), prepared seven contexts and preset-loaded one. Left to do:
the full gates on both commits (Release CTest, x86_64 under Rosetta, Extended, `rates`, ASan/UBSan,
TSan), push and CI, and the plan's decision and section 4.10 text for them. Open point: the Oscairn
plugin store installs only the `.clap`, so the built-in cartridges beside it are not listed there.

**Sixteenth session: tests run.** On `9c4ae45` (its committed tree, in a separate clone): Release CTest
37/37 (the 164 Classic cases bit-identical through engine, JUCE plugin and native CLAP on arm64, and
Oscairn's scanner, renderer and worker tests against an Oscairn build at `3b58efe`), the x86_64
baseline under Rosetta (engine, plugin, CLAP: 164 of 164) with the Extended test and `rates`,
ASan/UBSan 20/20, TSan 20/20. The docs commit changes no code. CI results are in the session report.

### Seventeenth session (2026-11-05): instant Performance changes, cartridge presets

Oscairn read at `eecdee4` (`oscairn/oscairn` main, after `e4a0ef8` and `347b2fc`; the local clone
could not pull over SSH, `git ls-remote` over HTTPS confirmed main was `eecdee4`), and later in the
session at `2d1424a`, which moved the extension to draft 2 (`858263b`, the ring-out buffer) and no
longer reads draft 1. The SDK headers were re-vendored unchanged from each (only `sound-contexts.h`
changed). Decisions 82 and 83
proposed (section 11, M4 list); sections 4.5 and 4.10 updated.

Commits: `610df88` (headers), `8c70eee` (snapshots prepared beside), `cb8d3b6` and `2b967b7` (the
sixteenth session's two presets commits, carried over; the two `publish` helpers they and `8c70eee`
added are one now), `a8ad4e7` (Octet 0.2.0, the first version change since 0.1.0); then for
draft 2: `2c32a8c` (headers at `2d1424a`), `5f78f85` (the ring-out buffer), `d92947b` (Octet 0.2.1,
since 0.2.0 offered draft 1).

**Tests** (each new one fails with its change undone): `oscairn-fm-test snapshots` (CTest
`oscairn-fm-snapshots`): a snapshot of two sounds (a brighter Default, sound B on channel 2)
prepared beside a playing instance and selected plays exactly as the snapshot loaded, over 40
blocks with notes on both channels; nothing is selected before the cue; a note held on the
outgoing sound (a prepared one with a slow release) rings out beside the incoming sound, the output
that sound alone plus the incoming one alone, sample for sample, every block until it is idle (5.05 s
after its note-off); it refuses discard while held and while its release rings, then goes; a bad
snapshot (with a reason) and one without its Default are refused, and one needing two contexts where
one is free adds nothing and says why. Fails without the `get_extension` line. From `5f78f85` it
also holds a note on a prepared sound through a change with a ring-out buffer: before the cue the
buffer stays silent; after it, for 60 blocks through the note's release, the main output (its left
channel, which the test's instrument keeps) is the incoming sound alone and both channels of the
buffer the outgoing sound alone, sample for sample.
Fails with the plugin not passing the buffer to the engine. `threads` also prepares snapshots while
processing (29 in a run). `presets`: as in the sixteenth session; on the merged code it still fails
without the factory, without `prepare_preset` and without preset-load.

**Switch times** (Release, macOS arm64, machine under other load): preparing that two-sound snapshot
beside took 0.43 ms; the block applying the cue (loading both sounds into the engine) 23-24 us
against 1.9 us for a block without one (medians of 25). In Oscairn's own Session (a scratch probe
built on Oscairn `eecdee4`'s libraries following its `control-performance-switch` D -> E case, two
Octet parts with one sound each): D -> E with a note held on D prepared E's sounds beside D's in 1.8
ms (0 instances loaded, 2 reused, 2 beside), E -> D 1.8 ms, D -> E again 1.9 ms; D's held note kept
its sound and played on, D's sound was discarded once rung out, and each switch then matched a fresh
load sample for sample. With the build before (`cbfdc6b`), the first switch loaded two instances
(2.9 ms; cold open 45 ms) and later ones took warm instances (5.0 and 5.5 ms). Again at Oscairn
`2d1424a` with `d92947b`'s plugin (draft 2): D -> E with a note held 3.1 ms, E -> D 2.4 ms, D -> E
again 2.6 ms, each beside (0 loaded), all the probe's checks passed. Whether Oscairn hands Octet
its ring-out buffer during that switch is not something the probe checks (the plugin's own test
covers the buffer).

**Seventeenth session: tests run.** Each code commit on its committed tree (a worktree, builds
incremental across commits, Oscairn's host tests against an Oscairn build outside its repo at
`eecdee4` for the first five commits and `2d1424a` for the last three): Release CTest 37/37 on
`610df88`, 38/38 on `8c70eee`, 39/39 on `cb8d3b6`, `2b967b7`, `a8ad4e7`, `2c32a8c`, `5f78f85` and
`d92947b` (the 164 Classic cases bit-identical through engine, JUCE plugin and native CLAP on arm64);
ASan/UBSan and TSan 20, 21, then 22 of 22 on each. The x86_64 baseline under Rosetta (engine,
plugin, CLAP: 164 of 164), the Extended test and `rates` passed on `610df88`, `8c70eee`, `cb8d3b6`,
`2b967b7` and `a8ad4e7`. **Not run on `2c32a8c`, `5f78f85` and `d92947b`:** Rosetta stopped
starting x86_64 programs on this Mac during the session (every new x86_64 process, a two-line test
program included, hung in an uninterruptible wait; restarting `oahd` needs an administrator), so
for those three the x86_64 cases are covered only by CI's Linux x86_64 job.

`oscairn-plugin-check … org.oscairn.fm synth --param 1017=70` PASSED in the engine and with
`OSCAIRN_PLUGIN_WORKER`: on `a8ad4e7`'s plugin against Oscairn `eecdee4`, and on `d92947b`'s against
`2d1424a` (reopened and installed renders identical, rms 0).

CI on `610df88`, `8c70eee`, `cb8d3b6`, `2b967b7`, `a8ad4e7`, `2c32a8c`, `5f78f85` and `d92947b`:
`oscairn-tests` all 5 jobs passed on each; `dexed-pipeline` Linux Build and Windows Build passed on
each, and its macOS Build was cancelled about a second after it started on each (not looked into).

### Nineteenth session (2026-11-06): voice budget

Oscairn read at `2f05937` (`oscairn/oscairn` main, "Add oscairn.voice-budget/draft-1"; the local
clone could not pull over SSH and was already there): `voice-budget.h`, the SDK's
`voice-budget-plugin.hpp`, the porting guide's voice budget section and RFC 0018 step 4. Both headers
were vendored unchanged; the other three had not changed since `b9e90c7`. Octet's engine has its own
pool, not the SDK's sound-contexts Engine, so it uses the SDK's `voices::` helpers (section 4.5,
decision 85, the register's voice stealing entry). Octet 0.3.0.

Commits: `cd0628b` (headers), `02034a8` (the extension, the engine hook and the test), then the
version and this entry.

**Test** `oscairn-fm-test voice-budget` (CTest `oscairn-fm-voice-budget`), in blocks of 64 frames at
48 kHz: six held notes, the limit lowered to four: from that block's start the oldest two fade out (3
ms), are still counted and listed (more quietly) while they fade, get `NOTE_END` only in the block in
which the fade ends, and are gone after it but for the output DC filter's smoothly decaying offset
(compared with an instance playing only the four kept notes; largest step during the fade 0.0016
against the notes' own 0.0012 and their level 0.048); then the order: a released note before a held
one that is quieter and older, an Extended note holding release level 20 (loudness about 0.001 against
1.05 held) before a held one, the quietest held note and then the oldest, the new note's own sound
before another's older note, the lower priority, a sound's reserve (kept for another sound's note,
given up for its own), a sound no channel selects (no entry, priority 0) before the selected one, a
mono sound's held key kept, and a full pool: the quietest held voice with a budget, Dexed's oldest
without. Each part was checked to fail with its change undone: without the block-start check (the
oldest do not fade), with a cut instead of the fade (the voices stop being counted at once; with that
check bypassed, the step check fails at 0.032; the older `jump < level` threshold of the choke test
would not have caught it, so the test adds a step limit of the notes' own steps plus level / 64),
with `NOTE_END` sent at the give-up, with every voice taken as held, without loudness, with every
voice in context 0, without the mono flag, without the selected flag, without room made for a new
note (the fade then starts a block late), with Dexed's choice for a full pool under a budget, with the
order for a full pool without one, with fading voices left out of `active_voices` or `notes`, and
without the `get_extension` line.

**Tests run.** On `cd0628b` and `02034a8`, each on its committed tree (the headers commit in a worktree;
Oscairn's host tests against an Oscairn build of `2f05937` outside its repo): Release CTest 39/39 and
40/40 (the 164 Classic cases bit-identical through engine, JUCE plugin and native CLAP on arm64);
ASan/UBSan 22/22 and 23/23; TSan (`engine-two-rates`, `fm-threads`, as CI) 2/2 on each; under Rosetta
the x86_64 baseline (engine, plugin, CLAP: 164 of 164), the Extended test, and on `02034a8` the voice
budget test. The sanitizer builds were checked to link the ASan and TSan runtimes and to carry their
instrumentation in Octet's objects. `oscairn-plugin-check … org.oscairn.fm synth --param 1017=70`
PASSED in-process against Oscairn `2f05937` (whose host checks every entry of the extension when it
loads a plugin). Oscairn's worker protocol does not carry the extension yet, so in a worker Octet is
budgeted as a plugin without it; nothing here tests that path with the extension. The version commit's
gates and CI are in the session report.

### Twentieth session (2026-11-06): parts sharing one instance

Oscairn read at `b22173f` (`oscairn/oscairn` main; the local clone was there): RFC 0021's "Parts sharing
one engine", the porting guide's sharing, voice budget and ring-out sections, `ui.h`, the Basic Synth's
Output bus (`7a6da47`, `2e8cb59`, `173fc35`) and `47f33f1` (the SDK's audio thread kept off a context's
main-thread fields; Octet's budget reads only the audio thread's `loadedId`, so nothing to carry over).
Section 4.5 and decisions 86-88. Octet 0.4.0.

Commits: `43619f7` (headers at `b22173f`: `ui.h` added, a comment in `voice-budget.h`), `94dcc73` (MPE
scoped to channel 1's sound), `c7adf33` (output buses, the Output Bus and `oscairn.ui`), `39afbfd` (a
voice budget test with an entry per part), `1f574e9` (formats 2), then the version and this entry.

**Tests** (each new one shown failing with its change undone, unless said): `contexts` (two parts, one
note each, a bend on channel 1 and then on channel 2 against each part alone; fails with either half
of the MPE change undone). `oscairn-fm-test buses` (CTest `oscairn-fm-buses`): nine stereo ports, the
Output Bus's range and every declared role (fails without the kept roles); two parts on one instance,
sound A on channel 1 and Main, sound B on channel 2 and bus 1, against an instance each, sample for
sample over 60 blocks (one of 37 frames) with a bend and a level edit on channel 2 only, the other
buses silent (fails with every lane on Main); a change of bus while a note is held: the main output
stays the note alone sample for sample, the next note plays on bus 1 within 1e-5 of what it adds on
one output (fails when the whole sound moves); a change back, and a third bus (the older lane fades,
largest step 0.0051 against the note's own 0.0135, then a new note plays on bus 2); a retiring sound on
bus 1 plays on there, the ring-out buffer silent (fails when it rings out); the bus in a sound's state,
in the snapshot and in patch text (fails with 134 not saved); formats 1 refused (fails with a reader
that takes format 1). `voice-budget`: two sounds on two channels with an entry each (reserve 2 and
priority 3, reserve 1 and priority 7) at a limit of 4 give up one voice each; this passes on the plugin
before this work too (no change was needed), and with every voice given to one context it fails, but at
an earlier check of the same test. `no-alloc` changes buses while sounds play.

**Oscairn's own tests.** Neither `shared-engine` nor `control-shared-parts` loads Octet as they are: both
set the Basic Synth's parameter IDs. Copies with Octet's IDs, built against an Oscairn `b22173f` build
outside its repository (not committed anywhere), pass on `c7adf33`'s plugin: two parts on one instance
against two instances, largest difference 0, with the meters, notes heard, channels taken and an entry
per part; three parts through the Session against three instances, largest difference 0, the save and
reopen, the spill past eight sounds, undo and redo, and changes into and out of the shared layout. On
`a4e1f06`'s plugin both stop at the start (no output bus parameter). `oscairn-plugin-check ...
org.oscairn.fm synth --param 1017=70` PASSED in-process and with `OSCAIRN_PLUGIN_WORKER` on `c7adf33`.

**Tests run.** On each committed tree (Oscairn's host tests against the `b22173f` build): Release CTest
40/40 on `94dcc73`, 41/41 on `c7adf33`, `39afbfd` and `1f574e9` (the 164 Classic cases bit-identical
through engine, JUCE plugin and native CLAP on arm64); ASan/UBSan 23/23, then 24/24; TSan
(`engine-two-rates`, `fm-threads`) 2/2 on each; under Rosetta the x86_64 baseline (engine, plugin,
CLAP: 164 of 164) and the Extended test on each. A first gate run of `1f574e9` failed the Extended
test (the init sounds in `assets/octet` were still format 1); they were moved to format 2 in the same
commit before it was pushed and every gate run again. The version commit's gates and CI are in the
session report.

**Draft 4 (later in the session).** Oscairn `0a5c14f` replaced `oscairn.sound-snapshots/draft-3` with
draft 4 and shares instances between parts on its own. Commits: `c4f8845` (the Default's channels
reported from creation; Oscairn shares only an instance whose sound every channel selects, so a second
Octet added to a Performance had taken an instance of its own), `cddb1b5` (draft 4, headers at
`0a5c14f`), then Octet 0.4.1 and this paragraph. Tests: `contexts` checks a new instance's Default
channels before any process (fails without the change); `buses` checks `sound_values` (a prepared
sound's values, none while selected, an edit made while selected) and `save_sound` (loaded into a new
instance it is the only sound, on every channel, and plays on Main as the same sound prepared on its
own, sample for sample; fails with the bus not reset, with values read from the engine before the
sound is loaded, and with values for a selected sound). Oscairn's own `control-shared-parts` at
`0a5c14f`, copied with Octet's parameter IDs (outside both repositories) and built from an export:
three Octet parts saved with an instance each open on one instance and sound as the three instances
did (largest difference 0), saved as format 13 and reopened the same, nine parts spill to a second
instance as they are read and as they are added, adding joins, undo and redo, a sharing part saved as a
Sound, changes between the layouts and the protocol all pass, in-process and with every plugin in a
worker; on `e54e176`'s plugin (draft 3) it fails from the first check (three instances). Its
`shared-engine` copy passes too (largest difference 0). `oscairn-plugin-check` PASSED against
`0a5c14f`, in-process and in a worker. Release CTest 41/41 on `c4f8845` and `cddb1b5`, and again on
`cddb1b5` with Oscairn's host tests from the `0a5c14f` build; ASan/UBSan 24/24, TSan 2/2 and the x86_64
Rosetta gates on both.

**The converter** (not changed): `perfconv/fmx.py` writes `oscairn-fm-state 1` and `octet-contexts 1`,
which Octet 0.4.0 refuses; it needs format 2 headers (the Output Bus can be left out, defaulting to
Main). Performances converted before this are refused by 0.4.0 until converted again. Writing shared
nodes would need Performance format 12's `sound` per node and the engine's snapshot in its first
node's folder, once Oscairn forms shared engines when opening a Performance.

### Twenty-first session (2026-11-06): reset as a fresh instance

Oscairn main `11ca03a` reuses a running instance at a Performance change: it calls CLAP `reset`
on the audio thread, then loads the incoming state, and the instance must then render as a fresh
instance with that state, sample for sample, with a note in the next block at full level
(`docs/plugin-porting.md`, "Expect a second life"). An audit found reset nearly complete; a
memory diff and render comparisons found two gaps, fixed in `916083c`. Decision 89. Octet 0.4.2.

- **MPE detection.** Two notes on one channel turn it off for good (`keydown`), and neither reset
  nor activation turned it on again: after a reset an MPE source played as a plain one.
  `resetControllers` (constructor, prepare, reset) now turns it on, as a new engine has it.
- **Ended notes.** Reset left the notes it ended reported as playing (`noteActive`) until the next
  render sent their `NOTE_END`, and for that render they counted as voices of their sound: the
  second lane of a sound whose bus had changed while they sounded rendered too, on the same bus as
  the first (reset gives both lanes the sound's bus), and with no state loaded after the reset the
  notes played in that block came out wrong on that bus (at first at twice a fresh instance's
  level). Reset now ends them without `NOTE_END` (decision 89).

**Memory diff** (scratch tool, not committed: the plugin's source compiled with access control off
and every allocation zero-filled, a played instance against a fresh one, field by field): an
instance that played (notes held and released on three channels, the sustain pedal, bend,
pressure, wheel, breath, foot, portamento, MPE detection off, a second prepared sound on channel
3, the Output Bus changed while notes sounded, twice on the Default and once on the second sound)
and was reset, against a fresh one,
both given the same state, another state first, or no load (against a fresh one given the state it
kept). After the two changes the engine and the plugin differ only in pointers (to each instance's
own members, voices and slots), FM cores' scratch buffers, the second sound's Output Bus (a
parameter, which reset keeps, with the lane bus it sets) and fields written before they are next
read: a stopped voice's note data (`Dx7Note::init` sets everything that differs at the next note;
its `initialised_` is false), channel, velocity, address, bus and sound (set at its next key-down;
its address is read only while its note is reported), the last voice of a sound (read only when that
voice still has a note of that sound; reset clears every voice's note), an idle second lane's bus
(set when it next takes voices) and its filter's settings and coefficients (copied from the sound
before each render; the coefficients recomputed at its first render after `PluginFx::init`). Before
the changes it also differed in every controller set's MPE flag.

**Tests.** `oscairn-fm-test-linked reset` (CTest `oscairn-fm-reset`): notes in the block after a
reset sound at full level (case 78, a voice loud from its start), and no `NOTE_END` comes for the
notes it ended; every 8th DX7 case, the four MPE ones and two with the Extended profile (LFO 2 and
the part LFO free-running, random pitch and pan, alternate pan, the filter), all with the output
filter on, render as a fresh instance on every port after a reset with the same state loaded
again, after another state, with no load (against a fresh instance given the state it kept), and
deactivated, loaded and activated (against one loaded before activation): 26 cases, four ways
each; three prepared sounds, one taken off its channel while it sounds and one never selected,
play as in an instance that never played; reset allocates nothing. Without the MPE change the MPE
cases differ all four ways (12 renders); without the `NOTE_END` change the `NOTE_END` check fails,
and without that check every case differs with no load (26 renders). The version commit's gates
and CI are in the session report.

### Twenty-second session (2026-11-07): the name Oscairn FM

Jacob renamed the plugin from Octet to Oscairn FM (section 9, "The name Oscairn FM"): display name,
preset provider name, the built-in cartridge folder, the snapshot header and the init sound files,
in `9f3505f`. No behaviour changes otherwise. Oscairn FM 0.4.3.

### Twenty-third session (2026-11-07): the built-in cartridges as an asset library

An Oscairn plugin package carries only the `.clap`, so Oscairn ships the 33 banks and
`SynprezFM-NOTICE` as the asset library `org.oscairn.fm-cartridges` 1.0.0 (`Cartridges/*.syx`,
`Cartridges/SynprezFM-NOTICE`, `LICENSE`). In `9f58348` the preset provider's init asks the
indexer for `oscairn.assets/draft-2` and, when `library_root` finds the library at 1.0.0 or later,
declares its `Cartridges` folder; otherwise (no extension, or not installed) the folder beside the
`.clap`, as before, for other hosts. The desktop Dexed's folder is unchanged. Presets load by the
path discovery gives, so nothing calls `library_root` at run time. `assets.h` re-vendored from
Oscairn `8a8fcc6`. Oscairn FM 0.4.4, which requires `org.oscairn.fm-cartridges` >= 1.0.0 for its
built-in cartridges under Oscairn.

### Twenty-fourth session (2026-11-07): one copy of the built-in cartridges

A live install of 0.4.4 into an empty Oscairn data folder listed 2,112 presets instead of 1,056: the
provider also declared the desktop Dexed's cartridge folder (`DigitalSuburban/Dexed/Cartridges`),
which a separately installed Dexed fills with the same 33 banks. Oscairn FM is a separate product, so
in `5a6755d` it no longer indexes that folder, under Oscairn or in other hosts; the desktop Dexed is
unchanged. In `687eb32`, under Oscairn (the indexer offers `oscairn.assets`) the provider declares only
the `Cartridges` folder `library_root` gives for `org.oscairn.fm-cartridges`, and nothing when the
library is not installed; without the extension, the folder beside the `.clap`, as before. There is no
Oscairn cartridge user folder for Oscairn FM: Oscairn gives users their own Sounds, so none is
declared. A cartridge the host passes by path still loads. Oscairn FM 0.4.5.

### Fifth session (2026-10-31): tests run

Code commits `204249a` (no 16-bit rounding of Extended voices), `c5fbc05` (part values and
macros), `95ec1b4` (signed pitch bend ranges), `ec2bb5e` (level velocity table), `eeb132d`
(10.3 Hz output highpass); measurement `d14c301` (`fm/compare_m7.py`). For each code commit's
tree, locally: Release CTest 36/36 (the 164 Classic cases bit-identical through engine, JUCE
plugin and native CLAP on arm64; the Extended test now also checks a quiet sine's harmonics
without the rounding, each part value and macro against the operator or filter values it stands
for, the velocity transform and its NOTE_END, detune, signed bend ranges, the level velocity
table and the output highpass), the x86_64 baseline under Rosetta (engine, plugin, CLAP: 164 of
164 identical), ASan/UBSan 19/19, TSan 19/19. For `c5fbc05` the sanitizer runs were on its tree before a
one-word comment fix in `ext_voice.h`; Release CTest ran after it. CI results are in the session
report.

The Octet renders used for comparisons are made with `forks/measurements/m4/octet_render.py`
(local, now mapping the part values and macros); the equivalence searches with `m7fit.py` and
the campaigns with `m7gen.py` (local, like the other campaign generators: they build probes).

### Sixth session (2026-10-31): tests run

Code commits `e360357` (the test tool's `render-seq`), `fff4592` (mono, key assign, portamento),
`ff55562` (fractional part velocities), `ed7c9a2` (random pitch and pan); measurement `abfcd13`,
`3139a5b` (`fm/compare_seq.py`, tolerances `seq`). For each code commit's tree, locally: Release
CTest 36/36 (the 164 Classic cases bit-identical through engine, JUCE plugin and native CLAP on
arm64; the Extended test now also checks mono's last-key priority and return, retrigger against
legato, portamento rate, fingered and full-time, rate 2's octave start, the time modes, two
note-offs for a key pressed twice, Single against Multi, fractional velocities, and the random
pitch and pan ranges), the x86_64 baseline under Rosetta (engine, plugin, CLAP: 164 of 164
identical), ASan/UBSan 19/19, TSan 19/19. `ed7c9a2`'s tree is the one tested before the work was
split into commits (the same files). CI results are in the session report.

### Seventh session (2026-10-31): tests run

Code commits `036a7c0` (the 2nd LFO per voice, its start phase), `c7bd7ae` (its pitch depth),
`99fb48b` (pitch depth conversion), `d7cf201` (its delay's fade in), `3624879` (its amplitude depth),
`c3d4e65` (the part's attack and sustain offsets), `7b4f3e9` (key-on delay in mono legato); measurement `113cf1f`, `815b67b`
(`fm/compare_lfo2.py`, tolerances `lfo2`). For each code commit's tree, locally: Release CTest
36/36 (the 164 Classic cases bit-identical through engine, JUCE plugin and native CLAP on arm64; the
Extended test now also checks that two notes with the 2nd LFO's key-on reset, with and without a
delay, play as the sum of each alone, the pitch depth at depths 30 and 99 and offsets 4 and 7 with
the square starting low, the amplitude depth at depths 50 and 85 and offset 3, the delay's fade in
as long as its hold at delays 40, 70 and 99, the pitch depth's conversion and round trip, the
sustain offset table and the attack offset's factor, and a legato note taking over the key-on
delay, both while the first note sounds and while it waits), the x86_64 baseline under Rosetta (engine,
plugin, CLAP: 164 of 164 identical) and the Extended test under Rosetta, ASan/UBSan 19/19, TSan
19/19. The new per-voice, pitch depth, start, delay and legato key-on delay tests were each checked to
fail with the change undone (the amplitude and AEG tests were not). Four commits had their messages corrected
after these runs and before they were pushed, two of them also comments (`c7bd7ae`, `c3d4e65`;
Release CTest ran again on `c3d4e65`). CI results are in the session report.

### Eighth session (2026-10-31): tests run

Code commits `c59a932` (modulator EG levels), `8188190` (Ratio Shift), `45d6419` (Texture), `e36240c`
(the retrigger fade). For each code commit's tree, locally: Release CTest 36/36 (the 164
Classic cases bit-identical through engine, JUCE plugin and native CLAP on arm64; the Extended test now
also checks that a modulator's EG level 84 plays as 85 and level 70 at EG 85 as level 56 at EG 99
within 0.1 dB while a carrier's EG levels 84 and 85 differ, the ratio macro's table and +44 at ratio
1 playing as ratio 3, the ratio macro leaving a carrier alone, the texture's waves on a carrier and a
modulator and its table, and the retrigger fade's level 2.7-6.5 ms in and the old note gone after
13.3 ms), the x86_64 baseline under Rosetta (engine, plugin, CLAP: 164 of 164 identical) and the
Extended test under Rosetta, ASan/UBSan 19/19, TSan 19/19. Each new test was checked to fail with its
change undone. The first gate run of the modulator EG commit failed to build the sanitizer trees (a
header edit had been lost while splitting the work into commits); it was restored and every gate run
again on the committed tree. CI results are in the session report.

### Ninth session (2026-11-01): tests run

Code commits `07ec388` (waves 1 and 2 half a cycle on), `202348f` (a formant wave's centre modulated
once), `4550932` (odd waves as E(2t + m) sin(t + m)), `4102745` (rising segments), `a217a5b` (carrier
EG levels in twos); measurement `64d41f3` (`fm/compare_mod.py` skips a case with nothing to check).
For each code commit, on its committed tree (the work tree held nothing else of the engine's), locally:
Release CTest 36/36 (the 164 Classic cases bit-identical through engine, JUCE plugin and native CLAP on
arm64; the Extended test now also checks three modulated waves against the reference's harmonics,
where waves 1 and 2 peak, a rising decay 2's level 0.1 and 0.2 s in, and the carrier EG level steps
and table), the x86_64 baseline under Rosetta (engine, plugin, CLAP: 164 of 164 identical) and the
Extended test under Rosetta, ASan/UBSan 19/19, TSan 19/19. Each new test was checked to fail with its
change undone (the modulated-wave rows with all three wave changes undone at once). Four commit
messages were corrected before they were pushed (same trees); one of those corrections first
went to the wrong, already pushed, commit locally, and the odd-wave commit was made again on top of
the pushed one before anything was pushed. CI results are in the session report.

### Tenth session (2026-11-01): tests run

Code commits `ebe7c42` (texture from the operator's own wave), `73d3b18` (the 2nd LFO's amplitude
bend), `d766d55` (fixed-frequency detune), `58a19f3` (a comment); measurement `92f1c44`, `75c6079`
(`fm/compare_bend.py`). For each code commit, on its committed tree (the work tree held nothing else
of the engine's), locally: Release CTest 36/36 (the 164 Classic cases bit-identical through engine,
JUCE plugin and native CLAP on arm64; the Extended test now also checks texture -25 and -50 on a carrier
with all harmonics 1 at spread 3, the amplitude swing at depth 99 offsets 3 and 4, and a fixed
operator's own and the part's detune at keys 48 and 72), the x86_64 baseline under Rosetta (engine,
plugin, CLAP: 164 of 164 identical) and the Extended test under Rosetta, ASan/UBSan 19/19, TSan 19/19.
Each new test was checked to fail with its change undone. CI results are in the session report.

### Eleventh session (2026-11-01): tests run

Code commits `f0198b4` (the CPU benchmark and its CI step), `bd83f35` (portamento in steps), `08ee53b`
(formant 2's sign); measurement `f3861d9` (`fm/make_waves.py`), `63558f7` (`fm/glide_steps.py`, its
self-test passes). For each code commit, on its committed tree (the work tree held nothing else of the
engine's), locally: Release CTest 36/36 (the 164 Classic cases bit-identical through engine, JUCE plugin
and native CLAP on arm64; the Extended test now also checks three glide steps at time 64 and the glide
rate between whole steps, and harmonic 15 of formant 2 at spread 5, formant 0), the x86_64 baseline
under Rosetta (engine, plugin, CLAP: 164 of 164 identical) and the Extended test under Rosetta,
ASan/UBSan 19/19, TSan 19/19. The step test and the formant test were each checked to fail with their
change undone (the glide rate check passes either way). CI results are in the session report.

### Twelfth session (2026-11-01): tests run

Code commits `21d8453` and `842ee57` (the engine's controllers set before the first prepare),
`174b8fd` (the free-running glide clock), `998251d` (the texture path); measurement `f105288`
(`fm/glide_grid.py`, its self-test passes). For each code commit, on its committed tree (checked out
alone, the plan's edits stashed), locally: Release CTest 36/36 (the 164 Classic cases bit-identical
through engine, JUCE plugin and native CLAP on arm64; the Extended test now also checks one glide step
at each predicted tick of two glides 0.8 s apart and the pitch held between ticks, and four texture
places), the x86_64 baseline under Rosetta (engine, plugin, CLAP: 164 of 164 identical) and the
Extended test under Rosetta, ASan/UBSan 19/19, TSan 19/19. The glide step test and the texture test
were each checked to fail with their change undone. The first gate runs of the glide clock commit
failed under UBSan (oscairn-fm-topology and oscairn-fm-extended: a bool loaded as 102 in
`SynthEngine::syncChannelControllers`, from controllers never set before the first prepare, exposed
when the engine's members moved). `21d8453` was meant to fix that and did not (Controllers has its own
constructor, so value-initialising it zeroes nothing); it was already pushed, and `842ee57` is the fix
that works. Every gate was run again on the three later trees. CI results are in the session report.

### Twenty-fifth session (2026-11-07): rich views

Oscairn main `4961c35` added rich views to `oscairn.ui` (draft 4): a parameter's hint names a view,
its field there and its index, and `algorithm()` gives each algorithm's routing; Desktop and Panel
draw envelopes, key scaling and FM operator views from the parameters and set them when dragged. Read
at `2885698` (`4961c35` and one later commit to a packaging script): `docs/plugin-porting.md` ("Rich
views: what each engine should declare"), `ui.h`, RFC 0019 and the host's reading of the hints.
Decisions 90-92. Oscairn FM 0.4.6.

Commits: `7a081b1` (headers at `4961c35`: `ui.h` at draft 4, the other five unchanged; Oscairn FM
answers draft 4 only, since a draft 3 host's hint is smaller than the one it fills), `1934c1e` (the
views and the routings), then the version and this entry.

| View | Parameters | Fields |
| --- | --- | --- |
| 1 Classic voice | 1, 2; per operator n (1-6) `n001`-`n013`, `n016`-`n021` | algorithm, feedback; rates 1-4 and levels 1-4, breakpoint, left and right depths and curves, velocity depth, operator level, fixed, ratio (Coarse), fine, detune |
| 2 Extended voice | 40, 41; per operator n (1-8) `n031`-`n034`, `n041`-`n049`, `n051`-`n057` | algorithm, feedback; Hold Time as time 1, Attack to Release times 2-5 and levels 2-5 (no level 0: it starts at the Release (Hold) Level); the same frequency, level and scaling fields (Low and High as left and right) |
| 3 Classic pitch EG | 20-27 | rates 1-4, levels 1-4 |
| 4 Extended pitch EG | 28-36 | Initial Level as level 0; Attack to Release times 1-4 and levels 1-4 |
| 5 Extended operators' pitch EGs | per operator `n037`-`n040` | Initial Level as level 0, Attack Time and Level as segment 1, Decay Time as time 2 |
| 6 Extended filter EG | 68-77 | Hold Level as level 0, Hold Time as time 1, Attack to Release times 2-5 and levels 2-5 |

Every parameter in a view has a hint (automatic widget, no priority, range or group), also those with
no role; the roles are decision 88's. The routings are `msfa/algorithms.h`'s graphs, which the
`topology` test checks against renders. Nothing changed on the audio thread.

**Tests** (each shown failing with its change undone). `buses` checks that Oscairn FM answers
`oscairn.ui/draft-4` and not draft 3 (fails with the draft 3 header), and the roles now regardless of
whether a parameter has a hint. `oscairn-fm-test views` (CTest `oscairn-fm-views`): every
parameter's view, field and index against a second statement of the table above, written from the
parameters' modules and names (fails on 0.4.5's plugin, with hints only for roles, with the Extended
pitch EG left out); the number of parameters in each view; the curves' value texts carry their sign;
a value a third of the way up each view parameter's range, set through `process`, is what
`get_value` reports; `algorithm()` for values 0-89 of both algorithm parameters (the 32 and 88
routings equal the engine's graphs, values outside them refused; fails with the Extended routings
off by one), DX7 algorithms 1 and 32 and the first Extended one spelled out, and no routing for other
parameters. The `get_value` check is not specific to this change (it passes wherever the views do).
`no-alloc` also edits every parameter of a view while notes play, in both profiles, and calls
`param_hint` and `algorithm()` under the allocation guard; on 0.4.5 it fails because `algorithm()`
is missing, and the processing part alone passes there too (nothing changed on the audio thread).

**Oscairn's own checks** (built from a `2885698` export, outside both repositories, against
`1934c1e`'s plugin): `oscairn-worker-host-test` with Oscairn FM: the parameters with their hints,
views, fields, indexes and routings are the same in the engine and in a worker, and it renders
identically. A copy-style check (not committed anywhere) loads the plugin through Oscairn's plugin
host in the engine and in a worker: six views of 116, 162, 8, 9, 32 and 10 parameters, every
routing of both algorithm parameters, the 24 roles; on 0.4.5's plugin the views and routings fail.
Oscairn has no rich-view test that loads another plugin (the desktop's `desktop-gaps` uses its own
fixture plugin).

**Not sound differences:** the views are drawn by the host, so how they look is Oscairn's; where
they differ from the reference's screens is decision 91 (pitch levels drawn from the bottom; an
operator's pitch EG drawn holding until the note ends and then falling to the bottom, where it
decays at once back to the middle).

### Twenty-sixth session (2026-11-07): envelope times for the rich views

Oscairn main `b1fcb88`-`cf63cd6` added `oscairn.ui` draft 5: an optional `seconds()` at the end of
`oscairn_plugin_ui_t` gives how long an envelope stage takes at a value, so Desktop and Panel draw
stages by time; without it the host reads times from value texts ending in `ms` or `s`, and
otherwise draws by the parameters' ranges. Oscairn FM's rates are bare numbers, so it needs
`seconds()`. Read: `docs/plugin-porting.md` (the Oscairn FM draft 5 section) and `ui.h`. Decision 93.
Oscairn FM 0.4.7.

Commits: `7ae1a0a` (headers at `cf63cd6`: `ui.h` at draft 5, the other five unchanged; Oscairn FM
answers draft 5 only), `8bf6218` (the Classic rates: the guide's relation, MSFA's `Env` at 44.1 kHz
decaying across the whole level range, `3824 * 65536` per 64-sample block at the rate's increment;
rises are quicker and keyboard rate scaling is left out), `9eddf8a` (the Extended filter EG's
hold and segments, decision 93), then the version and this entry. Nothing changed on the audio
thread.

**Tests** (each shown failing with its change undone). `buses` checks that Oscairn FM answers
`oscairn.ui/draft-5` and not draft 3 (fails with the draft 4 header). `views` checks `seconds()`
for every Classic operator's four rates at rates 0, 1, 7, 20, 33, 50, 64, 77, 90, 98 and 99 against
`Env` itself (attack at 99 to level 99 at full output level, then decay 1 at the rate to level 0,
its blocks counted), within one 64-sample block (fails without `seconds()`, and with the rate's
41/64 scaling made 41/63); the filter EG's five times at time values 0, 21, 30, 51, 64, 100 and 127
against `ExtFilterEnv` (one stage at the value, the others at 0, the samples until the level
arrives), within two samples (fails without the change, and with the hold taken from the segment
table); and no time for a level, an Extended operator's time, a pitch EG's rate or time, or the
algorithm.

**Not sound differences:** the times only draw the envelopes. The Classic times are a full-range
decay at 44.1 kHz; a rise, a partial segment, keyboard rate scaling and the output level change
the engine's real time, as the guide accepts ("approximate is enough").

### Twenty-seventh session (2026-11-13): polyphony

Jacob's first runs on the FANTOM-07 (4x Cortex-A53 at 1.8 GHz, 48 kHz, 128-frame blocks, a worker
gets half a block, 1.33 ms) played EPIANO 9 cleanly to the 16-note limit: 110 us a block at 1 note,
220 at 8, 381 at 16, about 18 us a note, so about 55-60 such notes fit one part's allowance; worker
memory 24 MiB. And Oscairn's kit found parts sharing one instance sharing its 16 voices unfairly
(two layered parts at 16 keys: 0 and 16 voices).

- **Structure.** One array of voices in `SynthEngine`, each a `ProcessorVoice` (88 bytes) owning a
  `Dx7Note` (10,776 bytes, the Extended note's state included; record layout from clang), allocated
  in `prepare`. Every loop over voices and the Oscairn glue's voice budget (`keeping`, `victim`,
  `steal`, `active_voices`, `notes`) took the fixed 16; nothing else in the engine is sized by it.
  Mono keeps a pool voice per held key (decision 40) and key assign Single fades the old voice
  while the new one starts (decision 41), so both take pool voices as before.
- **Change.** The pool is `SynthEngine::setPolyphony` (1 to `MAX_VOICES` 128, Dexed's 16 unless
  set, applied at `prepare`); the desktop Dexed keeps 16. A new note looks at Dexed's first 16
  voices as Dexed does and goes past them only when all 16 sound, so with 16 or fewer sounding every
  patch plays as on Dexed's 16, a voice's phases without key sync included; with every voice
  sounding, Dexed's choice over the whole pool. Oscairn FM takes 128 (`OSCAIRN_FM_POLYPHONY`
  sets 1 to 128): memory, 1.39 MB of notes against 174 KB, about 1.2 MB more an instance; the
  voices are allocated at activation, so the host's voice budget, not the pool, is what limits a
  device. Without limits from the host, several sounds share the pool fairly (register).
- **CPU.** Instructions retired (macOS arm64, `/usr/bin/time -l`, 20,000 blocks of 128 frames at
  48 kHz, one single-carrier Classic voice): the 128 pool's loops add about 3,700 instructions a
  block at rest, against about 8,600 for one sounding voice. Wall-clock numbers on this Mac were
  not usable this session (load average over 100). On the A53, at about 18 us a note and the
  extra loops a few microseconds a block (an estimate from the instruction ratio, not measured
  there), one part fits about 55-60 EPIANO 9 notes and, at the earlier 5-6.5 times a Classic voice,
  about 10 Extended ones: past those the host's budget gives voices up first.
- **Tests.** `engine-pool-identity` (`dexed-render-engine --pool-identity`): the 164 cases on 128
  voices sample for sample as on 16 up to Dexed's first steal (136 cases steal, and differ after),
  and 40 voices without key sync playing a 17th note after 16 silent ones (14 of them silent by
  then) the same; a single choice over the whole pool fails it (43 differ).
  `oscairn-fm-polyphony`: 128 held notes sound, a 129th takes Dexed's choice, a host limit of 64
  holds below the pool, and 2 and 4 layered parts at 16 keys keep 16 each (fails on a 16 pool).
  `oscairn-fm-polyphony-16`: the same at 16 voices, the parts keeping 8 and 4 each (fails without
  the shares: the first part keeps 4). The Dexed baseline through the plugin, `two-rates`, `notes`
  and `voice-budget` run at `OSCAIRN_FM_POLYPHONY=16`, the pool their cases steal from.
- **What the extension can say.** `set_limits` gives an instance limit and, per context, a limit,
  a reserve, a priority and mono or legato; Oscairn FM honours all of them. Oscairn sends reserves
  only with a budget; without one the equal shares above stand in. Oscairn cannot learn the pool's
  size or that it is shared by an instance's sounds (`active_voices` only counts), so it cannot
  count shared sounds against it when it decides whether to share.

### How to run the tests

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEXED_COPY_PLUGIN_AFTER_BUILD=OFF \
      [-DDEXED_OSCAIRN_BUILD_DIR=<oscairn build>]
cmake --build build --target dexed-render-plugin dexed-render-engine Dexed_CLAP oscairn_fm \
      oscairn-fm-test oscairn-fm-test-linked
ctest --test-dir build
```

### Releasing

A plugin's version changes whenever its shipped binary does, so every release (including
the first workflow build after hand-built packages) bumps it.

The package version is Oscairn FM's descriptor version (`source/oscairn/OscairnFm.cpp`);
the bundle is the one plugin, so no second version exists. Bump the descriptor, commit,
then tag the package version and push the tag:

```
git tag oscairn-v0.4.9
git push origin oscairn-v0.4.9
```

The [release workflow](../.github/workflows/release.yml) builds the `.clap` for each
platform from a clean Release configure (the same `-DDEXED_NO_ALSA=ON -DDEXED_SKIP_VST3=ON`
as the test workflow, so it stays a single file with no non-system libraries), runs the
tests and the device dependency checks, and creates a GitHub Release holding
`oscairn-fm-<platform>.clap`, `licenses.tar.gz` and `oscairn-release.json`. The tag
version must match the descriptor; the release fails if it does not. To try the workflow
without a tag, run it manually (`workflow_dispatch`) with *dry run* checked: everything
runs and the files land as a workflow artifact instead of a release. The repo-specific
facts (names, ids, licence expression, licence files, the asset dependency) live in
`source/oscairn/tools/release/release.conf`.
