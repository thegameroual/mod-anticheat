/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence. */
#include "detectors/MovementDetectors.h"
#include "scoring/ViolationScorer.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <vector>

int main()
{
    using namespace anticheat;
    constexpr std::size_t players = 1000;
    constexpr std::size_t rounds = 1000;
    std::vector<MovementState> movement(players);
    std::vector<ScoreState> scores(players);
    std::vector<DetectionContext> contexts(players);
    DetectorConfig detectors;
    ScoreConfig scoring;
    for (std::size_t i = 0; i < players; ++i)
    {
        contexts[i].nowMs = 1000;
        contexts[i].clientTimeMs = 1000;
        contexts[i].latencyMs = 100;
        contexts[i].position.y = static_cast<float>(i);
        EvaluateMovement(contexts[i], movement[i], detectors);
    }
    std::uint64_t observed = 0;
    auto start = std::chrono::steady_clock::now();
    for (std::size_t round = 0; round < rounds; ++round)
    {
        for (std::size_t i = 0; i < players; ++i)
        {
            auto& context = contexts[i];
            context.serverPosition = context.position;
            context.position.x += i % 100 == 0 ? 2.0f : 0.7f; // 1% simulated speed anomalies.
            context.nowMs += 100;
            context.clientTimeMs += 100;
            auto results = EvaluateMovement(context, movement[i], detectors);
            auto accepted = ViolationScorer::Observe(scores[i], results, context.nowMs, scoring);
            observed += accepted.count;
        }
    }
    auto elapsed = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - start).count();
    std::uint64_t incidents = 0;
    for (auto const& state : scores)
        incidents += state.totalAccepted;
    std::cout << "Pure movement + scoring benchmark (excludes core hooks, VMAP, database and mutexes)\n"
              << "players=" << players << " events=" << players * rounds << " reports=" << observed << " incidents=" << incidents
              << "\n" << std::fixed << std::setprecision(2)
              << "elapsed_ms=" << elapsed / 1000000.0 << " ns_per_event=" << elapsed / (players * rounds)
              << "\nMovementState_bytes=" << sizeof(MovementState) << " ScoreState_bytes=" << sizeof(ScoreState) << '\n';
}
