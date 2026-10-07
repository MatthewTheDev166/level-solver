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

static bool isSimulationFinished(PlayLayer* playLayer, float levelLength, float startX, float lastHazardX = 0.0f) {
    if (!playLayer || !playLayer->m_player1) return false;

    // A dead or crashed player can NEVER be considered finished!
    if (playLayer->m_player1->m_isDead || (playLayer->m_player2 && playLayer->m_player2->m_isDead) || playLayer->m_playerDied) {
        return false;
    }

    if (playLayer->m_hasCompletedLevel) {
        return true;
    }

    float currentX = playLayer->m_player1->getPositionX();
    bool pastHazards = (lastHazardX <= startX + 10.0f) || (currentX > lastHazardX + 50.0f);
    if (currentX >= levelLength && pastHazards) {
        return true;
    }

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

    // Read configured solver mode preference (default: spawn-respawn)
    std::string modeVal = geode::Mod::get()->getSavedValue<std::string>("solver-mode", "spawn-respawn");
    m_solverMode = (modeVal == "checkpoints") ? SolverMode::Checkpoints : SolverMode::SpawnRespawn;

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
    m_lastHazardX = m_startX;
    if (playLayer->m_objects) {
        for (unsigned int i = 0; i < playLayer->m_objects->count(); ++i) {
            if (auto obj = static_cast<GameObject*>(playLayer->m_objects->objectAtIndex(i))) {
                float ox = obj->getPositionX();
                if (ox > maxObjX) {
                    maxObjX = ox;
                }
                if (HazardDetector::isHazardObject(obj)) {
                    if (ox > m_lastHazardX) {
                        m_lastHazardX = ox;
                    }
                }
            }
        }
    }

    float robtopEndX = playLayer->getEndPosition().x;
    if (robtopEndX > m_startX + 50.0f) {
        m_levelLength = robtopEndX;
    } else if (maxObjX > m_startX + 50.0f) {
        m_levelLength = maxObjX + 60.0f;
    } else {
        m_levelLength = m_startX + 600.0f;
    }

    if (m_lastHazardX + 60.0f > m_levelLength) {
        m_levelLength = m_lastHazardX + 60.0f;
    }

    geode::log::info("[LevelSolver] SwarmSolver starting in mode: {} (startX={:.1f}, levelLength={:.1f}, lastHazardX={:.1f})",
        (m_solverMode == SolverMode::SpawnRespawn ? "Spawn-Respawn" : "Checkpoints"),
        m_startX, m_levelLength, m_lastHazardX);

    m_maxReachedX = m_startX;
    m_currentTick = 0;
    m_activeWaveIndex = 0;
    m_waveRetryCount = 0;
    m_backtrackCount = 0;

    HazardDetector::buildIndex(playLayer->m_objects);
    DeterministicPRNG::clampSeed();
    CheatAPIIntegrator::notifyCheatStarted();
    HeadlessEngine::get().enableHeadless();

    playLayer->m_isPracticeMode = true;
    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;
    playLayer->m_isPaused = false;
    playLayer->m_hasCompletedLevel = false;

    if (playLayer->m_checkpointArray) {
        playLayer->m_checkpointArray->removeAllObjects();
    }

    // Capture initial root snapshot at spawn (tick 0)
    m_rootSnapshot.capture(playLayer->m_player1, 0, DeterministicPRNG::STATIC_SEED);
    m_hasPlayer2 = playLayer->m_gameState.m_isDualMode && (playLayer->m_player2 != nullptr);
    if (m_hasPlayer2) {
        m_rootSnapshot2.capture(playLayer->m_player2, 0, DeterministicPRNG::STATIC_SEED);
    }

    m_frontierTick = 0;
    m_frontierX = m_startX;
    m_frontierMode = m_rootSnapshot.mode;
    m_verifiedPrefix.clear();

    if (m_solverMode == SolverMode::Checkpoints) {
        playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
        BeamCheckpoint rootCp;
        rootCp.nativeCheckpoint = playLayer->createCheckpoint();
        if (rootCp.nativeCheckpoint) {
            rootCp.nativeCheckpoint->retain();
        }
        rootCp.snapshot = m_rootSnapshot;
        rootCp.snapshot2 = m_rootSnapshot2;
        rootCp.hasPlayer2 = m_hasPlayer2;
        rootCp.startTick = 0;
        rootCp.startX = m_startX;
        m_checkpointStack.push_back(std::move(rootCp));
    }

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = (m_solverMode == SolverMode::SpawnRespawn)
        ? "Prefix swarm active from spawn (160 bots)..."
        : "Checkpoint swarm active (160 bots)...";
    m_telemetry.currentX = m_startX;
    m_telemetry.targetEndX = m_levelLength;
    m_telemetry.isVerified = false;
    m_telemetry.totalBots = m_currentPopulationSize;
    m_telemetry.aliveBots = m_currentPopulationSize;
    m_telemetry.waveAttempt = 1;
}

void SwarmSolver::resume(PlayLayer* playLayer) {
    if (!playLayer || !playLayer->m_player1) {
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
    for (auto& cp : m_checkpointStack) {
        if (cp.nativeCheckpoint) {
            cp.nativeCheckpoint->release();
            cp.nativeCheckpoint = nullptr;
        }
    }
    m_checkpointStack.clear();
    m_verifiedPrefix.clear();
    m_resolvedMacro.clear();
    m_trajectorySamples.clear();
    m_activePopulation.clear();
    m_currentWaveSurvivors.clear();
    m_currentBotIndex = 0;
    m_startX = 0.0f;
    m_levelLength = 0.0f;
    m_lastHazardX = 0.0f;
    m_maxReachedX = 0.0f;
    m_currentTick = 0;
    m_frontierTick = 0;
    m_frontierX = 0.0f;
    m_activeWaveIndex = 0;
    m_waveRetryCount = 0;
    m_backtrackCount = 0;
    m_telemetry = TelemetryMetrics{};
}

std::vector<SwarmBot> SwarmSolver::generatePopulation(
    VehicleMode mode,
    uint32_t startTick,
    uint32_t horizonTicks,
    const std::vector<SwarmBot>& previousSurvivors,
    uint32_t waveRetryCount,
    float playerSpeed,
    float startX,
    cocos2d::CCArray* levelObjects,
    bool startsHeld
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

    // 1. Raw Baseline 1: Pure Idle (release / no input)
    addBotWithActions({ { 0, false } });

    // 2. Raw Baseline 2: Full Hold (press from tick 0 to horizon end)
    addBotWithActions({ { 0, true } });

    // If starts held from previous segment: test releasing at various ticks
    if (startsHeld) {
        for (uint32_t t = 1; t < horizonTicks - 1; ++t) {
            addBotWithActions({ { t, false } });
        }
    }

    // 3. Single Jumps / Presses at each tick t with varying hold durations
    for (uint32_t t = 0; t < horizonTicks; t += 2) {
        for (uint32_t dur : { 2u, 4u, 8u, 14u, 24u, 36u }) {
            std::vector<TickAction> acts;
            acts.push_back({ t, true });
            if (t + dur < horizonTicks) {
                acts.push_back({ t + dur, false });
            }
            addBotWithActions(acts);
        }
    }

    // 4. Double Taps / Micro-Pulses (two jump pulses in horizon)
    for (uint32_t t1 = 0; t1 < horizonTicks / 2; t1 += 4) {
        for (uint32_t gap : { 6u, 12u, 18u }) {
            uint32_t t2 = t1 + gap;
            if (t2 + 4 < horizonTicks) {
                std::vector<TickAction> acts;
                acts.push_back({ t1, true });
                acts.push_back({ t1 + 3, false });
                acts.push_back({ t2, true });
                acts.push_back({ t2 + 3, false });
                addBotWithActions(acts);
            }
        }
    }

    // 5. Vehicle-Specific Flight Patterns (Wave / Ship / UFO / Swing)
    if (mode == VehicleMode::Wave) {
        // High-frequency zigzag waveforms
        for (uint32_t freq : { 2u, 3u, 4u, 6u, 8u, 12u }) {
            std::vector<TickAction> waveActs;
            bool st = false;
            for (uint32_t t = 0; t < horizonTicks; t += freq) {
                waveActs.push_back({ t, st });
                st = !st;
            }
            addBotWithActions(waveActs);
        }
    } else if (mode == VehicleMode::Ship || mode == VehicleMode::Swing || mode == VehicleMode::UFO) {
        // Pulse flutter holds
        for (uint32_t pulseLen : { 4u, 8u, 12u, 16u, 20u }) {
            for (uint32_t gapLen : { 4u, 8u, 12u }) {
                std::vector<TickAction> shipActs;
                uint32_t cur = 0;
                while (cur < horizonTicks) {
                    shipActs.push_back({ cur, true });
                    cur += pulseLen;
                    if (cur < horizonTicks) {
                        shipActs.push_back({ cur, false });
                        cur += gapLen;
                    }
                }
                addBotWithActions(shipActs);
            }
        }
    }

    // 6. Targeted Mutations from previous wave's attempts if all died
    if (waveRetryCount > 0 && !previousSurvivors.empty()) {
        for (const auto& parent : previousSurvivors) {
            if (population.size() >= m_currentPopulationSize) break;
            if (parent.deathTick > startTick) {
                uint32_t deathLocal = parent.deathTick - startTick;
                for (uint32_t lead : { 1u, 2u, 4u, 7u, 12u, 18u, 25u }) {
                    if (deathLocal >= lead) {
                        uint32_t flipT = deathLocal - lead;
                        std::vector<TickAction> flipped = parent.segmentActions;
                        flipped.erase(std::remove_if(flipped.begin(), flipped.end(),
                            [flipT](const TickAction& a) { return a.tick >= flipT; }), flipped.end());
                        flipped.push_back({ flipT, true });
                        if (flipT + 8 < horizonTicks) {
                            flipped.push_back({ flipT + 8, false });
                        }
                        addBotWithActions(flipped);
                    }
                }
            }
        }
    }

    // 7. Pseudo-random candidate timelines to saturate full population
    std::uniform_int_distribution<uint32_t> tickDist(0, horizonTicks - 1);
    std::uniform_int_distribution<int> boolDist(0, 1);
    while (population.size() < m_currentPopulationSize) {
        std::vector<TickAction> rndActs;
        uint32_t numActions = 1 + (rng() % 4);
        for (uint32_t i = 0; i < numActions; ++i) {
            rndActs.push_back({ tickDist(rng), boolDist(rng) == 1 });
        }
        std::sort(rndActs.begin(), rndActs.end(), [](const TickAction& a, const TickAction& b) {
            return a.tick < b.tick;
        });
        addBotWithActions(rndActs);
    }

    return population;
}

void SwarmSolver::simulateBot(
    PlayLayer* playLayer,
    SwarmBot& bot,
    uint32_t horizonTicks
) {
    if (m_solverMode == SolverMode::SpawnRespawn) {
        // --- Spawn-Respawn Mode: Deterministic execution from spawn (tick 0) ---
        DeterministicPRNG::clampSeed(m_rootSnapshot.rngSeed);

        m_rootSnapshot.restore(playLayer->m_player1);
        if (m_hasPlayer2 && playLayer->m_player2) {
            m_rootSnapshot2.restore(playLayer->m_player2);
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

        size_t prefixIdx = 0;
        size_t segIdx = 0;
        bool lastButton = m_rootSnapshot.isHolding;
        if (lastButton) playLayer->handleButton(true, 1, true);
        else playLayer->handleButton(false, 1, true);

        uint32_t totalSimulationTicks = m_frontierTick + horizonTicks;
        bool completed = false;

        for (uint32_t step = 0; step < totalSimulationTicks; ++step) {
            if (step < m_frontierTick) {
                // Verified prefix inputs
                while (prefixIdx < m_verifiedPrefix.size() && m_verifiedPrefix[prefixIdx].tick <= step) {
                    bool btn = m_verifiedPrefix[prefixIdx].pressed;
                    if (btn != lastButton) {
                        playLayer->handleButton(btn, 1, true);
                        lastButton = btn;
                    }
                    prefixIdx++;
                }
            } else {
                // Candidate frontier inputs
                uint32_t localStep = step - m_frontierTick;
                while (segIdx < bot.segmentActions.size() && bot.segmentActions[segIdx].tick <= localStep) {
                    bool btn = bot.segmentActions[segIdx].pressed;
                    if (btn != lastButton) {
                        playLayer->handleButton(btn, 1, true);
                        lastButton = btn;
                    }
                    segIdx++;
                }
            }

            playLayer->update(HeadlessEngine::FIXED_DT);

            if (isSimulationFinished(playLayer, m_levelLength, m_startX, m_lastHazardX)) {
                completed = true;
                break;
            }

            bool isDead = playLayer->m_player1->m_isDead ||
                          (playLayer->m_player2 && playLayer->m_player2->m_isDead) ||
                          playLayer->m_playerDied;
            if (isDead) {
                bot.survived = false;
                bot.deathTick = step;
                bot.finalX = playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0.0f;
                return;
            }
        }

        playLayer->m_queuedButtons.clear();
        bot.survived = true;
        bot.finalX = playLayer->m_player1->getPositionX();

        if (completed) {
            bot.fitnessScore = 10000.0f;
        } else {
            float progress = bot.finalX - m_frontierX;
            size_t nearbyObstacles = 0;
            float clearance = HazardDetector::calculateClearance(
                playLayer->m_player1->getPosition(),
                playLayer->m_objects,
                nearbyObstacles
            );
            bot.fitnessScore = progress * 10.0f + clearance;
        }
    } else {
        // --- Classic Checkpoint Stack Mode ---
        if (m_checkpointStack.empty()) return;
        const auto& currentCp = m_checkpointStack.back();

        DeterministicPRNG::clampSeed(currentCp.snapshot.rngSeed);
        if (currentCp.nativeCheckpoint) {
            playLayer->loadFromCheckpoint(currentCp.nativeCheckpoint);
        }
        currentCp.snapshot.restore(playLayer->m_player1);
        if (currentCp.hasPlayer2 && playLayer->m_player2) {
            currentCp.snapshot2.restore(playLayer->m_player2);
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

        size_t segIdx = 0;
        bool lastButton = currentCp.snapshot.isHolding;
        if (lastButton) playLayer->handleButton(true, 1, true);
        else playLayer->handleButton(false, 1, true);

        bool completed = false;

        for (uint32_t step = 0; step < horizonTicks; ++step) {
            while (segIdx < bot.segmentActions.size() && bot.segmentActions[segIdx].tick <= step) {
                bool btn = bot.segmentActions[segIdx].pressed;
                if (btn != lastButton) {
                    playLayer->handleButton(btn, 1, true);
                    lastButton = btn;
                }
                segIdx++;
            }

            playLayer->update(HeadlessEngine::FIXED_DT);

            if (isSimulationFinished(playLayer, m_levelLength, m_startX, m_lastHazardX)) {
                completed = true;
                break;
            }

            bool isDead = playLayer->m_player1->m_isDead ||
                          (playLayer->m_player2 && playLayer->m_player2->m_isDead) ||
                          playLayer->m_playerDied;
            if (isDead) {
                bot.survived = false;
                bot.deathTick = currentCp.startTick + step;
                bot.finalX = playLayer->m_player1 ? playLayer->m_player1->getPositionX() : 0.0f;
                return;
            }
        }

        playLayer->m_queuedButtons.clear();
        bot.survived = true;
        bot.finalX = playLayer->m_player1->getPositionX();

        if (completed) {
            bot.fitnessScore = 10000.0f;
        } else {
            float progress = bot.finalX - currentCp.startX;
            size_t nearbyObstacles = 0;
            float clearance = HazardDetector::calculateClearance(
                playLayer->m_player1->getPosition(),
                playLayer->m_objects,
                nearbyObstacles
            );
            bot.fitnessScore = progress * 10.0f + clearance;
        }
    }
}

void SwarmSolver::stepSwarmBatch(PlayLayer* playLayer, uint32_t maxSteps) {
    if (!m_isRunning || m_isCompleted || !playLayer || !playLayer->m_player1) {
        return;
    }

    auto startBatch = std::chrono::high_resolution_clock::now();
    const auto timeBudget = std::chrono::milliseconds(12);

    VehicleMode mode = (m_solverMode == SolverMode::SpawnRespawn)
        ? m_frontierMode
        : (m_checkpointStack.empty() ? VehicleMode::Cube : m_checkpointStack.back().snapshot.mode);

    uint32_t horizonTicks = (mode == VehicleMode::Wave) ? 36u : ((mode == VehicleMode::Ship || mode == VehicleMode::Swing || mode == VehicleMode::UFO) ? 48u : 60u);

    // Generate population for wave if not currently evaluating one
    if (m_activePopulation.empty() || m_currentBotIndex >= m_activePopulation.size()) {
        m_activeWaveIndex++;
        uint32_t startTick = (m_solverMode == SolverMode::SpawnRespawn) ? m_frontierTick : m_checkpointStack.back().startTick;
        float startX = (m_solverMode == SolverMode::SpawnRespawn) ? m_frontierX : m_checkpointStack.back().startX;

        m_activePopulation = generatePopulation(
            mode,
            startTick,
            horizonTicks,
            m_currentWaveSurvivors,
            m_waveRetryCount,
            1.0f,
            startX,
            playLayer->m_objects,
            false
        );
        m_currentBotIndex = 0;
        m_currentWaveSurvivors.clear();
        m_currentHorizonTicks = horizonTicks;
        m_telemetry.totalBots = m_currentPopulationSize;
        m_telemetry.aliveBots = m_currentPopulationSize;
    }

    uint32_t simulatedTicks = 0;

    // Evaluate bots in population within frame time budget
    while (m_currentBotIndex < m_activePopulation.size()) {
        auto& bot = m_activePopulation[m_currentBotIndex++];
        simulateBot(playLayer, bot, m_currentHorizonTicks);
        simulatedTicks += (m_solverMode == SolverMode::SpawnRespawn) ? (m_frontierTick + m_currentHorizonTicks) : m_currentHorizonTicks;

        if (bot.survived) {
            m_currentWaveSurvivors.push_back(bot);
            if (bot.finalX > m_maxReachedX) {
                m_maxReachedX = bot.finalX;
            }
            if (bot.fitnessScore >= 5000.0f || isSimulationFinished(playLayer, m_levelLength, m_startX, m_lastHazardX)) {
                // Winning bot! Append actions and finalize
                std::vector<TickAction> fullMacro;
                if (m_solverMode == SolverMode::SpawnRespawn) {
                    fullMacro = m_verifiedPrefix;
                    for (const auto& act : bot.segmentActions) {
                        fullMacro.push_back({ m_frontierTick + act.tick, act.pressed });
                    }
                } else {
                    fullMacro = m_checkpointStack.back().macroHistory;
                    for (const auto& act : bot.segmentActions) {
                        fullMacro.push_back({ m_checkpointStack.back().startTick + act.tick, act.pressed });
                    }
                }
                finalizeSolution(playLayer, fullMacro);
                return;
            }
        }

        // Real-time alive bots counter decrements as bots fail simulation
        m_telemetry.aliveBots = m_currentPopulationSize - (m_currentBotIndex - m_currentWaveSurvivors.size());

        auto now = std::chrono::high_resolution_clock::now();
        if (now - startBatch >= timeBudget) {
            break;
        }
    }

    auto endBatch = std::chrono::high_resolution_clock::now();
    double batchDuration = std::chrono::duration<double>(endBatch - startBatch).count();
    HeadlessEngine::get().recordStepBatch(simulatedTicks, batchDuration);

    // Wave completed: process survivors or rewind/backtrack
    if (m_currentBotIndex >= m_activePopulation.size()) {
        m_lastSurvivorCount = m_currentWaveSurvivors.size();

        if (!m_currentWaveSurvivors.empty()) {
            std::sort(m_currentWaveSurvivors.begin(), m_currentWaveSurvivors.end(), [](const SwarmBot& a, const SwarmBot& b) {
                return a.fitnessScore > b.fitnessScore;
            });

            const auto& bestBot = m_currentWaveSurvivors.front();

            if (m_solverMode == SolverMode::SpawnRespawn) {
                // Extend verified prefix with best bot's actions
                for (const auto& act : bestBot.segmentActions) {
                    m_verifiedPrefix.push_back({ m_frontierTick + act.tick, act.pressed });
                }

                // Deduplicate consecutive identical actions
                std::vector<TickAction> cleanPrefix;
                bool lastP = false;
                for (const auto& a : m_verifiedPrefix) {
                    if (cleanPrefix.empty() || a.pressed != lastP) {
                        cleanPrefix.push_back(a);
                        lastP = a.pressed;
                    }
                }
                m_verifiedPrefix = cleanPrefix;

                m_frontierTick += m_currentHorizonTicks;
                m_frontierX = bestBot.finalX;
                m_currentTick = m_frontierTick;
                m_waveRetryCount = 0;
                m_activePopulation.clear();
                m_currentWaveSurvivors.clear();
                m_currentBotIndex = 0;

                geode::log::info("[LevelSolver] Prefix Wave #{} succeeded! Extended frontier to tick {} (X={:.1f}, {} survivors, fitness={:.1f})",
                    m_activeWaveIndex, m_frontierTick, m_frontierX, m_lastSurvivorCount, bestBot.fitnessScore);
            } else {
                // Classic checkpoint stack mode
                auto& currentCp = m_checkpointStack.back();
                BeamCheckpoint nextCp;
                nextCp.startTick = currentCp.startTick + m_currentHorizonTicks;
                nextCp.startX = bestBot.finalX;
                nextCp.macroHistory = currentCp.macroHistory;
                for (const auto& act : bestBot.segmentActions) {
                    nextCp.macroHistory.push_back({ currentCp.startTick + act.tick, act.pressed });
                }

                playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
                nextCp.nativeCheckpoint = playLayer->createCheckpoint();
                if (nextCp.nativeCheckpoint) {
                    nextCp.nativeCheckpoint->retain();
                }
                if (playLayer->m_checkpointArray && playLayer->m_checkpointArray->count() > 0) {
                    playLayer->m_checkpointArray->removeAllObjects();
                }

                nextCp.snapshot.capture(playLayer->m_player1, nextCp.startTick, DeterministicPRNG::getCurrentSeed());
                if (m_hasPlayer2 && playLayer->m_player2) {
                    nextCp.snapshot2.capture(playLayer->m_player2, nextCp.startTick, DeterministicPRNG::getCurrentSeed());
                }

                m_checkpointStack.push_back(std::move(nextCp));
                m_currentTick = m_checkpointStack.back().startTick;
                m_maxReachedX = m_checkpointStack.back().startX;
                m_waveRetryCount = 0;
                m_activePopulation.clear();
                m_currentWaveSurvivors.clear();
                m_currentBotIndex = 0;

                geode::log::info("[LevelSolver] Checkpoint Wave #{} passed! New checkpoint at tick {} (X={:.1f}, {} survivors)",
                    m_activeWaveIndex, m_currentTick, m_maxReachedX, m_lastSurvivorCount);
            }
        } else {
            // Wave wipeout: all bots died!
            m_waveRetryCount++;
            m_backtrackCount++;

            if (m_solverMode == SolverMode::SpawnRespawn) {
                // Prefix-Locked Swarm: rewind frontier tick slightly and trim prefix
                const uint32_t rewindTicks = 35;
                uint32_t newFrontier = (m_frontierTick > rewindTicks) ? (m_frontierTick - rewindTicks) : 0;
                m_frontierTick = newFrontier;

                // Trim prefix
                m_verifiedPrefix.erase(std::remove_if(m_verifiedPrefix.begin(), m_verifiedPrefix.end(),
                    [newFrontier](const TickAction& a) { return a.tick >= newFrontier; }), m_verifiedPrefix.end());

                m_currentTick = m_frontierTick;
                m_activePopulation.clear();
                m_currentWaveSurvivors.clear();
                m_currentBotIndex = 0;

                geode::log::warn("[LevelSolver] Wave #{} wipeout! Rewound frontier to tick {} (backtrack #{})",
                    m_activeWaveIndex, m_frontierTick, m_backtrackCount);
            } else {
                // Classic Checkpoint Stack mode: pop failed checkpoint if repeated failures
                auto& currentCp = m_checkpointStack.back();
                currentCp.failedWaves++;
                m_activePopulation.clear();
                m_currentWaveSurvivors.clear();
                m_currentBotIndex = 0;

                if (currentCp.failedWaves >= 5 && m_checkpointStack.size() > 1) {
                    if (currentCp.nativeCheckpoint) {
                        currentCp.nativeCheckpoint->release();
                        currentCp.nativeCheckpoint = nullptr;
                    }
                    m_checkpointStack.pop_back();
                    geode::log::warn("[LevelSolver] Checkpoint wave failed 5 times: popped to tick {}",
                        m_checkpointStack.back().startTick);
                }
            }
        }
    }

    // Update live telemetry
    float totalDist = m_levelLength - m_startX;
    float currentDist = m_maxReachedX - m_startX;
    m_telemetry.explorationHorizon = (totalDist > 0.001f) ? std::clamp((currentDist / totalDist) * 100.0f, 0.0f, 100.0f) : 0.0f;
    m_telemetry.currentX = m_maxReachedX;
    m_telemetry.currentTick = m_currentTick;
    m_telemetry.rewindCount = m_backtrackCount;
    m_telemetry.frontierSize = (m_solverMode == SolverMode::SpawnRespawn) ? (m_frontierTick / 60) : m_checkpointStack.size();
    m_telemetry.activeWave = m_activeWaveIndex;
    m_telemetry.waveAttempt = (m_waveRetryCount % 10) + 1;
    m_telemetry.ticksPerSecond = HeadlessEngine::get().getTicksPerSecond();
}

void SwarmSolver::finalizeSolution(PlayLayer* playLayer, const std::vector<TickAction>& winningActions) {
    m_isCompleted = true;
    m_isRunning = false;

    // Clean and sort winning macro
    MacroManager::get().setActions(winningActions);
    m_resolvedMacro = MacroManager::get().getActions();

    // Record trajectory sample
    if (playLayer && playLayer->m_player1) {
        m_trajectorySamples.push_back({
            m_currentTick,
            playLayer->m_player1->getPositionX(),
            playLayer->m_player1->getPositionY()
        });
        MacroManager::get().setTrajectory(m_trajectorySamples);
    }

    // Save macro locally and export to Mega Hack replays
    MacroManager::get().saveMacro(m_levelID, m_levelName);
    GDRExporter::exportReplays(m_levelName, m_levelID, m_resolvedMacro);

    HeadlessEngine::get().disableHeadless();
    CheatAPIIntegrator::notifyCheatEnded();

    m_telemetry.status = SolverStatus::Solved;
    m_telemetry.explorationHorizon = 100.0f;
    m_telemetry.isVerified = true;
    m_telemetry.detailMessage = fmt::format("Solved in {} waves! Macro saved with {} inputs.",
        m_activeWaveIndex, m_resolvedMacro.size());

    geode::log::info("[LevelSolver] Level solved successfully in {} waves! {} inputs generated.",
        m_activeWaveIndex, m_resolvedMacro.size());
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
