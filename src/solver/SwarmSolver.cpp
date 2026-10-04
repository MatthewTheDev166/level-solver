#include "SwarmSolver.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../replay/MacroManager.hpp"
#include <chrono>
#include <algorithm>
#include <random>

namespace solver {

SwarmSolver& SwarmSolver::get() {
    static SwarmSolver instance;
    return instance;
}

void SwarmSolver::start(PlayLayer* playLayer) {
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
    m_currentTick = 0;

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

    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
    playLayer->updateVisibility(0.0f);

    BeamCheckpoint root;
    root.snapshot.capture(playLayer->m_player1, 0, DeterministicPRNG::STATIC_SEED);
    root.startTick = 0;
    root.startX = m_startX;
    root.macroHistory = {};
    root.runnerUps = {};
    root.runnerUpIndex = 0;
    root.failedWaves = 0;

    m_checkpointStack.push_back(root);

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "Genetic Swarm active...";
    m_telemetry.currentX = m_startX;
    m_telemetry.targetEndX = m_levelLength;
    m_telemetry.checkpointDepth = 1;
    m_telemetry.populationSize = 100;
    geode::log::info("[LevelSolver] SwarmSolver started at X={:.1f}, target end X={:.1f}", m_startX, m_levelLength);
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
    m_telemetry.detailMessage = "Swarm resumed...";
    geode::log::info("[LevelSolver] SwarmSolver resumed at checkpoint depth {}", m_checkpointStack.size());
}

void SwarmSolver::stop() {
    if (m_isRunning) {
        m_isRunning = false;
        HeadlessEngine::get().disableHeadless();
        CheatAPIIntegrator::notifyCheatEnded();
        HazardDetector::clearIndex();
        m_telemetry.status = SolverStatus::Paused;
        m_telemetry.detailMessage = "Swarm paused by user";
        geode::log::info("[LevelSolver] SwarmSolver stopped");
    }
}

void SwarmSolver::reset() {
    m_isRunning = false;
    m_isCompleted = false;
    m_checkpointStack.clear();
    m_resolvedMacro.clear();
    m_activeWaveIndex = 0;
    m_waveRetryCount = 0;
    m_backtrackCount = 0;
    m_lastSurvivorCount = 0;
    m_currentTick = 0;
    m_maxReachedX = 0.0f;
    m_telemetry = TelemetryMetrics();
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

void SwarmSolver::finalizeSolution(const std::vector<TickAction>& winningActions) {
    m_resolvedMacro = winningActions;
    m_isCompleted = true;
    m_isRunning = false;

    // Ensure clean final release
    if (!m_resolvedMacro.empty() && m_resolvedMacro.back().pressed) {
        TickAction act;
        act.tick = m_resolvedMacro.back().tick + 1;
        act.pressed = false;
        m_resolvedMacro.push_back(act);
    }

    MacroManager::get().setActions(m_resolvedMacro);
    MacroManager::get().saveMacro(m_levelID);

    HeadlessEngine::get().disableHeadless();
    HazardDetector::clearIndex();

    m_telemetry.status = SolverStatus::Solved;
    m_telemetry.explorationHorizon = 100.0f;
    m_telemetry.detailMessage = "100% Solved! Ready for replay.";
    geode::log::info("[LevelSolver] Level solved 100%! Saved {} macro actions to disk.", m_resolvedMacro.size());
}

std::vector<SwarmBot> SwarmSolver::generatePopulation(
    VehicleMode mode,
    uint32_t startTick,
    uint32_t horizonTicks,
    const std::vector<SwarmBot>& previousSurvivors,
    uint32_t waveRetryCount
) {
    std::vector<SwarmBot> population;
    population.reserve(m_currentPopulationSize);

    auto addBotWithActions = [&](const std::vector<TickAction>& acts) {
        SwarmBot b;
        b.segmentActions = acts;
        b.survived = false;
        b.finalX = 0.0f;
        population.push_back(std::move(b));
    };

    // Seed 1: Release entirely (idle/slide)
    addBotWithActions({ { 0, false } });

    // Seed 2: Hold entirely
    addBotWithActions({ { 0, true } });

    // Seed 3 to 6: Pulse / Flap square waves (rhythm hold/release)
    for (uint32_t period : { 4u, 6u, 8u, 12u, 16u }) {
        std::vector<TickAction> acts;
        bool state = true;
        for (uint32_t t = 0; t < horizonTicks; t += period) {
            acts.push_back({ t, state });
            state = !state;
        }
        addBotWithActions(acts);
    }

    // Seed 7 to 10: Jump pulses at intervals
    for (uint32_t jumpAt : { 0u, 8u, 16u, 24u, 36u, 50u, 75u }) {
        if (jumpAt + 12 < horizonTicks) {
            addBotWithActions({ { jumpAt, true }, { jumpAt + 12, false } });
        }
    }

    // Deterministic PRNG for reproducibility
    static std::mt19937 s_rng(1337);
    std::uniform_int_distribution<uint32_t> randTick(0, horizonTicks > 0 ? horizonTicks - 1 : 1);
    std::uniform_int_distribution<uint32_t> randDuration(3, 30);
    std::uniform_int_distribution<int> randJitter(-3, 3);

    // If previous survivors exist, generate mutations from the best survivor
    if (!previousSurvivors.empty()) {
        const auto& base = previousSurvivors.front().segmentActions;
        while (population.size() < m_currentPopulationSize) {
            std::vector<TickAction> mutated = base;
            // Apply jitter to each transition
            for (auto& act : mutated) {
                int newT = static_cast<int>(act.tick) + randJitter(s_rng);
                act.tick = static_cast<uint32_t>(std::clamp(newT, 0, static_cast<int>(horizonTicks - 1)));
            }
            // Sort by tick and remove duplicate timestamps
            std::sort(mutated.begin(), mutated.end(), [](const TickAction& a, const TickAction& b) {
                return a.tick < b.tick;
            });
            addBotWithActions(mutated);
        }
        return population;
    }

    // Otherwise, generate diverse random candidates
    while (population.size() < m_currentPopulationSize) {
        std::vector<TickAction> acts;
        uint32_t currentT = 0;
        bool state = (population.size() % 2 == 0);

        while (currentT < horizonTicks) {
            acts.push_back({ currentT, state });
            uint32_t dur = randDuration(s_rng);
            currentT += dur;
            state = !state;
        }
        addBotWithActions(acts);
    }

    return population;
}

void SwarmSolver::simulateBot(
    PlayLayer* playLayer,
    const BeamCheckpoint& checkpoint,
    SwarmBot& bot,
    uint32_t horizonTicks
) {
    // Restore parent checkpoint snapshot
    checkpoint.snapshot.restore(playLayer->m_player1);

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;
    playLayer->m_isPaused = false;

    // Position camera
    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
    playLayer->updateVisibility(HeadlessEngine::FIXED_DT);

    size_t actionIdx = 0;
    bool currentButton = false;
    bool completed = false;

    for (uint32_t step = 0; step < horizonTicks; ++step) {
        // Apply input scheduled for this segment tick
        while (actionIdx < bot.segmentActions.size() && bot.segmentActions[actionIdx].tick <= step) {
            currentButton = bot.segmentActions[actionIdx].pressed;
            actionIdx++;
        }

        if (currentButton) {
            playLayer->handleButton(true, 1, true);
            playLayer->m_player1->pushButton(PlayerButton::Jump);
        } else {
            playLayer->handleButton(false, 1, true);
            playLayer->m_player1->releaseButton(PlayerButton::Jump);
        }

        // Step physics
        playLayer->update(HeadlessEngine::FIXED_DT);

        // Keep camera updated periodically
        if ((step & 7) == 0) {
            playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
            playLayer->updateVisibility(HeadlessEngine::FIXED_DT);
        }

        // Check if finished level
        if (playLayer->m_player1->getPositionX() >= m_levelLength || playLayer->m_hasCompletedLevel) {
            completed = true;
            bot.survived = true;
            bot.finalX = playLayer->m_player1->getPositionX();
            bot.deathTick = checkpoint.startTick + step;
            break;
        }

        // Check death
        if (playLayer->m_player1->m_isDead || playLayer->m_playerDied) {
            bot.survived = false;
            bot.finalX = playLayer->m_player1->getPositionX();
            bot.deathTick = checkpoint.startTick + step;
            playLayer->handleButton(false, 1, true);
            playLayer->m_player1->releaseButton(PlayerButton::Jump);
            playLayer->m_playerDied = false;
            playLayer->m_player1->m_isDead = false;
            return;
        }
    }

    if (!completed) {
        bot.survived = true;
        bot.finalX = playLayer->m_player1->getPositionX();
        bot.deathTick = checkpoint.startTick + horizonTicks;
    }

    // Clearance score
    size_t nearbyObs = 0;
    bot.clearance = HazardDetector::calculateClearance(playLayer->m_player1->getPosition(), playLayer->m_objects, nearbyObs);
    bot.fitnessScore = bot.finalX + (0.05f * bot.clearance);

    // Release button at end of simulation
    playLayer->handleButton(false, 1, true);
    playLayer->m_player1->releaseButton(PlayerButton::Jump);
}

void SwarmSolver::stepSwarmBatch(PlayLayer* playLayer, uint32_t maxSteps) {
    if (!m_isRunning || m_isCompleted || !playLayer || !playLayer->m_player1) return;

    if (m_checkpointStack.empty()) {
        m_isRunning = false;
        m_telemetry.status = SolverStatus::Failed;
        m_telemetry.detailMessage = "Search space exhausted (all checkpoints failed)";
        HeadlessEngine::get().disableHeadless();
        HazardDetector::clearIndex();
        CheatAPIIntegrator::notifyCheatEnded();
        geode::log::warn("[LevelSolver] {}", m_telemetry.detailMessage);
        return;
    }

    auto startBatch = std::chrono::high_resolution_clock::now();
    const auto timeBudget = std::chrono::milliseconds(12);

    auto& currentCp = m_checkpointStack.back();
    VehicleMode mode = currentCp.snapshot.mode;

    // Continuous modes (Ship/Wave/Swing) use short 0.3s horizons; discrete modes use 0.5s horizons
    uint32_t horizonTicks = (mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing) ? 72 : 120;

    // Check if level already completed at this checkpoint
    if (currentCp.startX >= m_levelLength) {
        finalizeSolution(currentCp.macroHistory);
        return;
    }

    // Generate population for this wave
    m_activeWaveIndex++;
    auto population = generatePopulation(mode, currentCp.startTick, horizonTicks, currentCp.runnerUps, currentCp.failedWaves);

    uint32_t simulatedTicks = 0;
    std::vector<SwarmBot> survivors;

    for (auto& bot : population) {
        simulateBot(playLayer, currentCp, bot, horizonTicks);
        simulatedTicks += horizonTicks;

        if (bot.survived) {
            survivors.push_back(bot);
            if (bot.finalX > m_maxReachedX) {
                m_maxReachedX = bot.finalX;
            }
            if (bot.finalX >= m_levelLength || playLayer->m_hasCompletedLevel) {
                // Winning bot! Append its actions and complete!
                std::vector<TickAction> fullMacro = currentCp.macroHistory;
                for (const auto& act : bot.segmentActions) {
                    fullMacro.push_back({ currentCp.startTick + act.tick, act.pressed });
                }
                finalizeSolution(fullMacro);
                return;
            }
        }

        auto now = std::chrono::high_resolution_clock::now();
        if (now - startBatch >= timeBudget) {
            break;
        }
    }

    m_lastSurvivorCount = survivors.size();

    auto endBatch = std::chrono::high_resolution_clock::now();
    double batchDuration = std::chrono::duration<double>(endBatch - startBatch).count();
    HeadlessEngine::get().recordStepBatch(simulatedTicks, batchDuration);

    // Process wave results
    if (!survivors.empty()) {
        // Sort survivors descending by fitness score
        std::sort(survivors.begin(), survivors.end(), [](const SwarmBot& a, const SwarmBot& b) {
            return a.fitnessScore > b.fitnessScore;
        });

        const auto& bestBot = survivors.front();

        // Save top alternate runner-ups into current checkpoint
        currentCp.runnerUps.clear();
        for (size_t i = 1; i < survivors.size() && i <= 4; ++i) {
            currentCp.runnerUps.push_back(survivors[i]);
        }

        // Advance: build and push next checkpoint
        BeamCheckpoint nextCp;

        // Run best bot to capture target snapshot
        simulateBot(playLayer, currentCp, const_cast<SwarmBot&>(bestBot), horizonTicks);
        nextCp.snapshot.capture(playLayer->m_player1, currentCp.startTick + horizonTicks);
        nextCp.startTick = currentCp.startTick + horizonTicks;
        nextCp.startX = bestBot.finalX;
        nextCp.macroHistory = currentCp.macroHistory;

        // Append best bot inputs with absolute ticks
        for (const auto& act : bestBot.segmentActions) {
            nextCp.macroHistory.push_back({ currentCp.startTick + act.tick, act.pressed });
        }

        nextCp.runnerUps = {};
        nextCp.runnerUpIndex = 0;
        nextCp.failedWaves = 0;

        m_checkpointStack.push_back(std::move(nextCp));
        m_currentTick = nextCp.startTick;

        geode::log::info("[LevelSolver] Swarm advanced: Depth={}, X={:.1f} ({:.1f}%), Survivors={}/{}",
            m_checkpointStack.size(), nextCp.startX,
            m_levelLength > m_startX ? ((nextCp.startX - m_startX) / (m_levelLength - m_startX)) * 100.0f : 0.0f,
            survivors.size(), m_currentPopulationSize
        );
    } else {
        // No survivors in this wave
        currentCp.failedWaves++;
        geode::log::warn("[LevelSolver] Wave wiped out at X={:.1f} (strike {}/3)", currentCp.startX, currentCp.failedWaves);

        if (currentCp.failedWaves >= 3) {
            // Dead-end detected: Backtrack!
            m_backtrackCount++;
            geode::log::warn("[LevelSolver] Backtracking from dead-end at X={:.1f}", currentCp.startX);

            m_checkpointStack.pop_back();

            if (!m_checkpointStack.empty()) {
                auto& parentCp = m_checkpointStack.back();
                if (parentCp.runnerUpIndex + 1 < parentCp.runnerUps.size()) {
                    parentCp.runnerUpIndex++;
                    parentCp.failedWaves = 0;
                    geode::log::info("[LevelSolver] Activating runner-up #{} at X={:.1f}", parentCp.runnerUpIndex + 1, parentCp.startX);
                }
            }
        }
    }

    // Update telemetry metrics
    m_telemetry.currentTick = m_checkpointStack.empty() ? 0 : m_checkpointStack.back().startTick;
    m_telemetry.currentX = m_maxReachedX;
    m_telemetry.targetEndX = m_levelLength;
    float totalDist = m_levelLength - m_startX;
    if (totalDist > 0.0f) {
        m_telemetry.explorationHorizon = std::clamp(((m_maxReachedX - m_startX) / totalDist) * 100.0f, 0.0f, 100.0f);
    }
    m_telemetry.activeWave = m_activeWaveIndex;
    m_telemetry.populationSize = m_currentPopulationSize;
    m_telemetry.survivorCount = m_lastSurvivorCount;
    m_telemetry.checkpointDepth = m_checkpointStack.size();
    m_telemetry.backtrackCount = m_backtrackCount;
    m_telemetry.ticksPerSecond = HeadlessEngine::get().getTicksPerSecond();
    m_telemetry.memoryFootprintBytes = m_checkpointStack.size() * sizeof(BeamCheckpoint);
    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = fmt::format("Wave #{} | Depth {} | Backtracks {}", m_activeWaveIndex, m_checkpointStack.size(), m_backtrackCount);
}

} // namespace solver
