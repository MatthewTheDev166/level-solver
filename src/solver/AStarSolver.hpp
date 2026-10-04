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

class AStarSolver {
public:
    static constexpr float ALPHA_ACTION_SWITCH = 0.5f;
    static constexpr float BETA_HAZARD_CLEARANCE = 0.2f;
    static constexpr float DEFAULT_HORIZON_PERCENT = 5.0f;
    static constexpr float DENSE_HORIZON_PERCENT = 2.0f;
    static constexpr size_t HIGH_OBSTACLE_DENSITY_THRESHOLD = 500;

    static AStarSolver& get();

    void start(PlayLayer* playLayer);
    void stop();
    void reset();

    bool isRunning() const;
    bool isCompleted() const;

    // Headless step loop called during PlayLayer::update
    void stepSearchBatch(PlayLayer* playLayer, uint32_t maxSteps = 5000);

    TelemetryMetrics getTelemetry() const;

private:
    AStarSolver() = default;

    float calculateHeuristic(float progressX, uint32_t actionSwitches, float clearance);
    float getTargetSegmentPercent(size_t nearbyObstacles) const;
    void commitCheckpoint(const SearchNode& node);
    void reconstructSolution();

    bool m_isRunning = false;
    bool m_isCompleted = false;

    float m_levelLength = 1000.0f;
    float m_startX = 0.0f;
    float m_currentCheckpointX = 0.0f;
    float m_currentMilestoneTargetX = 0.0f;
    float m_maxReachedX = 0.0f;

    std::priority_queue<SearchNode> m_openQueue;
    SpatialGrid m_spatialGrid;

    // Checkpoint history for path reconstruction
    std::vector<SearchNode> m_milestoneCheckpoints;
    std::vector<TickAction> m_resolvedActions;

    // Local segment nodes for backtracking
    std::vector<SearchNode> m_nodePool;

    TelemetryMetrics m_telemetry;
    uint32_t m_currentTick = 0;
    int m_levelID = 0;
};

} // namespace solver
