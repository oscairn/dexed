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
(`source/oscairn/`, target `oscairn_fm`, display name "Octet", `org.oscairn.fm`), not the JUCE
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
| Plugin "Dexed", CLAP ID `com.digital-suburban.dexed`, features `instrument FM DX7` | Oscairn build: "Octet" (decided 2026-10-31; was proposed as "Oscairn FM"), CLAP ID `org.oscairn.fm` (matches `org.oscairn.basic-synth` etc.), features `instrument synthesizer` and the CLAP `fm` feature if defined. The desktop Dexed build keeps its own name and ID |
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
3. **Name and ID.** The CLAP ID stays `org.oscairn.fm`; the display name is "Octet" (it may
   change again).
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
- Whether 2nd LFO key-on reset and the Part LFO are per voice or shared in FM-X.
- Licences of the submodules other than JUCE, and the origin of `source/msfa/tuning.*`.
- The origin and licence of the banks in `assets/builtin_pgm.zip`.

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
  master tune, the engine keeps standard tuning.

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

**Decisions taken while implementing (for Jacob to confirm)**

1. Extended values are separate parameters (IDs above), not shared with Classic ones even where
   the range matches; Classic Transpose and the settings (100-199) apply to both profiles.
2. The operator Pitch Velocity Sensitivity had no audible effect in the reference (-7..+7 at
   depths -64 and +63); it is kept as a parameter and not applied.
3. The reference's +0.91 cent tuning is not copied into the engine (standard tuning in M4).
4. Back to Classic only when: the algorithm is the image of a DX7 algorithm with the other two
   operators silent; one key-on reset and one LFO pitch depth offset for all operators; no
   hold, operator pitch EG, pitch velocity, key follow or negative velocity; amplitude depth
   offsets 0, 2, 4 or 7; LFO phase 0 and the normal speed range. Rates, times, fine and detune
   convert to the nearest value (approximate both ways).
5. Unmeasured, using the Classic curves: level scaling, level velocity (mirrored for negative
   values; measured and replaced in the fifth session, see M7), time key follow, LFO depth offsets, LFO delay; the LFO start phase is taken as the
   start of the drawn wave.
6. The init sounds are named "Classic Init" and "Extended Init" (no product names).
7. Measured differences Octet keeps on purpose (second session): the operator waves are
   band-limited (the reference's are not, and alias audibly at the top of the keyboard), and a
   formant wave whose centre is above Nyquist is silent (the reference plays a non-harmonic
   tone near Nyquist there). Both are departures from the reference's behaviour; the gate's
   aliasing bound asks for the first, the second follows from it.
8. Operator wave names: Wave (Sine, All harmonics 1, All harmonics 2, Odd harmonics 1, Odd
   harmonics 2, Formant 1, Formant 2), Spread, Formant (parameters 1000 n + 60..62).
9. The wave tables take 7.7 MB, built once per process in SynthEngine's constructor (the
   desktop Dexed builds them too, since it shares the engine).
10. Extended carriers play the reference's output level per algorithm (measured, 0 to
    +13.55 dB above algorithm 1) and are 6.57 dB lower overall than a Classic carrier, the
    level of the reference's probes; the absolute level may also include the probes' Part and
    master volume, so Jacob may prefer another overall level (normalising to the loudest
    algorithm instead would make algorithm 1 7 dB quieter than the reference).
11. A modulator's level follows its own measured curve (0.75 dB per step, audible down to
    level 6), different from a carrier's; an operator is a modulator when its algorithm does
    not route it to the output (no algorithm does both).
12. Operator waves 1 and 2 carry a constant (they run from 0 to their peak) because the
    reference's modulators show it; as carriers the constant reaches the output only as a DC
    step that Dexed's output DC filter removes (the reference's own output highpass does the
    same). A carrier-only offset-free version would be indistinguishable after that filter.
13. The reference's output highpass (about 10.3 Hz, first order) is not copied; Dexed's DC
    filter (about 20 Hz) stays for Extended sounds too, 1 dB lower at 33 Hz (note 24).
    Reversed in the fifth session (decision 38).
14. CI: the macOS job of `oscairn-tests` now runs on macos-15 (GitHub's macos-14 brownout);
    `dexed-pipeline` is left as upstream has it.
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
18. Filter coefficients and the filter EG are updated once per 64-sample block, ramping the
    coefficients linearly across it (the FEG's fastest measured segments, 0-20, are instant
    in the reference and take one block in Octet).
19. FEG time velocity sensitivity is kept as a parameter and not applied (the reference made
    a segment instant at velocity 127 with both -64 and +63; unresolved); negative FEG time
    key follow mirrors the positive rule, though the reference measured shorter there too.
20. Octet's FEG segments run straight in cents; the reference's bend slightly (up to about 90
    cents along a 1200-cent segment).
21. A sound whose filter is not Thru at gain 255 does not convert back to Classic (Dexed's
    output filter is a plugin setting, not part of the sound).
22. Carrier feedback (found in M6): the feedback amount fitted in M4 now acts on a carrier's
    signal before the reference's output level, which `312febe` added later and which had
    halved the feedback (decision 10's level applies to the output only).
23. Common pitch EG (fourth session): parameters 28-39 under Extended/Pitch EG, depth shown as
    "8 octaves" ... "0.5 octave". A sound whose common pitch EG levels are not all 0 does not
    convert to Classic, and "convert to Extended" does not map the DX7 pitch EG onto it (it
    could, approximately; not done).
24. Depth velocity sensitivity scales the common pitch EG's rates as well as its levels, so
    segment times stay the same at every velocity (measured at one point: 1.35 % off).
25. Pitch key follow (56) stores the documented value 0-127 and shows the percent each value
    plays, unlike cutoff key follow's signed steps (decision 16), because its percent steps
    are uneven. Centre (57) is a MIDI note, C3 = 60 by default.
26. Random pitch and random pan draw from one xorshift generator per sound (two values per
    Extended note), reset by prepare, so renders are deterministic; both spreads are uniform
    approximations (random pan's left bias in the reference is not modelled).
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
32. Extended voices skip Dexed's 16-bit rounding of each voice (fifth session): each voice's
    output goes to the mix in floating point (full scale unchanged, clipped at +-1). Classic
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
    once; fractional part velocities are rounded.
36. Mod Depth cannot raise a modulator past level 99, in Octet as in the reference (`m7e`: a
    static pair at levels 60, 80 and 99 with the macro taking it to 99-147 plays exactly as
    level 99, 20 cases within 0.16 dB rms).
37. Extended sounds bend by the part's signed ranges, Pitch Bend Upper and Lower (253, 254,
    -48..+24 semitones, defaults +2 and -2, the documented values), not by the plugin's Pitch
    Bend Up/Down settings; the plugin's bend step setting, when on, still quantises as in
    Dexed. Implemented from the Data List only (the recorder cannot bend).
38. Extended sounds' output filter uses the reference's highpass (first order, 10.3 Hz) in place
    of Dexed's DC filter; Classic sounds keep Dexed's exactly (the contexts switch it with their
    profile).

### M5 (in progress, 2026-10-31; third session below)

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
  left bias is not modelled). The positions add up and clamp at -64/+63.
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

### M7 (in progress, fifth session, 2026-10-31: part values and four of the seven macros)

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
  0.4 dB rms of the equivalent values on a lone carrier). A positive attack offset reaches
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
  effect.

| Gate (`fm/compare_m7.py`) | Result |
| --- | --- |
| Pitch, 1 cent | **Met:** note shift 4 of 4, detune 49 of 49 (the table comes from `m7c`'s own renders for 37 of them; the 12 `m7a` cases at notes 36 and 72 are independent) |
| Filter offsets, level within 0.5 dB | **Met:** 18 of 18 |
| AEG offsets, harmonics within 1 dB | 44 of 62: decay met at every offset tested, release at all but -8 (1.0 dB); attack misses at +16 (3.1 dB on the carrier, 3.6 on the pair), +24 (2.0, pair) and +32/+63 (1.9-5.6 dB, both probes); sustain misses at -64, -32, -16, +16, +32 and +63 (1.2-2.3 dB: the reference moves the level by 26 values at -16 where 1.5 per step gives 24, and stops at 98 at +16) |
| Mod depth, harmonics within 1 dB | Without feedback (probes A, B): 12 of 12. Static EG at level 85 (C) and the pair with feedback 4 (D): 8 of 12 (+25 and +50 miss by 1.7-3.7 dB). Feedback 1, 2, 7 (`df`): 7 of 12 (+50 misses at feedback 1 and 2 by 1.9-2.6 dB; at feedback 7 the base case misses by 17.7 dB, so a modulator with feedback 7 is not reproduced, an M4 matter, and -50 and +50 miss too). Stack of three: 3 of 4 (+25: 8.8 dB). `m7a`: 8 of 18 (pair: -25 and +25 by 1.0-1.1 dB, +50 and +99 by 8.8 dB; static pair with feedback 4: +25 to +99 by 1.8-8.4 dB, unexplained: in a static pair without feedback the reference stops at level 99 as Octet does (decision 36), so the misses come from the envelope levels or the feedback, not from a level past 99; the wave probe's base case already misses by 3.2 dB, an M5 matter) |
| Envelope macros, harmonics within 1 dB | Pair and static probes: 44 of 48 (attack +25 1.0 dB on the pair, sustain +25, +50, +99 1.5 dB); wave probe 4 of 24 (its base case misses by 3.2 dB) |
| Velocity, level within 0.5 dB | **56 of 64** with the measured level velocity table below (29 of 64 with M4's, which used Dexed's curve: up to 14 dB off). Not met: velocities 2 and 3 at sensitivity +7 (0.8-1.5 dB: the table is straight between velocities 1 and 8 where the curve bends), fractional part velocities (0.5 plays 12 dB quieter in the reference than Octet's rounding to 1; 0.98 1.9 dB), and part velocity 36 (0.8 dB, between table points) |
| Ratio shift and texture | Not modelled (17 of 25 and 2 of 19 cases happen to meet) |

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

**Not done in M7 (recorder limit and time).** Mono and poly with legato, key assign,
portamento (switch, time, fingered and full-time modes, rate and time modes), and pitch bend
cannot be measured: the recorder plays one note per job and sends no controllers, so
nothing between two notes and no bend can be rendered. The controller-set and key-controller
masks only matter with Performance-level controller assignments (not part of the engine
renders). Pitch bend ranges are implemented from the Data List (decision 37) and checked only in
Octet's own test. Not measured either: how the macros treat multi-operator feedback loops (Octet
divides the depth on the loop's last operator only), the macros' rounding at odd values, the
AEG offsets' rounding, detune on fixed-frequency operators (Octet applies it to every
operator).

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

### How to run the tests

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDEXED_COPY_PLUGIN_AFTER_BUILD=OFF \
      [-DDEXED_OSCAIRN_BUILD_DIR=<oscairn build>]
cmake --build build --target dexed-render-plugin dexed-render-engine Dexed_CLAP oscairn_fm \
      oscairn-fm-test oscairn-fm-test-linked
ctest --test-dir build
```
