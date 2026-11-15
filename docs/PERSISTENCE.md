# Persistence and scoring evidence

The repository stores detached values; it never keeps a `Player*` or
`WorldSession*`. Movement threads only enqueue accepted evidence into a bounded
queue protected by a short mutex. SQL formatting and database submission run in the
world update after map workers have joined. The database pool handles execution.
Observation records are formatted on the movement thread after releasing the player lock.

Only one transaction is in flight. Its records count against the queue limit,
and remain owned until `AsyncCommitTransaction().m_future` confirms success.
Failure keeps the same batch for retry after `retryMs` and marks persistence
unhealthy until a successful retry. Automatic actions must be suspended while
that health flag is false. A random process prefix
and monotonic sequence make each event key unique; retry uses the same key and
`ON DUPLICATE KEY UPDATE` does not insert a second report. A process crash can
still lose memory-only queued reports. This is not a durable message broker.

The module does not extend the core's global character prepared-statement enum.
All SQL is centralized in `ReportSql.cpp`: fixed table and column identifiers,
finite numeric values with the classic locale, and bounded text encoded as UTF-8
hex literals. SQL quotes, backslashes, null bytes, or `NO_BACKSLASH_ESCAPES` cannot
turn text into query syntax. Malformed UTF-8 is replaced before conversion.
There is no database connection acquisition or escaping on a movement thread.

Ordinary reports stop at `capacity - reservedActions`; action records may use
the reserve. In-flight records also occupy capacity. Rejected events increment
visible metrics; world update logs overload. The manager must decline automatic
sanctions for a session after movement evidence enqueue fails. Action enqueue failure
must also decline the action. No finite memory queue guarantees preservation of
every event under permanent overload or database outage. Operators must monitor
rejections and failed batches; increase capacity only after measuring the cause.
Rejected Warden diagnostics increment rejection metrics without changing movement policy.

The shutdown hook runs before `CharacterDatabase.Close()`. It waits for pending
transactions and drains the remaining records within a configured deadline.
After two failed shutdown transactions or a timeout, it logs the event keys and
evidence of every unconfirmed record. A timeout means database durability was
not established; an in-flight transaction may still finish successfully.

## Installation and migration

The base SQL uses `CREATE TABLE IF NOT EXISTS`. It never drops an existing table.
Legacy `anticheat_reports` and `anticheat_daily_stats` remain untouched; the module
no longer writes them. Their old `average_score` values contained counts, so
they cannot be converted into meaningful averages without original score data.

Existing installations apply
`data/sql/db-characters/updates/2026_11_15_00_anticheat_events_v2.sql`.
Both the migration and base schema create `anticheat_events_v2` and the
`anticheat_daily_stats_v2` view. Reapplying either is safe for existing rows.
The database account needs `CREATE VIEW` for installation and `INSERT`/`UPDATE`
for retry-safe runtime inserts. Schema installation is an administrator task.

`action=0` means accepted detector evidence. `action=1/2` means a kick/temporary
ban decision queued by the policy, not independent proof that the core completed
that operation. Decisions include their supporting score and explanation.
Warden penalties are evidence only and never cause a second module sanction.

Daily statistics group immutable evidence by character, account and UTC day.
`total_reports` counts accepted incidents, not packets. `average_score` is exactly
`AVG(score)` of those records. This is an event-sampled mean of the session score
after an observation, not a time-weighted average and not an average severity.
Action records are excluded so a sanction does not double-count evidence.
Reconnects start fresh live detection/score state while the database history
remains. No per-session REPLACE operation can overwrite another session's history.

The view computes on demand; it is never queried in the packet path. Retention
and exports are administrative operations. No default background deletion is
performed. Export evidence before an intentional retention cleanup.

## Verification boundary

Portable tests cover queue capacity (including in-flight batches), reserved
action capacity, retry retention, configuration shrinkage, numeric rejection,
UTF-8 boundaries, SQL injection-shaped text and locale-independent numeric
formatting. Database integration should also exercise schema reapplication,
duplicate keys, transaction rollback/retry, UTC-day aggregation, and outage at
shutdown against the actual MySQL version used by the server. See
[testing and validation](VALIDATION.md) for the verified scope.

On 2026-11-15, `tests/SqlIntegration.py` passed against a disposable MySQL 8.4.13
instance with no network listener. It compiles the actual SQL formatter, tests
base/migration reapplication with legacy data, rollback and idempotent retries,
quotes/backslashes/UTF-8/invalid bytes under `NO_BACKSLASH_ESCAPES`, numeric locale,
and UTC aggregates with exact means excluding action records. The private
daemon is stopped in `finally`; no existing database is used. Reproduce on Linux:

```sh
python3 tests/SqlIntegration.py --workspace /path/to/disposable-validation
```

This does not run the AzerothCore pool or its shutdown callback. Those require
a configured worldserver and remain part of server integration testing.
