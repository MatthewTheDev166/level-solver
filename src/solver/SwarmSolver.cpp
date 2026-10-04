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

    float endX = playLayer->getEndPosition().x;
    if (maxObjX > m_startX + 100.0f) {
        m_levelLength = std::max(endX, maxObjX + 150.0f);
    } else {
        // Empty / blank test level: set concise test length (~4s run, ~8 waves)
        m_levelLength = m_startX + 1200.0f;
    }
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

    if (playLayer->m_anticheatSpike) {
        playLayer->m_anticheatSpike->setPosition({-9999.0f, -9999.0f});
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
    geode::log::info("[LevelSolver] SwarmSolver started at X={:.1f}, target end X={:.1f} (maxObjX={:.1f})", m_startX, m_levelLength, maxObjX);
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
    m_activePopulation.clear();
    m_currentBotIndex = 0;
    m_currentWaveSurvivors.clear();
    m_currentHorizonTicks = 0;
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
        if (population.size() >= m_currentPopulationSize) return;
        SwarmBot b;
        b.segmentActions = acts;
        b.survived = false;
        b.finalX = 0.0f;
        population.push_back(std::move(b));
    };

    static std::mt19937 s_rng(1337 + waveRetryCount * 31);
    std::uniform_int_distribution<uint32_t> randDuration(4, 32);
    std::uniform_int_distribution<int> randJitter(-3, 3);

    // 1. Baseline: Idle / release entirely
    addBotWithActions({ { 0, false } });

    // 2. Baseline: Hold entirely
    addBotWithActions({ { 0, true } });

    bool isContinuousMode = (mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing || mode == VehicleMode::UFO);

    if (isContinuousMode) {
        // Multi-frequency pulse/flap waveforms
        for (uint32_t period : { 4u, 6u, 8u, 10u, 12u, 16u, 20u, 24u, 32u }) {
            std::vector<TickAction> acts50;
            bool state = true;
            for (uint32_t t = 0; t < horizonTicks; t += period) {
                acts50.push_back({ t, state });
                state = !state;
            }
            addBotWithActions(acts50);

            std::vector<TickAction> actsInv;
            state = false;
            for (uint32_t t = 0; t < horizonTicks; t += period) {
                actsInv.push_back({ t, state });
                state = !state;
            }
            addBotWithActions(actsInv);
        }
    } else {
        // Discrete mode (Cube, Robot, Ball, Spider):
        // Dense jump timing coverage across the horizon every 4 ticks
        for (uint32_t jumpAt = 0; jumpAt + 8 < horizonTicks; jumpAt += 4) {
            // Short tap (8 ticks)
            addBotWithActions({ { jumpAt, true }, { jumpAt + 8, false } });

            // Medium jump (20 ticks)
            if (jumpAt + 20 < horizonTicks) {
                addBotWithActions({ { jumpAt, true }, { jumpAt + 20, false } });
            }

            // Full jump (40 ticks)
            if (jumpAt + 40 < horizonTicks) {
                addBotWithActions({ { jumpAt, true }, { jumpAt + 40, false } });
            }
        }

        // Double jump combinations
        for (uint32_t jumpAt = 0; jumpAt + 40 < horizonTicks; jumpAt += 12) {
            addBotWithActions({
                { jumpAt, true },
                { jumpAt + 12, false },
                { jumpAt + 28, true },
                { jumpAt + 40, false }
            });
        }
    }

    // If previous survivors exist, generate mutations from the best survivors
    if (!previousSurvivors.empty()) {
        size_t survivorIdx = 0;
        while (population.size() < m_currentPopulationSize) {
            const auto& base = previousSurvivors[survivorIdx % previousSurvivors.size()].segmentActions;
            survivorIdx++;

            std::vector<TickAction> mutated = base;
            for (auto& act : mutated) {
                int newT = static_cast<int>(act.tick) + randJitter(s_rng);
                act.tick = static_cast<uint32_t>(std::clamp(newT, 0, static_cast<int>(horizonTicks > 0 ? horizonTicks - 1 : 0)));
            }

            if (s_rng() % 3 == 0 && mutated.size() > 2) {
                mutated.erase(mutated.begin() + (s_rng() % mutated.size()));
            }

            std::sort(mutated.begin(), mutated.end(), [](const TickAction& a, const TickAction& b) {
                return a.tick < b.tick;
            });
            mutated.erase(std::unique(mutated.begin(), mutated.end(), [](const TickAction& a, const TickAction& b) {
                return a.tick == b.tick;
            }), mutated.end());

            addBotWithActions(mutated);
        }
        return population;
    }

    // Otherwise, generate diverse random candidates to fill up to population size
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

void SwarmSolver::handleBacktrack(PlayLayer* playLayer) {
    while (m_checkpointStack.size() > 1) {
        m_backtrackCount++;
        auto deadEndCp = m_checkpointStack.back();
        m_checkpointStack.pop_back();
        geode::log::warn("[LevelSolver] Backtracking from dead-end at X={:.1f} (remaining depth {})", deadEndCp.startX, m_checkpointStack.size());

        auto& parentCp = m_checkpointStack.back();
        if (parentCp.runnerUpIndex < parentCp.runnerUps.size()) {
            // Activate alternate surviving runner-up branch!
            const auto& altBot = parentCp.runnerUps[parentCp.runnerUpIndex++];
            parentCp.failedWaves = 0;
            geode::log::info("[LevelSolver] Activating runner-up #{} at X={:.1f}", parentCp.runnerUpIndex, parentCp.startX);

            VehicleMode parentMode = parentCp.snapshot.mode;
            uint32_t altHorizon = (parentMode == VehicleMode::Ship || parentMode == VehicleMode::Wave || parentMode == VehicleMode::Swing) ? 72 : 120;

            BeamCheckpoint branchCp;
            simulateBot(playLayer, parentCp, const_cast<SwarmBot&>(altBot), altHorizon);
            branchCp.snapshot.capture(playLayer->m_player1, parentCp.startTick + altHorizon);
            branchCp.startTick = parentCp.startTick + altHorizon;
            branchCp.startX = altBot.finalX;
            branchCp.macroHistory = parentCp.macroHistory;
            for (const auto& act : altBot.segmentActions) {
                branchCp.macroHistory.push_back({ parentCp.startTick + act.tick, act.pressed });
            }
            branchCp.runnerUps = {};
            branchCp.runnerUpIndex = 0;
            branchCp.failedWaves = 0;

            m_checkpointStack.push_back(std::move(branchCp));
            m_currentTick = m_checkpointStack.back().startTick;
            return;
        }
        // If parent has exhausted all runner-ups, loop continues and pops parent to try parent's parent
    }

    // At root checkpoint (depth 1, X=0): preserve root indefinitely with fresh stochastic mutations
    if (!m_checkpointStack.empty()) {
        m_checkpointStack.front().failedWaves = 0;
        m_checkpointStack.front().runnerUpIndex = 0;
        m_checkpointStack.front().runnerUps.clear();
        geode::log::info("[LevelSolver] At root checkpoint (0%), generating fresh stochastic mutations");
    }
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
    const auto timeBudget = std::chrono::milliseconds(16);

    auto& currentCp = m_checkpointStack.back();
    VehicleMode mode = currentCp.snapshot.mode;

    // Continuous modes (Ship/Wave/Swing) use short 0.3s horizons; discrete modes use 0.5s horizons
    uint32_t horizonTicks = (mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing) ? 72 : 120;

    // Check if level already completed at this checkpoint
    if (currentCp.startX >= m_levelLength) {
        finalizeSolution(currentCp.macroHistory);
        return;
    }

    // Generate new population for wave if not currently evaluating one
    if (m_activePopulation.empty() || m_currentBotIndex >= m_activePopulation.size()) {
        m_activeWaveIndex++;
        m_activePopulation = generatePopulation(mode, currentCp.startTick, horizonTicks, currentCp.runnerUps, currentCp.failedWaves);
        m_currentBotIndex = 0;
        m_currentWaveSurvivors.clear();
        m_currentHorizonTicks = horizonTicks;
    }

    uint32_t simulatedTicks = 0;

    // Simulate bots in chunks across frames
    while (m_currentBotIndex < m_activePopulation.size()) {
        auto& bot = m_activePopulation[m_currentBotIndex++];
        simulateBot(playLayer, currentCp, bot, m_currentHorizonTicks);
        simulatedTicks += m_currentHorizonTicks;

        if (bot.survived) {
            m_currentWaveSurvivors.push_back(bot);
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
            // Frame budget elapsed; keep current wave state and continue remaining bots next frame!
            break;
        }
    }

    auto endBatch = std::chrono::high_resolution_clock::now();
    double batchDuration = std::chrono::duration<double>(endBatch - startBatch).count();
    HeadlessEngine::get().recordStepBatch(simulatedTicks, batchDuration);

    // Only commit wave outcome when the ENTIRE population has finished evaluating!
    if (m_currentBotIndex >= m_activePopulation.size()) {
        m_lastSurvivorCount = m_currentWaveSurvivors.size();

        if (!m_currentWaveSurvivors.empty()) {
            // Sort survivors descending by fitness score
            std::sort(m_currentWaveSurvivors.begin(), m_currentWaveSurvivors.end(), [](const SwarmBot& a, const SwarmBot& b) {
                return a.fitnessScore > b.fitnessScore;
            });

            const auto& bestBot = m_currentWaveSurvivors.front();

            // Save top alternate runner-ups into current checkpoint
            currentCp.runnerUps.clear();
            for (size_t i = 1; i < m_currentWaveSurvivors.size() && i <= 4; ++i) {
                currentCp.runnerUps.push_back(m_currentWaveSurvivors[i]);
            }
            currentCp.runnerUpIndex = 0;

            // Advance: build and push next checkpoint
            BeamCheckpoint nextCp;

            // Run best bot to capture target snapshot
            simulateBot(playLayer, currentCp, const_cast<SwarmBot&>(bestBot), m_currentHorizonTicks);
            nextCp.snapshot.capture(playLayer->m_player1, currentCp.startTick + m_currentHorizonTicks);
            nextCp.startTick = currentCp.startTick + m_currentHorizonTicks;
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
            m_currentTick = m_checkpointStack.back().startTick;

            geode::log::info("[LevelSolver] Swarm advanced: Depth={}, X={:.1f} ({:.1f}%), Survivors={}/{}",
                m_checkpointStack.size(), m_checkpointStack.back().startX,
                m_levelLength > m_startX ? ((m_checkpointStack.back().startX - m_startX) / (m_levelLength - m_startX)) * 100.0f : 0.0f,
                m_lastSurvivorCount, m_currentPopulationSize
            );
        } else {
            // Entire wave evaluated and 0 survivors found: record a strike
            currentCp.failedWaves++;
            geode::log::warn("[LevelSolver] Wave wiped out at X={:.1f} (strike {}/10)", currentCp.startX, currentCp.failedWaves);

            if (currentCp.failedWaves >= 10) {
                handleBacktrack(playLayer);
            }
        }

        // Clear active wave so next frame generates a fresh wave or proceeds from new checkpoint
        m_activePopulation.clear();
        m_currentBotIndex = 0;
        m_currentWaveSurvivors.clear();
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
