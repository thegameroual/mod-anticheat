# AzerothCore integration and execution model

The reference source is official AzerothCore commit
[`a4b1f8a040fa82782b34578bc37293f215d0e908`](https://github.com/azerothcore/azerothcore-wotlk/tree/a4b1f8a040fa82782b34578bc37293f215d0e908).
Compatibility is assessed against this exact checkout, not an unspecified `master`.
See [testing and validation](VALIDATION.md) for build commands and verified scope. Source inspection and a
successful build do not validate gameplay behaviour or custom cores.

## Hook ordering and threads

These statements follow the referenced source, not the names of its hooks:

* `Opcodes.cpp` marks movement as `PROCESS_THREADSAFE`.
  `WorldSession.cpp:MapSessionFilter::Process` permits those packets during map
  updates. `Map.cpp:Update` calls the session update with that filter. Different
  maps may therefore invoke movement and spell callbacks concurrently.
* `World::Update` runs world-session processing, then `MapMgr::Update`, then
  `ScriptMgr::OnWorldUpdate`. `MapMgr::Update` waits for its worker pool before
  returning. Player resolution, GM-session enumeration and sanctions are confined
  to the module's world hook, after that barrier.
* `WorldSession::ProcessMovementInfo` calls `OnPlayerMove` after core verification
  and before `HandleMoverRelocation`. At this point the incoming timestamp has not
  yet undergone `SynchronizeMovement`, and `Player::GetPosition()` still supplies
  the preceding server position. The adapter preserves both values separately.
* `HandleDoubleJump` and `CheckMovementInfo` returning false cause the core to
  **kick** the player. The module does not register those punitive validators.
  Observation mode therefore cannot accidentally kick through a validator.
* `AnticheatSetCanFlybyServer` receives a flag read from a client ACK in
  `MiscHandler.cpp`. `Player::CanFly()` also reads client-updated movement flags.
  Neither is used as permission to fly. `IsFreeFlying()` checks server auras.
* `AnticheatSetUnderACKmount` is also called from client speed ACK processing,
  before the speed discrepancy check. It is not an authoritative server-event
  hook, and is not registered. Mount transition allowances have explicit deadlines.
* `WorldSession::IsBot()` does not exist in this upstream checkout.
  Bot modules need their own explicitly reviewed adapter; no nonstandard method
  is required by this module.

The registry mutex protects only lookup, insertion, removal and snapshots of
`shared_ptr<Entry>`. A separate mutex protects each player's mutable state.
Registry and player mutexes are never held together. Detectors and scoring only
operate on detached values while holding the player mutex. Map queries, chat,
logging, database submission and penalties occur outside it. Repository enqueue
has its own short queue lock; no SQL executes under a player lock.

No `Player*`, map pointer or session pointer is retained after a callback. An opaque
numeric session-address token, GUID, account and monotonically increasing login
generation identify a session. Logout marks the entry inactive and removes it;
in-progress holders keep the entry allocated without keeping a player alive.
World-side actions resolve the player again and verify the session identity and
map/instance/phase. Reload publishes an immutable settings snapshot; the next
movement under a new revision resets movement evidence and score. A sanction
already applied during the session stays latched.
Incident keys additionally carry an evidence epoch, incremented when a GM reset or
configuration reload restarts incident counters. Closing snapshots retain the preceding
epoch and revision. The world sweep advances incident expiry/interruption without inventing
observations; alert and enforcement policies operate on detached incident state.

## Authoritative server movement orders

The standard `ServerScript::OnPacketSent` hook runs inside
`WorldSession::SendPacket`, before the socket check. It also covers client-less
sessions. The adapter first filters a small opcode list, then reads only a checked
packed mover GUID and order-counter prefix. It accesses no player from this hook.
The packet's mover, session token and account must match a registered entry.

Supported orders are flight enable/disable, water/land walking, feather/normal
falling, hover enable/disable, gravity disable/enable, knockback and forced
walk/run/swim/flight speed changes. Permissions have explicit inverse commands.
Their cached authority is cleared at teleport/map transitions because aura removals
while out of world may emit no inverse order. Active server auras are read again;
custom scripts granting movement without an aura must reissue their order after a
transfer. Their transition grace expires using a monotonic server clock. Knockback allowance
ends at landing or after five seconds. Server-authorized splines, charging and server jump
generators are independently observed from server state in the movement callback.

Speed packets end with a little-endian float; run speed has an extra byte between
counter and float. The parser checks exact length and finite nonnegative speed.
Only an actual change from the last server speed grants transition grace. A
client can induce corrective speed packets by sending an incorrect ACK; repeating
the same correction does **not** renew the allowance. A bounded history retains
independent deadlines for each movement mode. Speed and teleport checks integrate
the greater authorized old/new speed until each original deadline; accumulated
travel and observation windows remain intact. History overflow is counted and
inhibits automatic sanctions for that session. Flight, waterwalking and falling orders update their
own detector grace; repeatedly toggling one authorized effect cannot disable
unrelated detections. Automatic sanctions wait until these transition windows end.

The packet-send hook is reentrant with other engine calls. No call that can send a
packet is made while holding a player-state mutex. The global policy sweep runs
at most once per second; repository completion polling remains on each world tick.

## Transports

`GetTransport()` alone is not proof of legitimate boarding: the reference core can
assign it from a client-provided transport GUID. The module consequently grants no
blanket exemption for a transport flag or pointer.

For a moving transport, the adapter finds the transport in the player's map,
checks finite passenger offsets (horizontal radius at most 150 yards and absolute
Z offset at most 100 yards), transforms them using the server transport position,
and requires agreement with the packet's world position within five yards. The
preceding server position must also lie inside that local envelope. Verified
passengers are analyzed in transport-local coordinates, retaining speed limits.
The core consumes client world and passenger-local positions at different points.
Their small accepted discrepancy must never trigger a trusted correction or reset
speed/root/clock evidence. Automatic server-position rebasing is therefore disabled
while a transport is claimed; explicit teleport/forced-movement transitions still apply.
Terrain, climb, jump/gravity and geometry heuristics are omitted in this frame;
world terrain cannot describe a moving ship's deck. Records retain world positions.

Changing ships or entering/leaving a transport establishes a reference from the
server position, never the unverified incoming position. Invalid claims remain
subject to world-coordinate speed/teleport analysis and produce a diagnostic
signal. Vehicles and taxis remain server-owned exemptions.

The envelope is a conservative integration assumption, not a collision model.
Very large custom transports, elevators represented as dynamic game objects,
phase changes and transport rotations need in-game validation. The module cannot
prove that a claimed position lies on a particular deck from these hooks alone.

## Optional Warden forwarding

`patches/warden-hook.patch` changes four core files. It defines
`AC_MOD_ANTICHEAT_WARDEN_HOOK`, adds a PlayerScript notification and dispatches it
after `Warden::ApplyPenalty` has completed its own decision. The module automatically
compiles its subscriber only when this marker is present. Standard detectors
require no core patch.

From the reference core root:

```sh
git apply --check modules/mod-anticheat/patches/warden-hook.patch
git apply modules/mod-anticheat/patches/warden-hook.patch
# Reconfigure and rebuild the core and module together.
```

The patch has been applied and compiled against the reference commit. Forwarding
records Warden diagnostics, with a per-session cooldown. They do not enter the
movement score or enforcement policy; Warden owns its kick/ban. A cooldown can
coalesce several checks into one module record; the Warden log remains the full
source of Warden events. Disabling the module or Warden reporting disables this
forwarding, without changing Warden itself.

Do not assume the patch remains applicable after a core update. Recheck its four
contexts, hook signatures, penalty ordering and compiler result. Remove it with
`git apply -R` only against the corresponding unmodified patch context.

## Shutdown and limits

`worldserver/Main.cpp` invokes `OnShutdown` before closing database pools. The
RAII map teardown later invokes `OnAfterUnloadAllMaps`, also before `StopDB`.
Both hooks call the repository's idempotent bounded drain. Logout does not require
dereferencing a departed player to save data: records are already detached values.

Queue rejection is counted. Rejected movement/action evidence disables automatic
sanctions for the affected session; rejected Warden diagnostics are counted and logged
without affecting the independent movement policy. A known database batch failure also suppresses sanctions until a batch
succeeds. Accepted enqueue is not a promise of disk durability; a process crash
before database commit may lose queued evidence. A finite shutdown deadline is
reported as failure, never silently called a successful flush.

Remaining integration validation includes live login/spell/mount/map ordering,
client packet captures, very long knockbacks, reconnect races under load and
server orders sent before `OnPlayerLogin`. Auras cover normal pre-login flight and
waterwalking, but custom direct movement orders before registration may not be
observed. Independent core checks still apply their own movement penalties;
observation mode controls this module's decisions, not the entire core.
