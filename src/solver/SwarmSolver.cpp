#include "SwarmSolver.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../replay/MacroManager.hpp"
#include "../replay/GDRExporter.hpp"
#include <chrono>
#include <algorithm>
#include <random>

namespace solver {

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

SwarmSolver& SwarmSolver::get() {
    static SwarmSolver instance;
    return instance;
}

void SwarmSolver::start(PlayLayer* playLayer) {
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
        m_levelLength = m_startX + 600.0f; // Blank level fallback
    }

    m_maxReachedX = m_startX;
    m_currentTick = 0;
    m_activeWaveIndex = 0;
    m_waveRetryCount = 0;
    m_backtrackCount = 0;

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
    playLayer->m_hasCompletedLevel = false;

    if (playLayer->m_checkpointArray) {
        playLayer->m_checkpointArray->removeAllObjects();
    }

    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
    playLayer->updateVisibility(0.0f);

    BeamCheckpoint root;
    root.nativeCheckpoint = playLayer->createCheckpoint();
    if (root.nativeCheckpoint) {
        root.nativeCheckpoint->retain();
    }
    if (playLayer->m_checkpointArray && playLayer->m_checkpointArray->count() > 0) {
        playLayer->m_checkpointArray->removeAllObjects();
    }

    root.snapshot.capture(playLayer->m_player1, 0, DeterministicPRNG::STATIC_SEED);
    bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
    root.hasPlayer2 = isDual;
    if (isDual) {
        root.snapshot2.capture(playLayer->m_player2, 0, DeterministicPRNG::STATIC_SEED);
    }
    root.startTick = 0;
    root.startX = m_startX;
    m_checkpointStack.push_back(std::move(root));

    // Run death detection self-test
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
    m_telemetry.detailMessage = "Hazard-aware swarm active...";
    m_telemetry.currentX = m_startX;
    m_telemetry.targetEndX = m_levelLength;
    m_telemetry.isVerified = false;

    geode::log::info("[LevelSolver] SwarmSolver started at X={:.1f}, target end X={:.1f}, mode={}, hazards={}",
        m_startX, m_levelLength, static_cast<int>(m_checkpointStack.front().snapshot.mode), HazardDetector::hasHazards());
}

void SwarmSolver::resume(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1 || m_checkpointStack.empty()) {
        start(playLayer);
        return;
    }
    m_isRunning = true;
    HazardDetector::buildIndex(playLayer->m_objects);
    DeterministicPRNG::clampSeed();
    CheatAPIIntegrator::notifyCheatStarted();
    HeadlessEngine::get().enableHeadless();
    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "Swarm search resumed...";
    geode::log::info("[LevelSolver] SwarmSolver resumed at tick {}", m_currentTick);
}

void SwarmSolver::stop() {
    if (m_isRunning) {
        m_isRunning = false;
        HeadlessEngine::get().disableHeadless();
        CheatAPIIntegrator::notifyCheatEnded();
        HazardDetector::clearIndex();
        m_telemetry.status = SolverStatus::Paused;
        m_telemetry.detailMessage = "Search paused by user";
        geode::log::info("[LevelSolver] SwarmSolver stopped");
    }
}

void SwarmSolver::reset() {
    m_isRunning = false;
    m_isCompleted = false;
    m_checkpointStack.clear();
    m_resolvedMacro.clear();
    m_trajectorySamples.clear();
    m_partialProgressSeeds.clear();
    m_activePopulation.clear();
    m_currentWaveSurvivors.clear();
    m_currentBotIndex = 0;
    m_startX = 0.0f;
    m_levelLength = 0.0f;
    m_maxReachedX = 0.0f;
    m_currentTick = 0;
    m_activeWaveIndex = 0;
    m_waveRetryCount = 0;
    m_backtrackCount = 0;
    m_telemetry = TelemetryMetrics{};
}

bool SwarmSolver::runSelfTest(PlayLayer* playLayer) {
    if (!HazardDetector::hasHazards() || m_checkpointStack.empty()) {
        return true;
    }

    const auto& root = m_checkpointStack.front();
    if (root.nativeCheckpoint) {
        playLayer->loadFromCheckpoint(root.nativeCheckpoint);
    }
    root.snapshot.restore(playLayer->m_player1);

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    playLayer->m_queuedButtons.clear();
    playLayer->m_player1->releaseButton(PlayerButton::Jump);

    bool reachedEndWithoutDying = true;
    for (uint32_t t = 0; t < 600; ++t) {
        playLayer->update(HeadlessEngine::FIXED_DT);
        if (playLayer->m_player1->m_isDead || playLayer->m_playerDied) {
            reachedEndWithoutDying = false;
            break;
        }
        if (isSimulationFinished(playLayer, m_levelLength, m_startX)) {
            break;
        }
    }

    // Restore root state
    if (root.nativeCheckpoint) {
        playLayer->loadFromCheckpoint(root.nativeCheckpoint);
    }
    root.snapshot.restore(playLayer->m_player1);
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    playLayer->m_queuedButtons.clear();

    if (reachedEndWithoutDying) {
        geode::log::error("[LevelSolver] Self-test failed: idle run completed level without dying! Death detection is blocked by another mod.");
        return false;
    }
    return true;
}

std::vector<SwarmBot> SwarmSolver::generatePopulation(
    VehicleMode mode,
    uint32_t startTick,
    uint32_t horizonTicks,
    const std::vector<SwarmBot>& previousSurvivors,
    uint32_t waveRetryCount,
    float playerSpeed,
    float startX,
    cocos2d::CCArray* levelObjects
) {
    std::vector<SwarmBot> population;
    population.reserve(m_currentPopulationSize);

    auto addBotWithActions = [&](const std::vector<TickAction>& acts) {
        if (population.size() >= m_currentPopulationSize) return;
        SwarmBot b;
        b.segmentActions = acts;
        b.survived = false;
        b.finalX = 0.0f;
        population.push_back(std::move(b));
    };

    std::mt19937 rng(1337 + (waveRetryCount + m_backtrackCount * 17) * 7919 + startTick * 31 + m_activeWaveIndex * 101);

    // 1. Baseline 1: Pure Idle (crucial for safe ground; eliminates metronome clicking)
    addBotWithActions({ { 0, false } });

    // 2. Baseline 2: Full Hold
    addBotWithActions({ { 0, true } });

    float spd = playerSpeed > 0.1f ? playerSpeed : 1.0f;
    float unitsPerTick = 1.298f * spd;

    // 3. Scan ahead for upcoming hazards and interactables using spatial buckets
    float scanDistance = horizonTicks * unitsPerTick + 30.0f;
    auto interactables = HazardDetector::getInteractablesInWindow(startX, startX + scanDistance, levelObjects);

    // 4. Hazard-aware takeoff window sampling for jumping modes (Cube, Robot, Ball, Spider)
    bool isJumpMode = (mode == VehicleMode::Cube || mode == VehicleMode::Robot || mode == VehicleMode::Ball || mode == VehicleMode::Spider);
    if (isJumpMode) {
        // Find nearest hazard ahead
        float nearestHazardX = -1.0f;
        if (levelObjects) {
            for (unsigned int i = 0; i < levelObjects->count(); ++i) {
                if (auto obj = static_cast<GameObject*>(levelObjects->objectAtIndex(i))) {
                    float ox = obj->getPositionX();
                    if (ox > startX - 10.0f && ox < startX + scanDistance && HazardDetector::isHazardObject(obj)) {
                        if (nearestHazardX < 0.0f || ox < nearestHazardX) {
                            nearestHazardX = ox;
                        }
                    }
                }
            }
        }

        if (nearestHazardX > startX) {
            float distToHazard = nearestHazardX - startX;
            int reachTick = static_cast<int>(std::round(distToHazard / unitsPerTick));

            // Sample physical jump takeoff window across every tick of the approach (16 to 38 units lead)
            int minLead = static_cast<int>(std::round(16.0f / unitsPerTick));
            int maxLead = static_cast<int>(std::round(38.0f / unitsPerTick));

            for (int lead = minLead; lead <= maxLead; lead += 1) {
                int takeoff = reachTick - lead;
                if (takeoff >= 0 && takeoff < static_cast<int>(horizonTicks)) {
                    uint32_t t = static_cast<uint32_t>(takeoff);
                    // Single jump tap (duration 12-14 ticks for cube)
                    uint32_t dur = (mode == VehicleMode::Robot) ? 20 : 13;
                    std::vector<TickAction> acts;
                    acts.push_back({ t, true });
                    if (t + dur < horizonTicks) {
                        acts.push_back({ t + dur, false });
                    }
                    addBotWithActions(acts);

                    // Sustained jump hold
                    std::vector<TickAction> holdActs;
                    holdActs.push_back({ t, true });
                    addBotWithActions(holdActs);
                }
            }
        }
    }

    // 5. Orb and Pad timing injection
    for (auto obj : interactables) {
        if (!obj) continue;
        float ox = obj->getPositionX();
        float distToOrb = ox - startX;
        if (distToOrb > 0.0f && distToOrb < scanDistance) {
            int orbTick = static_cast<int>(std::round(distToOrb / unitsPerTick));
            bool isDash = HazardDetector::isDashOrb(obj);

            // Test taps around the orb activation radius
            for (int offset = -6; offset <= 6; offset += 2) {
                int tapTick = orbTick + offset;
                if (tapTick >= 0 && tapTick < static_cast<int>(horizonTicks)) {
                    uint32_t t = static_cast<uint32_t>(tapTick);
                    std::vector<TickAction> acts;
                    // Ensure button was released before tapping the orb
                    if (t > 4) {
                        acts.push_back({ t - 4, false });
                    }
                    acts.push_back({ t, true });
                    uint32_t dur = isDash ? 18 : 6;
                    if (t + dur < horizonTicks) {
                        acts.push_back({ t + dur, false });
                    }
                    addBotWithActions(acts);
                }
            }
        }
    }

    // 6. Continuous flight modes (Wave, Ship, Swing, UFO)
    bool isContinuous = (mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing || mode == VehicleMode::UFO);
    if (isContinuous) {
        // Multi-frequency micro-taps and varied duty cycles
        for (uint32_t period : { 2u, 3u, 4u, 6u, 8u, 10u, 12u, 16u, 20u }) {
            // Balanced square wave
            std::vector<TickAction> sq;
            bool st = true;
            for (uint32_t t = 0; t < horizonTicks; t += period) {
                sq.push_back({ t, st });
                st = !st;
            }
            addBotWithActions(sq);

            // Inverted square wave
            std::vector<TickAction> sqInv;
            st = false;
            for (uint32_t t = 0; t < horizonTicks; t += period) {
                sqInv.push_back({ t, st });
                st = !st;
            }
            addBotWithActions(sqInv);

            // Upward bias (climb pattern)
            std::vector<TickAction> climb;
            for (uint32_t t = 0; t < horizonTicks; t += (period * 2)) {
                climb.push_back({ t, true });
                if (t + period + 2 < horizonTicks) {
                    climb.push_back({ t + period + 2, false });
                }
            }
            addBotWithActions(climb);

            // Downward bias (dive pattern)
            std::vector<TickAction> dive;
            for (uint32_t t = 0; t < horizonTicks; t += (period * 2)) {
                dive.push_back({ t, false });
                if (t + period - 1 < horizonTicks) {
                    dive.push_back({ t + period - 1, true });
                }
            }
            addBotWithActions(dive);
        }
    }

    // 7. Genetic breeding / targeted mutations from previous survivors or seeds
    if (!previousSurvivors.empty()) {
        // Keep top survivors verbatim (elitism)
        for (size_t i = 0; i < previousSurvivors.size() && i < 4; ++i) {
            addBotWithActions(previousSurvivors[i].segmentActions);
        }

        // Targeted mutations
        size_t sIdx = 0;
        while (population.size() < m_currentPopulationSize && sIdx < previousSurvivors.size() * 8) {
            const auto& parent = previousSurvivors[sIdx % previousSurvivors.size()];
            sIdx++;

            if (!parent.survived && parent.deathTick > startTick) {
                // Shift takeoff earlier before the obstacle death point
                uint32_t localDeath = parent.deathTick - startTick;
                uint32_t shift = 4 + (rng() % 16);
                uint32_t earlyT = (localDeath > shift) ? (localDeath - shift) : 0;
                std::vector<TickAction> mutated = parent.segmentActions;
                mutated.push_back({ earlyT, true });
                if (earlyT + 12 < horizonTicks) {
                    mutated.push_back({ earlyT + 12, false });
                }
                std::sort(mutated.begin(), mutated.end(), [](const TickAction& a, const TickAction& b) {
                    return a.tick < b.tick;
                });
                addBotWithActions(mutated);
            } else {
                // Jitter click timings by +-1 to +-3 ticks
                std::vector<TickAction> mutated = parent.segmentActions;
                for (auto& act : mutated) {
                    int delta = (static_cast<int>(rng() % 5)) - 2;
                    int newT = static_cast<int>(act.tick) + delta;
                    act.tick = static_cast<uint32_t>(std::clamp(newT, 0, static_cast<int>(horizonTicks - 1)));
                }
                addBotWithActions(mutated);
            }
        }
    }

    // 8. Fill remaining slots with exploratory random intervals
    while (population.size() < m_currentPopulationSize) {
        std::vector<TickAction> rndActs;
        uint32_t curT = rng() % 8;
        bool curSt = (rng() % 2 == 1);
        while (curT < horizonTicks) {
            rndActs.push_back({ curT, curSt });
            uint32_t dur = 3 + (rng() % 24);
            curT += dur;
            curSt = !curSt;
        }
        addBotWithActions(rndActs);
    }

    return population;
}

void SwarmSolver::simulateBot(
    PlayLayer* playLayer,
    const BeamCheckpoint& checkpoint,
    SwarmBot& bot,
    uint32_t horizonTicks
) {
    // Fast in-memory physics restore (zero CheckpointObject heap allocations!)
    checkpoint.snapshot.restore(playLayer->m_player1);
    if (checkpoint.hasPlayer2 && playLayer->m_player2) {
        checkpoint.snapshot2.restore(playLayer->m_player2);
    }

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;
    playLayer->m_isPaused = false;
    playLayer->m_hasCompletedLevel = false;

    playLayer->m_queuedButtons.clear();
    if (playLayer->m_player1) {
        playLayer->m_player1->releaseButton(PlayerButton::Jump);
        playLayer->m_player1->m_jumpBuffered = false;
    }
    if (playLayer->m_player2) {
        playLayer->m_player2->releaseButton(PlayerButton::Jump);
        playLayer->m_player2->m_jumpBuffered = false;
    }

    size_t actionIdx = 0;
    bool currentButton = false;
    bool lastButton = false;
    bool completed = false;

    float prevX = playLayer->m_player1->getPositionX();
    uint32_t jammedTicks = 0;

    for (uint32_t step = 0; step < horizonTicks; ++step) {
        // Apply input scheduled for this segment tick
        while (actionIdx < bot.segmentActions.size() && bot.segmentActions[actionIdx].tick <= step) {
            currentButton = bot.segmentActions[actionIdx].pressed;
            actionIdx++;
        }

        if (currentButton != lastButton) {
            playLayer->handleButton(currentButton, 1, true);
            lastButton = currentButton;
        }

        playLayer->update(HeadlessEngine::FIXED_DT);

        // Check death
        bool isDead = playLayer->m_player1->m_isDead ||
                      (playLayer->m_player2 && playLayer->m_player2->m_isDead) ||
                      playLayer->m_playerDied;
        if (isDead) {
            bot.survived = false;
            bot.deathTick = checkpoint.startTick + step;
            bot.finalX = playLayer->m_player1->getPositionX();
            bot.clearance = 0.0f;
            bot.fitnessScore = -100.0f;
            return;
        }

        // Check wall jam
        float currX = playLayer->m_player1->getPositionX();
        bool goingLeft = playLayer->m_player1->m_isGoingLeft;
        float dx = goingLeft ? (prevX - currX) : (currX - prevX);
        if (dx < 0.001f && !playLayer->m_player1->m_isDashing && !playLayer->m_player1->m_isSpider) {
            jammedTicks++;
            if (jammedTicks >= 4) {
                bot.survived = false;
                bot.deathTick = checkpoint.startTick + step;
                bot.finalX = currX;
                bot.clearance = 0.0f;
                bot.fitnessScore = -50.0f;
                return;
            }
        } else {
            jammedTicks = 0;
        }
        prevX = currX;

        // Check level completion
        if (isSimulationFinished(playLayer, m_levelLength, checkpoint.startX)) {
            completed = true;
            break;
        }
    }

    if (lastButton) {
        playLayer->handleButton(false, 1, true);
    }
    playLayer->m_queuedButtons.clear();

    // Bot survived the full segment alive!
    bot.survived = true;
    bot.finalX = playLayer->m_player1->getPositionX();
    size_t nearbyCount = 0;
    bot.clearance = HazardDetector::calculateClearance(playLayer->m_player1->getPosition(), playLayer->m_objects, nearbyCount);

    // Calculate fitness score
    float fitness = 100.0f + (bot.finalX - checkpoint.startX);
    fitness += std::min(bot.clearance, 60.0f) * 0.3f;

    // Stable ground bonus for Cube/Robot/Ball
    if (playLayer->m_player1->m_isOnGround) {
        fitness += 35.0f;
    } else if (std::abs(playLayer->m_player1->m_yVelocity) > 8.0f) {
        fitness -= 15.0f; // Airborne falling penalty at segment boundary
    }

    // Input cleanliness penalty: penalize excessive jumping on flat ground to stop metronomes
    size_t jumpCount = 0;
    for (const auto& act : bot.segmentActions) {
        if (act.pressed) jumpCount++;
    }
    if (jumpCount == 0) {
        fitness += 20.0f; // Reward pure safe idle
    } else {
        fitness -= (2.0f * static_cast<float>(jumpCount));
    }

    if (completed) {
        fitness += 10000.0f;
    }

    bot.fitnessScore = fitness;
}

void SwarmSolver::stepSwarmBatch(PlayLayer* playLayer, uint32_t maxSteps) {
    if (!m_isRunning || m_isCompleted || !playLayer || !playLayer->m_player1 || m_checkpointStack.empty()) {
        return;
    }

    auto startBatch = std::chrono::high_resolution_clock::now();
    const auto timeBudget = std::chrono::milliseconds(12);

    auto& currentCp = m_checkpointStack.back();
    VehicleMode mode = currentCp.snapshot.mode;
    uint32_t horizonTicks = (mode == VehicleMode::Wave || mode == VehicleMode::Ship) ? 60u : 48u;

    // Generate population for wave if not currently evaluating one
    if (m_activePopulation.empty() || m_currentBotIndex >= m_activePopulation.size()) {
        m_activeWaveIndex++;
        const auto& seeds = (currentCp.failedWaves > 0 && !m_partialProgressSeeds.empty()) ? m_partialProgressSeeds : currentCp.runnerUps;
        m_activePopulation = generatePopulation(
            mode,
            currentCp.startTick,
            horizonTicks,
            seeds,
            currentCp.failedWaves,
            currentCp.snapshot.playerSpeed,
            currentCp.startX,
            playLayer->m_objects
        );
        m_currentBotIndex = 0;
        m_currentWaveSurvivors.clear();
        m_currentHorizonTicks = horizonTicks;
    }

    uint32_t simulatedTicks = 0;

    // Evaluate bots in population within frame time budget
    while (m_currentBotIndex < m_activePopulation.size()) {
        auto& bot = m_activePopulation[m_currentBotIndex++];
        simulateBot(playLayer, currentCp, bot, m_currentHorizonTicks);
        simulatedTicks += m_currentHorizonTicks;

        if (bot.survived) {
            m_currentWaveSurvivors.push_back(bot);
            if (bot.finalX > m_maxReachedX) {
                m_maxReachedX = bot.finalX;
            }
            if (isSimulationFinished(playLayer, m_levelLength, currentCp.startX)) {
                // Winning bot! Append actions and finalize
                std::vector<TickAction> fullMacro = currentCp.macroHistory;
                for (const auto& act : bot.segmentActions) {
                    fullMacro.push_back({ currentCp.startTick + act.tick, act.pressed });
                }
                finalizeSolution(playLayer, fullMacro);
                return;
            }
        }

        auto now = std::chrono::high_resolution_clock::now();
        if (now - startBatch >= timeBudget) {
            break;
        }
    }

    auto endBatch = std::chrono::high_resolution_clock::now();
    double batchDuration = std::chrono::duration<double>(endBatch - startBatch).count();
    HeadlessEngine::get().recordStepBatch(simulatedTicks, batchDuration);

    // Wave completed: process survivors or trigger backtrack
    if (m_currentBotIndex >= m_activePopulation.size()) {
        m_lastSurvivorCount = m_currentWaveSurvivors.size();

        if (!m_currentWaveSurvivors.empty()) {
            // Sort survivors descending by fitness score
            std::sort(m_currentWaveSurvivors.begin(), m_currentWaveSurvivors.end(), [](const SwarmBot& a, const SwarmBot& b) {
                return a.fitnessScore > b.fitnessScore;
            });

            const auto& bestBot = m_currentWaveSurvivors.front();

            // Commit winning bot into a new BeamCheckpoint
            BeamCheckpoint nextCp;
            nextCp.startTick = currentCp.startTick + m_currentHorizonTicks;
            nextCp.macroHistory = currentCp.macroHistory;
            for (const auto& act : bestBot.segmentActions) {
                nextCp.macroHistory.push_back({ currentCp.startTick + act.tick, act.pressed });
            }

            // Advance simulation to the new checkpoint boundary using best bot
            currentCp.snapshot.restore(playLayer->m_player1);
            if (currentCp.hasPlayer2 && playLayer->m_player2) {
                currentCp.snapshot2.restore(playLayer->m_player2);
            }
            playLayer->m_started = true;
            playLayer->m_inResetDelay = false;
            playLayer->m_playerDied = false;
            playLayer->m_player1->m_isDead = false;
            playLayer->m_queuedButtons.clear();

            size_t aIdx = 0;
            bool btn = false;
            bool lastB = false;
            for (uint32_t s = 0; s < m_currentHorizonTicks; ++s) {
                while (aIdx < bestBot.segmentActions.size() && bestBot.segmentActions[aIdx].tick <= s) {
                    btn = bestBot.segmentActions[aIdx].pressed;
                    aIdx++;
                }
                if (btn != lastB) {
                    playLayer->handleButton(btn, 1, true);
                    lastB = btn;
                }
                playLayer->update(HeadlessEngine::FIXED_DT);
            }
            if (lastB) playLayer->handleButton(false, 1, true);
            playLayer->m_queuedButtons.clear();

            // Capture new checkpoint state
            nextCp.snapshot.capture(playLayer->m_player1, nextCp.startTick, DeterministicPRNG::STATIC_SEED);
            bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
            nextCp.hasPlayer2 = isDual;
            if (isDual) {
                nextCp.snapshot2.capture(playLayer->m_player2, nextCp.startTick, DeterministicPRNG::STATIC_SEED);
            }
            nextCp.startX = playLayer->m_player1->getPositionX();

            // Create native checkpoint ONCE for committed boundary
            nextCp.nativeCheckpoint = playLayer->createCheckpoint();
            if (nextCp.nativeCheckpoint) {
                nextCp.nativeCheckpoint->retain();
            }
            if (playLayer->m_checkpointArray && playLayer->m_checkpointArray->count() > 0) {
                playLayer->m_checkpointArray->removeAllObjects();
            }

            // Store alternate survivors as runner-ups for backtracking
            for (size_t i = 1; i < m_currentWaveSurvivors.size() && i < 4; ++i) {
                nextCp.runnerUps.push_back(m_currentWaveSurvivors[i]);
            }

            m_checkpointStack.push_back(std::move(nextCp));
            m_currentTick = m_checkpointStack.back().startTick;
            m_maxReachedX = m_checkpointStack.back().startX;
            m_waveRetryCount = 0;
            m_partialProgressSeeds.clear();
            m_activePopulation.clear();

            geode::log::info("[LevelSolver] Wave #{} passed (X={:.1f}, tick {}, {} survivors, best fitness={:.1f})",
                m_activeWaveIndex, m_maxReachedX, m_currentTick, m_lastSurvivorCount, bestBot.fitnessScore);
        } else {
            // All 160 bots died! NEVER checkpoint a dead bot!
            m_waveRetryCount++;
            currentCp.failedWaves++;
            geode::log::warn("[LevelSolver] Wave #{} produced 0 survivors at X={:.1f} (failed {} times)",
                m_activeWaveIndex, currentCp.startX, currentCp.failedWaves);

            if (currentCp.failedWaves >= 3) {
                handleBacktrack(playLayer);
            } else {
                // Retry wave with wider mutations
                m_activePopulation.clear();
            }
        }
    }

    // Update telemetry metrics
    m_telemetry.currentTick = m_currentTick;
    m_telemetry.currentX = m_maxReachedX;
    m_telemetry.targetEndX = m_levelLength;
    float totalDist = m_levelLength - m_startX;
    if (totalDist > 0.0f) {
        m_telemetry.explorationHorizon = std::clamp(((m_maxReachedX - m_startX) / totalDist) * 100.0f, 0.0f, 100.0f);
    }
    m_telemetry.frontierSize = m_checkpointStack.size();
    m_telemetry.currentWidth = m_currentPopulationSize;
    m_telemetry.rewindCount = m_backtrackCount;
    m_telemetry.deepestTick = m_currentTick;
    m_telemetry.ticksPerSecond = HeadlessEngine::get().getTicksPerSecond();
    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = fmt::format("Swarm Wave #{} | Stack {} | Survivors {} | TPS {:.0f}",
        m_activeWaveIndex, m_checkpointStack.size(), m_lastSurvivorCount, m_telemetry.ticksPerSecond);
}

void SwarmSolver::handleBacktrack(PlayLayer* playLayer) {
    m_backtrackCount++;
    geode::log::warn("[LevelSolver] Backtracking from X={:.1f} (backtrack #{})",
        m_checkpointStack.back().startX, m_backtrackCount);

    if (m_checkpointStack.size() <= 1) {
        // At root: reset failed waves and expand population exploration
        m_checkpointStack.front().failedWaves = 0;
        m_activePopulation.clear();
        m_partialProgressSeeds.clear();
        return;
    }

    // Pop the blocked checkpoint
    m_checkpointStack.pop_back();

    auto& parentCp = m_checkpointStack.back();
    if (parentCp.nativeCheckpoint) {
        playLayer->loadFromCheckpoint(parentCp.nativeCheckpoint);
    }
    parentCp.snapshot.restore(playLayer->m_player1);
    if (parentCp.hasPlayer2 && playLayer->m_player2) {
        parentCp.snapshot2.restore(playLayer->m_player2);
    }

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    playLayer->m_queuedButtons.clear();

    m_currentTick = parentCp.startTick;
    m_maxReachedX = parentCp.startX;
    m_activePopulation.clear();
    m_currentWaveSurvivors.clear();
    m_currentBotIndex = 0;
}

void SwarmSolver::finalizeSolution(PlayLayer* playLayer, const std::vector<TickAction>& winningActions) {
    geode::log::info("[LevelSolver] Swarm reached 100%! Verifying complete macro with {} inputs...",
        winningActions.size());

    // 1. Compress into edge-triggered actions
    std::vector<TickAction> compressed;
    bool lastBtn = false;
    for (const auto& act : winningActions) {
        if (act.pressed != lastBtn) {
            compressed.push_back(act);
            lastBtn = act.pressed;
        }
    }
    if (!compressed.empty() && compressed.back().pressed) {
        compressed.push_back({ compressed.back().tick + 1, false });
    }

    // Refuse 0-input macro if level has hazards
    if (HazardDetector::hasHazards() && compressed.empty()) {
        geode::log::error("[LevelSolver] Refusing 0-input macro on hazard level! Continuing search...");
        handleBacktrack(playLayer);
        return;
    }

    // 2. Verification simulation run from tick 0
    const auto& root = m_checkpointStack.front();
    if (root.nativeCheckpoint) {
        playLayer->loadFromCheckpoint(root.nativeCheckpoint);
    }
    root.snapshot.restore(playLayer->m_player1);
    if (root.hasPlayer2 && playLayer->m_player2) {
        root.snapshot2.restore(playLayer->m_player2);
    }

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
    playLayer->m_queuedButtons.clear();
    playLayer->m_player1->releaseButton(PlayerButton::Jump);

    std::vector<TrajectorySample> trajectory;
    size_t actIdx = 0;
    bool currBtn = false;
    bool lastSimBtn = false;
    bool verified = true;
    uint32_t totalTicks = compressed.empty() ? 240 : (compressed.back().tick + 240);

    for (uint32_t t = 0; t <= totalTicks; ++t) {
        while (actIdx < compressed.size() && compressed[actIdx].tick <= t) {
            currBtn = compressed[actIdx].pressed;
            actIdx++;
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

        bool dead = playLayer->m_player1->m_isDead ||
                    (playLayer->m_player2 && playLayer->m_player2->m_isDead) ||
                    playLayer->m_playerDied;
        if (dead) {
            geode::log::warn("[LevelSolver] Swarm verification failed at tick {} (died at X={:.1f})! Backtracking...",
                t, playLayer->m_player1->getPositionX());
            verified = false;
            break;
        }

        if (isSimulationFinished(playLayer, m_levelLength, m_startX)) {
            break;
        }
    }

    if (lastSimBtn) playLayer->handleButton(false, 1, true);
    playLayer->m_queuedButtons.clear();

    if (!verified) {
        handleBacktrack(playLayer);
        return;
    }

    // Solution verified successfully!
    m_resolvedMacro = compressed;
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

bool SwarmSolver::isRunning() const {
    return m_isRunning;
}

bool SwarmSolver::isCompleted() const {
    return m_isCompleted;
}

TelemetryMetrics SwarmSolver::getTelemetry() const {
    return m_telemetry;
}

const std::vector<TickAction>& SwarmSolver::getResolvedMacro() const {
    return m_resolvedMacro;
}

const std::vector<TrajectorySample>& SwarmSolver::getTrajectory() const {
    return m_trajectorySamples;
}

} // namespace solver
