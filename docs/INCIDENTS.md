# Incident persistence and decision model

This document distinguishes code-level guarantees from behaviour that still requires
gameplay validation. Synthetic traces use a controlled server clock; they do not model
actual network delivery or establish detection accuracy on a running server.

## Baseline audit

The starting revision was `90c69eb`. The eight proposed findings were checked against
the implementation rather than inferred from the detector names.

| Finding | Classification at the starting revision | Evidence and consequence |
|---|---|---|
| A continuous anomaly is accepted once while score decays | Confirmed | `ViolationScorer::Observe` skips an active episode after `Decay`; only its first accepted result adds weight. |
| Persistent speed anomalies can lose meaningful current risk | Confirmed | Default Speed weight 5 and confidence 0.9 add 4.5 points. With decay 1 point/second, that isolated contribution disappears after 4.5 seconds even if subsequent windows remain abnormal. |
| Useful alerts require score 50 | Confirmed | The world update tests `score >= 50` before sending GM alerts. A persistent Speed incident alone cannot reach it. |
| Two evidence families are mandatory | Confirmed | Configuration rejects fewer than two families; enforcement independently uses a hard minimum of two. |
| Duration alone cannot make one family actionable | Confirmed | Enforcement uses accepted episode timestamps, score and family count; duration of an active incident is not represented. |
| Some observations are intentionally diagnostic | Confirmed safeguard | Scoring explicitly restricts actionable types. Geometry, clocks, jump/gravity and Warden do not become movement-sanction evidence through their configured weight. This restriction must remain. |
| Existing sanction tests do not prove default-configuration reachability | Confirmed, with scope | Several policy tests disable decay. They are useful unit tests for branching but cannot establish that distributed settings reach a decision in real detector sequences. |
| Missing results do not establish normal movement | Confirmed | An absent type starts `cleaning` regardless of whether its detector evaluated anything. Speed returns without a result between window closures. One absent observation followed by silence can satisfy the old wall-clock rearm condition. |

None of these eight findings had already been corrected at the starting revision.
Whether legitimate movement produces false positives, and how often, is not verifiable
from this environment. The audit confirms state-machine behaviour, not the prevalence of
cheating or network conditions on a live server.

## Evaluations and incident identity

Each detector now labels its evaluation as normal, anomalous, not evaluated, or exempt.
An evaluation includes a server-time interval and an explicit reliability flag. Speed
does not report a normal evaluation merely because a packet arrives before its window
closes. Sampled diagnostics likewise distinguish a skipped query from a normal result.

One incident retains its identifier while the behaviour continues or observation is
temporarily interrupted. Its lifecycle is open, persistent, interrupted, resolved or
expired. Normal and abnormal coverage, counters, latest observed/allowed measurements
and heuristic confidence are separate from raw signal counts and current risk.
The identifier is scoped by the session and configuration identity in persisted records.

Only newly evaluated, non-overlapping time can contribute coverage. By default, complete
three-second confirmation windows qualify persistence. Duplicate intervals, packets at
the same timestamp and intermediate non-evaluations cannot create additional duration.
An interval that is exempt or unreliable cannot support the sanction qualification streak.
Zero-duration observations can retain diagnostic details but add neither coverage nor
freshness and do not erase already qualified time. Positive intervals explicitly marked
unreliable interrupt qualification instead of being silently bridged.
An observation gap over six seconds interrupts that streak. Silence can interrupt or
expire an incident, but cannot confirm cheating or accumulate normal movement.

An evaluated normal interval clears the current abnormal qualification streak. Ten seconds
of evaluated normal coverage resolve the incident by default. An incident without another
anomalous observation expires after 120 seconds. These changes preserve its reports; they
do not rewrite its initial detection as normal. A later anomaly after resolution/expiry
gets a new identifier. Qualified duration is capped by the evidence window and counters
are bounded; the implementation keeps one current incident per detector and fixed-size
recent evidence rather than an unbounded packet history.

## Current risk

The initial contribution remains the configured detector weight multiplied by heuristic
confidence, subject to the initial-observation cap. On each completed qualifying window,
the detector's risk is raised, if necessary, to a bounded floor:

```text
weight * confidence + PersistencePerSecond * qualified_seconds * min(window_minimum_ratio, 2)
```

The ratio used for each completed window is the minimum across its evaluated fragments.
The defaults are two points per evaluated second and a maximum of 200 per incident.
This is a time-supported risk floor, not an additional increment for every packet or
every expired cooldown. Between evaluations, risk still decays at one point per second.
Normal movement, an interruption or an exemption prevents the preceding qualification
streak from continuing to raise the floor. An isolated observation therefore cannot
accumulate risk by waiting.

Speed and Teleport share the displacement family; Fly and Waterwalk share the permission
family; IgnoreControl supplies the control family. The current score is the sum of the
maximum detector risk in each independent family. Correlated Speed/Teleport results can
retain separate report details but do not sum into two independent risk contributions.
Diagnostics and Warden never contribute to movement-sanction risk, regardless of weight.

## Alerts and actions

Mode 0 stores observations without alerts or sanctions. Modes 1 and 2 can notify GMs
using the alert policy independently of the score thresholds for actions. The default
persistence alert requires three observed abnormal windows covering at least nine seconds and
a measured ratio of at least 1.1. Boolean permission signals have no meaningful distance
ratio, so they use observed duration and confidence instead. A large repeated measured anomaly can alert after two windows
when its ratio reaches 3. Freshness is limited to six seconds, and reminders for the same
incident are separated by at least 60 seconds. Messages identify the incident, detector,
covered duration, windows, measurements and reason; they do not declare a player guilty.
The default confidence minimum is 0.8. An eligible diagnostic can alert if that minimum is
explicitly lowered and reliable evaluated intervals support it; its risk and sanction
eligibility remain zero. A sampled point query alone cannot prove a duration of bad collision.

Automatic actions still require mode 2, adequate current risk, acceptable latency, fresh
evidence, a matching session, no protected transition, and healthy persistence/authority.
There are two distinct routes:

| Route | Default requirements beyond the shared safeguards |
|---|---|
| Corroborated families | At least two independent families, each supported by at least five qualified windows and 30 seconds of observed abnormal coverage; score 100 for a kick or 150 for a temporary ban. |
| Persistent measured Speed | Explicit opt-in; a recent contiguous suffix of at least 20 qualified windows and 60 seconds, with each window's minimum measured ratio at least 1.5. The default action for this route is a kick; a separate opt-in is required to permit temporary bans. |

The single-family route is restricted to Speed's integrated distance budget. It is not
enabled by reducing the multi-family minimum. A recent normal interval, unobserved gap,
transition or lower-severity interval can invalidate its required streak. The 64-window
Speed history retains the minimum severity of every contributing fragment. A weak window
breaks the strong suffix, but does not veto the incident forever: later sufficiently strong
contiguous windows can qualify. A weak partial window currently in progress also prevents
acting on an older strong suffix. A score alone
never substitutes for these requirements. Permanent automatic bans remain unsupported,
and the per-session action latch prevents repeated enforcement.

## Reporting frequency and configuration changes

Opening an incident creates one historical incident. Subsequent lifecycle changes and
periodic snapshots describe that same incident rather than increasing incident totals.
The snapshot interval defaults to 15 seconds; it is not a SQL write per packet. SQL records
include session/configuration identity so observations from different policies can be
distinguished. Alerts and decisions retain their explanatory measurements.

Reload validation rejects non-finite/out-of-range values and inconsistent timing requirements.
A valid policy reload starts fresh live evidence under its new revision; prior SQL history
is retained. Reset and reconnect also discard live qualification without removing history.
Existing enforcement and persistence-failure latches must not be bypassed by a GM score reset.
See [persistence](PERSISTENCE.md) for schema migration and failure semantics.

The configuration template is the authoritative list of supported options. The incident
model adds `Score.ConfirmationWindowMs`, `Score.MaxObservationGapMs`,
`Score.ReportIntervalMs`, `Score.PersistencePerSecond` and `Score.MaxIncidentRisk`.
Alert thresholds use the `Alert.*` keys, and the opt-in route uses
`Enforcement.SingleFamilyEnabled`, `SingleFamilyBan`, `SingleMinDurationMs`,
`SingleMinWindows` and `SingleMinSeverity`. `Enforcement.FreshnessMs` limits decision age.
The obsolete `Score.CooldownMs`, `Score.GlobalCooldownMs` and `GM.AlertFrequencyMs`
are replaced by evaluated-time confirmation, `Alert.RepeatMs` and the separate
`Warden.CooldownMs`. `Score.RearmMs` now means evaluated normal coverage, not elapsed time
since an absent detector result. Existing configuration should be migrated explicitly.

## Speed budget and measured severity

With constant authorized speed `v`, window duration `T`, and capped latency `L`, the
normal distance budget is `v * T * SpeedHack.Tolerance + v * L + 1 yard`.
The implementation integrates server-authorized speed changes across the interval and
uses the peak permitted speed for the capped latency allowance. Ground movement uses XY;
authorized swimming and flight use 3D. An early burst must exceed at least the full configured
window budget to trigger before the normal window closes, and a distance must also exceed
five yards. That early-trigger gate is distinct from the reported measurement budget: once
triggered, severity uses the actual evaluated interval's allowance, not three seconds of
unobserved time. Early fragments accumulate into non-overlapping confirmation windows.

At the distributed values, `v = 7 yd/s`, `T = 3 s`, tolerance `1.25` and zero latency
produce `27.25 yd`, or an effective average boundary of about `9.083 yd/s`. With 500 ms
latency allowance, the budget becomes `30.75 yd`, about `10.25 yd/s`. These are receipt-time
budgets rather than exact physical velocity measurements. The fixed margin and latency
allowance intentionally reduce sensitivity to small excesses. The multiplicative tolerance
remains 25%; no accuracy claim justifies reducing it to 5%.

Severity ratios describe observed distance divided by its effective budget. They are
explainable measurements, not probabilities that a player is cheating. Confidence values
are heuristic policy inputs, not statistically calibrated probabilities.

## Deterministic sequence chronologies

These rows come from the executed Release `anticheat_tests sequences` run on 2026-11-16.
The synthetic server clock starts at 1 second. Position advances use a supplied speed of
20 yd/s against an authorized 7 yd/s, with 100 ms input intervals and zero latency.
The latest Speed result is 28 yd against a 13.25 yd interval budget: after 1.4 seconds,
28 yd exceeds the 27.25 yd full-window early-trigger gate, while the measured interval
allowance is `7 * 1.4 * 1.25 + 1 = 13.25 yd`. This explains why the report does not show
the ordinary completed-window budget of 27.25 yd.

| Server time | Scenario/state | Latest observed / allowed (yd) | Confirmed coverage / windows | Current risk | Alert history | Decision |
|---|---|---|---|---|---|---|
| 1 s | Initial state | 0 / 0 | 0 s / 0 | 0 | None | None |
| 4 s | One Speed incident open | 28 / 13.25 | 0 s / 0 | 2.90 | None | None |
| 10 s | Same incident persistent | 28 / 13.25 | 6 s / 2 | 26.50 | None yet | None |
| 31 s | Same incident persistent | 28 / 13.25 | 27 s / 9 | 110.50 | First alert at server 10.8 s in modes 1/2 | None |
| 61 s | Same incident persistent | 28 / 13.25 | 57 s / 19 | 197.40 | Previously alerted | None yet |
| 91 s | Same incident persistent | 28 / 13.25 | 87 s / 29 | 198.20 | Reminder after the configured delay | Single-family kick eligible since server 61.2 s, only in mode 2 with opt-in |

The portable policy first alerts after 9.8 elapsed seconds and first permits the opt-in
Speed kick after 60.2 elapsed seconds. Mode 0 emits no alert; modes 0 and 1 never authorize
an action. Mode 2 without the single-family option also leaves this one-family trace
unsanctioned. The test continues evaluating eligibility after the first decision to expose
the chronology; a separate assertion sets the action latch and verifies no repeated action.
In the actual adapter the world sweep can add up to about one second of scheduling delay.

Other executed outcomes are:

| Sequence | Observed outcome |
|---|---|
| Normal movement for 180 seconds | Zero incidents, zero risk, no alert or action even with the single-family route enabled. |
| Three seconds of abnormal movement, then 30 normal seconds | Incident 1 remains recorded; at server 34 s it is resolved with risk zero and no alert/action. |
| Persistent Speed plus unauthorized flight | Two independent families; first action eligibility after 30.8 elapsed seconds with distributed decay and thresholds. At server 61 s the risk is 335.40 and the policy permits a temporary ban. |
| Same Speed motion sampled at 50/100/250 ms | One incident per trace; tests bound the small timing/risk differences caused by window closure rather than requiring identical floating-point samples. |
| Jitter versus batched intervals with zero-time duplicates | Both first permit the opt-in action after 60 elapsed seconds. At server 64 s, coverage is 63 versus 60 seconds and risk 200 versus 197; sampling boundaries explain the bounded difference. |
| Silence after anomalous movement | Coverage does not grow; qualification is interrupted, risk decays and no alert/action follows from the missing observations. |
| Marginal 9.2 yd/s motion | An incident is recorded, but severity stays below the default alert and single-family thresholds. |
| Repeated diagnostics or same-timestamp bursts | No movement sanction is created by volume or misconfigured diagnostic weights. |

Additional tests cover unreliable intervals, resolved-incident expiration, severity changes,
bounded recent strong windows, stationary rooted players, legitimate transports and
authorized transitions. A transport exemption does not suppress unrelated flight/waterwalk
observations. These are tests of supplied contexts and arithmetic, not live spell, vessel,
packet, client or network tests. No detector or corrective movement was added for the
additional ambiguous scenarios reviewed below.

The implementation changes are concentrated in the existing detector evaluation, scorer,
alert/enforcement policy, manager adapter, typed configuration and persistence files. The
new [v3 migration](../data/sql/db-characters/updates/2026_11_15_01_anticheat_incidents_v3.sql)
preserves earlier history; [sequence tests](../tests/SequenceTests.cpp) exercise the portable
chain. [Validation](VALIDATION.md) records executed checks and the increased measured cost
of tracking persistent evidence. Full-server runtime and gameplay accuracy remain unverified.

## Coverage review of additional scenarios

### Implementation file map

| Area | Changed files |
|---|---|
| Evaluation contract | `src/core/DetectionResult.h`, `DetectionContext.h`, `MovementState.h`; `src/detectors/MovementDetectors.cpp` |
| Incident lifecycle and risk | `src/scoring/ViolationScorer.h`, `ViolationScorer.cpp` |
| Alerts and action routes | `src/enforcement/EnforcementManager.h`, `EnforcementManager.cpp` |
| Core adapter and configuration | `src/core/AnticheatMgr.cpp`, `AnticheatConfig.h`, `AnticheatConfig.cpp`; `src/integration/ConfigLoader.cpp`; `conf/mod_anticheat.conf.dist` |
| Reporting | `src/persistence/ReportQueue.h`, `ReportQueue.cpp`, `ReportRepository.cpp`, `ReportSql.cpp`; base SQL and the new v3 migration |
| Verification | `CMakeLists.txt`; new `tests/SequenceTests.cpp`; movement, policy, configuration, repository, SQL fixture/integration, benchmark and test-runner files |
| Public documentation | `README.md`, `docs/DETECTORS.md`, `INTEGRATION.md`, `PERSISTENCE.md`, `VALIDATION.md` and this document |

The existing CI discovers the fifth CTest suite without a workflow change. No remote CI run
or repository setting change was performed for this local revision.

The [official anticheat module source](https://github.com/azerothcore/mod-anticheat/blob/41e983a4ae559c03512b1d65dbd814635cdc2d30/src/AnticheatMgr.cpp)
was inspected as a source of scenarios, not imported as a trusted reference implementation.
The core API references below target
`a4b1f8a040fa82782b34578bc37293f215d0e908`.

### Knockback without an apparent response

`Unit::KnockbackFrom` sends a mover GUID, order counter, horizontal direction and horizontal/
vertical speeds for a client-controlled mover; other movers use server spline motion.
`WorldSession::HandleMoveKnockBackAck` reads a movement snapshot, processes it through the
movement path and broadcasts the client-supplied jump values. The current adapter observes
the outgoing order only as a bounded exemption, not as a complete expected trajectory.
[Unit orders](https://github.com/azerothcore/azerothcore-wotlk/blob/a4b1f8a040fa82782b34578bc37293f215d0e908/src/server/game/Entities/Unit/Unit.cpp#L15073),
[ACK handling](https://github.com/azerothcore/azerothcore-wotlk/blob/a4b1f8a040fa82782b34578bc37293f215d0e908/src/server/game/Handlers/MovementHandler.cpp#L824).

An unchanged position does not establish deliberate refusal: the order can encounter
root/control state, collision, a later server order or delayed observations. A useful
diagnostic would first require bounded order/ACK correlation, recorded impulse and origin,
subsequent evaluated movement intervals and an explicit cancellation reason. None of these
may be inferred from silence. No new rejection detector or corrective motion is added.

### Vertical movement without flight flags

Incoming positions and sampled terrain heights are available. Server flight, fall-modifying
auras, splines and transport context provide some exemptions. A terrain height alone does
not establish that no supporting bridge, moving object or platform exists. The official
module's horizontal-at-constant-height heuristic includes numerous area exceptions, which
cannot be treated as a complete collision model.

Existing Climb and Gravity diagnostics cover some supplied contexts. A reliable new airborne
contract would need time-aligned support/collision observations, including dynamic objects,
and a record of authorized vertical impulses. The current detached snapshot lacks that
support provenance. A flagless vertical client remains an explicitly documented blind spot;
adding another name around the same ambiguous height check would not close it.

### Swimming and the environment

`Map::IsUnderWater` accepts phase, position and collision height. However, the core updates
`Player::IsInWater()` from incoming movement flags before calling `OnPlayerMove`; it is not
an independent authorization signal. The adapter therefore queries terrain for swim-speed
authority rather than trusting that flag. The core also explicitly accommodates jumps
underwater without a swimming flag.
[Map API](https://github.com/azerothcore/azerothcore-wotlk/blob/a4b1f8a040fa82782b34578bc37293f215d0e908/src/server/game/Maps/Map.h#L260),
[movement ordering](https://github.com/azerothcore/azerothcore-wotlk/blob/a4b1f8a040fa82782b34578bc37293f215d0e908/src/server/game/Handlers/MovementHandler.cpp#L651).

A future mismatch diagnostic needs matched-position liquid samples with known validity,
depth/surface margins and consecutive eligible intervals after entry/exit/jump transitions.
The present snapshot does not distinguish all those cases. One missing flag or a cached
water state is insufficient, so no broad swim anomaly is promoted to an incident proof.

### Leaving a battleground start area

The pinned core already calls `Battleground::_CheckSafePositions` during `STATUS_WAIT_JOIN`.
It compares each non-GM player's squared distance to their battleground-team start position
against `GetStartMaxDist()`, and can return them to that position. Despite its name, this
value is initialized from `MaxStartDistSq`, not a linear radius.
[Existing core check](https://github.com/azerothcore/azerothcore-wotlk/blob/a4b1f8a040fa82782b34578bc37293f215d0e908/src/server/game/Battlegrounds/Battleground.cpp#L336),
[squared-distance initialization](https://github.com/azerothcore/azerothcore-wotlk/blob/a4b1f8a040fa82782b34578bc37293f215d0e908/src/server/game/Battlegrounds/BattlegroundMgr.cpp#L455).

A bounded diagnostic is possible using the incoming position, instance, battleground team,
opening status and configured start envelope. Those metadata are not currently captured
in `DetectionContext`; the existing post-update battleground hook can run after the core's
correction, so observing that hook alone can miss the violation. There is no dedicated
safe-position-violation notification in the inspected interface. This gap is recorded
without duplicating the core's teleport or importing hard-coded map rectangles. Correct
team selection, zero/unconfigured envelopes and match-opening transitions must be covered
before such an adapter extension is enabled.
