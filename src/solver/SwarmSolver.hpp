#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include "../core/Types.hpp"
#include "../engine/HazardDetector.hpp"
#include <vector>
#include <cstdint>
#include <string>

namespace solver {

enum class SolverMode : uint8_t {
    SpawnRespawn = 0,
    Checkpoints = 1
};

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
    float getLevelLength() const { return m_levelLength; }
    int getLevelID() const { return m_levelID; }
    const std::string& getLevelName() const { return m_levelName; }

    const std::vector<TickAction>& getResolvedMacro() const;
    const std::vector<TrajectorySample>& getTrajectory() const;

    SolverMode getSolverMode() const { return m_solverMode; }
    void setSolverMode(SolverMode mode) { m_solverMode = mode; }

private:
    SwarmSolver() = default;

    // Generate hazard-aware swarm population for current segment
    std::vector<SwarmBot> generatePopulation(
        VehicleMode mode,
        uint32_t startTick,
        uint32_t horizonTicks,
        const std::vector<SwarmBot>& previousSurvivors,
        uint32_t waveRetryCount,
        float playerSpeed,
        float startX,
        cocos2d::CCArray* levelObjects,
        bool startsHeld = false
    );

    // Rollout a single bot through the horizon
    void simulateBot(
        PlayLayer* playLayer,
        SwarmBot& bot,
        uint32_t horizonTicks
    );

    // Commit a solution upon reaching 100%
    void finalizeSolution(PlayLayer* playLayer, const std::vector<TickAction>& winningActions);

    SolverMode m_solverMode = SolverMode::SpawnRespawn;
    bool m_isRunning = false;
    bool m_isCompleted = false;
    int m_levelID = 0;
    std::string m_levelName;
    float m_startX = 0.0f;
    float m_levelLength = 0.0f;
    float m_lastHazardX = 0.0f;
    float m_maxReachedX = 0.0f;
    uint32_t m_currentTick = 0;

    // Prefix-Locked Swarm from Spawn state
    uint32_t m_frontierTick = 0;
    float m_frontierX = 0.0f;
    VehicleMode m_frontierMode = VehicleMode::Cube;
    std::vector<TickAction> m_verifiedPrefix;
    PlayerSnapshot m_rootSnapshot;
    PlayerSnapshot m_rootSnapshot2;
    bool m_hasPlayer2 = false;

    // Classic Checkpoints fallback stack
    std::vector<BeamCheckpoint> m_checkpointStack;

    std::vector<TickAction> m_resolvedMacro;
    std::vector<TrajectorySample> m_trajectorySamples;

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
