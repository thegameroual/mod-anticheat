# Testing and validation

Verified locally on 2026-11-16. Reference core:
`a4b1f8a040fa82782b34578bc37293f215d0e908` (official AzerothCore).

## Executed checks

| Check | Result and scope |
|---|---|
| Portable build | C++17 engine, C++20 test executable; GCC 15.2.0, Debug and Release, strict compiler warnings: passed |
| Portable tests | Five CTest suites, 21,181 assertions including 144 sequence assertions: passed |
| AddressSanitizer + UndefinedBehaviorSanitizer | Clang 21.1.8, all five suites: passed, no findings |
| ThreadSanitizer | GCC 15.2.0, all five suites: passed, no findings |
| SQL integration | V3 schema and aggregates, private MySQL 8.4.13 with networking disabled: passed |
| Static analysis | Cppcheck 2.19.0 warning/performance/portability on module sources and tests: passed |
| Upstream C++ style checks | The pinned core's `apps/codestyle/codestyle-cpp.py`, run against module `src/`: passed |
| Whitespace | `git diff --check`: passed |
| Standard core integration | Final unpatched-core module archive compiled successfully with the real pinned core headers and dependencies |
| Optional Warden patch | Current module bridge and modified `Warden.cpp`/`PlayerScript.cpp` core units compiled successfully against the pinned core |

Test framework assertions remain enabled in Release builds. The generated movement tests
cover 10,000 reproducible variations of legitimate speeds and packet intervals. The movement
suite contains 20,686 assertions and the repository suite 42. These counts include repeated
sequence checks, not independent integration scenarios. Compared with the previous 30,803
assertions, the policy tests now use explicit evaluation intervals instead of the obsolete
cooldown contract. The 10,000-iteration burst remains, with consolidated assertions. The
lower assertion total is neither a coverage metric nor evidence of reduced behaviour testing.

`SequenceTests.cpp` runs the actual portable chain `EvaluateMovement -> ViolationScorer ->
AlertPolicy -> EnforcementManager` using a controlled clock and distributed defaults.
Only mode/route options are changed where required; the default score decay remains enabled.
The cases cover legitimate movement, isolated and persistent anomalies, silence, unevaluated
detectors, packet-frequency changes, jitter, duplicate timestamps, correlated evidence,
diagnostics, transitions, reconnects and degraded-action guards. See the
[recorded chronologies](INCIDENTS.md#deterministic-sequence-chronologies).

The TSan run exercises the portable suite, including concurrent immutable configuration
publication. It does **not** instrument or execute live AzerothCore map/session callbacks,
the actual manager registry, or the database worker pool. Source audit plus explicit locking
supports their execution model, but a passing portable TSan run does not establish that the
whole server is race-free.

## Commands and environment

Execution environment: WSL2 Ubuntu 26.04, six logical processors, approximately 6 GiB RAM,
CMake 4.2.3, Ninja 1.13.2. Commands below use MODULE and CORE as placeholders for absolute checkout paths.

```sh
cmake -S MODULE -B unit -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build unit --parallel 2
ctest --test-dir unit --output-on-failure
unit/anticheat_tests
unit/anticheat_tests sequences

cmake -S MODULE -B asan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DANTICHEAT_ASAN=ON -DANTICHEAT_UBSAN=ON -DCMAKE_CXX_COMPILER=clang++

cmake --build asan --parallel 2
ctest --test-dir asan --output-on-failure

cmake -S MODULE -B tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DANTICHEAT_TSAN=ON
cmake --build tsan --parallel 2
ctest --test-dir tsan --output-on-failure

python3 tests/SqlIntegration.py --workspace /tmp/anticheat-sql-validation
cppcheck --enable=warning,performance,portability --error-exitcode=1 --std=c++20 \
  --suppress=missingIncludeSystem --inline-suppr -I src src tests
python3 CORE/apps/codestyle/codestyle-cpp.py
```


MySQL checks created their own temporary directory and private Unix socket; no existing
server database was used. They exercised the actual `ReportSql.cpp` output, strict mode and
`NO_BACKSLASH_ESCAPES`, quotes/backslashes/UTF-8/malformed bytes, rollback, duplicate retries,
schema and migration reapplication, preservation of v2/legacy rows, UTC grouping, and exact
mean/count semantics including openings/Warden but excluding lifecycle, alerts and decisions.
The private server was stopped after testing. Real
worldserver connection loss and shutdown callback ordering were not executed by this test.

## Synthetic performance

Final GCC Release microbenchmark, 1,000 simulated players and 1,000,000 movement/scoring
observations, with 1% continuous speed anomalies:

```text
elapsed_ms=253.68
ns_per_event=253.68
reports=70
incidents=10
MovementState_bytes=240
ScoreState_bytes=2832
```

Ten incidents and 70 emitted snapshots from ten continuously anomalous players illustrate
stable identity with bounded updates. This benchmark excludes the adapter, registry/queue locks, aura and terrain
lookups, VMAP, SQL, network scheduling and allocator costs of accepted reports. It ran on a
shared development machine. It is not a whole-server benchmark or a player-capacity claim.
The earlier baseline run measured 98.19 ms and a 1,496-byte `ScoreState`. This revision adds
coverage tracking, incident lifecycle processing and recent qualified Speed windows; its
measured cost and state size are higher, not a performance improvement. The timing runs
were not a controlled comparison in the same measurement batch, so their ratio should not
be interpreted as a precise regression factor. The structure sizes above are not the
complete per-player memory footprint.

Cost bounds remain explicit: fixed-size results/evidence, no SQL work on movement
threads, per-player state locks, sampled geometry, no per-packet incident logging, one
in-flight database batch, and a policy sweep limited to once per second. Runtime counters
are available to measure actual load; their real deployment cost remains to be measured.

## Core integration build

Place the module in CORE/modules/mod-anticheat. Configure the pinned core with:

```sh
cmake -S CORE -B core-fast -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DSCRIPTS=none -DMODULES=static -DTOOLS_BUILD=none -DAPPS_BUILD=world-only \
  -DUSE_COREPCH=OFF -DUSE_SCRIPTPCH=OFF
cmake --build core-fast --target modules --parallel 2
```

The requested target is the module archive plus its core dependencies. This is a real
AzerothCore build, not a mock-header syntax test. It does not link or start a complete
`worldserver` executable. Warden's changed core translation units are checked separately.
Both the standard archive and the optional bridge compiled successfully. After applying
the patch to the validation checkout, these additional commands also succeeded:

```sh
git -C CORE apply --check MODULE/patches/warden-hook.patch
git -C CORE apply MODULE/patches/warden-hook.patch
cmake --build core-fast --target modules --parallel 2
ninja -C core-fast -j2 \
  src/server/game/CMakeFiles/game.dir/Warden/Warden.cpp.o \
  src/server/game/CMakeFiles/game.dir/Scripting/ScriptDefines/PlayerScript.cpp.o
```

CORE and MODULE stand for absolute checkout paths. Apply the patch only when optional Warden forwarding is wanted. Compilation checks the bridge signatures and changed translation units; it does not validate runtime client/Warden behavior or full-server linking.

The [CI workflow](../.github/workflows/validation.yml) runs portable sanitizer suites and pinned-core compilation. SQL and gameplay tests require separate execution.
No remote workflow was triggered or claimed successful for these local changes, and no
new server infrastructure was installed. The patch was removed from the isolated validation
checkout after compilation. No deployed core was changed.

## Required before production

1. Build/link the complete intended server with all deployed modules, then start it against
   a staging database and matching maps/VMAPs.
2. Keep mode 0 and capture real ordinary movement: all mounts, transports/elevators, vehicles,
   possession, flight removal, water transitions, Blink, Charge, knockbacks, roots/stuns,
   map/phase changes and reconnects. Include custom spells and scripted movement.
3. Replay controlled suspicious movement and real latency/jitter/burst conditions. Measure
   false-positive and false-negative behavior; calibrate confidence, windows and policy.
4. Exercise database outages, full queues, shutdown deadlines and restart. Verify monitoring
   sees failures and that action decisions remain inhibited as designed.
5. Load-test map workers and database throughput, measuring runtime counters and memory.
   If needed, instrument the full server with sanitizers; portable tests are insufficient.
6. Validate optional Warden with the actual client and core penalty configuration. Verify
   that only Warden performs Warden penalties and the module merely records them.
7. Move to mode 1 for GM review. Consider mode 2 only after the observed false positives are
   acceptable, keeping temporary bans and an operational review/rollback procedure.

Known remaining limitations include invisible vertical movement without flight flags,
ambiguous terrain/collision/fall-damage evidence, interrupted observation streams, custom
transport envelopes, very long knockbacks, non-aura server permissions sent before login,
and loss of memory-only reports on a process crash. These are documented limitations,
not successful validations or a claim of production readiness.
