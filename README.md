# mod-anticheat

Movement anomaly monitoring for AzerothCore WotLK 3.3.5a. The module separates
movement analysis, evidence scoring, enforcement and storage. **The default is
observation only. Production detection accuracy has not been established.**

The implementation targets AzerothCore commit
[`a4b1f8a040fa82782b34578bc37293f215d0e908`](https://github.com/azerothcore/azerothcore-wotlk/tree/a4b1f8a040fa82782b34578bc37293f215d0e908).
See [testing and validation](docs/VALIDATION.md) for build instructions, verified
results and the checks required before deployment.

## Capabilities and limits

The module observes speed budgets using server time, unauthorized flight and water-walk
flags, position discontinuities and displacement while rooted or stunned. Server movement
orders, teleport/map hooks, authoritative effects and bounded grace periods handle
legitimate transitions. Client timestamps never increase a speed allowance. Transport
movement is evaluated in a verified local coordinate frame; vehicles and taxis are exempt.

Climb, jump, gravity, clock disagreement and blocked line of sight are retained as
**diagnostic observations**. They cannot authorize automatic sanctions. The gravity flag
check does not establish that fall damage was suppressed. Line of sight is not a player
collision test. See [detector contracts and scenarios](docs/DETECTORS.md).

This is a heuristic movement monitor, not a guarantee against cheating. It does not prove
client integrity, prevent every low-rate movement exploit, or validate all custom spells,
transports, maps and other modules. Continuous anomalies are deliberately counted once;
reviewing persistent reports is part of operating the module.

## Architecture

```text
src/
  core/          immutable validated config, movement state/context, manager
  detectors/     independent movement algorithms, fixed-capacity results
  scoring/       incident deduplication, cooldown, bounded recent evidence
  enforcement/   pure policy decisions; no detector can kick or ban
  persistence/   bounded typed queue, retry-safe SQL and async repository
  integration/   AzerothCore hooks, config loading, optional Warden bridge
  anticheat_loader.cpp
tests/           portable C++ scenarios, SQL integration, microbenchmark
```

Movement hooks can run on different map workers. The registry holds shared ownership of
session entries and each entry protects its mutable state. Expensive map queries, database
operations and external actions run outside player-state locks. Configuration is replaced
as one immutable snapshot; invalid reloads retain the last valid revision. Player pointers
are not retained in queues or asynchronous callbacks. World updates process GM alerts and
sanctions after map workers finish. See [integration and thread model](docs/INTEGRATION.md).

## Installation

1. Use a private test AzerothCore checkout at the identified commit.
2. Place this repository at `modules/mod-anticheat`.
3. Build AzerothCore normally. Standard movement detection needs no core patch.
4. Install the characters schema below and inspect the updater logs. Back up an existing
   database before applying any migration.
5. Copy `conf/mod_anticheat.conf.dist` to the server's module configuration directory as
   `mod_anticheat.conf`; leave `Anticheat.Mode = 0` during validation.

```sh
# Inside an AzerothCore checkout:
git clone https://github.com/thegameroual/mod-anticheat.git modules/mod-anticheat
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DSCRIPTS=static -DMODULES=static
cmake --build build --parallel 2
```

For a fresh characters database, import:

```sql
SOURCE modules/mod-anticheat/data/sql/db-characters/base/mod_anticheat.sql;
```

For an existing installation, import:

```sql
SOURCE modules/mod-anticheat/data/sql/db-characters/updates/2026_11_15_00_anticheat_events_v2.sql;
```

Both scripts preserve existing evidence. The module writes `anticheat_events_v2`; the
`anticheat_daily_stats_v2` view groups evidence by UTC day. Legacy tables remain available
but are no longer written. Their old `average_score` cannot be repaired from counts alone.
The new mean is `AVG(score)` at accepted observations, not a time-weighted average.
See [storage, capacity, failure handling and migration](docs/PERSISTENCE.md).

## Configuration, scoring and enforcement

The [configuration template](conf/mod_anticheat.conf.dist) contains every supported key.
Modes are `0` observation/storage, `1` observation/storage plus GM alerts, and `2` automatic
enforcement. Legacy `MaxReportsForKick`, `MaxReportsForBan`, `BanDuration`, `GM.Announce`
and `AlertFrequency` keys no longer enable actions. Migrate them explicitly.

Raw anomaly observations, accepted incidents, live score and historical SQL records are
separate. A continuous anomaly is accepted once per detector; another incident requires a
clean interval and a detector cooldown. The clean interval must exceed two speed windows
plus transition grace. A global scoring cooldown limits bursts; one observation adds at
most 20 points by default. Accepted actionable evidence adds weight times confidence.
Score decays at one point per second by default, without reducing historical counts.
Diagnostic observations are stored without adding sanction score.
The live score decays independently of the recent-evidence window. Older residual
score cannot replace the requirement for enough fresh, independent episodes.

Mode 2 requires all of the following:

- A configured score threshold and at least five recent episodes by default.
- At least two independent evidence families: displacement (speed/teleport), movement
  permission (flight/water-walk), or control (root/stun).
- At least 30 seconds between the first and last supporting episodes, inside a 120-second window.
- Acceptable latency, no protected movement transition, a matching live session and adequate
  evidence queue capacity. An earlier movement-evidence enqueue failure inhibits that session's actions.
- No action already applied in the session. Multiple detections in one packet count as one episode.

Threshold zero disables its corresponding action. Ban duration must be positive and is
limited to 30 days; default is 24 hours. Permanent automatic bans are unsupported. Every
action decision records its score, episode count, families, span, session and configuration
revision. Warden retains sole ownership of Warden penalties.

Automatic enforcement remains a policy to validate, not a calibrated guarantee. Custom
movement abilities and network behavior can produce false positives. Database outage may
leave reports in memory; a process crash can lose uncommitted records. Overload and failed
shutdown flushes are logged explicitly. Alerting and operational monitoring are required.

## GM commands

| Command | Access | Effect |
|---|---|---|
| `.anticheat player [name]` | GM | Current session counts and score; defaults to selection |
| `.anticheat global` | GM | Mode, players, instrumentation and persistence status |
| `.anticheat reload` | Admin | Reload module config files and validate a complete snapshot |
| `.anticheat reset` | Admin | Reset live scores; retain stored evidence and action latches |

The destructive `.anticheat purge` command has been removed. Retention/deletion is an
explicit database administration operation. Valid reloads invalidate prior live evidence
before new analysis; SQL history is preserved.
The reload command rereads all module configuration files through ConfigMgr, but publishes
only this module's snapshot; it does not invoke other modules' reload hooks. Malformed values,
non-finite numbers and invalid ranges reject the candidate instead of silently using defaults.

## Optional Warden integration

The [patch](patches/warden-hook.patch) targets the pinned core and modifies four core files.
It introduces a narrow scripting hook and a feature macro detected by the module. No
module source edit or build flag is required. The module builds without the patch.

```sh
# In the pinned core checkout, before rebuilding:
git apply --check modules/mod-anticheat/patches/warden-hook.patch
git apply modules/mod-anticheat/patches/warden-hook.patch
# To revert, use git apply -R with the same patch.
```

The bridge records Warden reports after Warden handles its own penalty. These reports never
add movement sanction score. The hook is version-specific: recheck application, signatures,
ordering and compilation after every core update. Actual client/Warden operation still
requires testing with a running server.

## Tests and measurements

The portable C++17 engine has no external test-framework dependency. Core integration and
the configuration-concurrency tests use AzerothCore's C++20 toolchain. Build and run:

```sh
cmake -S . -B build-tests -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tests --parallel 2
ctest --test-dir build-tests --output-on-failure
# Separate sanitizer builds:
cmake -S . -B build-asan -DANTICHEAT_ASAN=ON -DANTICHEAT_UBSAN=ON
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure
cmake -S . -B build-tsan -DANTICHEAT_TSAN=ON
cmake --build build-tsan --parallel 2
ctest --test-dir build-tsan --output-on-failure
```

[SQL integration](tests/SqlIntegration.py) starts a private MySQL process with networking
disabled and its own temporary datadir. It does not use the running server's database.
It requires Python 3, a C++ compiler, `mysql`, `mysqladmin` and `mysqld`; run as a non-root user:
`python3 tests/SqlIntegration.py --workspace /tmp/anticheat-validation`.

Build `anticheat_benchmark` in Release mode for a synthetic movement/scoring measurement.
It excludes map queries, SQL, locks and actual packet scheduling. Runtime
`Anticheat.Instrumentation = 1` exposes counters and movement/scoring timing, including
state-lock acquisition; measure on the intended
server before drawing capacity conclusions. There is no measured whole-server speedup or
certified player capacity. See [validation and deployment checklist](docs/VALIDATION.md).

## License and contribution

GNU GPL version 2; see [LICENSE](LICENSE). Original AzerothCore notices are retained.
The implementation uses the C++ standard library and AzerothCore APIs; no third-party test
framework or proprietary code is bundled. Keep applicable notices when distributing source
or binaries, and distribute corresponding source as required by the license.

For contributions, describe the affected movement scenario and include a regression test
where possible. Run the portable suites and compile integration changes against the pinned
core; report any checks you could not run. Follow the existing C++ style, retain copyright
notices, and distinguish observed behaviour from assumptions. When contributing upstream,
follow AzerothCore's contribution rules, including disclosure of AI assistance.
