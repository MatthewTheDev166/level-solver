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
    float currentX = playLayer->m_player1->getPositionX();

    // Physical sanity: a single wave segment horizon cannot jump forward more than 350 units
    if ((currentX - startX) > 350.0f) {
        return false;
    }

    // RobTop 100% level percent calculation
    float percent = playLayer->getCurrentPercent();
    if (percent >= 99.5f && currentX >= (levelLength - 100.0f)) {
        return true;
    }

    if (currentX >= levelLength) return true;
    if (playLayer->m_hasCompletedLevel && currentX >= (levelLength - 100.0f)) return true;
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

    if (maxObjX > m_startX + 50.0f) {
        m_levelLength = maxObjX + 100.0f;
    } else {
        float endX = playLayer->getEndPosition().x;
        if (endX > m_startX + 50.0f) {
            m_levelLength = endX;
        } else {
            // Blank level with no objects
            m_levelLength = m_startX + 600.0f;
        }
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

    playLayer->m_endPosition = cocos2d::CCPoint{ m_levelLength, 0.0f };
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
    root.snapshot.capture(playLayer->m_player1, 0, DeterministicPRNG::STATIC_SEED);
    root.startTick = 0;
    root.startX = m_startX;
    root.macroHistory = {};
    root.runnerUps = {};
    root.runnerUpIndex = 0;
    root.failedWaves = 0;

    m_checkpointStack.push_back(std::move(root));

    m_telemetry.status = SolverStatus::Searching;
    m_telemetry.detailMessage = "Genetic Swarm active...";
    m_telemetry.currentX = m_startX;
    m_telemetry.targetEndX = m_levelLength;
    m_telemetry.checkpointDepth = 1;
    m_telemetry.populationSize = m_currentPopulationSize;
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
    m_partialProgressSeeds.clear();
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
    if (m_maxReachedX < (m_levelLength - 100.0f)) {
        geode::log::error("[LevelSolver] Refusing premature finalizeSolution: maxReachedX={:.1f} is far from levelLength={:.1f}!",
            m_maxReachedX, m_levelLength);
        return;
    }

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
    MacroManager::get().saveMacro(m_levelID, m_levelName);

    // Export native Mega Hack GDR2 (.gdr2) and GDR JSON (.json)
    auto exportRes = GDRExporter::exportReplays(m_levelName, m_levelID, m_resolvedMacro);
    if (exportRes.success) {
        geode::log::info("[LevelSolver] Exported Mega Hack macro to: {}", exportRes.gdr2Path.string());
    }

    HeadlessEngine::get().disableHeadless();
    HazardDetector::clearIndex();

    m_telemetry.status = SolverStatus::Solved;
    m_telemetry.explorationHorizon = 100.0f;
    m_telemetry.detailMessage = "100% Solved! Ready for replay.";
    geode::log::info("[LevelSolver] Level solved 100%! Saved {} macro actions to disk.", m_resolvedMacro.size());
}

static std::vector<TickAction> sanitizeAndInjectAction(
    const std::vector<TickAction>& baseActions,
    uint32_t injectPressTick,
    uint32_t holdDuration,
    uint32_t maxTick
) {
    uint32_t injectReleaseTick = std::min(injectPressTick + holdDuration, maxTick > 0 ? maxTick - 1 : 0);
    if (injectReleaseTick <= injectPressTick) injectReleaseTick = injectPressTick + 1;

    // 1. Rebuild timeline, clearing any holds that overlap [injectPressTick - 2, injectReleaseTick]
    // so the new press is guaranteed to have a clean preceding release!
    std::vector<TickAction> sanitized;
    for (size_t i = 0; i < baseActions.size(); ++i) {
        auto act = baseActions[i];
        if (act.tick >= (injectPressTick > 2 ? injectPressTick - 2 : 0) && act.tick <= injectReleaseTick) {
            continue;
        }
        if (act.pressed && act.tick < injectPressTick && i + 1 < baseActions.size()) {
            if (baseActions[i + 1].tick >= (injectPressTick > 2 ? injectPressTick - 2 : 0)) {
                sanitized.push_back(act);
                uint32_t truncatedRelease = (injectPressTick > 3) ? injectPressTick - 3 : act.tick + 1;
                sanitized.push_back({ std::max(act.tick + 1, truncatedRelease), false });
                i++;
                continue;
            }
        }
        sanitized.push_back(act);
    }

    // 2. Inject the new crisp press and release
    sanitized.push_back({ injectPressTick, true });
    sanitized.push_back({ injectReleaseTick, false });

    // 3. Sort strictly by tick
    std::sort(sanitized.begin(), sanitized.end(), [](const TickAction& a, const TickAction& b) {
        if (a.tick == b.tick) return !a.pressed && b.pressed;
        return a.tick < b.tick;
    });

    // 4. Deduplicate consecutive identical states
    std::vector<TickAction> clean;
    bool lastState = false;
    for (const auto& act : sanitized) {
        if (clean.empty()) {
            if (act.pressed) {
                clean.push_back(act);
                lastState = true;
            }
        } else if (act.pressed != lastState) {
            clean.push_back(act);
            lastState = act.pressed;
        }
    }
    return clean;
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

    std::mt19937 s_rng(1337 + (waveRetryCount + m_backtrackCount * 17) * 7919 + startTick * 31 + m_activeWaveIndex * 101);
    std::uniform_int_distribution<uint32_t> randDuration(3, 36);
    std::uniform_int_distribution<int> randJitter(-3, 3);

    // 1. Baselines: Pure Idle (crucial for flat ground/safe paths!) and Full Hold
    addBotWithActions({ { 0, false } });
    addBotWithActions({ { 0, true } });

    bool isContinuousMode = (mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing || mode == VehicleMode::UFO);

    // 2. Genetic breeding / targeted mutations from previous survivors or furthest partial progress bots
    if (!previousSurvivors.empty()) {
        // Keep top survivors verbatim (elitism)
        for (size_t i = 0; i < previousSurvivors.size() && i < 4; ++i) {
            addBotWithActions(previousSurvivors[i].segmentActions);
        }

        size_t survivorIdx = 0;
        size_t mutationBudget = static_cast<size_t>(m_currentPopulationSize * 0.35f);
        while (population.size() < mutationBudget && survivorIdx < previousSurvivors.size() * 10) {
            const auto& parent = previousSurvivors[survivorIdx % previousSurvivors.size()];
            survivorIdx++;

            if (!parent.survived && parent.deathTick > startTick) {
                // Targeted mutation: inject evasive jump/orb tap prior to death, cleanly truncating any prior hold!
                uint32_t localDeath = parent.deathTick - startTick;
                float spd = playerSpeed > 0.1f ? playerSpeed : 1.0f;
                uint32_t minLead = static_cast<uint32_t>(14.0f * spd);
                uint32_t maxLeadSpan = static_cast<uint32_t>(36.0f * spd);
                uint32_t preLead = minLead + (s_rng() % (maxLeadSpan + 1));
                uint32_t jumpT = (localDeath > preLead) ? (localDeath - preLead) : (localDeath > 4 ? localDeath - 4 : 0);
                uint32_t dur = (mode == VehicleMode::Robot) ? (16 + (s_rng() % 24)) : (mode == VehicleMode::Cube ? 14 : 10);
                auto mutated = sanitizeAndInjectAction(parent.segmentActions, jumpT, dur, horizonTicks);
                addBotWithActions(mutated);
            } else {
                std::vector<TickAction> mutated = parent.segmentActions;
                // Jitter existing actions
                for (auto& act : mutated) {
                    int newT = static_cast<int>(act.tick) + randJitter(s_rng);
                    act.tick = static_cast<uint32_t>(std::clamp(newT, 0, static_cast<int>(horizonTicks > 0 ? horizonTicks - 1 : 0)));
                }

                // Mutation: add a tap in the second half of the horizon
                if (s_rng() % 3 == 0) {
                    uint32_t tapTick = (horizonTicks / 2) + (s_rng() % (horizonTicks > 2 ? horizonTicks / 2 : 1));
                    mutated = sanitizeAndInjectAction(mutated, tapTick, 10, horizonTicks);
                }

                // Clean and sort mutated actions
                std::sort(mutated.begin(), mutated.end(), [](const TickAction& a, const TickAction& b) {
                    return a.tick < b.tick;
                });
                mutated.erase(std::unique(mutated.begin(), mutated.end(), [](const TickAction& a, const TickAction& b) {
                    return a.tick == b.tick;
                }), mutated.end());

                addBotWithActions(mutated);
            }
        }
    }

    if (isContinuousMode) {
        // Continuous mode (Ship/Wave/Swing/UFO):
        // Systematic multi-frequency waveforms with phase shifts across wave retries
        uint32_t phaseOffset = (waveRetryCount * 3 + m_backtrackCount * 5) % 16;
        for (uint32_t period : { 4u, 6u, 8u, 10u, 12u, 16u, 20u, 24u, 32u }) {
            // 50% duty cycle
            std::vector<TickAction> acts50;
            bool state = true;
            for (uint32_t t = phaseOffset; t < horizonTicks; t += period) {
                acts50.push_back({ t, state });
                state = !state;
            }
            addBotWithActions(acts50);

            // Inverted 50% duty cycle
            std::vector<TickAction> actsInv;
            state = false;
            for (uint32_t t = phaseOffset; t < horizonTicks; t += period) {
                actsInv.push_back({ t, state });
                state = !state;
            }
            addBotWithActions(actsInv);

            // 25% feathering duty cycle (short pulses)
            if (period >= 8) {
                std::vector<TickAction> actsFeather;
                for (uint32_t t = phaseOffset; t + (period / 4) < horizonTicks; t += period) {
                    actsFeather.push_back({ t, true });
                    actsFeather.push_back({ t + (period / 4), false });
                }
                addBotWithActions(actsFeather);
            }

            // 75% climb duty cycle
            if (period >= 8) {
                std::vector<TickAction> actsClimb;
                for (uint32_t t = phaseOffset; t + (3 * period / 4) < horizonTicks; t += period) {
                    actsClimb.push_back({ t, true });
                    actsClimb.push_back({ t + (3 * period / 4), false });
                }
                addBotWithActions(actsClimb);
            }
        }
    } else {
        // Discrete mode (Cube, Ball, Robot, Spider):
        uint32_t phase = (waveRetryCount + m_backtrackCount) % 2;

        // 1. Upcoming Interactable-Aware Targeted Candidates (Orbs & Dash Rings)
        auto interactables = HazardDetector::getInteractablesInWindow(startX - 10.0f, startX + 260.0f, levelObjects);
        float spd = playerSpeed > 0.1f ? playerSpeed : 1.0f;
        float unitsPerTick = 1.30f * spd;

        for (GameObject* obj : interactables) {
            if (!obj) continue;
            float dist = obj->getPositionX() - startX;
            int estTick = static_cast<int>(dist / unitsPerTick);
            if (estTick < 0 || estTick >= static_cast<int>(horizonTicks + 15)) continue;

            if (HazardDetector::isDashOrb(obj)) {
                // Dash Orb: sustained hold profiles across the horizon
                for (int j1 = std::max(0, estTick - 22); j1 <= estTick - 8; j1 += 4) {
                    for (uint32_t hold : { 24u, 48u, 72u, 96u, horizonTicks }) {
                        addBotWithActions({
                            { static_cast<uint32_t>(j1), true },
                            { static_cast<uint32_t>(j1 + 7), false },
                            { static_cast<uint32_t>(estTick), true },
                            { std::min(static_cast<uint32_t>(estTick + hold), horizonTicks > 0 ? horizonTicks - 1 : 0), false }
                        });
                    }
                }
                for (int t = std::max(0, estTick - 4); t <= estTick + 4; t += 2) {
                    for (uint32_t hold : { 24u, 48u, 72u, 96u, horizonTicks }) {
                        addBotWithActions({
                            { static_cast<uint32_t>(t), true },
                            { std::min(static_cast<uint32_t>(t + hold), horizonTicks > 0 ? horizonTicks - 1 : 0), false }
                        });
                    }
                }
            } else if (HazardDetector::isOrb(obj)) {
                // Regular Orb (Pink, Yellow, Red, Blue, Green, Black, Spider, Teleport):
                // Fine-grained takeoff + mid-air orb tap clusters (solves quad spikes with pink orb!)
                for (int j1 = std::max(0, estTick - 26); j1 <= std::max(0, estTick - 8); j1 += 2) {
                    for (int j2 = std::max(j1 + 10, estTick - 5); j2 <= std::min(static_cast<int>(horizonTicks) - 1, estTick + 6); j2 += 2) {
                        addBotWithActions({
                            { static_cast<uint32_t>(j1), true },
                            { static_cast<uint32_t>(j1 + 7), false },
                            { static_cast<uint32_t>(j2), true },
                            { static_cast<uint32_t>(std::min(j2 + 10, static_cast<int>(horizonTicks) - 1)), false }
                        });
                    }
                }
                // Direct tap for low/grounded orbs
                for (int t = std::max(0, estTick - 4); t <= std::min(static_cast<int>(horizonTicks) - 1, estTick + 4); t += 2) {
                    addBotWithActions({
                        { static_cast<uint32_t>(t), true },
                        { static_cast<uint32_t>(std::min(t + 10, static_cast<int>(horizonTicks) - 1)), false }
                    });
                }
            }
        }

        // 2. Systematic Multi-Jump & Orb Sweep (Universal Coverage)
        for (uint32_t j1 = phase; j1 + 24 < horizonTicks && population.size() < static_cast<size_t>(m_currentPopulationSize * 0.70f); j1 += 4) {
            for (uint32_t dt : { 12u, 16u, 20u, 24u, 28u, 32u }) {
                if (j1 + dt < horizonTicks) {
                    addBotWithActions({
                        { j1, true },
                        { j1 + 7, false },
                        { j1 + dt, true },
                        { std::min(j1 + dt + 10, horizonTicks - 1), false }
                    });
                    // Dash / buffer hold
                    addBotWithActions({
                        { j1, true },
                        { j1 + 7, false },
                        { j1 + dt, true },
                        { std::min(j1 + dt + 45, horizonTicks - 1), false }
                    });
                }
            }
            // 3-action chain (Ground Jump -> Orb 1 -> Orb 2)
            if (j1 + 44 < horizonTicks) {
                addBotWithActions({
                    { j1, true },
                    { j1 + 7, false },
                    { j1 + 18, true },
                    { j1 + 24, false },
                    { j1 + 36, true },
                    { std::min(j1 + 44, horizonTicks - 1), false }
                });
            }
        }

        // 3. Single Clean Jumps (Full Horizon Grid)
        if (mode == VehicleMode::Robot) {
            for (uint32_t jumpAt = phase; jumpAt + 8 < horizonTicks && population.size() < static_cast<size_t>(m_currentPopulationSize * 0.85f); jumpAt += 4) {
                addBotWithActions({ { jumpAt, true }, { jumpAt + 10, false } });
                addBotWithActions({ { jumpAt, true }, { jumpAt + 22, false } });
                addBotWithActions({ { jumpAt, true }, { jumpAt + 36, false } });
            }
        } else {
            for (uint32_t jumpAt = phase; jumpAt + 4 < horizonTicks && population.size() < static_cast<size_t>(m_currentPopulationSize * 0.85f); jumpAt += 2) {
                addBotWithActions({ { jumpAt, true }, { jumpAt + 14, false } });
                if ((jumpAt % 8) == phase) {
                    addBotWithActions({ { jumpAt, true }, { std::min(jumpAt + 48, horizonTicks > 0 ? horizonTicks - 1 : 0), false } });
                }
            }
        }
    }

    // Fill remaining population with fresh pseudo-random exploratory sequences
    while (population.size() < m_currentPopulationSize) {
        std::vector<TickAction> acts;
        uint32_t currentT = s_rng() % 10;
        bool state = (s_rng() % 2 == 0);

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
    // Restore parent checkpoint using RobTop's full practice checkpoint when available
    if (checkpoint.nativeCheckpoint) {
        playLayer->loadFromCheckpoint(checkpoint.nativeCheckpoint);
    }
    // Unconditionally restore player snapshot so state and position never leak across bots!
    checkpoint.snapshot.restore(playLayer->m_player1);

    playLayer->m_started = true;
    playLayer->m_inResetDelay = false;
    playLayer->m_playerDied = false;
    playLayer->m_player1->m_isDead = false;
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;
    playLayer->m_isPaused = false;
    playLayer->m_hasCompletedLevel = false;

    // Position camera
    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());

    // Ensure clean input queue and player button state before simulation
    playLayer->m_queuedButtons.clear();
    if (playLayer->m_player1) {
        playLayer->m_player1->releaseButton(PlayerButton::Jump);
        playLayer->m_player1->m_jumpBuffered = false;
    }
    if (playLayer->m_player2) {
        playLayer->m_player2->releaseButton(PlayerButton::Jump);
        playLayer->m_player2->m_jumpBuffered = false;
    }

    if (checkpoint.hasPlayer2 && playLayer->m_player2) {
        checkpoint.snapshot2.restore(playLayer->m_player2);
    }

    size_t actionIdx = 0;
    bool currentButton = false;
    bool lastButton = false;
    bool completed = false;
    float xNearEnd = checkpoint.startX;

    float prevX = playLayer->m_player1->getPositionX();
    uint32_t jammedTicks = 0;

    for (uint32_t step = 0; step < horizonTicks; ++step) {
        // Track position near end of segment to detect wall jams
        if (step + 10 == horizonTicks) {
            xNearEnd = playLayer->m_player1->getPositionX();
        }

        // Apply input scheduled for this segment tick
        while (actionIdx < bot.segmentActions.size() && bot.segmentActions[actionIdx].tick <= step) {
            currentButton = bot.segmentActions[actionIdx].pressed;
            actionIdx++;
        }

        // Only dispatch button on state transition (edge-triggered, no 240Hz spam!)
        if (currentButton != lastButton) {
            playLayer->handleButton(currentButton, 1, true);
            lastButton = currentButton;
        }

        // Step physics
        playLayer->update(HeadlessEngine::FIXED_DT);

        // Keep camera locked to player position so RobTop's active section collision structures update
        playLayer->moveCameraToPos(playLayer->m_player1->getPosition());

        // Check if finished level
        if (isSimulationFinished(playLayer, m_levelLength, checkpoint.startX)) {
            completed = true;
            bot.survived = true;
            bot.finalX = playLayer->m_player1->getPositionX();
            bot.deathTick = checkpoint.startTick + step;
            break;
        }

        // Check death (covers both Player 1 and Player 2 in dual mode)
        bool isDead = playLayer->m_player1->m_isDead || 
                      (playLayer->m_player2 && playLayer->m_player2->m_isDead) || 
                      playLayer->m_playerDied;
        if (isDead) {
            bot.survived = false;
            bot.finalX = playLayer->m_player1->getPositionX();
            bot.deathTick = checkpoint.startTick + step;
            if (lastButton) {
                playLayer->handleButton(false, 1, true);
                lastButton = false;
            }
            playLayer->m_playerDied = false;
            playLayer->m_player1->m_isDead = false;
            if (playLayer->m_player2) {
                playLayer->m_player2->m_isDead = false;
            }
            playLayer->m_queuedButtons.clear();
            if (playLayer->m_player1) {
                playLayer->m_player1->releaseButton(PlayerButton::Jump);
                playLayer->m_player1->m_jumpBuffered = false;
            }
            if (playLayer->m_player2) {
                playLayer->m_player2->releaseButton(PlayerButton::Jump);
                playLayer->m_player2->m_jumpBuffered = false;
            }
            return;
        }

        // Wall jam detection: accounts for 2.2 reverse mode (going left)
        float currentX = playLayer->m_player1->getPositionX();
        bool isGoingLeft = playLayer->m_player1->m_isGoingLeft;
        float deltaX = isGoingLeft ? (prevX - currentX) : (currentX - prevX);

        if (step > 4 && deltaX < 0.001f && !playLayer->m_player1->m_isDashing && !playLayer->m_player1->m_isSpider) {
            jammedTicks++;
            if (jammedTicks >= 4) {
                bot.survived = false;
                bot.finalX = currentX;
                bot.deathTick = checkpoint.startTick + step;
                if (lastButton) {
                    playLayer->handleButton(false, 1, true);
                    lastButton = false;
                }
                playLayer->m_playerDied = false;
                playLayer->m_player1->m_isDead = false;
                if (playLayer->m_player2) {
                    playLayer->m_player2->m_isDead = false;
                }
                playLayer->m_queuedButtons.clear();
                if (playLayer->m_player1) {
                    playLayer->m_player1->releaseButton(PlayerButton::Jump);
                    playLayer->m_player1->m_jumpBuffered = false;
                }
                if (playLayer->m_player2) {
                    playLayer->m_player2->releaseButton(PlayerButton::Jump);
                    playLayer->m_player2->m_jumpBuffered = false;
                }
                return;
            }
        } else {
            jammedTicks = 0;
        }
        prevX = currentX;
    }

    if (!completed && isSimulationFinished(playLayer, m_levelLength, checkpoint.startX)) {
        completed = true;
        bot.survived = true;
        bot.finalX = playLayer->m_player1->getPositionX();
        bot.deathTick = checkpoint.startTick + horizonTicks;
    }

    bool isGoingLeft = playLayer->m_player1->m_isGoingLeft;
    if (!completed) {
        bot.finalX = playLayer->m_player1->getPositionX();
        bot.deathTick = checkpoint.startTick + horizonTicks;

        float netProgress = isGoingLeft ? (checkpoint.startX - bot.finalX) : (bot.finalX - checkpoint.startX);
        float progressInLastTicks = isGoingLeft ? (xNearEnd - bot.finalX) : (bot.finalX - xNearEnd);

        // A valid survivor must make net progress and must still be moving near the end
        if (netProgress > 25.0f && progressInLastTicks > 1.5f) {
            bot.survived = true;
        } else {
            bot.survived = false;
        }
    }

    // Clearance score: distance to nearest hazard
    size_t nearbyObs = 0;
    bot.clearance = HazardDetector::calculateClearance(playLayer->m_player1->getPosition(), playLayer->m_objects, nearbyObs);
    bool hasNearbyInteractable = HazardDetector::isNearInteractable(playLayer->m_player1->getPosition(), playLayer->m_objects, 220.0f);

    VehicleMode botMode = checkpoint.snapshot.mode;
    bool isContinuous = (botMode == VehicleMode::Ship || botMode == VehicleMode::Wave || botMode == VehicleMode::Swing || botMode == VehicleMode::UFO);

    // Fitness score: distance made along level path
    float progress = isGoingLeft ? (checkpoint.startX - bot.finalX) : (bot.finalX - checkpoint.startX);
    float fitness = progress + (0.05f * bot.clearance);
    if (!isContinuous) {
        // Count active jump presses in this segment
        size_t jumpCount = 0;
        for (const auto& act : bot.segmentActions) {
            if (act.pressed) jumpCount++;
        }

        if (jumpCount == 0) {
            // Pure idle: strong stability bonus on safe ground
            fitness += 35.0f;
        } else if (nearbyObs == 0 && !hasNearbyInteractable) {
            // Unnecessary jumping on clear ground: heavy penalty to eliminate metronome pattern
            fitness -= (20.0f * static_cast<float>(jumpCount));
        } else {
            // Necessary jump near obstacles or orbs: slight penalty per input to reward minimal actions
            fitness -= (3.0f * static_cast<float>(jumpCount));
        }

        // Stable grounded landing preference for checkpoints
        if (playLayer->m_player1->m_isOnGround) {
            fitness += 20.0f;
        } else if (std::abs(playLayer->m_player1->m_yVelocity) > 6.0f) {
            fitness -= 15.0f; // Airborne falling/flying penalty at segment boundary
        }
    }
    bot.fitnessScore = fitness;

    // Release button at end of simulation if left pressed
    if (lastButton) {
        playLayer->handleButton(false, 1, true);
        lastButton = false;
    }
    playLayer->m_queuedButtons.clear();
    if (playLayer->m_player1) {
        playLayer->m_player1->releaseButton(PlayerButton::Jump);
        playLayer->m_player1->m_jumpBuffered = false;
    }
    if (playLayer->m_player2) {
        playLayer->m_player2->releaseButton(PlayerButton::Jump);
        playLayer->m_player2->m_jumpBuffered = false;
    }
}

void SwarmSolver::handleBacktrack(PlayLayer* playLayer) {
    while (m_checkpointStack.size() > 1) {
        m_backtrackCount++;
        auto deadEndCp = m_checkpointStack.back();
        m_checkpointStack.pop_back();
        geode::log::warn("[LevelSolver] Backtracking from dead-end at X={:.1f} (remaining depth {})", deadEndCp.startX, m_checkpointStack.size());

        auto& parentCp = m_checkpointStack.back();
        while (parentCp.runnerUpIndex < parentCp.runnerUps.size()) {
            const auto& altBot = parentCp.runnerUps[parentCp.runnerUpIndex++];
            if (!altBot.survived) continue; // Skip partial survivors during backtrack branching

            parentCp.failedWaves = 0;
            geode::log::info("[LevelSolver] Activating runner-up #{} at X={:.1f}", parentCp.runnerUpIndex, parentCp.startX);

            VehicleMode parentMode = parentCp.snapshot.mode;
            uint32_t altHorizon = (parentMode == VehicleMode::Ship || parentMode == VehicleMode::Wave || parentMode == VehicleMode::Swing) ? 72 : 120;

            BeamCheckpoint branchCp;
            simulateBot(playLayer, parentCp, const_cast<SwarmBot&>(altBot), altHorizon);
            branchCp.nativeCheckpoint = playLayer->createCheckpoint();
            if (branchCp.nativeCheckpoint) {
                branchCp.nativeCheckpoint->retain();
            }
            branchCp.snapshot.capture(playLayer->m_player1, parentCp.startTick + altHorizon);
            bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
            branchCp.hasPlayer2 = isDual;
            if (isDual) {
                branchCp.snapshot2.capture(playLayer->m_player2, parentCp.startTick + altHorizon);
            }
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
        m_backtrackCount++;
        m_checkpointStack.front().failedWaves = 0;
        m_checkpointStack.front().runnerUpIndex = 0;
        m_checkpointStack.front().runnerUps.clear();
        geode::log::info("[LevelSolver] At root checkpoint (0%), generating fresh stochastic mutations (epoch {})", m_backtrackCount);
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
    const auto timeBudget = std::chrono::milliseconds(12);

    auto& currentCp = m_checkpointStack.back();
    VehicleMode mode = currentCp.snapshot.mode;

    // Continuous modes (Ship/Wave/Swing) use short 0.3s horizons; discrete modes use 0.5s horizons
    uint32_t horizonTicks = (mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing) ? 72 : 120;

    // Check if level already completed at this checkpoint
    if (isSimulationFinished(playLayer, m_levelLength, currentCp.startX) || currentCp.startX >= m_levelLength) {
        finalizeSolution(currentCp.macroHistory);
        return;
    }

    // Adaptive population scaling: boost from 160 to 240 bots on obstacle strikes
    m_currentPopulationSize = (currentCp.failedWaves > 0) ? 240 : 160;

    // Generate new population for wave if not currently evaluating one
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
            playLayer ? playLayer->m_objects : nullptr
        );
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
            if (isSimulationFinished(playLayer, m_levelLength, currentCp.startX)) {
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

            // Save diverse top alternate runner-ups into current checkpoint
            currentCp.runnerUps.clear();
            for (size_t i = 1; i < m_currentWaveSurvivors.size() && currentCp.runnerUps.size() < 4; ++i) {
                bool isDuplicate = false;
                for (const auto& existing : currentCp.runnerUps) {
                    if (existing.segmentActions == m_currentWaveSurvivors[i].segmentActions) {
                        isDuplicate = true;
                        break;
                    }
                }
                if (!isDuplicate) {
                    currentCp.runnerUps.push_back(m_currentWaveSurvivors[i]);
                }
            }
            currentCp.runnerUpIndex = 0;

            // Advance: build and push next checkpoint
            BeamCheckpoint nextCp;

            // Run best bot to capture target snapshot
            simulateBot(playLayer, currentCp, const_cast<SwarmBot&>(bestBot), m_currentHorizonTicks);
            if (isSimulationFinished(playLayer, m_levelLength, currentCp.startX)) {
                std::vector<TickAction> fullMacro = currentCp.macroHistory;
                for (const auto& act : bestBot.segmentActions) {
                    fullMacro.push_back({ currentCp.startTick + act.tick, act.pressed });
                }
                finalizeSolution(fullMacro);
                return;
            }

            nextCp.nativeCheckpoint = playLayer->createCheckpoint();
            if (nextCp.nativeCheckpoint) {
                nextCp.nativeCheckpoint->retain();
            }
            nextCp.snapshot.capture(playLayer->m_player1, currentCp.startTick + m_currentHorizonTicks);
            bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
            nextCp.hasPlayer2 = isDual;
            if (isDual) {
                nextCp.snapshot2.capture(playLayer->m_player2, currentCp.startTick + m_currentHorizonTicks);
            }
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
            m_partialProgressSeeds.clear();

            geode::log::info("[LevelSolver] Swarm advanced: Depth={}, X={:.1f} ({:.1f}%), Survivors={}/{}",
                m_checkpointStack.size(), m_checkpointStack.back().startX,
                m_levelLength > m_startX ? ((m_checkpointStack.back().startX - m_startX) / (m_levelLength - m_startX)) * 100.0f : 0.0f,
                m_lastSurvivorCount, m_currentPopulationSize
            );
        } else {
            // Entire wave evaluated and 0 survivors found:
            currentCp.failedWaves++;

            // Collect the furthest partial survivors to breed off them on the next wave!
            std::sort(m_activePopulation.begin(), m_activePopulation.end(), [](const SwarmBot& a, const SwarmBot& b) {
                return a.finalX > b.finalX;
            });

            float waveMaxX = m_activePopulation.empty() ? 0.0f : m_activePopulation.front().finalX;

            // Seed top furthest partial bots into m_partialProgressSeeds (do NOT overwrite runnerUps!)
            m_partialProgressSeeds.clear();
            for (size_t i = 0; i < m_activePopulation.size() && m_partialProgressSeeds.size() < 4; ++i) {
                if (m_activePopulation[i].finalX > currentCp.startX + 10.0f) {
                    m_partialProgressSeeds.push_back(m_activePopulation[i]);
                }
            }

            geode::log::warn("[LevelSolver] Wave #{} wiped out at X={:.1f} (strike {}/10, best reached X={:.1f})",
                m_activeWaveIndex, currentCp.startX, currentCp.failedWaves, waveMaxX);

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
    if (playLayer && playLayer->getCurrentPercent() > m_telemetry.explorationHorizon) {
        m_telemetry.explorationHorizon = std::clamp(playLayer->getCurrentPercent(), 0.0f, 100.0f);
    }
    m_telemetry.activeWave = m_activeWaveIndex;
    m_telemetry.populationSize = m_currentPopulationSize;
    m_telemetry.survivorCount = m_lastSurvivorCount;
    m_telemetry.currentBotIndex = m_currentBotIndex;
    m_telemetry.currentSurvivors = m_currentWaveSurvivors.size();
    m_telemetry.checkpointDepth = m_checkpointStack.size();
    m_telemetry.backtrackCount = m_backtrackCount;
    m_telemetry.ticksPerSecond = HeadlessEngine::get().getTicksPerSecond();
    m_telemetry.memoryFootprintBytes = m_checkpointStack.size() * sizeof(BeamCheckpoint);
    m_telemetry.status = SolverStatus::Searching;
    if (currentCp.failedWaves > 0) {
        m_telemetry.detailMessage = fmt::format("Wave #{} retry (strike {}/10) | Depth {} | Backtracks {}",
            m_activeWaveIndex, currentCp.failedWaves, m_checkpointStack.size(), m_backtrackCount);
    } else {
        m_telemetry.detailMessage = fmt::format("Wave #{} | Depth {} | Backtracks {}",
            m_activeWaveIndex, m_checkpointStack.size(), m_backtrackCount);
    }
}

} // namespace solver
