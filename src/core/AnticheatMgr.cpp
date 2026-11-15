/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "AnticheatMgr.h"
#include "AnticheatConfig.h"
#include "MovementState.h"
#include "detectors/MovementDetectors.h"
#include "enforcement/EnforcementManager.h"
#include "integration/ConfigLoader.h"
#include "integration/MovementIntent.h"
#include "persistence/ReportRepository.h"
#include "BanMgr.h"
#include "Chat.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "Transport.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace anticheat
{
namespace
{
std::uint64_t Now()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

Vec3 PositionOf(Position const& p) { return {p.GetPositionX(), p.GetPositionY(), p.GetPositionZ()}; }
std::uintptr_t SessionToken(WorldSession const* session) { return reinterpret_cast<std::uintptr_t>(session); }

bool Exempt(Player* player)
{
    return !player->IsInWorld() || player->IsGameMaster() || player->IsInFlight() ||
        player->GetVehicle();
}
}

struct AnticheatMgr::Impl
{
    struct Settings { AnticheatConfig config; std::uint64_t revision = 1; };
    struct Entry
    {
        std::mutex mutex;
        ObjectGuid guid;
        std::uint32_t account = 0;
        std::string name;
        std::uintptr_t session = 0;
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
        bool active = true;
        MovementState movement;
        ScoreState score;
        EnforcementState enforcement;
        bool persistenceDegraded = false;
        bool authorityDegraded = false;
        bool serverFlight = false;
        bool serverWaterwalk = false;
        bool serverSlowFall = false;
        bool serverHover = false;
        bool serverNoGravity = false;
        std::array<float, MAX_MOVE_TYPE> serverSpeeds{};
        SpeedTransitionHistory speedTransitions;
        ObjectGuid transportGuid;
        std::uint64_t knockbackUntil = 0;
        std::uint64_t lastAlert = 0;
        std::uint64_t lastWarden = 0;
        std::uint32_t latency = 0;
        Vec3 position;
        std::uint32_t mapId = 0;
        std::uint32_t instanceId = 0;
        std::uint32_t phaseMask = 0;
    };

    std::atomic<std::shared_ptr<Settings const>> settings{std::make_shared<Settings>()};
    std::mutex registryMutex;
    std::unordered_map<ObjectGuid, std::shared_ptr<Entry>> players;
    std::uint64_t nextGeneration = 0;
    ReportRepository repository;
    std::atomic<std::uint64_t> movements{0};
    std::atomic<std::uint64_t> detectorNanoseconds{0};
    std::atomic<std::uint64_t> groundQueries{0};
    std::atomic<std::uint64_t> geometryQueries{0};
    std::atomic<std::uint64_t> lostReports{0};
    std::atomic<std::uint64_t> authorityOverflows{0};
    std::uint64_t reportedLost = 0;
    std::uint64_t reportedAuthorityOverflows = 0;
    std::uint64_t lastLostReport = 0;
    std::uint64_t lastMetrics = 0;
    std::uint64_t lastSweep = 0;

    std::shared_ptr<Entry> Find(ObjectGuid guid)
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = players.find(guid);
        return it == players.end() ? nullptr : it->second;
    }

    std::vector<std::shared_ptr<Entry>> Snapshot()
    {
        std::vector<std::shared_ptr<Entry>> result;
        std::lock_guard<std::mutex> lock(registryMutex);
        result.reserve(players.size());
        for (auto const& pair : players)
            result.push_back(pair.second);
        return result;
    }

    static ReportRecord Record(Entry const& entry, DetectionResult const& result,
        Vec3 position, std::uint32_t mapId, float score)
    {
        ReportRecord record;
        record.guid = entry.guid.GetCounter();
        record.account = entry.account;
        record.name = entry.name;
        record.mapId = mapId;
        record.type = result.type;
        record.x = position.x;
        record.y = position.y;
        record.z = position.z;
        record.confidence = result.confidence;
        record.score = score;
        record.actionable = result.actionable;
        record.details = Acore::StringFormat("{}; observed={:.3f}; allowed={:.3f}; session={}",
            result.reason, result.observed, result.allowed, entry.generation);
        return record;
    }
};

AnticheatMgr::AnticheatMgr() : _impl(std::make_unique<Impl>()) { }
AnticheatMgr::~AnticheatMgr() = default;
AnticheatMgr& AnticheatMgr::Instance() { static AnticheatMgr instance; return instance; }

bool AnticheatMgr::LoadConfig()
{
    auto candidate = std::make_shared<Impl::Settings>();
    std::vector<std::string> errors;
    if (!LoadAnticheatConfig(candidate->config, errors))
    {
        for (auto const& error : errors)
            LOG_ERROR("module.anticheat", "Configuration rejected: {}", error);
        return false;
    }
    candidate->revision = _impl->settings.load()->revision + 1;
    _impl->repository.Configure(candidate->config.repository);
    _impl->settings.store(candidate);
    LOG_INFO("module.anticheat", "Configuration revision {} loaded, enabled={}, mode={}",
        candidate->revision, candidate->config.enabled, static_cast<unsigned>(candidate->config.enforcement.mode));
    return true;
}

void AnticheatMgr::OnLogin(Player* player)
{
    if (!player || !player->GetSession())
        return;
    auto settings = _impl->settings.load();
    auto entry = std::make_shared<Impl::Entry>();
    entry->guid = player->GetGUID();
    entry->account = player->GetSession()->GetAccountId();
    entry->session = SessionToken(player->GetSession());
    entry->name = player->GetName();
    entry->revision = settings->revision;
    entry->movement.GrantGrace(Now(), 5000);
    entry->mapId = player->GetMapId();
    entry->instanceId = player->GetInstanceId();
    entry->phaseMask = player->GetPhaseMask();
    for (unsigned i = 0; i < MAX_MOVE_TYPE; ++i)
        entry->serverSpeeds[i] = player->GetSpeed(static_cast<UnitMoveType>(i));
    std::shared_ptr<Impl::Entry> replaced;
    {
        std::lock_guard<std::mutex> lock(_impl->registryMutex);
        entry->generation = ++_impl->nextGeneration;
        auto existing = _impl->players.find(entry->guid);
        if (existing != _impl->players.end())
            replaced = existing->second;
        _impl->players[entry->guid] = entry;
    }
    if (replaced)
    {
        std::lock_guard<std::mutex> lock(replaced->mutex);
        replaced->active = false;
    }
    if (settings->config.enabled && settings->config.loginMessage && player->GetSession()->GetSecurity() > SEC_PLAYER)
        ChatHandler(player->GetSession()).SendSysMessage("Anticheat active; use .anticheat global for status.");
}

void AnticheatMgr::OnLogout(Player* player)
{
    if (!player)
        return;
    auto entry = _impl->Find(player->GetGUID());
    if (!entry || entry->session != SessionToken(player->GetSession()))
        return;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->active = false;
    }
    std::lock_guard<std::mutex> lock(_impl->registryMutex);
    auto it = _impl->players.find(entry->guid);
    if (it != _impl->players.end() && it->second == entry)
        _impl->players.erase(it);
}

void AnticheatMgr::OnTransition(Player* player)
{
    if (!player)
        return;
    auto entry = _impl->Find(player->GetGUID());
    if (!entry || entry->session != SessionToken(player->GetSession()))
        return;
    auto settings = _impl->settings.load();
    std::lock_guard<std::mutex> lock(entry->mutex);
    entry->movement.Reset();
    entry->movement.GrantGrace(Now(), settings->config.detectors.transitionGraceMs);
    // Auras can disappear while out of world, where core setters emit no
    // inverse packet. Rebuild authority from live auras/new orders on arrival.
    entry->serverFlight = false;
    entry->serverWaterwalk = false;
    entry->serverSlowFall = false;
    entry->serverHover = false;
    entry->serverNoGravity = false;
    entry->knockbackUntil = 0;
    entry->speedTransitions.Clear();
    entry->transportGuid = ObjectGuid::Empty;
}

void AnticheatMgr::OnMovement(Player* player, MovementInfo const& movement, uint32 opcode)
{
    if (!player || !player->GetSession())
        return;
    auto settings = _impl->settings.load();
    if (!settings->config.enabled)
        return;
    auto entry = _impl->Find(player->GetGUID());
    if (!entry || entry->session != SessionToken(player->GetSession()))
        return;

    DetectionContext context;
    context.nowMs = Now();
    context.position = PositionOf(movement.pos);
    Vec3 const worldPosition = context.position;
    context.serverPosition = PositionOf(player->GetPosition());
    context.mapId = player->GetMapId();
    context.clientTimeMs = movement.time;
    context.latencyMs = player->GetSession()->GetLatency();
    context.exempt = Exempt(player);
    context.teleport = player->IsBeingTeleported();
    context.forcedMovement = !player->movespline->Finalized() ||
        player->HasUnitState(UNIT_STATE_IGNORE_ANTISPEEDHACK | UNIT_STATE_JUMPING);
    context.flightAllowed = player->IsFreeFlying();
    context.waterWalkAllowed = player->HasAuraType(SPELL_AURA_WATER_WALK) || !player->IsAlive();
    context.slowFallAllowed = player->HasAuraType(SPELL_AURA_FEATHER_FALL) ||
        player->HasAuraType(SPELL_AURA_SAFE_FALL) || player->HasAuraType(SPELL_AURA_HOVER);
    context.rooted = player->HasUnitState(UNIT_STATE_ROOT | UNIT_STATE_STUNNED);
    // IsInWater is updated from the incoming swimming flag before this hook.
    // Query server terrain to avoid turning that flag into swim-speed authority.
    if (movement.HasMovementFlag(MOVEMENTFLAG_SWIMMING) || player->IsInWater())
        context.swimmingAllowed = player->GetMap()->IsUnderWater(player->GetPhaseMask(),
            context.serverPosition.x, context.serverPosition.y, context.serverPosition.z,
            player->GetCollisionHeight());
    context.falling = movement.HasMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
    context.flying = movement.HasMovementFlag(MOVEMENTFLAG_FLYING);
    context.canFly = movement.HasMovementFlag(MOVEMENTFLAG_CAN_FLY);
    context.swimming = movement.HasMovementFlag(MOVEMENTFLAG_SWIMMING);
    context.waterWalking = movement.HasMovementFlag(MOVEMENTFLAG_WATERWALKING);
    context.disableGravity = movement.HasMovementFlag(MOVEMENTFLAG_DISABLE_GRAVITY);
    context.onTransport = movement.HasMovementFlag(MOVEMENTFLAG_ONTRANSPORT);
    context.jump = opcode == MSG_MOVE_JUMP;
    context.landed = opcode == MSG_MOVE_FALL_LAND || opcode == MSG_MOVE_START_SWIM;
    std::uint32_t const instanceId = player->GetInstanceId();
    std::uint32_t const phaseMask = player->GetPhaseMask();
    ObjectGuid verifiedTransport;
    if (context.onTransport)
        if (Transport* transport = player->GetMap()->GetTransport(movement.transport.guid))
        {
            Vec3 local = PositionOf(movement.transport.pos);
            Vec3 localServer = context.serverPosition;
            transport->CalculatePassengerOffset(localServer.x, localServer.y, localServer.z);
            Vec3 expectedWorld = local;
            transport->CalculatePassengerPosition(expectedWorld.x, expectedWorld.y, expectedWorld.z);
            auto inEnvelope = [](Vec3 const& p)
            {
                return p.IsFinite() && std::hypot(p.x, p.y) <= 150.0f && std::abs(p.z) <= 100.0f;
            };
            if ((transport->GetPhaseMask() & phaseMask) && inEnvelope(local) && inEnvelope(localServer) &&
                std::hypot(std::hypot(expectedWorld.x - worldPosition.x, expectedWorld.y - worldPosition.y),
                    expectedWorld.z - worldPosition.z) <= 5.0f)
            {
                verifiedTransport = transport->GetGUID();
                context.position = local;
                context.serverPosition = localServer;
                context.serverTransport = true;
                context.swimmingAllowed = false;
            }
        }
    context.transportInvalid = context.onTransport && !context.serverTransport;
    bool sampleGround = false;
    bool sampleGeometry = false;
    UnitMoveType speedMode = MOVE_RUN;
    Vec3 previousPosition;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->active || entry->revision > settings->revision)
            return;
        if (entry->revision != settings->revision)
        {
            entry->movement.Reset();
            entry->score = {};
            entry->movement.GrantGrace(context.nowMs, settings->config.detectors.transitionGraceMs);
            entry->revision = settings->revision;
        }
        if (entry->instanceId != instanceId || entry->phaseMask != phaseMask)
        {
            entry->movement.Reset();
            entry->movement.GrantGrace(context.nowMs, settings->config.detectors.transitionGraceMs);
        }
        if (entry->transportGuid != verifiedTransport)
        {
            // The engine translates its reference into the new coordinate frame
            // without clearing accumulated speed/clock/control evidence.
            context.transportFrameChanged = true;
            entry->transportGuid = verifiedTransport;
        }
        context.flightAllowed = context.flightAllowed || entry->serverFlight;
        context.waterWalkAllowed = context.waterWalkAllowed || entry->serverWaterwalk;
        context.slowFallAllowed = context.slowFallAllowed || entry->serverSlowFall ||
            entry->serverHover || entry->serverNoGravity;
        context.forcedMovement = context.forcedMovement || context.nowMs < entry->knockbackUntil;
        if (context.landed)
            entry->knockbackUntil = 0; // The landing packet still receives its existing allowance.
        speedMode = context.flightAllowed ? MOVE_FLIGHT : context.swimmingAllowed ? MOVE_SWIM :
            movement.HasMovementFlag(MOVEMENTFLAG_WALKING) ? MOVE_WALK : MOVE_RUN;
        // Use server speeds. WALKING can only lower the permitted budget.
        previousPosition = entry->movement.previousPosition;
        sampleGround = ShouldSampleGround(context, entry->movement, settings->config.detectors);
        sampleGeometry = ShouldSampleGeometry(context, entry->movement, settings->config.detectors);
    }
    context.allowedSpeed = player->GetSpeed(speedMode);
    if (sampleGround)
    {
        context.groundSampled = true;
        context.groundZ = player->GetMap()->GetHeight(phaseMask,
            context.position.x, context.position.y, context.position.z + 2.0f);
        context.groundKnown = std::isfinite(context.groundZ) && context.groundZ > -100000.0f;
        ++_impl->groundQueries;
    }
    if (sampleGeometry)
    {
        context.geometrySampled = true;
        context.lineBlocked = !player->GetMap()->isInLineOfSight(previousPosition.x,
            previousPosition.y, previousPosition.z + 1.0f, context.position.x,
            context.position.y, context.position.z + 1.0f, phaseMask,
            LINEOFSIGHT_ALL_CHECKS, VMAP::ModelIgnoreFlags::Nothing);
        ++_impl->geometryQueries;
    }
    ScoreUpdate update;
    float score = 0;
    auto const start = settings->config.instrumentation ? std::chrono::steady_clock::now() :
        std::chrono::steady_clock::time_point{};
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (!entry->active || entry->revision != settings->revision)
            return;
        auto const activeSpeed = entry->speedTransitions.ForMode(speedMode, context.nowMs);
        entry->movement.speedGraceMaxSpeed = activeSpeed.maximum;
        entry->movement.speedGracePeakSpeed = activeSpeed.maximum;
        entry->movement.speedGraceUntilMs = activeSpeed.expires;
        if (entry->movement.initialized && context.nowMs > entry->movement.previousTimeMs)
        {
            // Integrate each historical transition only until its original
            // expiry; an interval can span several overlapping ACK allowances.
            auto const budget = entry->speedTransitions.Budget(speedMode,
                entry->movement.previousTimeMs, context.nowMs,
                std::max(context.allowedSpeed, entry->movement.previousSpeed));
            auto const elapsed = context.nowMs - entry->movement.previousTimeMs;
            entry->movement.speedGraceMaxSpeed = static_cast<float>(budget.distance * 1000.0 / elapsed);
            entry->movement.speedGraceUntilMs = context.nowMs;
            entry->movement.speedGracePeakSpeed = std::max(activeSpeed.maximum, budget.peak);
        }
        auto batch = EvaluateMovement(context, entry->movement, settings->config.detectors);
        if (context.latencyMs > settings->config.enforcement.maxLatencyMs || entry->authorityDegraded)
            for (std::size_t i = 0; i < batch.size; ++i)
                batch.results[i].actionable = false;
        update = ViolationScorer::Observe(entry->score, batch, context.nowMs, settings->config.scoring);
        score = entry->score.score;
        entry->latency = context.latencyMs;
        entry->position = worldPosition;
        entry->mapId = context.mapId;
        entry->instanceId = instanceId;
        entry->phaseMask = phaseMask;
    }
    ++_impl->movements;
    if (settings->config.instrumentation)
        _impl->detectorNanoseconds += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
    for (std::size_t i = 0; i < update.count; ++i)
        if (!_impl->repository.Enqueue(Impl::Record(*entry, update.accepted[i], worldPosition, context.mapId, score)))
        {
            ++_impl->lostReports;
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->persistenceDegraded = true;
        }
}

void AnticheatMgr::OnServerPacket(WorldSession* session, WorldPacket const& packet)
{
    auto const opcode = packet.GetOpcode();
    switch (opcode)
    {
        case SMSG_MOVE_SET_CAN_FLY: case SMSG_MOVE_UNSET_CAN_FLY:
        case SMSG_MOVE_WATER_WALK: case SMSG_MOVE_LAND_WALK:
        case SMSG_MOVE_FEATHER_FALL: case SMSG_MOVE_NORMAL_FALL:
        case SMSG_MOVE_SET_HOVER: case SMSG_MOVE_UNSET_HOVER:
        case SMSG_MOVE_GRAVITY_DISABLE: case SMSG_MOVE_GRAVITY_ENABLE:
        case SMSG_FORCE_RUN_SPEED_CHANGE: case SMSG_FORCE_RUN_BACK_SPEED_CHANGE:
        case SMSG_FORCE_SWIM_SPEED_CHANGE: case SMSG_FORCE_SWIM_BACK_SPEED_CHANGE:
        case SMSG_FORCE_WALK_SPEED_CHANGE:
        case SMSG_FORCE_FLIGHT_SPEED_CHANGE: case SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE:
        case SMSG_MOVE_KNOCK_BACK: break;
        default: return;
    }
    if (!session)
        return;
    std::uint64_t rawGuid = 0;
    if (!ReadPackedMoverGuid(packet.contents(), packet.size(), rawGuid))
        return;
    auto entry = _impl->Find(ObjectGuid(rawGuid));
    if (!entry || entry->account != session->GetAccountId() || entry->session != SessionToken(session))
        return;
    unsigned speedType = MAX_MOVE_TYPE;
    switch (opcode)
    {
        case SMSG_FORCE_RUN_SPEED_CHANGE: speedType = MOVE_RUN; break;
        case SMSG_FORCE_RUN_BACK_SPEED_CHANGE: speedType = MOVE_RUN_BACK; break;
        case SMSG_FORCE_SWIM_SPEED_CHANGE: speedType = MOVE_SWIM; break;
        case SMSG_FORCE_SWIM_BACK_SPEED_CHANGE: speedType = MOVE_SWIM_BACK; break;
        case SMSG_FORCE_WALK_SPEED_CHANGE: speedType = MOVE_WALK; break;
        case SMSG_FORCE_FLIGHT_SPEED_CHANGE: speedType = MOVE_FLIGHT; break;
        case SMSG_FORCE_FLIGHT_BACK_SPEED_CHANGE: speedType = MOVE_FLIGHT_BACK; break;
        default: break;
    }
    float newSpeed = 0;
    if (speedType != MAX_MOVE_TYPE && !ReadServerSpeed(packet.contents(), packet.size(), speedType == MOVE_RUN, newSpeed))
        return;
    auto settings = _impl->settings.load();
    auto const now = Now();
    std::lock_guard<std::mutex> lock(entry->mutex);
    if (!entry->active)
        return;
    if (speedType != MAX_MOVE_TYPE)
    {
        if (std::abs(entry->serverSpeeds[speedType] - newSpeed) < 0.001f)
            return; // Repeated client-triggered corrections cannot renew grace.
        auto const retainSince = entry->movement.initialized &&
            now - entry->movement.previousTimeMs <= settings->config.detectors.maxSampleGapMs ?
            entry->movement.previousTimeMs : now;
        if (!entry->speedTransitions.Record(speedType, entry->serverSpeeds[speedType], newSpeed,
            now, settings->config.detectors.transitionGraceMs, retainSince))
        {
            entry->authorityDegraded = true;
            ++_impl->authorityOverflows;
        }
        entry->serverSpeeds[speedType] = newSpeed;
    }
    switch (opcode)
    {
        case SMSG_MOVE_SET_CAN_FLY: case SMSG_MOVE_UNSET_CAN_FLY:
            if (entry->serverFlight == (opcode == SMSG_MOVE_SET_CAN_FLY))
                return;
            entry->serverFlight = opcode == SMSG_MOVE_SET_CAN_FLY;
            entry->movement.flightGraceUntilMs = now + settings->config.detectors.transitionGraceMs;
            break;
        case SMSG_MOVE_WATER_WALK: case SMSG_MOVE_LAND_WALK:
            if (entry->serverWaterwalk == (opcode == SMSG_MOVE_WATER_WALK))
                return;
            entry->serverWaterwalk = opcode == SMSG_MOVE_WATER_WALK;
            entry->movement.waterWalkGraceUntilMs = now + settings->config.detectors.transitionGraceMs;
            break;
        case SMSG_MOVE_FEATHER_FALL: case SMSG_MOVE_NORMAL_FALL:
            if (entry->serverSlowFall == (opcode == SMSG_MOVE_FEATHER_FALL))
                return;
            entry->serverSlowFall = opcode == SMSG_MOVE_FEATHER_FALL;
            entry->movement.slowFallGraceUntilMs = now + settings->config.detectors.transitionGraceMs;
            break;
        case SMSG_MOVE_SET_HOVER: case SMSG_MOVE_UNSET_HOVER:
            if (entry->serverHover == (opcode == SMSG_MOVE_SET_HOVER))
                return;
            entry->serverHover = opcode == SMSG_MOVE_SET_HOVER;
            entry->movement.slowFallGraceUntilMs = now + settings->config.detectors.transitionGraceMs;
            break;
        case SMSG_MOVE_GRAVITY_DISABLE: case SMSG_MOVE_GRAVITY_ENABLE:
            if (entry->serverNoGravity == (opcode == SMSG_MOVE_GRAVITY_DISABLE))
                return;
            entry->serverNoGravity = opcode == SMSG_MOVE_GRAVITY_DISABLE;
            entry->movement.slowFallGraceUntilMs = now + settings->config.detectors.transitionGraceMs;
            break;
        case SMSG_MOVE_KNOCK_BACK: entry->knockbackUntil = now + 5000; break;
        default: break;
    }
}

void AnticheatMgr::OnWorldUpdate(uint32 /*diff*/)
{
    auto settings = _impl->settings.load();
    auto const now = Now();
    _impl->repository.Update(now); // Observe a completed failure before deciding sanctions.
    bool const persistenceHealthy = _impl->repository.Metrics().healthy;
    // This hook runs after MapMgr::Update has joined all map workers.
    // Resolving live players and touching global sessions belongs here only.
    bool const sweep = now - _impl->lastSweep >= 1000;
    if (sweep)
        _impl->lastSweep = now;
    for (auto const& entry : sweep ? _impl->Snapshot() : std::vector<std::shared_ptr<Impl::Entry>>{})
    {
        EnforcementDecision decision;
        bool alert = false;
        float score = 0;
        std::uint64_t total = 0;
        Vec3 position;
        std::uint32_t mapId = 0;
        std::uint32_t instanceId = 0;
        std::uint32_t phaseMask = 0;
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            if (!entry->active || entry->revision != settings->revision)
                continue;
            ViolationScorer::Decay(entry->score, now, settings->config.scoring);
            score = entry->score.score;
            total = entry->score.totalAccepted;
            position = entry->position;
            mapId = entry->mapId;
            instanceId = entry->instanceId;
            phaseMask = entry->phaseMask;
            if (settings->config.enabled)
            {
                decision = EnforcementManager::Decide(entry->score, entry->enforcement,
                    now, entry->latency, !persistenceHealthy || entry->persistenceDegraded || entry->authorityDegraded ||
                    now < entry->movement.graceUntilMs || now < entry->knockbackUntil ||
                    entry->speedTransitions.Active(now) || now < entry->movement.flightGraceUntilMs ||
                    now < entry->movement.waterWalkGraceUntilMs || now < entry->movement.slowFallGraceUntilMs,
                    settings->config.enforcement, settings->config.scoring);
                alert = settings->config.enforcement.mode != EnforcementMode::Observation &&
                    score >= 50 && now - entry->lastAlert >= settings->config.alertFrequencyMs;
                if (alert)
                    entry->lastAlert = now;
            }
        }
        if (alert)
        {
            auto text = Acore::StringFormat("[Anticheat] {}: score {:.1f}, {} distinct observations.",
                entry->name, score, total);
            for (auto const& pair : sWorldSessionMgr->GetAllSessions())
                if (pair.second && pair.second->GetPlayer() &&
                    pair.second->GetSecurity() >= settings->config.gmLevel)
                    ChatHandler(pair.second).SendSysMessage(text);
        }
        if (decision.action == Action::None)
            continue;
        Player* player = ObjectAccessor::FindPlayer(entry->guid);
        if (!player || SessionToken(player->GetSession()) != entry->session ||
            Exempt(player) || player->IsBeingTeleported() ||
            player->GetMapId() != mapId || player->GetInstanceId() != instanceId ||
            player->GetPhaseMask() != phaseMask ||
            player->GetSession()->GetLatency() > settings->config.enforcement.maxLatencyMs ||
            !player->movespline->Finalized())
            continue;
        DetectionResult result{DetectionType::Speed, 1.0f, true, score, 0, "enforcement policy"};
        auto record = Impl::Record(*entry, result, position, mapId, score);
        record.action = decision.action;
        record.details = Acore::StringFormat("action={}; score={:.2f}; events={}; families={}; span_ms={}; session={}; config={}",
            static_cast<unsigned>(decision.action), score, decision.evidenceCount,
            decision.familyMask, decision.spanMs, entry->generation, settings->revision);
        if (!_impl->repository.Enqueue(record))
        {
            ++_impl->lostReports;
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->persistenceDegraded = true;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->enforcement.applied = true;
        }
        LOG_WARN("module.anticheat", "Enforcement {} account {}: {}", entry->name, entry->account, record.details);
        if (decision.action == Action::Ban)
        {
            auto const result = sBan->BanAccountByPlayerName(entry->name,
                std::to_string(settings->config.enforcement.banSeconds) + "s", record.details, "Anticheat");
            if (result != BAN_SUCCESS)
                LOG_ERROR("module.anticheat", "Ban failed for account {} with result {}; session will still be kicked",
                    entry->account, static_cast<unsigned>(result));
        }
        player->GetSession()->KickPlayer("Anticheat: repeated corroborated movement anomalies");
    }
    _impl->repository.Update(now);
    auto const lost = _impl->lostReports.load();
    if (lost != _impl->reportedLost && now - _impl->lastLostReport >= 5000)
    {
        LOG_ERROR("module.anticheat", "{} observations could not be queued; affected sessions cannot be auto-sanctioned", lost);
        _impl->reportedLost = lost;
        _impl->lastLostReport = now;
    }
    auto const overflows = _impl->authorityOverflows.load();
    if (overflows != _impl->reportedAuthorityOverflows)
    {
        LOG_ERROR("module.anticheat", "{} server-speed history overflows; affected sessions are diagnostic only", overflows);
        _impl->reportedAuthorityOverflows = overflows;
    }
    if (settings->config.instrumentation && now - _impl->lastMetrics >= 60000)
    {
        _impl->lastMetrics = now;
        LOG_INFO("module.anticheat", "Metrics cumulative: movements={}, detector_ns={}, height_queries={}, los_queries={}",
            _impl->movements.load(), _impl->detectorNanoseconds.load(),
            _impl->groundQueries.load(), _impl->geometryQueries.load());
    }
}

void AnticheatMgr::OnShutdown() { _impl->repository.Shutdown(); }

void AnticheatMgr::ShowPlayerReport(Player* gm, Player* player)
{
    if (!gm || !player)
        return;
    auto entry = _impl->Find(player->GetGUID());
    if (!entry)
    {
        ChatHandler(gm->GetSession()).SendSysMessage("No anticheat state for this session.");
        return;
    }
    ScoreState score;
    bool degraded;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        score = entry->score;
        degraded = entry->persistenceDegraded || entry->authorityDegraded;
    }
    ChatHandler handler(gm->GetSession());
    handler.PSendSysMessage("{}: score {:.2f}; accepted {}; raw {}; enforcement inhibited={}",
        entry->name, score.score, score.totalAccepted, score.rawObservations, degraded);
    for (std::size_t i = 0; i < DetectionCount; ++i)
        if (score.counts[i])
            handler.PSendSysMessage("{}: {}", DetectionNames[i], score.counts[i]);
}

void AnticheatMgr::ShowGlobalReport(Player* gm)
{
    if (!gm)
        return;
    auto settings = _impl->settings.load();
    auto entries = _impl->Snapshot();
    ChatHandler(gm->GetSession()).PSendSysMessage(
        "Anticheat: enabled={}, mode={}, players={}, movement samples={}, dropped={}, config={}",
        settings->config.enabled, static_cast<unsigned>(settings->config.enforcement.mode),
        entries.size(), _impl->movements.load(), _impl->lostReports.load(), settings->revision);
    auto const metrics = _impl->repository.Metrics();
    ChatHandler(gm->GetSession()).PSendSysMessage(
        "Persistence: queued={}, in flight={}, committed={}, failed batches={}, healthy={}",
        metrics.queued, metrics.inFlight, metrics.committed, metrics.failedBatches, metrics.healthy);
}

void AnticheatMgr::ResetSessionReports(Player* gm)
{
    if (!gm)
        return;
    for (auto const& entry : _impl->Snapshot())
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        entry->score = {};
        // Keep sanctions, server authority, and persistence-failure latches.
    }
    ChatHandler(gm->GetSession()).SendSysMessage("Session scores reset. Stored evidence is retained.");
    LOG_INFO("module.anticheat", "Session scores reset by {}", gm->GetName());
}

void AnticheatMgr::OnWardenAlert(Player* player, uint16 checkId, std::string const& reason)
{
    if (!player)
        return;
    auto settings = _impl->settings.load();
    if (!settings->config.enabled || !settings->config.detectors.enabled[Index(DetectionType::Warden)])
        return;
    auto entry = _impl->Find(player->GetGUID());
    if (!entry || entry->session != SessionToken(player->GetSession()))
        return;
    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        auto const now = Now();
        if (!entry->active || now - entry->lastWarden < settings->config.scoring.cooldownMs)
            return;
        entry->lastWarden = now;
    }
    DetectionResult result{DetectionType::Warden, 1, false, 0, 0, "Warden owns the penalty"};
    auto record = Impl::Record(*entry, result, PositionOf(player->GetPosition()), player->GetMapId(), 0);
    record.details = Acore::StringFormat("check={}; {}; Warden penalty already processed", checkId, reason);
    if (!_impl->repository.Enqueue(record))
        ++_impl->lostReports;
}
}
