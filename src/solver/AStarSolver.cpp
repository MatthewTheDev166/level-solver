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

void AStarSolver::start(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1) return;

    reset();
    m_isRunning = true;
    m_isCompleted = false;

    if (playLayer->m_level) {
        m_levelID = playLayer->m_level->m_levelID.value();
    } else {
        m_levelID = 0;
    }

    m_startX = playLayer->m_player1->getPositionX();
    float endX = playLayer->getEndPosition().x;
    if (endX <= m_startX + 100.0f) {
        endX = m_startX + 20000.0f;
    }
    m_levelLength = endX;
    m_maxReachedX = m_startX;

    HazardDetector::buildIndex(playLayer->m_objects);
    DeterministicPRNG::clampSeed();
    CheatAPIIntegrator::notifyCheatStarted();
    HeadlessEngine::get().enableHeadless();

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;

    PlayerSnapshot initialSnap;
    initialSnap.capture(playLayer->m_player1, 0, DeterministicPRNG::STATIC_SEED);

    size_t nearbyObs = 0;
    float initialClearance = HazardDetector::calculateClearance(initialSnap.position, playLayer->m_objects, nearbyObs);

    SearchNode root;
    root.snapshot = initialSnap;
    root.gScore = m_startX;
    root.hazardClearance = initialClearance;
    root.fScore = calculateHeuristic(m_startX, 0, root.hazardClearance);
    root.action = ActionType::None;
    root.actionSwitches = 0;
    root.holdDuration = 0;
    root.parentIndex = UINT32_MAX;

    m_openQueue.push(root);

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "A* Search in progress...";
    m_telemetry.currentX = m_startX;
    m_telemetry.targetEndX = m_levelLength;
    geode::log::info("[LevelSolver] Solver started at X={}, target end X={}", m_startX, m_levelLength);
}

void AStarSolver::resume(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1 || m_openQueue.empty()) {
        start(playLayer);
        return;
    }

    m_isRunning = true;
    HazardDetector::buildIndex(playLayer->m_objects);
    DeterministicPRNG::clampSeed();
    CheatAPIIntegrator::notifyCheatStarted();
    HeadlessEngine::get().enableHeadless();

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "A* Search in progress...";
    geode::log::info("[LevelSolver] Solver resumed with {} open nodes", m_openQueue.size());
}

void AStarSolver::stop() {
    if (m_isRunning) {
        m_isRunning = false;
        HeadlessEngine::get().disableHeadless();
        CheatAPIIntegrator::notifyCheatEnded();
        HazardDetector::clearIndex();
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


void AStarSolver::reconstructSolution(uint32_t winningNodeIndex) {
    m_resolvedActions.clear();

    if (winningNodeIndex >= m_nodePool.size()) {
        geode::log::error("[LevelSolver] Invalid winning node index {}", winningNodeIndex);
        return;
    }

    // Trace path from winning node back to root
    std::vector<SearchNode> winningPath;
    uint32_t currIdx = winningNodeIndex;
    while (currIdx != UINT32_MAX && currIdx < m_nodePool.size()) {
        winningPath.push_back(m_nodePool[currIdx]);
        if (currIdx == 0) break;
        currIdx = m_nodePool[currIdx].parentIndex;
    }

    std::reverse(winningPath.begin(), winningPath.end());

    bool lastState = false;
    for (size_t i = 0; i < winningPath.size(); ++i) {
        const auto& node = winningPath[i];
        bool isPressed = (node.action == ActionType::Jump);
        if (isPressed != lastState || i == 0) {
            TickAction act;
            act.tick = node.snapshot.tick;
            act.pressed = isPressed;
            m_resolvedActions.push_back(act);
            lastState = isPressed;
        }
    }

    // Ensure final release at end
    if (lastState && !winningPath.empty()) {
        TickAction act;
        act.tick = winningPath.back().snapshot.tick + 1;
        act.pressed = false;
        m_resolvedActions.push_back(act);
    }

    // Save macro via MacroManager
    MacroManager::get().setActions(m_resolvedActions);
    MacroManager::get().saveMacro(m_levelID);

    geode::log::info("[LevelSolver] Reconstructed {} macro actions from {} path nodes",
        m_resolvedActions.size(), winningPath.size());
}


void AStarSolver::stepSearchBatch(PlayLayer* playLayer, uint32_t maxSteps) {
    if (!m_isRunning || m_isCompleted || !playLayer || !playLayer->m_player1) return;

    auto startBatch = std::chrono::high_resolution_clock::now();
    uint32_t stepsDone = 0;

    // Time budget per frame (12ms) ensures Geometry Dash remains completely smooth
    const auto timeBudget = std::chrono::milliseconds(12);

    while (!m_openQueue.empty() && stepsDone < maxSteps && m_isRunning) {
        if ((stepsDone & 15) == 0 && stepsDone > 0) {
            auto now = std::chrono::high_resolution_clock::now();
            if (now - startBatch >= timeBudget) {
                break;
            }
        }

        SearchNode current = m_openQueue.top();
        m_openQueue.pop();

        // Check if level solved (100%)
        if (current.snapshot.position.x >= m_levelLength) {
            m_isCompleted = true;
            m_isRunning = false;
            m_telemetry.status = SolverStatus::Solved;
            m_telemetry.detailMessage = "Level solved 100%! Ready for replay.";

            uint32_t winningIndex = static_cast<uint32_t>(m_nodePool.size());
            m_nodePool.push_back(current);

            reconstructSolution(winningIndex);

            HeadlessEngine::get().disableHeadless();
            HazardDetector::clearIndex();
            playLayer->resetLevel();
            MacroManager::get().startReplay(playLayer);
            break;
        }

        // Store into node history pool
        uint32_t parentIndexInPool = static_cast<uint32_t>(m_nodePool.size());
        m_nodePool.push_back(current);

        // Restore snapshot into player
        current.snapshot.restore(playLayer->m_player1);
        m_currentTick = current.snapshot.tick;

        float currentX = playLayer->m_player1->getPositionX();
        if (currentX > m_maxReachedX) {
            m_maxReachedX = currentX;
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

            playLayer->m_started = true;
            playLayer->m_inResetDelay = false;
            playLayer->m_playerDied = false;
            playLayer->m_resumeTimer = 0;
            playLayer->m_extraDelta = 0.0;

            // Apply candidate action
            if (act == ActionType::Jump) {
                playLayer->m_player1->pushButton(PlayerButton::Jump);
            } else {
                playLayer->m_player1->releaseButton(PlayerButton::Jump);
            }

            // Headless fixed-step physics advance
            playLayer->update(HeadlessEngine::FIXED_DT);
            stepsDone++;

            // If player died, backtrack
            if (playLayer->m_player1->m_isDead || playLayer->m_playerDied) {
                playLayer->m_player1->releaseButton(PlayerButton::Jump);
                playLayer->m_playerDied = false;
                continue;
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
                nextNode.parentIndex = parentIndexInPool;
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
    m_telemetry.currentX = m_maxReachedX;
    m_telemetry.targetEndX = m_levelLength;
    float totalDist = m_levelLength - m_startX;
    if (totalDist > 0.0f) {
        m_telemetry.explorationHorizon = std::clamp(((m_maxReachedX - m_startX) / totalDist) * 100.0f, 0.0f, 100.0f);
    }
    m_telemetry.openNodes = m_openQueue.size();
    m_telemetry.exploredNodes = m_nodePool.size();
    m_telemetry.prunedStates = m_spatialGrid.getPrunedCount();
    m_telemetry.ticksPerSecond = HeadlessEngine::get().getTicksPerSecond();
    m_telemetry.memoryFootprintBytes = (m_openQueue.size() + m_nodePool.size()) * sizeof(SearchNode) + m_spatialGrid.size() * 32;

    static uint32_t s_logThrottle = 0;
    if (++s_logThrottle % 60 == 0) {
        geode::log::info("[LevelSolver] Progress: horizon={:.1f}%, tick={}, open={}, pruned={}, rate={:.0f} tps, maxReachedX={:.0f}/{}",
            m_telemetry.explorationHorizon, m_currentTick, m_openQueue.size(), m_spatialGrid.getPrunedCount(), m_telemetry.ticksPerSecond, m_maxReachedX, m_levelLength);
    }

    if (m_openQueue.empty() && m_isRunning && !m_isCompleted) {
        m_isRunning = false;
        m_telemetry.status = SolverStatus::Failed;
        m_telemetry.detailMessage = "Search space exhausted without finding completion path.";
        HeadlessEngine::get().disableHeadless();
        HazardDetector::clearIndex();
        CheatAPIIntegrator::notifyCheatEnded();
    }
}

TelemetryMetrics AStarSolver::getTelemetry() const {
    return m_telemetry;
}

} // namespace solver
