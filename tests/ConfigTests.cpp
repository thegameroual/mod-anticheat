#include "TestHarness.h"
#include "core/AnticheatConfig.h"
#include "core/ConfigValue.h"
#include "integration/MovementIntent.h"
#include <array>
#include <limits>
#include <memory>
#include <atomic>
#include <thread>
#include <vector>
#include <fstream>
#include <map>

using namespace anticheat;

void RunConfigTests()
{
    AnticheatConfig config;
    // Verify the distributed policy equals the defaults used by end-to-end traces.
    std::ifstream input(ANTICHEAT_CONFIG_PATH);
    CHECK(input.good());
    std::map<std::string, std::string> distributed;
    std::string line;
    while (std::getline(input, line))
    {
        auto equals = line.find('=');
        if (line.rfind("Anticheat.", 0) != 0 || equals == std::string::npos) continue;
        auto key = line.substr(10, equals - 10);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        distributed[key] = line.substr(equals + 1);
    }
    auto same = [&](char const* key, auto expected)
    {
        auto parsed = expected;
        CHECK(distributed.count(key) == 1);
        CHECK(ParseConfigValue(distributed.at(key), parsed));
        CHECK(parsed == expected);
    };
    same("Mode", static_cast<unsigned>(config.enforcement.mode));
    same("Score.RearmMs", config.scoring.rearmMs);
    same("Score.EvidenceWindowMs", config.scoring.evidenceWindowMs);
    same("Score.DecayPerSecond", config.scoring.decayPerSecond);
    same("Score.MinConfidence", config.scoring.minConfidence);
    same("Score.MinObservationConfidence", config.scoring.minObservationConfidence);
    same("Score.MaxPerObservation", config.scoring.maxScorePerObservation);
    same("SpeedHack.Tolerance", config.detectors.speedTolerance);
    same("Movement.SpeedWindowMs", config.detectors.speedWindowMs);
    same("Movement.MaxSampleGapMs", config.detectors.maxSampleGapMs);
    same("Movement.TransitionGraceMs", config.detectors.transitionGraceMs);
    same("Movement.LatencyCapMs", config.detectors.latencyCapMs);
    same("Enforcement.KickScore", config.enforcement.kickScore);
    same("Enforcement.BanScore", config.enforcement.banScore);
    same("Enforcement.MinEvents", config.enforcement.minEvents);
    same("Enforcement.MinFamilies", config.enforcement.minFamilies);
    same("Enforcement.MinSpanMs", config.enforcement.minSpanMs);
    same("Enforcement.MaxLatencyMs", config.enforcement.maxLatencyMs);
    same("Enforcement.BanSeconds", config.enforcement.banSeconds);
    same("Score.ConfirmationWindowMs", config.scoring.confirmationWindowMs);
    same("Score.MaxObservationGapMs", config.scoring.maxObservationGapMs);
    same("Score.ReportIntervalMs", config.scoring.reportIntervalMs);
    same("Score.PersistencePerSecond", config.scoring.persistencePerSecond);
    same("Score.MaxIncidentRisk", config.scoring.maxIncidentRisk);
    same("Alert.MinDurationMs", config.alerts.minDurationMs);
    same("Alert.RepeatMs", config.alerts.repeatMs);
    same("Alert.FreshnessMs", config.alerts.freshnessMs);
    same("Alert.MinWindows", config.alerts.minWindows);
    same("Alert.MinSeverity", config.alerts.minSeverity);
    same("Alert.SevereSeverity", config.alerts.severeSeverity);
    same("Alert.MinConfidence", config.alerts.minConfidence);
    same("Enforcement.SingleFamilyEnabled", config.enforcement.singleFamilyEnabled);
    same("Enforcement.SingleFamilyBan", config.enforcement.singleFamilyBan);
    same("Enforcement.SingleMinDurationMs", config.enforcement.singleMinDurationMs);
    same("Enforcement.SingleMinWindows", config.enforcement.singleMinWindows);
    same("Enforcement.SingleMinSeverity", config.enforcement.singleMinSeverity);
    same("Enforcement.FreshnessMs", config.enforcement.freshnessMs);
    same("Warden.CooldownMs", config.wardenCooldownMs);
    constexpr char const* weights[] = {"SpeedHack", "FlyHack", "Teleport", "Climb", "Jump", "Gravity",
        "Waterwalk", "Nofall", "IgnoreControl", "FakePing", "NoClip", "Warden"};
    for (std::size_t i = 0; i < DetectionCount; ++i)
        same((std::string("Weight.") + weights[i]).c_str(), config.scoring.weights[i]);
    CHECK(config.Validate().empty());
    CHECK(config.enforcement.mode == EnforcementMode::Observation);
    auto reject = [](AnticheatConfig const& value) { CHECK(!value.Validate().empty()); };
    config.alerts.freshnessMs = config.scoring.maxObservationGapMs + 1;
    reject(config);
    config = {};
    config.scoring.persistencePerSecond = std::numeric_limits<float>::infinity();
    reject(config);
    config = {};
    config.enforcement.singleMinWindows = 65;
    reject(config);
    config = {};
    config.scoring.confirmationWindowMs = config.detectors.speedWindowMs - 1;
    reject(config);
    config = {};
    config.enforcement.kickScore = 1000;
    reject(config);
    config = {};
    config.detectors.speedTolerance = std::numeric_limits<float>::quiet_NaN();
    reject(config);
    config = {};
    config.scoring.weights[0] = -1;
    reject(config);
    config = {};
    config.scoring.rearmMs = 3000; // A sparse speed observation is not a clean episode.
    reject(config);
    config = {};
    config.repository.reservedActions = config.repository.capacity;
    reject(config);
    config = {};
    config.enforcement.banSeconds = 0;
    reject(config);
    config = {};
    config.enforcement.minFamilies = 1;
    reject(config);
    config = {};
    config.enforcement.minSpanMs = config.scoring.evidenceWindowMs;
    reject(config);
    config = {};
    config.enforcement.mode = static_cast<EnforcementMode>(3);
    reject(config);

    float tolerance = 1.25f;
    CHECK(ParseConfigValue(" 1.5 ", tolerance));
    CHECK_NEAR(tolerance, 1.5f, 0.001f);
    CHECK(!ParseConfigValue("1.5seconds", tolerance));
    CHECK(!ParseConfigValue("1,5", tolerance));
    CHECK(!ParseConfigValue("NaN", tolerance));
    CHECK(!ParseConfigValue("inf", tolerance));
    CHECK(!ParseConfigValue("", tolerance));
    CHECK_NEAR(tolerance, 1.5f, 0.001f); // Failed parsing never replaces the previous value.
    std::uint32_t integer = 7;
    CHECK(!ParseConfigValue("-1", integer));
    CHECK(!ParseConfigValue("4294967296", integer));
    CHECK(!ParseConfigValue("1.5", integer));
    CHECK(integer == 7);
    CHECK(ParseConfigValue("4294967295", integer));
    bool enabled = true;
    CHECK(ParseConfigValue("false", enabled));
    CHECK(!enabled);
    CHECK(!ParseConfigValue("banana", enabled));
    CHECK(!ParseConfigValue("2", enabled));

    // Outgoing movement intent parsing is bounded, independent of packet cursors.
    std::uint64_t guid = 99;
    CHECK(!ReadPackedMoverGuid(nullptr, 0, guid));
    std::array<std::uint8_t, 13> full{0xff, 1, 2, 3, 4, 5, 6, 7, 8, 0, 0, 0, 0};
    CHECK(ReadPackedMoverGuid(full.data(), full.size(), guid));
    CHECK(guid == 0x0807060504030201ull);
    for (std::size_t size = 0; size < full.size(); ++size)
        CHECK(!ReadPackedMoverGuid(full.data(), size, guid));
    std::array<std::uint8_t, 5> zero{};
    CHECK(!ReadPackedMoverGuid(zero.data(), zero.size(), guid));
    std::array<std::uint8_t, 6> sparse{1, 42, 0, 0, 0, 0};
    CHECK(ReadPackedMoverGuid(sparse.data(), sparse.size(), guid));
    CHECK(guid == 42);
    std::array<std::uint8_t, 11> runSpeed{1, 42, 0, 0, 0, 0, 1, 0, 0, 0xe0, 0x40};
    float speed = 0;
    CHECK(ReadServerSpeed(runSpeed.data(), runSpeed.size(), true, speed));
    CHECK_NEAR(speed, 7.0f, 0.001f);
    CHECK(!ReadServerSpeed(runSpeed.data(), runSpeed.size(), false, speed));
    CHECK(!ReadServerSpeed(runSpeed.data(), runSpeed.size() - 1, true, speed));
    std::array<std::uint8_t, 10> swimSpeed{1, 42, 0, 0, 0, 0, 0, 0, 0x80, 0x40};
    CHECK(ReadServerSpeed(swimSpeed.data(), swimSpeed.size(), false, speed));
    CHECK_NEAR(speed, 4.0f, 0.001f);
    swimSpeed[9] = 0xc0; // Negative server speed cannot grant allowance.
    CHECK(!ReadServerSpeed(swimSpeed.data(), swimSpeed.size(), false, speed));
    swimSpeed[8] = 0x80;
    swimSpeed[9] = 0x7f; // +infinity.
    CHECK(!ReadServerSpeed(swimSpeed.data(), swimSpeed.size(), false, speed));
    swimSpeed[8] = 0xc0; // NaN.
    CHECK(!ReadServerSpeed(swimSpeed.data(), swimSpeed.size(), false, speed));

    // Actual server speed changes retain separate movement modes and original
    // deadlines. Repeated slower mount toggles cannot preserve an earlier peak.
    SpeedTransitionHistory transitions;
    CHECK(transitions.Record(1, 42, 7, 1000, 1500, 1000));
    CHECK(transitions.Record(2, 7, 100, 1100, 1500, 1000)); // Unrelated flight mode.
    CHECK_NEAR(transitions.ForMode(1, 1200).maximum, 42, 0.001);
    CHECK(transitions.ForMode(0, 1200).maximum == 0); // Walk gets no run/flight allowance.
    CHECK(transitions.Record(1, 7, 14, 1500, 1500, 1000));
    CHECK(transitions.Record(1, 14, 7, 2000, 1500, 1000));
    CHECK(transitions.ForMode(1, 2400).expires == 2500);
    CHECK_NEAR(transitions.ForMode(1, 2500).maximum, 14, 0.001);
    auto budget = transitions.Budget(1, 2400, 2700, 7);
    CHECK_NEAR(budget.distance, 7.0, 0.001); // 0.1s at42 + 0.2s at14, not flight100.
    CHECK_NEAR(budget.peak, 42, 0.001);
    CHECK(transitions.Record(1, 7, 7, 3400, 1500, 3400)); // Corrective ACK changes nothing.
    CHECK(transitions.ForMode(1, 3500).maximum == 0);
    CHECK_NEAR(transitions.Budget(1, 3500, 4500, 7).distance, 7, 0.001);
    CHECK(!transitions.Active(3500));
    transitions.Clear();
    CHECK(!transitions.Active(1000));

    // Overflow is explicit. The adapter switches the affected session to
    // diagnostics rather than granting unbounded speed or silently forgetting it.
    for (unsigned i = 0; i < 32; ++i)
        CHECK(transitions.Record(1, 100.0f - i, 99.0f - i, 1000 + i, 1500, 1000));
    CHECK(!transitions.Record(1, 68, 67, 1032, 1500, 1000));
    CHECK(transitions.Record(1, 7, 14, 4000, 1500, 4000)); // Expired slots can be reused.

    // Exercise the exact immutable publication primitive used by the adapter.
    auto initial = std::make_shared<AnticheatConfig>();
    initial->scoring.weights[0] = initial->detectors.speedTolerance;
    std::atomic<std::shared_ptr<AnticheatConfig const>> snapshot{initial};
    std::atomic<bool> bad{false};
    std::vector<std::thread> readers;
    for (unsigned i = 0; i < 4; ++i)
        readers.emplace_back([&]
        {
            for (unsigned n = 0; n < 10000; ++n)
            {
                auto current = snapshot.load();
                if (current->detectors.speedTolerance != current->scoring.weights[0])
                    bad = true;
            }
        });
    for (unsigned n = 0; n < 1000; ++n)
    {
        auto next = std::make_shared<AnticheatConfig>();
        next->detectors.speedTolerance = static_cast<float>(n);
        next->scoring.weights[0] = static_cast<float>(n);
        snapshot.store(next);
    }
    for (auto& reader : readers)
        reader.join();
    CHECK(!bad.load());
}
