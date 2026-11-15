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
transports, maps and other modules. Continuous anomalies retain one incident identity while
their evaluated duration, risk and reports evolve. See [incident persistence and decisions](docs/INCIDENTS.md).

## Architecture

```text
src/
  core/          immutable validated config, movement state/context, manager
  detectors/     independent movement algorithms, fixed-capacity results
  scoring/       incident lifecycle, evaluated coverage, bounded risk and evidence
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
SOURCE modules/mod-anticheat/data/sql/db-characters/updates/2026_11_15_01_anticheat_incidents_v3.sql;
```

Both scripts preserve existing evidence. The module writes `anticheat_events_v3`; the
`anticheat_daily_stats_v3` view groups incident openings and independent Warden reports by
UTC day. Lifecycle snapshots, alerts and decisions do not inflate incident counts or the
mean score. Legacy and v2 tables remain available but are no longer written. Their history
is not reconstructed into incidents. The new mean is `AVG(score)` at openings/Warden reports,
not a time-weighted average.
See [storage, capacity, failure handling and migration](docs/PERSISTENCE.md).

## Configuration, scoring and enforcement

The [configuration template](conf/mod_anticheat.conf.dist) contains every supported key.
Modes are `0` observation/storage, `1` observation/storage plus GM alerts, and `2` automatic
enforcement. Legacy `MaxReportsForKick`, `MaxReportsForBan`, `BanDuration`, `GM.Announce`
and `AlertFrequency` keys no longer enable actions. Migrate them explicitly.

Raw observations, incident identity, evaluated coverage, current risk and SQL history are
separate. Detectors explicitly distinguish normal, anomalous, not-evaluated and exempt
intervals. Missing packets or an unfinished Speed window cannot create normal movement or
proof of persistence. Three-second confirmation windows use newly evaluated time only.

A continuous incident can maintain risk through a bounded floor derived from qualified
duration and measured severity. Risk still decays at one point per second between updates.
Ten seconds of evaluated normal movement resolve an incident; an observation gap over six
seconds interrupts its qualification. Lifecycle changes and 15-second snapshots preserve
its evolution without counting every packet as a new incident. Diagnostics never add
movement-sanction risk. Heuristic confidence is not a calibrated probability of cheating.

GM alerts in modes 1 and 2 use observed incident persistence and severity independently of
global score: by default, three windows covering nine seconds with ratio at least 1.1, or
two windows with ratio at least 3. Boolean permission signals use duration and confidence
instead of a distance ratio. Same-incident reminders are limited to once per 60 seconds.
Alert eligibility does not turn a diagnostic into sanction evidence.

Mode 2 provides two decision routes:

- **Corroboration:** at least two independent families, each supported by five qualified
  windows and 30 observed seconds, plus the configured score threshold. Speed/Teleport
  share one family; Fly/Waterwalk share another; IgnoreControl is the third.
- **Persistent Speed:** disabled by default. When explicitly enabled, requires 20 qualified
  recent contiguous windows, 60 observed seconds and a minimum observed/budget ratio of 1.5
  across every fragment in that suffix. Earlier weaker windows do not veto later qualification.
  It permits kicks; temporary bans require a second explicit option.

Both routes require recent evidence, acceptable latency, no protected transition, a matching
live session and no earlier action. Degraded persistence or server authority inhibits actions.
Current score sums independent family risks, taking the maximum of correlated detectors
within each family. A high residual score cannot replace fresh qualifying observations.
The [incident model](docs/INCIDENTS.md) explains the parameters, effective speed budget,
lifecycle, safeguards and interpretation of synthetic traces.

Threshold zero disables its corresponding action. Ban duration must be positive and is
limited to 30 days; default is 24 hours. Permanent automatic bans are unsupported. Every
action decision records its score, evaluated windows, families, duration, session and configuration
revision. Warden retains sole ownership of Warden penalties.

Automatic enforcement remains a policy to validate, not a calibrated guarantee. Custom
movement abilities and network behavior can produce false positives. Database outage may
leave reports in memory; a process crash can lose uncommitted records. Overload and failed
shutdown flushes are logged explicitly. Alerting and operational monitoring are required.

## GM commands

| Command | Access | Effect |
|---|---|---|
| `.anticheat player [name]` | GM | Session risk, incident IDs/phases, coverage and measurements; defaults to selection |
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
