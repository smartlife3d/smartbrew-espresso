# Brew Profiles

A brew profile is a JSON document that tells the controller what the pump should do during a
shot. It is a list of up to 10 phases. Each phase sets a target (pressure, flow, pump power or a
pause), shapes that target over time with a curve, and ends on a timeout or on exit triggers
(pressure, weight, flow or time). Two profile-wide limits sit above the phases: a target yield in
grams and a maximum shot time.

The profile engine runs inside the 100 Hz control loop (`src/control/profile_engine.cpp`). For the
control loop itself see [Architecture.md](Architecture.md); for how flow phases and the weight
cutoff work see [PumpFlow.md](PumpFlow.md); for the REST routes that store and select profiles see
[API.md](API.md).

## Example

This is the built-in `blooming_allonge` preset, exactly as it ships in
`ProfileEngine::defaultProfileJson()`:

```json
{
  "id": "blooming_allonge",
  "name": "Blooming Espresso (Light Roast)",
  "author": "SmartLife 3D",
  "targetWeightG": 40.0,
  "maxTotalTimeSec": 55.0,
  "phases": [
    { "name": "Fast Fill", "mode": "pressure", "startVal": 3.0, "endVal": 3.0,
      "curve": "linear", "maxPsmPct": 100,
      "transition": { "durationSec": 5.0, "logic": "OR", "triggers": [
        { "type": "pressure_ge", "value": 2.5 },
        { "type": "weight_ge", "value": 1.5 } ] } },
    { "name": "Zero-Power Bloom", "mode": "power", "startVal": 0.0, "endVal": 0.0,
      "curve": "instant", "maxPsmPct": 0,
      "transition": { "durationSec": 12.0, "logic": "OR", "triggers": [] } },
    { "name": "Ramp to Peak", "mode": "pressure", "startVal": 2.0, "endVal": 8.5,
      "curve": "ease_in_out", "maxPsmPct": 100,
      "transition": { "durationSec": 4.0, "logic": "OR", "triggers": [] } },
    { "name": "Declining Extraction", "mode": "pressure", "startVal": 8.5, "endVal": 5.5,
      "curve": "ease_out", "maxPsmPct": 100,
      "transition": { "durationSec": 30.0, "logic": "OR", "triggers": [
        { "type": "weight_ge", "value": 40.0 } ] } }
  ]
}
```

Files written by the boot-time seeding also carry `"seed_version": 1` (see
[Built-in profiles](#how-built-ins-are-seeded-and-updated)).

## Schema

### Profile fields

| Key | Type | Default | Notes |
|---|---|---|---|
| `id` | string | derived from `name` | Lower-case letters, digits and `_`, at most 32 characters after sanitising (see [Storage](#storage)). |
| `name` | string | `"Unnamed"` | Display name, at most 39 characters (`SB_PROFILE_NAME_LEN` 40 including the NUL). Alias: `label`. |
| `author` | string | `""` | At most 31 characters. |
| `targetWeightG` | number | `36.0` | Yield in grams that ends the shot through the predictive scale cutoff. Aliases: `target_weight`, `globalStopConditions.weight`. |
| `maxTotalTimeSec` | number | `60.0` | Shot time limit. Values below 1 are replaced by 60. Alias: `max_duration`. |
| `phases` | array | required | 1 to 10 phases (`SB_MAX_PHASES`). Extra phases are dropped. A profile with no phases is rejected (`"profile has no phases"`). Alias: `steps`. |
| `seed_version` | integer | absent | Only on files written from a built-in preset. The save endpoint removes it. |

### Phase fields

| Key | Type | Default | Notes |
|---|---|---|---|
| `name` | string | `"Phase"` | At most 27 characters (`SB_PHASE_NAME_LEN` 28 including the NUL). |
| `mode` | string | `"pressure"` | `pressure`, `flow`, `power` or `pause`. Alias: `type`. |
| `startVal` | number | `0` | Target at the start of the phase: bar, ml/s or duty %, depending on `mode`. Alias: `value`. |
| `endVal` | number | `startVal` | Target at the end of the phase. Alias: `value`. |
| `curve` | string | `"linear"` | `linear`, `ease_in`, `ease_out`, `ease_in_out`, `instant`, `constant` (alias `flat`). Unknown names fall back to `linear`. |
| `maxPsmPct` | integer | `100` | Pump duty ceiling for this phase, clamped to 0–100. Advanced. |
| `flowLimit` | number | `0` | Pressure phases: pump flow ceiling in ml/s (0 = none). Turned into a duty ceiling with the pump model at the current pressure, so a gentle fill behaves the same on any pump. Advanced. |
| `pressureLimit` | number | `0` | Flow phases: pressure ceiling in bar (0 = none). The pressure loop runs on the limit alongside and the lower duty wins. Advanced. |
| `transition.durationSec` | number | `5.0` | Phase length in seconds; also the timeout. Aliases: `duration`, `durationSec` on the phase itself. |
| `transition.logic` | string | `"OR"` | `OR` or `AND` (case-insensitive). |
| `transition.triggers` | array | `[]` | Up to 6 triggers (`SB_MAX_TRIGGERS`), each `{ "type": ..., "value": ... }`. Unknown types are skipped. |

When more than one duration key is present, the parser takes the first of `duration`,
`durationSec`, `transition.durationSec`, `stopConditions.time` (milliseconds) and `target.time`
(milliseconds).

## Phase modes

| Mode | Unit | What the pump does |
|---|---|---|
| `pressure` | bar | Closed-loop PID tracks the shaped target. Duty is capped at `maxPsmPct`. |
| `flow` | ml/s | Feed-forward from the pump model at the current pressure, plus a proportional trim of 12 % per ml/s of error against the measured flow. Capped at `maxPsmPct`. A target of 0.01 ml/s or less, or `maxPsmPct` 0, gives zero duty. Details in [PumpFlow.md](PumpFlow.md). |
| `power` | duty % | Open loop: the shaped target is the pump duty, capped at `maxPsmPct`. |
| `pause` | – | Zero duty. Pressure decays on its own. The target is reported as 0. |

The flow value used by `flow` phases and by `flow_ge` / `flow_le` triggers is the cup flow from
the scale (g/s divided by 1.03 g/ml) when a scale is connected and reports flow, and the filtered
pump-model flow otherwise.

## Curves

Each phase interpolates from `startVal` to `endVal`:

```
t      = elapsed phase time / durationSec      (clamped to 0..1; 1 when durationSec <= 0)
target = startVal + (endVal - startVal) * f(t)
```

| Curve | f(t) for 0 < t < 1 | f(1) |
|---|---|---|
| `linear` | `t` | 1 |
| `ease_in` | `t²` | 1 |
| `ease_out` | `1 − (1 − t)²` | 1 |
| `ease_in_out` | `t² · (3 − 2t)` (smoothstep) | 1 |
| `instant` | `0` | 1 |
| `constant` / `flat` | `0` | 0 |

`constant` always holds `startVal`. `instant` in the firmware also holds `startVal` and only
reaches `endVal` once the phase duration has elapsed, which is normally the moment the phase ends.
The designer preview draws `instant` at `endVal` for the whole phase. For a flat step, set
`startVal` and `endVal` to the same value.

## Triggers and phase exit

| Type | Fires when |
|---|---|
| `pressure_ge` | pressure ≥ value (bar) |
| `pressure_le` | pressure ≤ value (bar) |
| `weight_ge` | scale weight ≥ value (g) |
| `weight_le` | scale weight ≤ value (g) |
| `flow_ge` | flow ≥ value (ml/s) |
| `flow_le` | flow ≤ value (ml/s) |
| `time_sec` | phase time ≥ value (s) |

Rules, in the order `evaluateTransition()` applies them:

1. **No triggers.** The phase ends after `durationSec`, or after 4 s if `durationSec` is 0 or
   negative.
2. **Timeout.** If `durationSec` > 0 and has elapsed, the phase ends regardless of its triggers.
   With `durationSec` ≤ 0 a phase with triggers has no timeout and waits for them, bounded only
   by the profile limits below.
3. **OR** ends the phase when any trigger has fired. **AND** ends it when every trigger has fired.
   A trigger latches once it fires, so AND means "all have fired at some point during this
   phase", not "all are true at the same instant".
4. **`*_le` triggers** (`pressure_le`, `weight_le`, `flow_le`) have two guards:
   - a 0.25 s grace period after the phase starts, so a phase does not end at 0.00 s while the
     signal is still settling;
   - falling-edge arming: at phase entry the trigger is disarmed and only arms once the signal has
     been seen above the threshold. A `pressure_le 1.0` trigger in a phase that starts at 0.5 bar
     does nothing until pressure has risen above 1.0 bar and fallen back.

   The `*_ge` and `time_sec` triggers are armed from the start.
5. **Simulation mode** ignores physical triggers. Each phase runs its full `durationSec` (a phase
   with triggers and `durationSec` ≤ 0 runs 5 s), so the simulated graph matches the profile
   preview. `maxTotalTimeSec` and the target-weight stop still apply, the latter against the
   simulated weight.

The operator can skip to the next phase with `NEXT_PHASE` ([API.md](API.md)). When a phase ends,
the log line names the trigger, its measured value and the threshold.

## Profile-wide limits

| Limit | Source | Effect |
|---|---|---|
| Target yield | `targetWeightG` | The scale manager stops the shot early enough that the drip lands on target. Lag, lockout and learning are described in [PumpFlow.md](PumpFlow.md). |
| Shot time | `maxTotalTimeSec` | The engine ends the run; the controller stops with "profile complete". |
| Last phase done | – | The controller stops with "profile complete". |
| Absolute limit | 120 s, fixed in `brew_controller.cpp` | Stops the shot and sets `FAULT_MAX_TIME`. In simulation mode it stops without the fault. |

## Built-in profiles

| id | Name | Yield | Max time | Phases |
|---|---|---|---|---|
| `classic_9bar` | Classic 9-Bar | 36 g | 45 s | Pre-Infusion 3 bar, 4 s or `pressure_ge 3.0` → Ramp 3→9 bar `ease_in_out` 3 s → Peak Extraction 9 bar 20 s or `weight_ge 30` → Gentle Decline 9→6 bar `ease_out` 10 s or `weight_ge 36` |
| `turbo_shot` | Turbo Shot | 42 g | 25 s | Fast Ramp 2→6 bar `linear` 2.5 s → 6-Bar Flat 6 bar `constant` 18 s or `weight_ge 42` |
| `blooming_allonge` | Blooming Espresso (Light Roast) | 40 g | 55 s | Fast Fill 3 bar, 5 s or `pressure_ge 2.5` OR `weight_ge 1.5` → Zero-Power Bloom `power` 0 %, 12 s → Ramp to Peak 2→8.5 bar `ease_in_out` 4 s → Declining Extraction 8.5→5.5 bar `ease_out` 30 s or `weight_ge 40` |
| `gentle_lever` | Gentle Lever Profile | 36 g | 45 s | Low Soak 1.5 bar, 8 s or `weight_ge 1.0` → Spring Ramp 2→8.5 bar `ease_in_out` 3 s → Spring Decay 8.5→5 bar `ease_out` 25 s or `weight_ge 36` |
| `italian_dark` | Italian Dark Roast | 30 g | 35 s | Soft Pre-Wet 2 bar, 3 s → 9-Bar Flat Pull 9 bar `constant` 25 s or `weight_ge 30` |
| `londinium` | Londinium (Lever) | 40 g | 60 s | Fill 4 bar 10 s or `pressure_ge 4` → Soak `pause` 10 s or `pressure_le 0.7` → Rise 4→9 bar 1.5 s → Hold 9 bar 4 s → Lever Decline 9→3 bar 20 s or `weight_ge 40`; flow limit 3 ml/s from the rise on |
| `iuiuiu_classic` | IUIUIU Classic | 36 g | 75 s | Flow Pre-Infusion 3 ml/s ≤ 4 bar, 20 s or `pressure_ge 4` OR `weight_ge 4` → Soak `pause` 30 s or `pressure_le 2` OR `weight_ge 4` → Ramp 2→7.5 bar 5 s → Transition 7.5→6 bar 4 s (≤ 3 ml/s) → Declining Flow 2.5→1 ml/s ≤ 6 bar, 15 s or `weight_ge 36` |
| `adaptive` | Adaptive (Flow) | 36 g | 75 s | Fill 3 bar ≤ 7 ml/s, 20 s or `pressure_ge 3` → Hold 3 bar 6 s → Rise 3→9 bar 6 s → Adaptive Flow 2→2.5 ml/s ≤ 9 bar, 30 s or `weight_ge 36` |
| `blooming_flow` | Blooming Espresso (Flow) | 36 g | 80 s | Flow Fill 4 ml/s ≤ 7 bar, 20 s or `pressure_ge 7` → Bloom `pause` 30 s or `pressure_le 0.6` OR `weight_ge 5` → Ramp 0.5→2 ml/s ≤ 9 bar 5 s → Steady Flow 2 ml/s ≤ 6 bar, 15 s or `weight_ge 36` |
| `slow_pi_flat` | Slow Pre-Infusion + 9 Bar | 36 g | 60 s | Low-Flow Pre-Infusion 1.5 ml/s ≤ 4 bar, 15 s or `weight_ge 2` → Ramp 4→9 bar 4 s → 9-Bar Extraction 30 s or `weight_ge 36` |

Every built-in runs its pump without a power cap (`maxPsmPct` 100) except a `power` phase, where
the value is the pump power itself. `londinium`, `iuiuiu_classic`, `adaptive` and `blooming_flow`
are adapted from the default profiles of [Gaggiuino](https://github.com/Zer0-bit/gaggiuino)
(`src/eeprom_data/default_profiles.h`), licensed CC BY-NC 4.0; see [THIRD_PARTY.md](../THIRD_PARTY.md).
Gaggiuino's "0 = continue from the current value" start points are given fixed values here, and
its temperature setpoints are not used (this controller does not drive the heater).

All built-ins use OR logic. The presets live in
`ProfileEngine::getPresetJson()`. `classic_9bar` is the default active profile; if the active
profile cannot be loaded, the engine falls back to `blooming_allonge`.

### How built-ins are seeded and updated

At every boot `seedFilesystem()` in `src/main.cpp` checks the ten built-in files. It writes
`/profiles/<id>.json` from the preset, tagged with `"seed_version": SB_SEED_PROFILE_VERSION`
(currently 2), when one of these is true:

| Condition | Action |
|---|---|
| The file does not exist. | Seeded. |
| The file has a `seed_version` lower than the current one. | Replaced with the current preset. |
| The file has no `seed_version` and is byte-identical to the preset. | Tagged, so later updates reach it. |

Any other file is left alone. A file without `seed_version` that differs from the preset belongs
to the operator, and the save endpoint always strips `seed_version`. An edited built-in is
therefore never overwritten, even when the preset changes in a firmware update.

Consequences:

- Deleting a built-in does not remove it for good. It is re-created at the next boot, or sooner
  when it is loaded or opened in the designer. Deleting an edited built-in is the way to restore
  the factory version.
- **When you change a preset in `getPresetJson()`, bump `SB_SEED_PROFILE_VERSION` in
  `src/control/profile_engine.h`.** Without the bump, devices that already have the old seed keep
  it.

## Storage

| Path | Content |
|---|---|
| `/profiles/<id>.json` | One profile document per file, on LittleFS. |
| `/profiles/index.json` | `{"active":"<id>"}`: the profile loaded at boot. Created as `{"active":"classic_9bar"}`. |

Ids are sanitised by the web API (`sanitizeProfileId()`): the part after the last `/` is taken,
a `.json` suffix is removed, letters are lower-cased, runs of spaces, `-` and `_` become one `_`,
other characters are dropped, trailing `_` is trimmed, and the result is cut to 32 characters.
"My Turbo-Shot" becomes `my_turbo_shot`. If the document has no usable `id`, the name is used, and
then `custom`.

Saving a profile (`POST /api/v1/profiles`) writes the file atomically (temporary file, then
rename), makes it the active profile and queues it for loading. The request body may be up to
16 KB (larger bodies get HTTP 413); the parser works in an 8 KB JSON document, so a very verbose
file can still be rejected as malformed.

The control task owns the loaded profile. A profile selected while the machine is not idle is
not loaded mid-shot: the request is kept and applied when the machine returns to IDLE. The REST
reply reports this as `"deferred": true`.

## Designer

The cockpit has a visual profile designer under **Profile list → Edit** or **New Profile**. Phases
are edited by dragging the start and end handles on the curve (value) and the end handle or phase
boundary (duration), or with steppers on each phase card. The card badges cycle the mode
(`pressure`, `flow`, `power`, `pause`) and the curve (`linear`, `ease_in_out`, `ease_out`,
`constant`, `instant`). `ease_in` is accepted in JSON but is not offered in the designer. Saving
posts the native JSON to `POST /api/v1/profiles`. The full UI is described in
[Dashboard.md](Dashboard.md).

## Import and export

The profile list accepts a `.json` file by upload or drag-and-drop. The browser detects the
format (`detectProfileFormat()` in `data/index.html`), converts it to native JSON and saves it
through `POST /api/v1/profiles`.

| Format | Detected by | Conversion |
|---|---|---|
| Interchange JSON | `type: "pro"`, a `label`, or phases with `pump`, `valve`, `targets` or `phase` keys | `pump.target` `"flow"` → `flow` mode, else `pressure`; the value comes from `pump.flow` or `pump.pressure`. A phase with value 0 and a duration becomes `pause`. On phases after the first, `transition.type: "linear"` or `ramp: "smooth"` ramps from the previous phase's value (`linear` / `ease_in_out`). `targets` entries become triggers: `pressure` and `flow` with `operator` `gte`/`lte`, `volumetric` or `weight` → `weight_ge`. The last weight target becomes `targetWeightG`. `max_duration` → `maxTotalTimeSec`. |
| Curve JSON | `globalStopConditions`, or phases with `target.start`, `target.curve`, `target.time` or `stopConditions` | `type` → mode; `target.start` / `target.end` / `target.curve` → `startVal` / `endVal` / curve (only `linear`, `ease_in`, `ease_out`, `ease_in_out`, `instant`; others become `linear`); `target.time` or `stopConditions.time` (ms) → duration; `stopConditions.weight` / `.pressure` → `weight_ge` / `pressure_ge`; `globalStopConditions.weight` → `targetWeightG`. |
| Native | anything else | Normalised: missing values get defaults and the id is cleaned. |

Phases converted from interchange or curve JSON get `maxPsmPct` 100, OR logic and at most 4
triggers. Fields with no native
equivalent (temperature, valve) are dropped. The firmware parser understands the same foreign keys
on its own, so a file posted straight to the API without the browser is also accepted.

**Export.** **Download Active** saves the active profile as interchange JSON
(`exportProfileToInterchange()`); **Download All** packs every profile, each as interchange
JSON, into `smart-brew-profiles.zip`. The export is lossy:

- each phase becomes one `pump` setpoint taken from `endVal`, with a `linear` transition when
  `startVal` ≠ `endVal` (`instant` for the `instant` curve); other curve shapes are lost;
- `power` and `pause` phases are exported as `pressure` phases;
- `pressure_ge`, `pressure_le`, `flow_ge` and `weight_ge` become `targets`; `flow_le`,
  `weight_le` and `time_sec` are dropped;
- the last phase gets a `volumetric` target for `targetWeightG` if it has none, and every phase
  gets a `pumped ≥ 100` target;
- durations are rounded to whole seconds, `maxPsmPct` and AND logic are lost, and a fixed
  temperature of 93.0 is written.

Keep the native file (`GET /api/v1/profiles/detail?id=<id>`) when you need an exact copy.

See also: [Operation.md](Operation.md) for running shots, [Testing.md](Testing.md) for simulation
and bench tests, and [../README.md](../README.md) for the project overview.
