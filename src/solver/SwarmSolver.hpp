#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include "../core/Types.hpp"
#include "../engine/HazardDetector.hpp"
#include <vector>
#include <cstdint>

namespace solver {

class SwarmSolver {
public:
    static SwarmSolver& get();

    void start(PlayLayer* playLayer);
    void resume(PlayLayer* playLayer);
    void stop();
    void reset();

    void stepSwarmBatch(PlayLayer* playLayer, uint32_t maxSteps = 1000);

    bool isRunning() const;
    bool isCompleted() const;
    TelemetryMetrics getTelemetry() const;

    const std::vector<TickAction>& getResolvedMacro() const;

private:
    SwarmSolver() = default;

    // Population generation and genetic mutation
    std::vector<SwarmBot> generatePopulation(
        VehicleMode mode,
        uint32_t startTick,
        uint32_t horizonTicks,
        const std::vector<SwarmBot>& previousSurvivors,
        uint32_t waveRetryCount
    );

    // Rollout a single bot through the horizon
    void simulateBot(
        PlayLayer* playLayer,
        const BeamCheckpoint& checkpoint,
        SwarmBot& bot,
        uint32_t horizonTicks
    );

    // Commit a solution upon reaching 100%
    void finalizeSolution(const std::vector<TickAction>& winningActions);

    // True beam backtracking to parent or runner-up
    void handleBacktrack(PlayLayer* playLayer);

    bool m_isRunning = false;
    bool m_isCompleted = false;
    int m_levelID = 0;
    std::string m_levelName;
    float m_startX = 0.0f;
    float m_levelLength = 0.0f;
    float m_maxReachedX = 0.0f;
    uint32_t m_currentTick = 0;

    // Beam Checkpoint Tree Stack
    std::vector<BeamCheckpoint> m_checkpointStack;
    std::vector<TickAction> m_resolvedMacro;
    std::vector<SwarmBot> m_partialProgressSeeds;

    // Active wave state
    uint32_t m_activeWaveIndex = 0;
    uint32_t m_waveRetryCount = 0;
    uint32_t m_backtrackCount = 0;
    size_t m_currentPopulationSize = 160;
    size_t m_lastSurvivorCount = 0;

    // Stateful population evaluation across frame budgets
    std::vector<SwarmBot> m_activePopulation;
    size_t m_currentBotIndex = 0;
    std::vector<SwarmBot> m_currentWaveSurvivors;
    uint32_t m_currentHorizonTicks = 0;

    TelemetryMetrics m_telemetry;
};

} // namespace solver
