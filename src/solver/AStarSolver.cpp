#include "AStarSolver.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../replay/MacroManager.hpp"
#include <chrono>
#include <algorithm>

namespace solver {

AStarSolver& AStarSolver::get() {
    static AStarSolver instance;
    return instance;
}

float AStarSolver::calculateHeuristic(float progressX, uint32_t actionSwitches, float clearance) {
    return progressX - (ALPHA_ACTION_SWITCH * static_cast<float>(actionSwitches)) + (BETA_HAZARD_CLEARANCE * clearance);
}

float AStarSolver::getTargetSegmentPercent(size_t nearbyObstacles) const {
    if (nearbyObstacles >= HIGH_OBSTACLE_DENSITY_THRESHOLD) {
        return DENSE_HORIZON_PERCENT; // Adaptive 2% segment in dense areas
    }
    return DEFAULT_HORIZON_PERCENT; // 5% default segment
}

void AStarSolver::start(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1) return;

    reset();
    m_isRunning = true;
    m_isCompleted = false;

    // Retrieve level parameters
    if (playLayer->m_level) {
        m_levelID = playLayer->m_level->m_levelID.value();
    } else {
        m_levelID = 0;
    }

    m_startX = playLayer->m_player1->getPositionX();
    // Estimate level length using end position or level settings
    float endX = playLayer->getEndPosition().x;
    if (endX <= m_startX + 100.0f) {
        // Fallback length based on song length / speed if end position is unset
        endX = m_startX + 20000.0f;
    }
    m_levelLength = endX;
    m_currentCheckpointX = m_startX;
    m_maxReachedX = m_startX;

    // Initialize adaptive segment target
    size_t initialObstacles = 0;
    HazardDetector::calculateClearance(playLayer->m_player1->getPosition(), playLayer->m_objects, initialObstacles);
    float segPercent = getTargetSegmentPercent(initialObstacles);
    m_currentMilestoneTargetX = m_startX + (m_levelLength - m_startX) * (segPercent / 100.0f);

    // Seed deterministic PRNG
    DeterministicPRNG::clampSeed();

    // Notify CheatAPI
    CheatAPIIntegrator::notifyCheatStarted();

    // Enable headless simulation
    HeadlessEngine::get().enableHeadless();

    // Create root node
    PlayerSnapshot initialSnap;
    initialSnap.capture(playLayer->m_player1, 0, DeterministicPRNG::STATIC_SEED);

    SearchNode root;
    root.snapshot = initialSnap;
    root.gScore = m_startX;
    root.hazardClearance = 100.0f;
    root.fScore = calculateHeuristic(m_startX, 0, root.hazardClearance);
    root.action = ActionType::None;
    root.actionSwitches = 0;
    root.holdDuration = 0;

    m_openQueue.push(root);
    m_milestoneCheckpoints.push_back(root);

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "A* Search in progress...";
    geode::log::info("[LevelSolver] Solver started at X={}, target end X={}", m_startX, m_levelLength);
}

void AStarSolver::stop() {
    if (m_isRunning) {
        m_isRunning = false;
        HeadlessEngine::get().disableHeadless();
        CheatAPIIntegrator::notifyCheatEnded();
        m_telemetry.status = SolverStatus::Paused;
        m_telemetry.detailMessage = "Solver paused by user";
        geode::log::info("[LevelSolver] Solver stopped");
    }
}

void AStarSolver::reset() {
    m_isRunning = false;
    m_isCompleted = false;
    m_openQueue = std::priority_queue<SearchNode>();
    m_spatialGrid.clear();
    m_spatialGrid.resetPrunedCount();
    m_milestoneCheckpoints.clear();
    m_resolvedActions.clear();
    m_nodePool.clear();
    m_currentTick = 0;
    m_telemetry = TelemetryMetrics();
}

bool AStarSolver::isRunning() const {
    return m_isRunning;
}

bool AStarSolver::isCompleted() const {
    return m_isCompleted;
}

void AStarSolver::commitCheckpoint(const SearchNode& node) {
    m_milestoneCheckpoints.push_back(node);
    m_currentCheckpointX = node.snapshot.position.x;

    // Flush open queue and retain the milestone node
    m_openQueue = std::priority_queue<SearchNode>();
    m_openQueue.push(node);

    // Retain spatial grid but prune states from old segment
    m_spatialGrid.clear();

    geode::log::info("[LevelSolver] Committed milestone checkpoint at X={:.2f} ({:.1f}%)",
        m_currentCheckpointX,
        ((m_currentCheckpointX - m_startX) / (m_levelLength - m_startX)) * 100.0f
    );
}

void AStarSolver::reconstructSolution() {
    m_resolvedActions.clear();
    bool lastState = false;

    for (size_t i = 1; i < m_nodePool.size(); ++i) {
        const auto& node = m_nodePool[i];
        bool isPressed = (node.action == ActionType::Jump);
        if (isPressed != lastState || i == 1) {
            TickAction act;
            act.tick = node.snapshot.tick;
            act.pressed = isPressed;
            m_resolvedActions.push_back(act);
            lastState = isPressed;
        }
    }

    // Save macro via MacroManager
    MacroManager::get().setActions(m_resolvedActions);
    MacroManager::get().saveMacro(m_levelID);

    geode::log::info("[LevelSolver] Reconstructed {} discrete macro actions", m_resolvedActions.size());
}

void AStarSolver::stepSearchBatch(PlayLayer* playLayer, uint32_t maxSteps) {
    if (!m_isRunning || m_isCompleted || !playLayer || !playLayer->m_player1) return;

    auto startBatch = std::chrono::high_resolution_clock::now();
    uint32_t stepsDone = 0;

    while (!m_openQueue.empty() && stepsDone < maxSteps && m_isRunning) {
        SearchNode current = m_openQueue.top();
        m_openQueue.pop();

        // Restore snapshot into player
        current.snapshot.restore(playLayer->m_player1);
        m_currentTick = current.snapshot.tick;

        float currentX = playLayer->m_player1->getPositionX();
        if (currentX > m_maxReachedX) {
            m_maxReachedX = currentX;
        }

        // Store into node history pool
        m_nodePool.push_back(current);

        // Check if level solved (100%)
        if (currentX >= m_levelLength) {
            m_isCompleted = true;
            m_isRunning = false;
            m_telemetry.status = SolverStatus::Solved;
            m_telemetry.detailMessage = "Level solved 100%! Ready for replay.";
            commitCheckpoint(current);
            reconstructSolution();

            // Disable headless mode and trigger replay
            HeadlessEngine::get().disableHeadless();
            playLayer->resetLevel();
            MacroManager::get().startReplay(playLayer);
            break;
        }

        // Check if reached rolling-horizon milestone
        if (currentX >= m_currentMilestoneTargetX) {
            size_t nearbyObstacles = 0;
            HazardDetector::calculateClearance(playLayer->m_player1->getPosition(), playLayer->m_objects, nearbyObstacles);
            float segPercent = getTargetSegmentPercent(nearbyObstacles);
            m_currentMilestoneTargetX = currentX + (m_levelLength - m_startX) * (segPercent / 100.0f);
            commitCheckpoint(current);
        }

        // Gamemode-specific candidate action branching
        auto actions = ActionQuantizer::getCandidateActions(
            playLayer->m_player1,
            playLayer->m_objects,
            current.snapshot.tick,
            current.action,
            current.holdDuration
        );

        for (ActionType act : actions) {
            // Restore parent state prior to each branch step
            current.snapshot.restore(playLayer->m_player1);

            // Apply candidate action
            if (act == ActionType::Jump) {
                playLayer->m_player1->pushButton(PlayerButton::Jump);
            } else {
                playLayer->m_player1->releaseButton(PlayerButton::Jump);
            }

            // Headless fixed-step physics advance
            playLayer->update(HeadlessEngine::FIXED_DT);
            stepsDone++;

            // If player died, immediately backtrack
            if (playLayer->m_player1->m_isDead) {
                playLayer->m_player1->releaseButton(PlayerButton::Jump);
                continue; // Backtrack without queuing
            }

            // Survived: capture child state
            PlayerSnapshot nextSnap;
            nextSnap.capture(playLayer->m_player1, current.snapshot.tick + 1);

            size_t nearbyObs = 0;
            float clearance = HazardDetector::calculateClearance(nextSnap.position, playLayer->m_objects, nearbyObs);
            uint32_t switches = current.actionSwitches + (act != current.action ? 1 : 0);
            uint32_t holdDur = (act == ActionType::Jump) ? (current.holdDuration + 1) : 0;
            float fScore = calculateHeuristic(nextSnap.position.x, switches, clearance);

            // Spatial hashing grid pruning
            if (!m_spatialGrid.shouldPrune(nextSnap, fScore)) {
                SearchNode nextNode;
                nextNode.snapshot = nextSnap;
                nextNode.gScore = nextSnap.position.x;
                nextNode.fScore = fScore;
                nextNode.hazardClearance = clearance;
                nextNode.parentIndex = static_cast<uint32_t>(m_nodePool.size() - 1);
                nextNode.action = act;
                nextNode.actionSwitches = switches;
                nextNode.holdDuration = holdDur;

                m_openQueue.push(nextNode);
            }
        }
    }

    auto endBatch = std::chrono::high_resolution_clock::now();
    double batchDuration = std::chrono::duration<double>(endBatch - startBatch).count();
    HeadlessEngine::get().recordStepBatch(stepsDone, batchDuration);

    // Update telemetry metrics
    m_telemetry.currentTick = m_currentTick;
    float totalDist = m_levelLength - m_startX;
    if (totalDist > 0.0f) {
        m_telemetry.explorationHorizon = std::clamp(((m_maxReachedX - m_startX) / totalDist) * 100.0f, 0.0f, 100.0f);
    }
    m_telemetry.openNodes = m_openQueue.size();
    m_telemetry.prunedStates = m_spatialGrid.getPrunedCount();
    m_telemetry.ticksPerSecond = HeadlessEngine::get().getTicksPerSecond();
    m_telemetry.memoryFootprintBytes = (m_openQueue.size() + m_nodePool.size()) * sizeof(SearchNode) + m_spatialGrid.size() * 32;

    if (m_openQueue.empty() && m_isRunning && !m_isCompleted) {
        m_isRunning = false;
        m_telemetry.status = SolverStatus::Failed;
        m_telemetry.detailMessage = "Search space exhausted without finding completion path.";
        HeadlessEngine::get().disableHeadless();
        CheatAPIIntegrator::notifyCheatEnded();
    }
}

TelemetryMetrics AStarSolver::getTelemetry() const {
    return m_telemetry;
}

} // namespace solver
