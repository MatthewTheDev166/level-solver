#pragma once

#include "../core/Types.hpp"
#include "../engine/SpatialGrid.hpp"
#include "../engine/HazardDetector.hpp"
#include "../engine/ActionQuantizer.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <queue>
#include <vector>
#include <memory>

namespace solver {

/**
 * AStarSolver: Forward state-space A* tree search with spatial hashing.
 * Note: SwarmSolver (Genetic Swarm) is the primary solver driving TelemetryPopup.
 * AStarSolver is retained as a deterministic reference implementation.
 */
class AStarSolver {
public:
    static constexpr float ALPHA_ACTION_SWITCH = 0.5f;
    static constexpr float BETA_HAZARD_CLEARANCE = 0.2f;

    static AStarSolver& get();

    void start(PlayLayer* playLayer);
    void resume(PlayLayer* playLayer);
    void stop();
    void reset();

    bool isRunning() const;
    bool isCompleted() const;

    // Headless step loop called during PlayLayer::update
    void stepSearchBatch(PlayLayer* playLayer, uint32_t maxSteps = 600);

    TelemetryMetrics getTelemetry() const;

private:
    AStarSolver() = default;

    float calculateHeuristic(float progressX, uint32_t actionSwitches, float clearance);
    void reconstructSolution(uint32_t winningNodeIndex);

    bool m_isRunning = false;
    bool m_isCompleted = false;

    float m_levelLength = 1000.0f;
    float m_startX = 0.0f;
    float m_maxReachedX = 0.0f;

    std::priority_queue<SearchNode> m_openQueue;
    SpatialGrid m_spatialGrid;

    // Node pool storing all explored tree nodes with parent pointers
    std::vector<SearchNode> m_nodePool;
    std::vector<TickAction> m_resolvedActions;

    TelemetryMetrics m_telemetry;
    uint32_t m_currentTick = 0;
    int m_levelID = 0;

    CheckpointObject* m_milestoneCheckpoint = nullptr;
    float m_lastMilestoneX = 0.0f;
    size_t m_nearbyObstaclesDensity = 0;
};

} // namespace solver
