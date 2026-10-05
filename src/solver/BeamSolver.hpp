#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include "../core/Types.hpp"
#include "../engine/HazardDetector.hpp"
#include <vector>
#include <unordered_map>
#include <string>
#include <cstdint>

namespace solver {

class BeamSolver {
public:
    static BeamSolver& get();

    void start(PlayLayer* playLayer);
    void resume(PlayLayer* playLayer);
    void stop();
    void reset();

    void stepBeamBatch(PlayLayer* playLayer, uint32_t maxSteps = 1000);

    bool isRunning() const;
    bool isCompleted() const;
    TelemetryMetrics getTelemetry() const;
    float getLevelLength() const { return m_levelLength; }

    const std::vector<TickAction>& getResolvedMacro() const;
    const std::vector<TrajectorySample>& getTrajectory() const;

private:
    BeamSolver() = default;

    void finalizeSolution(PlayLayer* playLayer, const BeamNode& winningNode);
    void triggerRewind(PlayLayer* playLayer);
    bool runSelfTest(PlayLayer* playLayer);

    bool m_isRunning = false;
    bool m_isCompleted = false;
    int m_levelID = 0;
    std::string m_levelName;
    float m_startX = 0.0f;
    float m_levelLength = 0.0f;
    float m_maxReachedX = 0.0f;
    uint32_t m_currentTick = 0;
    uint32_t m_deepestTick = 0;
    float m_stuckX = 0.0f;
    uint32_t m_ticksSinceProgress = 0;

    // Beam frontier and search width
    size_t m_currentWidth = 96;
    size_t m_rewindCount = 0;
    std::vector<BeamNode> m_frontier;
    std::vector<SavedLayer> m_savedLayers;
    std::vector<ArenaNode> m_arena;

    // Root state for replaying / verification
    CheckpointObject* m_rootCheckpoint = nullptr;
    PlayerSnapshot m_rootSnapshot;
    PlayerSnapshot m_rootSnapshot2;
    bool m_hasPlayer2 = false;

    std::vector<TickAction> m_resolvedMacro;
    std::vector<TrajectorySample> m_trajectorySamples;
    TelemetryMetrics m_telemetry;
};

} // namespace solver
