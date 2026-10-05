#include "BeamSolver.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../replay/MacroManager.hpp"
#include "../replay/GDRExporter.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace solver {

struct DedupeKey {
    uint8_t mode = 0;
    bool upsideDown = false;
    bool mini = false;
    bool isOnGround = false;
    bool buttonDown = false;
    bool isDashing = false;
    int32_t quantY = 0;
    int32_t quantYVel = 0;
    int32_t quantX = 0;
    int32_t quantSpeed = 0;

    bool operator==(const DedupeKey& o) const {
        return mode == o.mode &&
               upsideDown == o.upsideDown &&
               mini == o.mini &&
               isOnGround == o.isOnGround &&
               buttonDown == o.buttonDown &&
               isDashing == o.isDashing &&
               quantY == o.quantY &&
               quantYVel == o.quantYVel &&
               quantX == o.quantX &&
               quantSpeed == o.quantSpeed;
    }
};

struct DedupeKeyHash {
    size_t operator()(const DedupeKey& k) const {
        size_t h = 0x9e3779b9;
        auto hashCombine = [&h](size_t val) {
            h ^= val + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        hashCombine(k.mode);
        hashCombine((k.upsideDown ? 1 : 0) | (k.mini ? 2 : 0) | (k.isOnGround ? 4 : 0) | (k.buttonDown ? 8 : 0) | (k.isDashing ? 16 : 0));
        hashCombine(static_cast<size_t>(k.quantY));
        hashCombine(static_cast<size_t>(k.quantYVel));
        hashCombine(static_cast<size_t>(k.quantX));
        hashCombine(static_cast<size_t>(k.quantSpeed));
        return h;
    }
};

static bool isSimulationFinished(PlayLayer* playLayer, float levelLength, float startX) {
    if (!playLayer || !playLayer->m_player1) return false;
    if (playLayer->m_level && playLayer->m_level->isPlatformer()) {
        return playLayer->m_hasCompletedLevel;
    }
    if (playLayer->m_hasCompletedLevel) return true;

    float currentX = playLayer->m_player1->getPositionX();
    if (currentX >= levelLength) return true;
    if (levelLength > startX + 50.0f && currentX >= (levelLength - 15.0f)) return true;

    float percent = playLayer->getCurrentPercent();
    if (percent >= 99.0f) return true;

    return false;
}

BeamSolver& BeamSolver::get() {
    static BeamSolver instance;
    return instance;
}

void BeamSolver::start(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1) return;

    reset();

    if (playLayer->m_level && (playLayer->m_level->isPlatformer() || playLayer->m_level->m_twoPlayerMode)) {
        geode::log::warn("[LevelSolver] Platformer and 2-Player modes are unsupported.");
        m_isRunning = false;
        m_telemetry.status = SolverStatus::Failed;
        m_telemetry.detailMessage = "Platformer and 2-Player modes unsupported";
        return;
    }

    m_isRunning = true;
    m_isCompleted = false;

    if (playLayer->m_level) {
        m_levelID = playLayer->m_level->m_levelID.value();
        m_levelName = playLayer->m_level->m_levelName;
    } else {
        m_levelID = 0;
        m_levelName = "local_level";
    }

    m_startX = playLayer->m_player1->getPositionX();
    float maxObjX = m_startX;
    if (playLayer->m_objects) {
        for (unsigned int i = 0; i < playLayer->m_objects->count(); ++i) {
            if (auto obj = static_cast<GameObject*>(playLayer->m_objects->objectAtIndex(i))) {
                if (obj->getPositionX() > maxObjX) {
                    maxObjX = obj->getPositionX();
                }
            }
        }
    }

    float robtopEndX = playLayer->getEndPosition().x;
    if (robtopEndX > m_startX + 50.0f) {
        m_levelLength = robtopEndX;
    } else if (maxObjX > m_startX + 50.0f) {
        m_levelLength = maxObjX;
    } else {
        // Blank level with no objects
        m_levelLength = m_startX + 600.0f;
    }
    m_maxReachedX = m_startX;
    m_currentTick = 0;
    m_deepestTick = 0;
    m_currentWidth = 96;
    m_rewindCount = 0;
    m_stuckX = 0.0f;
    m_ticksSinceProgress = 0;

    HazardDetector::buildIndex(playLayer->m_objects);
    DeterministicPRNG::clampSeed();
    CheatAPIIntegrator::notifyCheatStarted();
    HeadlessEngine::get().enableHeadless();

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;
    playLayer->m_isPaused = false;

    if (playLayer->m_player1->getPositionY() <= 106.0f) {
        playLayer->m_player1->m_isOnGround = true;
    }

    if (playLayer->m_anticheatSpike) {
        playLayer->m_anticheatSpike->setPosition({-9999.0f, -9999.0f});
    }

    if (playLayer->m_endPosition.x <= m_startX + 50.0f) {
        playLayer->m_endPosition = cocos2d::CCPoint{ m_levelLength, 0.0f };
    }
    playLayer->m_hasCompletedLevel = false;

    if (playLayer->m_checkpointArray) {
        playLayer->m_checkpointArray->removeAllObjects();
    }

    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
    playLayer->updateVisibility(0.0f);

    m_rootCheckpoint = playLayer->createCheckpoint();
    if (m_rootCheckpoint) {
        m_rootCheckpoint->retain();
    }
    m_rootSnapshot.capture(playLayer->m_player1, 0, DeterministicPRNG::STATIC_SEED);
    bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
    m_hasPlayer2 = isDual;
    if (isDual) {
        m_rootSnapshot2.capture(playLayer->m_player2, 0, DeterministicPRNG::STATIC_SEED);
    }

    // Build initial root node
    BeamNode root;
    root.checkpoint = m_rootCheckpoint;
    if (root.checkpoint) root.checkpoint->retain();
    root.p1 = m_rootSnapshot;
    root.p2 = m_rootSnapshot2;
    root.hasP2 = m_hasPlayer2;
    root.tick = 0;
    root.arenaIndex = -1;
    root.buttonDown = false;
    root.x = m_startX;
    root.y = m_rootSnapshot.position.y;
    root.clearance = 100.0f;
    m_frontier.push_back(std::move(root));

    // Save initial layer
    SavedLayer sl;
    sl.layerTick = 0;
    sl.maxReachedX = m_startX;
    sl.nodes = m_frontier;
    m_savedLayers.push_back(std::move(sl));

    // Run death detection self-test to detect noclip/blocking mods
    if (!runSelfTest(playLayer)) {
        m_isRunning = false;
        HeadlessEngine::get().disableHeadless();
        HazardDetector::clearIndex();
        CheatAPIIntegrator::notifyCheatEnded();
        m_telemetry.status = SolverStatus::Failed;
        m_telemetry.detailMessage = "Another mod (e.g. Mega Hack noclip or hacks) is blocking death detection. Disable it and retry.";
        FLAlertLayer::create("Solver Error", m_telemetry.detailMessage.c_str(), "OK")->show();
        return;
    }

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "Beam search active...";
    m_telemetry.currentX = m_startX;
    m_telemetry.targetEndX = m_levelLength;
    m_telemetry.frontierSize = m_frontier.size();
    m_telemetry.currentWidth = m_currentWidth;
    m_telemetry.rewindCount = m_rewindCount;
    m_telemetry.deepestTick = 0;
    m_telemetry.stuckX = 0.0f;
    m_telemetry.isVerified = false;

    geode::log::info("[LevelSolver] BeamSolver started at X={:.1f}, target end X={:.1f}, mode={}, hazards={}",
        m_startX, m_levelLength, static_cast<int>(m_rootSnapshot.mode), HazardDetector::hasHazards());
}

void BeamSolver::resume(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1 || m_frontier.empty()) {
        start(playLayer);
        return;
    }

    m_isRunning = true;
    HazardDetector::buildIndex(playLayer->m_objects);
    DeterministicPRNG::clampSeed();
    CheatAPIIntegrator::notifyCheatStarted();
    HeadlessEngine::get().enableHeadless();

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "Beam search resumed...";
    geode::log::info("[LevelSolver] BeamSolver resumed at tick {}", m_currentTick);
}

void BeamSolver::stop() {
    if (m_isRunning) {
        m_isRunning = false;
        HeadlessEngine::get().disableHeadless();
        CheatAPIIntegrator::notifyCheatEnded();
        HazardDetector::clearIndex();
        m_telemetry.status = SolverStatus::Paused;
        m_telemetry.detailMessage = "Search paused by user";
        geode::log::info("[LevelSolver] BeamSolver stopped");
    }
}

void BeamSolver::reset() {
    m_isRunning = false;
    m_isCompleted = false;

    if (m_rootCheckpoint) {
        m_rootCheckpoint->release();
        m_rootCheckpoint = nullptr;
    }

    m_frontier.clear();
    m_savedLayers.clear();
    m_arena.clear();
    m_resolvedMacro.clear();
    m_trajectorySamples.clear();

    m_startX = 0.0f;
    m_levelLength = 0.0f;
    m_maxReachedX = 0.0f;
    m_currentTick = 0;
    m_deepestTick = 0;
    m_stuckX = 0.0f;
    m_ticksSinceProgress = 0;
    m_currentWidth = 96;
    m_rewindCount = 0;
    m_hasPlayer2 = false;

    m_telemetry = TelemetryMetrics();
}

bool BeamSolver::isRunning() const {
    return m_isRunning;
}

bool BeamSolver::isCompleted() const {
    return m_isCompleted;
}

TelemetryMetrics BeamSolver::getTelemetry() const {
    return m_telemetry;
}

const std::vector<TickAction>& BeamSolver::getResolvedMacro() const {
    return m_resolvedMacro;
}

const std::vector<TrajectorySample>& BeamSolver::getTrajectory() const {
    return m_trajectorySamples;
}

bool BeamSolver::runSelfTest(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1) return true;
    if (!HazardDetector::hasHazards()) return true;

    // Simulate up to 400 ticks pure-idle: if player passes hazards and completes the level, noclip is active!
    if (m_rootCheckpoint) {
        playLayer->loadFromCheckpoint(m_rootCheckpoint);
    }
    m_rootSnapshot.restore(playLayer->m_player1);
    if (m_hasPlayer2 && playLayer->m_player2) {
        m_rootSnapshot2.restore(playLayer->m_player2);
    }

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    playLayer->m_queuedButtons.clear();
    playLayer->m_player1->releaseButton(PlayerButton::Jump);

    bool reachedEndWithoutDying = false;
    for (uint32_t t = 0; t < 400; ++t) {
        playLayer->update(HeadlessEngine::FIXED_DT);
        playLayer->moveCameraToPos(playLayer->m_player1->getPosition());

        if (playLayer->m_player1->m_isDead || (playLayer->m_player2 && playLayer->m_player2->m_isDead) || playLayer->m_playerDied) {
            // Player died on hazards as expected! Death detection is working.
            break;
        }

        if (isSimulationFinished(playLayer, m_levelLength, m_startX)) {
            reachedEndWithoutDying = true;
            break;
        }
    }

    // Restore root state
    if (m_rootCheckpoint) {
        playLayer->loadFromCheckpoint(m_rootCheckpoint);
    }
    m_rootSnapshot.restore(playLayer->m_player1);
    if (m_hasPlayer2 && playLayer->m_player2) {
        m_rootSnapshot2.restore(playLayer->m_player2);
    }
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
    playLayer->m_queuedButtons.clear();

    if (reachedEndWithoutDying) {
        geode::log::error("[LevelSolver] Self-test failed: idle run completed level without dying! Death detection is blocked by another mod.");
        return false;
    }
    return true;
}

void BeamSolver::triggerRewind(PlayLayer* playLayer) {
    m_rewindCount++;
    m_ticksSinceProgress = 0;

    bool sameStuck = (m_stuckX > 0.0f && std::abs(m_maxReachedX - m_stuckX) < 25.0f);
    m_stuckX = m_maxReachedX;

    // Double search width up to 1536
    if (m_currentWidth < 1536) {
        m_currentWidth = std::min(m_currentWidth * 2, static_cast<size_t>(1536));
    } else if (sameStuck || (m_rewindCount % 2 == 0)) {
        // At maximum width or repeatedly stuck near the same spot:
        // Pop the current layer to rewind progressively deeper!
        if (!m_savedLayers.empty()) {
            m_savedLayers.pop_back();
        }
    }

    if (m_savedLayers.empty()) {
        // Rewind to root
        m_frontier.clear();
        BeamNode root;
        root.checkpoint = m_rootCheckpoint;
        if (root.checkpoint) root.checkpoint->retain();
        root.p1 = m_rootSnapshot;
        root.p2 = m_rootSnapshot2;
        root.hasP2 = m_hasPlayer2;
        root.tick = 0;
        root.arenaIndex = -1;
        root.buttonDown = false;
        root.x = m_startX;
        root.y = m_rootSnapshot.position.y;
        root.clearance = 100.0f;
        m_frontier.push_back(std::move(root));
        m_currentTick = 0;
        m_maxReachedX = m_startX;
        geode::log::warn("[LevelSolver] Rewinding to ROOT with W={} (rewind #{})",
            m_currentWidth, m_rewindCount);
        return;
    }

    SavedLayer targetLayer = m_savedLayers.back();
    m_frontier = targetLayer.nodes;
    m_currentTick = targetLayer.layerTick;

    geode::log::warn("[LevelSolver] Search wiped out near X={:.1f}, rewinding to tick {} (X={:.1f}) with W={} (rewind #{})",
        m_stuckX, targetLayer.layerTick, targetLayer.maxReachedX, m_currentWidth, m_rewindCount);
}

void BeamSolver::finalizeSolution(PlayLayer* playLayer, const BeamNode& winningNode) {
    geode::log::info("[LevelSolver] Winning candidate reached level end (tick {}, X={:.1f})! Verifying solution...",
        winningNode.tick, winningNode.x);

    // 1. Walk parent pointers in m_arena
    std::vector<TickAction> rawActions;
    int32_t curr = winningNode.arenaIndex;
    while (curr >= 0 && curr < static_cast<int32_t>(m_arena.size())) {
        rawActions.push_back({ m_arena[curr].tick, m_arena[curr].buttonDown });
        curr = m_arena[curr].parentIndex;
    }
    std::reverse(rawActions.begin(), rawActions.end());

    // 2. Compress into edge-triggered actions
    std::vector<TickAction> compressedActions;
    bool lastBtn = false;
    for (const auto& act : rawActions) {
        if (compressedActions.empty()) {
            if (act.pressed) {
                compressedActions.push_back(act);
                lastBtn = true;
            }
        } else if (act.pressed != lastBtn) {
            compressedActions.push_back(act);
            lastBtn = act.pressed;
        }
    }
    if (!compressedActions.empty() && compressedActions.back().pressed) {
        compressedActions.push_back({ compressedActions.back().tick + 1, false });
    }

    // 3. Hazard verification check: refuse to save 0-input macro if level has hazards
    bool hasHazards = HazardDetector::hasHazards();
    if (hasHazards && compressedActions.empty()) {
        geode::log::error("[LevelSolver] Refusing to save 0-input macro on level with hazards! Continuing search...");
        triggerRewind(playLayer);
        return;
    }

    // 4. Verification simulation run from tick 0
    if (m_rootCheckpoint) {
        playLayer->loadFromCheckpoint(m_rootCheckpoint);
    }
    m_rootSnapshot.restore(playLayer->m_player1);
    if (m_hasPlayer2 && playLayer->m_player2) {
        m_rootSnapshot2.restore(playLayer->m_player2);
    }

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
    playLayer->m_queuedButtons.clear();
    playLayer->m_player1->releaseButton(PlayerButton::Jump);

    std::vector<TrajectorySample> trajectory;
    size_t actionIdx = 0;
    bool currBtn = false;
    bool lastSimBtn = false;
    bool verified = true;
    uint32_t totalTicks = winningNode.tick + 10;

    for (uint32_t t = 0; t <= totalTicks; ++t) {
        while (actionIdx < compressedActions.size() && compressedActions[actionIdx].tick <= t) {
            currBtn = compressedActions[actionIdx].pressed;
            actionIdx++;
        }
        if (currBtn != lastSimBtn) {
            playLayer->handleButton(currBtn, 1, true);
            lastSimBtn = currBtn;
        }

        if (t % 60 == 0) {
            trajectory.push_back({ t, playLayer->m_player1->getPositionX(), playLayer->m_player1->getPositionY() });
        }

        playLayer->update(HeadlessEngine::FIXED_DT);
        playLayer->moveCameraToPos(playLayer->m_player1->getPosition());

        bool dead = playLayer->m_player1->m_isDead || (playLayer->m_player2 && playLayer->m_player2->m_isDead) || playLayer->m_playerDied;
        if (dead) {
            geode::log::warn("[LevelSolver] verification FAILED at tick {} (died at X={:.1f})! Continuing search...",
                t, playLayer->m_player1->getPositionX());
            verified = false;
            break;
        }

        if (isSimulationFinished(playLayer, m_levelLength, m_startX)) {
            break;
        }
    }

    if (lastSimBtn) {
        playLayer->handleButton(false, 1, true);
    }
    playLayer->m_queuedButtons.clear();

    if (!verified) {
        triggerRewind(playLayer);
        return;
    }

    // Solution verified successfully!
    m_resolvedMacro = compressedActions;
    m_trajectorySamples = trajectory;
    m_isCompleted = true;
    m_isRunning = false;

    MacroManager::get().setActions(m_resolvedMacro);
    MacroManager::get().setTrajectory(m_trajectorySamples);
    MacroManager::get().saveMacro(m_levelID, m_levelName);

    auto exportRes = GDRExporter::exportReplays(m_levelName, m_levelID, m_resolvedMacro);
    if (exportRes.success) {
        geode::log::info("[LevelSolver] Exported Mega Hack macro to: {}", exportRes.gdr2Path.string());
    }

    HeadlessEngine::get().disableHeadless();
    HazardDetector::clearIndex();
    CheatAPIIntegrator::notifyCheatEnded();

    m_telemetry.status = SolverStatus::Solved;
    m_telemetry.explorationHorizon = 100.0f;
    m_telemetry.isVerified = true;
    m_telemetry.detailMessage = fmt::format("100% Solved & Verified! Saved {} inputs.", m_resolvedMacro.size());
    geode::log::info("[LevelSolver] Level solved 100% and verified! Saved {} macro actions to disk.", m_resolvedMacro.size());
}

void BeamSolver::stepBeamBatch(PlayLayer* playLayer, uint32_t maxSteps) {
    if (!m_isRunning || m_isCompleted || !playLayer || !playLayer->m_player1) return;

    auto startBatch = std::chrono::high_resolution_clock::now();
    const auto timeBudget = std::chrono::milliseconds(12);

    uint32_t simulatedTicks = 0;

    while (true) {
        if (m_frontier.empty()) {
            triggerRewind(playLayer);
            if (!m_isRunning) return;
            break;
        }

        // Check if any node in frontier is finished
        for (const auto& node : m_frontier) {
            if (node.x >= m_levelLength || (node.x >= m_levelLength - 100.0f && node.p1.position.x >= m_levelLength - 100.0f)) {
                finalizeSolution(playLayer, node);
                return;
            }
        }

        // Determine decision step k based on lead node
        float leadX = m_frontier.front().p1.position.x;
        for (const auto& n : m_frontier) {
            if (n.p1.position.x > leadX) leadX = n.p1.position.x;
        }

        VehicleMode leadMode = m_frontier.front().p1.mode;
        bool isWave = (leadMode == VehicleMode::Wave);
        bool isNearObject = HazardDetector::isNearAnyObject(leadX, isWave ? 80.0f : 60.0f);

        uint32_t k = (isWave && isNearObject) ? 1 : (isNearObject ? 1 : 2);

        // Expand frontier: for each node in frontier, try action = false and action = true
        std::unordered_map<DedupeKey, BeamNode, DedupeKeyHash> uniqueChildren;
        uniqueChildren.reserve(m_frontier.size() * 2);

        bool won = false;
        BeamNode winningChild;

        for (const auto& parentNode : m_frontier) {
            for (bool action : { parentNode.buttonDown, !parentNode.buttonDown }) {
                // Restore parent state
                if (parentNode.checkpoint) {
                    playLayer->loadFromCheckpoint(parentNode.checkpoint);
                }
                parentNode.p1.restore(playLayer->m_player1);
                if (parentNode.hasP2 && playLayer->m_player2) {
                    parentNode.p2.restore(playLayer->m_player2);
                }

                playLayer->m_started = true;
                playLayer->m_inResetDelay = false;
                playLayer->m_playerDied = false;
                playLayer->m_player1->m_isDead = false;
                if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
                playLayer->m_queuedButtons.clear();

                // Dispatch button if action differs from parent's current button state
                if (action != parentNode.buttonDown) {
                    playLayer->handleButton(action, 1, true);
                }

                // Simulate k ticks
                bool childDead = false;
                float prevStepX = playLayer->m_player1->getPositionX();
                uint32_t jammedTicks = 0;

                for (uint32_t step = 0; step < k; ++step) {
                    playLayer->update(HeadlessEngine::FIXED_DT);
                    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
                    simulatedTicks++;

                    // Check death
                    bool isDead = playLayer->m_player1->m_isDead ||
                                  (playLayer->m_player2 && playLayer->m_player2->m_isDead) ||
                                  playLayer->m_playerDied;
                    if (isDead) {
                        childDead = true;
                        break;
                    }

                    // Check wall jam
                    float currX = playLayer->m_player1->getPositionX();
                    bool goingLeft = playLayer->m_player1->m_isGoingLeft;
                    float dx = goingLeft ? (prevStepX - currX) : (currX - prevStepX);
                    if (dx < 0.001f && !playLayer->m_player1->m_isDashing && !playLayer->m_player1->m_isSpider) {
                        jammedTicks++;
                        if (jammedTicks >= 4) {
                            childDead = true;
                            break;
                        }
                    } else {
                        jammedTicks = 0;
                    }
                    prevStepX = currX;

                    // Check level completion
                    if (isSimulationFinished(playLayer, m_levelLength, m_startX)) {
                        won = true;
                        winningChild.p1.capture(playLayer->m_player1, parentNode.tick + step + 1);
                        winningChild.tick = parentNode.tick + step + 1;
                        winningChild.buttonDown = action;
                        winningChild.x = playLayer->m_player1->getPositionX();
                        winningChild.y = playLayer->m_player1->getPositionY();
                        int32_t aidx = static_cast<int32_t>(m_arena.size());
                        m_arena.push_back({ parentNode.arenaIndex, winningChild.tick, action });
                        winningChild.arenaIndex = aidx;
                        break;
                    }
                }

                if (childDead) {
                    continue;
                }

                if (won) {
                    break;
                }

                // Child survived!
                BeamNode child;
                child.p1.capture(playLayer->m_player1, parentNode.tick + k);
                bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
                child.hasP2 = isDual;
                if (isDual) {
                    child.p2.capture(playLayer->m_player2, parentNode.tick + k);
                }
                child.tick = parentNode.tick + k;
                child.buttonDown = action;
                child.x = playLayer->m_player1->getPositionX();
                child.y = playLayer->m_player1->getPositionY();

                // Create native checkpoint for every child to guarantee 100% collision sweep & physics accuracy
                child.checkpoint = playLayer->createCheckpoint();
                if (child.checkpoint) {
                    child.checkpoint->retain();
                }

                // Register into arena
                int32_t aidx = static_cast<int32_t>(m_arena.size());
                m_arena.push_back({ parentNode.arenaIndex, child.tick, action });
                child.arenaIndex = aidx;

                // Calculate clearance
                size_t nearbyCount = 0;
                child.clearance = HazardDetector::calculateClearance(child.p1.position, playLayer->m_objects, nearbyCount);

                // Build dedupe key
                DedupeKey key;
                key.mode = static_cast<uint8_t>(child.p1.mode);
                key.upsideDown = child.p1.isUpsideDown;
                key.mini = (child.p1.vehicleSize < 0.9f);
                key.isOnGround = child.p1.isOnGround;
                key.buttonDown = child.buttonDown;
                key.isDashing = child.p1.isDashing;
                if (child.p1.mode == VehicleMode::Wave || child.p1.mode == VehicleMode::Ship) {
                    key.quantY = static_cast<int32_t>(std::round(child.y / 0.25f));
                    key.quantYVel = (child.p1.mode == VehicleMode::Wave) ? 0 : static_cast<int32_t>(std::round(child.p1.yVelocity / 0.25f));
                } else {
                    key.quantY = static_cast<int32_t>(std::round(child.y / 0.5f));
                    key.quantYVel = static_cast<int32_t>(std::round(child.p1.yVelocity / 0.25f));
                }
                key.quantX = static_cast<int32_t>(std::round(child.x / 1.0f));
                key.quantSpeed = static_cast<int32_t>(std::round(child.p1.playerSpeed * 100.0f));

                auto it = uniqueChildren.find(key);
                if (it == uniqueChildren.end()) {
                    uniqueChildren.emplace(key, std::move(child));
                } else {
                    if (child.clearance > it->second.clearance) {
                        it->second = std::move(child);
                    }
                }
            }

            if (won) break;
        }

        if (won) {
            finalizeSolution(playLayer, winningChild);
            return;
        }

        if (uniqueChildren.empty()) {
            triggerRewind(playLayer);
            if (!m_isRunning) return;
            break;
        }

        // Separate ground nodes and airborne nodes to preserve ground agency
        std::vector<BeamNode> groundNodes;
        std::vector<BeamNode> airNodes;
        for (auto& pair : uniqueChildren) {
            if (pair.second.p1.isOnGround) {
                groundNodes.push_back(std::move(pair.second));
            } else {
                airNodes.push_back(std::move(pair.second));
            }
        }

        std::vector<BeamNode> newFrontier;
        newFrontier.reserve(m_currentWidth);

        // Keep all valid ground states (at most 2-4 nodes: button down/up, mini, etc.)
        for (auto& gn : groundNodes) {
            newFrontier.push_back(std::move(gn));
        }

        size_t airSlots = (m_currentWidth > newFrontier.size()) ? (m_currentWidth - newFrontier.size()) : 1;
        if (airNodes.size() <= airSlots) {
            for (auto& an : airNodes) {
                newFrontier.push_back(std::move(an));
            }
        } else {
            std::sort(airNodes.begin(), airNodes.end(), [](const BeamNode& a, const BeamNode& b) {
                return a.y < b.y;
            });

            size_t total = airNodes.size();
            for (size_t b = 0; b < airSlots; ++b) {
                size_t startIdx = (b * total) / airSlots;
                size_t endIdx = ((b + 1) * total) / airSlots;
                size_t bestIdx = startIdx;
                float bestClearance = airNodes[startIdx].clearance;
                for (size_t i = startIdx + 1; i < endIdx && i < total; ++i) {
                    if (airNodes[i].clearance > bestClearance) {
                        bestClearance = airNodes[i].clearance;
                        bestIdx = i;
                    }
                }
                newFrontier.push_back(std::move(airNodes[bestIdx]));
            }
        }

        m_frontier = std::move(newFrontier);
        m_currentTick = m_frontier.front().tick;

        // Check progress
        float bestX = m_frontier.front().x;
        for (const auto& n : m_frontier) {
            if (n.x > bestX) bestX = n.x;
        }

        if (bestX > m_maxReachedX + 1.0f) {
            m_maxReachedX = bestX;
            m_deepestTick = m_currentTick;
            m_ticksSinceProgress = 0;
            if (m_stuckX > 0.0f && bestX > m_stuckX + 15.0f) {
                m_stuckX = 0.0f;
                m_currentWidth = 96; // Reset width on successful pass
            }
        } else {
            m_ticksSinceProgress += k;
            if (m_ticksSinceProgress >= 240) {
                triggerRewind(playLayer);
                break;
            }
        }

        // Layer saving every 240 ticks
        if (m_savedLayers.empty() || (m_currentTick >= m_savedLayers.back().layerTick + 240)) {
            SavedLayer sl;
            sl.layerTick = m_currentTick;
            sl.maxReachedX = m_maxReachedX;
            sl.nodes = m_frontier;
            m_savedLayers.push_back(std::move(sl));
            if (m_savedLayers.size() > 6) {
                m_savedLayers.erase(m_savedLayers.begin());
            }
        }

        // Check time budget
        auto now = std::chrono::high_resolution_clock::now();
        if (now - startBatch >= timeBudget) {
            break;
        }
    }

    auto endBatch = std::chrono::high_resolution_clock::now();
    double batchDuration = std::chrono::duration<double>(endBatch - startBatch).count();
    HeadlessEngine::get().recordStepBatch(simulatedTicks, batchDuration);

    // Update telemetry
    m_telemetry.currentTick = m_currentTick;
    m_telemetry.currentX = m_maxReachedX;
    m_telemetry.targetEndX = m_levelLength;
    float totalDist = m_levelLength - m_startX;
    if (totalDist > 0.0f) {
        m_telemetry.explorationHorizon = std::clamp(((m_maxReachedX - m_startX) / totalDist) * 100.0f, 0.0f, 100.0f);
    }
    m_telemetry.frontierSize = m_frontier.size();
    m_telemetry.currentWidth = m_currentWidth;
    m_telemetry.rewindCount = m_rewindCount;
    m_telemetry.deepestTick = m_deepestTick;
    m_telemetry.stuckX = m_stuckX;
    m_telemetry.ticksPerSecond = HeadlessEngine::get().getTicksPerSecond();
    m_telemetry.status = SolverStatus::Searching;
    if (m_stuckX > 0.0f) {
        m_telemetry.detailMessage = fmt::format("Search active (stuck near X={:.1f}) | W={} | Rewinds {}",
            m_stuckX, m_currentWidth, m_rewindCount);
    } else {
        m_telemetry.detailMessage = fmt::format("Search active | W={} | Frontier {} | Rewinds {}",
            m_currentWidth, m_frontier.size(), m_rewindCount);
    }
}

} // namespace solver
