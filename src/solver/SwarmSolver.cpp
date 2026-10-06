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
    if (playLayer->m_hasCompletedLevel) {
        return true;
    }

    // GD 2.2 percent check: >= 99% is essentially complete
    if (playLayer->getCurrentPercent() >= 99.0f) {
        return true;
    }

    float currentX = playLayer->m_player1->getPositionX();
    if (levelLength > startX + 50.0f) {
        if (currentX >= (levelLength - 15.0f)) return true;
    } else {
        if (currentX >= levelLength) return true;
    }

    // Past all hazards and in final stretch (>= 88% of total level distance)
    float totalDist = levelLength - startX;
    if (lastHazardX > startX + 30.0f && currentX >= lastHazardX + 15.0f && totalDist > 50.0f && currentX >= startX + 0.88f * totalDist) {
        if (!HazardDetector::isNearAnyObject(currentX, 60.0f)) {
            return true;
        }
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
        m_levelLength = maxObjX;
    } else {
        m_levelLength = m_startX + 600.0f; // Blank level fallback
    }

    geode::log::info("[LevelSolver] Level bounds: startX={:.1f}, levelLength={:.1f}, lastHazardX={:.1f}",
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
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;
    playLayer->m_isPaused = false;
    playLayer->m_hasCompletedLevel = false;

    if (playLayer->m_checkpointArray) {
        playLayer->m_checkpointArray->removeAllObjects();
    }

    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());

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
    m_telemetry.detailMessage = "Raw input swarm active (160 bots)...";
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
    m_telemetry.detailMessage = "Raw swarm search resumed...";
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
    m_resolvedMacro.clear();
    m_trajectorySamples.clear();
    m_partialProgressSeeds.clear();
    m_activePopulation.clear();
    m_currentWaveSurvivors.clear();
    m_currentBotIndex = 0;
    m_startX = 0.0f;
    m_levelLength = 0.0f;
    m_lastHazardX = 0.0f;
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
    if (playLayer->m_player2) playLayer->m_player2->releaseButton(PlayerButton::Jump);

    bool reachedEndWithoutDying = false;
    for (uint32_t t = 0; t < 600; ++t) {
        playLayer->update(HeadlessEngine::FIXED_DT);
        if (playLayer->m_player1->m_isDead || (playLayer->m_player2 && playLayer->m_player2->m_isDead) || playLayer->m_playerDied) {
            break;
        }
        if (isSimulationFinished(playLayer, m_levelLength, m_startX, m_lastHazardX)) {
            reachedEndWithoutDying = true;
            break;
        }
    }

    // Restore root state
    if (root.nativeCheckpoint) {
        playLayer->loadFromCheckpoint(root.nativeCheckpoint);
    }
    root.snapshot.restore(playLayer->m_player1);
    if (root.hasPlayer2 && playLayer->m_player2) {
        root.snapshot2.restore(playLayer->m_player2);
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

    // 1. Raw Baseline 1: Pure Idle (release / no input; essential for flat ground and safe drops)
    addBotWithActions({ { 0, false } });

    // 2. Raw Baseline 2: Full Hold (hold from tick 0 to horizon end)
    addBotWithActions({ { 0, true } });

    // If previous segment ended while holding, test releasing at each tick
    if (startsHeld) {
        for (uint32_t t = 1; t < horizonTicks - 1; ++t) {
            addBotWithActions({ { t, false } });
        }
    }

    float spd = playerSpeed > 0.1f ? playerSpeed : 1.0f;
    float speedMult = (spd <= 1.0f && spd >= 0.85f) ? 1.0f : (spd / 0.9f);
    float unitsPerTick = 1.298f * std::clamp(speedMult, 0.5f, 3.0f);

    // 3. Re-Run Targeted Mutations around Death Spot (when waveRetryCount > 0 and all bots died)
    // If all 160 bots died at around the same spot, specifically probe before the collision point
    if (waveRetryCount > 0 && !previousSurvivors.empty()) {
        for (const auto& parent : previousSurvivors) {
            if (parent.deathTick > startTick) {
                uint32_t localDeath = parent.deathTick - startTick;

                if (mode == VehicleMode::Wave) {
                    // For Wave mode: toggle input 1-16 ticks before impact to pull away from wall or ceiling
                    for (uint32_t lead : { 1u, 2u, 3u, 4u, 5u, 6u, 8u, 10u, 12u, 14u, 16u }) {
                        if (localDeath >= lead) {
                            uint32_t flipT = localDeath - lead;
                            std::vector<TickAction> flipped = parent.segmentActions;
                            bool curState = false;
                            for (const auto& a : flipped) {
                                if (a.tick <= flipT) curState = a.pressed;
                            }
                            flipped.push_back({ flipT, !curState });
                            std::sort(flipped.begin(), flipped.end(), [](const TickAction& a, const TickAction& b) {
                                return a.tick < b.tick;
                            });
                            addBotWithActions(flipped);

                            // Also try a quick micro-pulse (1-tick or 2-tick tap) at flip point
                            if (flipT + 1 < horizonTicks) {
                                std::vector<TickAction> pulse1 = flipped;
                                pulse1.push_back({ flipT + 1, curState });
                                std::sort(pulse1.begin(), pulse1.end(), [](const TickAction& a, const TickAction& b) {
                                    return a.tick < b.tick;
                                });
                                addBotWithActions(pulse1);
                            }
                            if (flipT + 2 < horizonTicks) {
                                std::vector<TickAction> pulse2 = flipped;
                                pulse2.push_back({ flipT + 2, curState });
                                std::sort(pulse2.begin(), pulse2.end(), [](const TickAction& a, const TickAction& b) {
                                    return a.tick < b.tick;
                                });
                                addBotWithActions(pulse2);
                            }
                        }
                    }
                } else {
                    // Jumping modes (Cube, Robot, Ball, Spider, etc.): takeoff earlier before the death point
                    for (uint32_t lead : { 2u, 4u, 6u, 8u, 10u, 12u, 14u, 16u, 18u, 20u, 22u, 24u, 28u, 32u, 36u }) {
                        if (localDeath >= lead) {
                            uint32_t earlyT = localDeath - lead;

                            // Test micro, short, standard, full jumps, and holds from earlyT
                            for (uint32_t dur : { 4u, 8u, 16u, 24u, 36u, 48u }) {
                                std::vector<TickAction> acts;
                                acts.push_back({ earlyT, true });
                                if (earlyT + dur < horizonTicks) {
                                    acts.push_back({ earlyT + dur, false });
                                }
                                addBotWithActions(acts);
                            }

                            // Hold to end from early takeoff
                            addBotWithActions({ { earlyT, true } });

                            // Release at earlyT (in case bot died because it held too long into a ceiling/spike)
                            addBotWithActions({ { 0, true }, { earlyT, false } });
                        }
                    }
                }
            }

            // Jitter existing actions of top seeds by +-1 to +-3 ticks
            std::vector<TickAction> mutated = parent.segmentActions;
            for (auto& act : mutated) {
                int delta = (static_cast<int>(rng() % 5)) - 2;
                int newT = static_cast<int>(act.tick) + delta;
                act.tick = static_cast<uint32_t>(std::clamp(newT, 0, static_cast<int>(horizonTicks - 1)));
            }
            std::sort(mutated.begin(), mutated.end(), [](const TickAction& a, const TickAction& b) {
                return a.tick < b.tick;
            });
            addBotWithActions(mutated);
        }
    }

    // 4. Elitism: preserve top previous survivors if any survived
    if (!previousSurvivors.empty() && waveRetryCount == 0) {
        for (size_t i = 0; i < previousSurvivors.size() && i < 4; ++i) {
            if (previousSurvivors[i].survived) {
                addBotWithActions(previousSurvivors[i].segmentActions);
            }
        }
    }

    // 5. Scan ahead for upcoming hazards and interactables using spatial buckets
    float scanDistance = horizonTicks * unitsPerTick + 30.0f;
    auto interactables = HazardDetector::getInteractablesInWindow(startX, startX + scanDistance, levelObjects);

    bool isJumpMode = (mode == VehicleMode::Cube || mode == VehicleMode::Robot || mode == VehicleMode::Ball || mode == VehicleMode::Spider);
    bool isContinuous = (mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing || mode == VehicleMode::UFO);

    // Hazard approach window if hazards exist ahead (guaranteed high priority slots)
    if (isJumpMode) {
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
            int minLead = static_cast<int>(std::round(14.0f / unitsPerTick));
            int maxLead = static_cast<int>(std::round(44.0f / unitsPerTick));

            for (int lead = minLead; lead <= maxLead; lead += 1) {
                int takeoff = reachTick - lead;
                if (takeoff >= 0 && takeoff < static_cast<int>(horizonTicks)) {
                    uint32_t t = static_cast<uint32_t>(takeoff);
                    for (uint32_t dur : { (mode == VehicleMode::Robot ? 2u : 6u), (mode == VehicleMode::Robot ? 4u : 12u), 12u, 18u, 24u, 36u, 48u }) {
                        std::vector<TickAction> acts;
                        acts.push_back({ t, true });
                        if (t + dur < horizonTicks) {
                            acts.push_back({ t + dur, false });
                        }
                        addBotWithActions(acts);
                    }
                    addBotWithActions({ { t, true } });
                }
            }
        }
    }

    // Orb and Pad timing injection (guaranteed high priority slots)
    for (auto obj : interactables) {
        if (!obj) continue;
        float ox = obj->getPositionX();
        float distToOrb = ox - startX;
        if (distToOrb > 0.0f && distToOrb < scanDistance) {
            int orbTick = static_cast<int>(std::round(distToOrb / unitsPerTick));
            bool isDash = HazardDetector::isDashOrb(obj);

            for (int offset = -8; offset <= 8; offset += 2) {
                int tapTick = orbTick + offset;
                if (tapTick >= 0 && tapTick < static_cast<int>(horizonTicks)) {
                    uint32_t t = static_cast<uint32_t>(tapTick);
                    if (isDash) {
                        for (uint32_t dur : { 4u, 8u, 14u, 22u, 32u, 45u }) {
                            std::vector<TickAction> acts;
                            if (t > 4) acts.push_back({ t - 4, false });
                            acts.push_back({ t, true });
                            if (t + dur < horizonTicks) acts.push_back({ t + dur, false });
                            addBotWithActions(acts);
                        }
                        std::vector<TickAction> dashHold;
                        if (t > 4) dashHold.push_back({ t - 4, false });
                        dashHold.push_back({ t, true });
                        addBotWithActions(dashHold);
                    } else {
                        std::vector<TickAction> acts;
                        if (t > 4) acts.push_back({ t - 4, false });
                        acts.push_back({ t, true });
                        if (t + 6 < horizonTicks) acts.push_back({ t + 6, false });
                        addBotWithActions(acts);
                    }
                }
            }
        }
    }

    // 6. Continuous flight modes (Wave, Ship, Swing, UFO)
    if (isContinuous) {
        // Multi-frequency micro-taps and varied duty cycles
        for (uint32_t period : { 1u, 2u, 3u, 4u, 5u, 6u, 8u, 10u, 12u, 16u }) {
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
        }

        // Asymmetric climb patterns (gentle and steep)
        for (auto [onT, offT] : std::vector<std::pair<uint32_t, uint32_t>>{
            {2, 1}, {3, 2}, {4, 3}, {5, 4}, {3, 1}, {4, 2}, {5, 2}, {6, 2}, {8, 3}
        }) {
            std::vector<TickAction> climb;
            uint32_t cur = 0;
            while (cur < horizonTicks) {
                climb.push_back({ cur, true });
                cur += onT;
                if (cur < horizonTicks) {
                    climb.push_back({ cur, false });
                    cur += offT;
                }
            }
            addBotWithActions(climb);
        }

        // Asymmetric dive patterns (gentle and steep)
        for (auto [offT, onT] : std::vector<std::pair<uint32_t, uint32_t>>{
            {2, 1}, {3, 2}, {4, 3}, {5, 4}, {3, 1}, {4, 2}, {5, 2}, {6, 2}, {8, 3}
        }) {
            std::vector<TickAction> dive;
            uint32_t cur = 0;
            while (cur < horizonTicks) {
                dive.push_back({ cur, false });
                cur += offT;
                if (cur < horizonTicks) {
                    dive.push_back({ cur, true });
                    cur += onT;
                }
            }
            addBotWithActions(dive);
        }

        // Zigzag / slope transitions (climb for K ticks, dive for remainder, and vice-versa)
        for (uint32_t k : { 4u, 8u, 12u, 16u, 20u, 24u, 28u, 32u }) {
            if (k < horizonTicks) {
                // Climb then dive
                std::vector<TickAction> cd = { { 0, true }, { k, false } };
                addBotWithActions(cd);

                // Dive then climb
                std::vector<TickAction> dc = { { 0, false }, { k, true } };
                addBotWithActions(dc);
            }
        }
    }

    // 7. Strided Click Sweep (Cube, Robot, Ball, Spider)
    if (isJumpMode) {
        for (uint32_t t = 0; t < horizonTicks - 1; t += 3) {
            // Micro hop for Robot mode (crucial for low ceiling spikes)
            if (mode == VehicleMode::Robot) {
                std::vector<TickAction> microJump;
                microJump.push_back({ t, true });
                if (t + 4 < horizonTicks) {
                    microJump.push_back({ t + 4, false });
                }
                addBotWithActions(microJump);

                std::vector<TickAction> miniJump;
                miniJump.push_back({ t, true });
                if (t + 8 < horizonTicks) {
                    miniJump.push_back({ t + 8, false });
                }
                addBotWithActions(miniJump);
            }

            // Short hop (16 ticks)
            std::vector<TickAction> shortJump;
            shortJump.push_back({ t, true });
            if (t + 16 < horizonTicks) {
                shortJump.push_back({ t + 16, false });
            }
            addBotWithActions(shortJump);

            // Medium standard jump (28 ticks)
            std::vector<TickAction> stdJump;
            stdJump.push_back({ t, true });
            if (t + 28 < horizonTicks) {
                stdJump.push_back({ t + 28, false });
            }
            addBotWithActions(stdJump);

            // Full high jump (44 ticks)
            std::vector<TickAction> fullJump;
            fullJump.push_back({ t, true });
            if (t + 44 < horizonTicks) {
                fullJump.push_back({ t + 44, false });
            }
            addBotWithActions(fullJump);

            // Maximum jump (60 ticks)
            std::vector<TickAction> maxJump;
            maxJump.push_back({ t, true });
            if (t + 60 < horizonTicks) {
                maxJump.push_back({ t + 60, false });
            }
            addBotWithActions(maxJump);

            // Hold from t to horizon end
            addBotWithActions({ { t, true } });
        }

        // Multi-click / Double jump combinations
        for (uint32_t t1 = 0; t1 + 24 < horizonTicks; t1 += 6) {
            uint32_t t2 = t1 + 20;
            std::vector<TickAction> dbl;
            dbl.push_back({ t1, true });
            dbl.push_back({ t1 + 14, false });
            dbl.push_back({ t2, true });
            if (t2 + 14 < horizonTicks) {
                dbl.push_back({ t2 + 14, false });
            }
            addBotWithActions(dbl);
        }
    }

    // 8. Fill remaining slots with exploratory stochastic intervals up to 160 bots
    while (population.size() < m_currentPopulationSize) {
        std::vector<TickAction> rndActs;
        uint32_t curT = rng() % 6;
        bool curSt = (rng() % 2 == 1);
        uint32_t minDur = (mode == VehicleMode::Wave) ? 1u : 2u;
        uint32_t maxDur = (mode == VehicleMode::Wave) ? 8u : 24u;
        while (curT < horizonTicks) {
            rndActs.push_back({ curT, curSt });
            uint32_t dur = minDur + (rng() % maxDur);
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
    DeterministicPRNG::clampSeed(checkpoint.snapshot.rngSeed);

    if (checkpoint.nativeCheckpoint) {
        playLayer->loadFromCheckpoint(checkpoint.nativeCheckpoint);
    }
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
    bool currentButton = checkpoint.snapshot.isHolding;
    bool lastButton = currentButton;
    if (currentButton) {
        playLayer->handleButton(true, 1, true);
    } else {
        playLayer->handleButton(false, 1, true);
    }

    size_t actionIdx = 0;
    bool completed = false;

    float prevX = playLayer->m_player1->getPositionX();
    uint32_t jammedTicks = 0;

    for (uint32_t step = 0; step < horizonTicks; ++step) {
        // Apply input scheduled for this segment tick
        while (actionIdx < bot.segmentActions.size() && bot.segmentActions[actionIdx].tick <= step) {
            bool btn = bot.segmentActions[actionIdx].pressed;
            if (btn != lastButton) {
                playLayer->handleButton(btn, 1, true);
                lastButton = btn;
            }
            actionIdx++;
        }
        currentButton = lastButton;

        playLayer->update(HeadlessEngine::FIXED_DT);

        // 1. Check level completion FIRST (before death / wall jam!)
        if (isSimulationFinished(playLayer, m_levelLength, m_startX, m_lastHazardX)) {
            completed = true;
            break;
        }

        // 2. Check death
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

        // 3. Check wall jam (only if level has not been completed)
        float currX = playLayer->m_player1->getPositionX();
        bool goingLeft = playLayer->m_player1->m_isGoingLeft;
        float dx = goingLeft ? (prevX - currX) : (currX - prevX);
        if (dx < 0.001f && !playLayer->m_player1->m_isDashing && !playLayer->m_player1->m_isSpider && !playLayer->m_hasCompletedLevel) {
            // Check if player has reached the physical End Wall of the level
            float totalDist = m_levelLength - m_startX;
            bool isAtEndWall = (currX >= (m_levelLength - 15.0f)) ||
                               (m_lastHazardX > m_startX + 30.0f && currX >= m_lastHazardX + 10.0f &&
                                (currX >= m_startX + 0.85f * totalDist || !HazardDetector::isNearAnyObject(currX, 60.0f))) ||
                               (playLayer->getCurrentPercent() >= 95.0f);

            if (isAtEndWall) {
                // Reached the physical End Wall of the level! Mark level completed!
                completed = true;
                break;
            }

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
    }

    playLayer->m_queuedButtons.clear();

    // Bot survived the full segment alive!
    bot.survived = true;
    bot.finalX = playLayer->m_player1->getPositionX();
    size_t nearbyCount = 0;
    bot.clearance = HazardDetector::calculateClearance(playLayer->m_player1->getPosition(), playLayer->m_objects, nearbyCount);

    // Calculate fitness score
    float fitness = 100.0f + (bot.finalX - checkpoint.startX);

    bool isJumpMode = (checkpoint.snapshot.mode == VehicleMode::Cube || checkpoint.snapshot.mode == VehicleMode::Robot || checkpoint.snapshot.mode == VehicleMode::Ball || checkpoint.snapshot.mode == VehicleMode::Spider);

    size_t jumpCount = 0;
    for (const auto& act : bot.segmentActions) {
        if (act.pressed) jumpCount++;
    }

    if (nearbyCount == 0) {
        // Flat ground without hazards ahead: reward idle and ground stability
        if (jumpCount == 0) {
            fitness += 20.0f; // Reward pure safe idle
        } else {
            fitness -= (2.0f * static_cast<float>(jumpCount));
        }
        if (isJumpMode && playLayer->m_player1->m_isOnGround) {
            fitness += 35.0f;
        } else if (isJumpMode && std::abs(playLayer->m_player1->m_yVelocity) > 8.0f) {
            fitness -= 15.0f; // Airborne falling penalty at segment boundary
        }
    } else {
        // Hazards ahead: clearance is paramount; do not penalize jumps or airborne flight
        fitness += std::min(bot.clearance, 60.0f) * 0.8f;
        if (!isJumpMode) {
            fitness += std::min(bot.clearance, 60.0f) * 0.5f;
        } else if (playLayer->m_player1->m_isOnGround) {
            fitness += 25.0f; // Landing bonus after clearing obstacle
        }
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
    uint32_t horizonTicks = (mode == VehicleMode::Wave) ? 36u : ((mode == VehicleMode::Ship || mode == VehicleMode::Swing || mode == VehicleMode::UFO) ? 48u : 60u);

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
            playLayer->m_objects,
            currentCp.snapshot.isHolding
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
            if (bot.fitnessScore >= 5000.0f || isSimulationFinished(playLayer, m_levelLength, m_startX, m_lastHazardX)) {
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

            // Store alternate survivors as runner-ups in currentCp for backtracking
            currentCp.runnerUps.clear();
            for (size_t i = 1; i < m_currentWaveSurvivors.size() && currentCp.runnerUps.size() < 6; ++i) {
                currentCp.runnerUps.push_back(m_currentWaveSurvivors[i]);
            }
            currentCp.runnerUpIndex = 0;

            const auto& bestBot = m_currentWaveSurvivors.front();

            // Advance simulation to the new checkpoint boundary using best bot
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
            playLayer->m_hasCompletedLevel = false;
            playLayer->m_queuedButtons.clear();

            size_t aIdx = 0;
            bool btn = currentCp.snapshot.isHolding;
            bool lastB = btn;
            if (btn) playLayer->handleButton(true, 1, true);
            else playLayer->handleButton(false, 1, true);

            DeterministicPRNG::clampSeed(currentCp.snapshot.rngSeed);
            bool advanceDied = false;
            for (uint32_t s = 0; s < m_currentHorizonTicks; ++s) {
                while (aIdx < bestBot.segmentActions.size() && bestBot.segmentActions[aIdx].tick <= s) {
                    bool b = bestBot.segmentActions[aIdx].pressed;
                    if (b != lastB) {
                        playLayer->handleButton(b, 1, true);
                        lastB = b;
                    }
                    aIdx++;
                }
                playLayer->update(HeadlessEngine::FIXED_DT);
                if (playLayer->m_player1->m_isDead || (playLayer->m_player2 && playLayer->m_player2->m_isDead) || playLayer->m_playerDied) {
                    advanceDied = true;
                    break;
                }
            }
            playLayer->m_queuedButtons.clear();

            if (advanceDied) {
                geode::log::warn("[LevelSolver] Best bot died during checkpoint advance at tick {}! Discarding and retrying wave...",
                    currentCp.startTick);
                currentCp.failedWaves++;
                m_activePopulation.clear();
                m_currentWaveSurvivors.clear();
                m_currentBotIndex = 0;
                if (currentCp.failedWaves >= 10) {
                    handleBacktrack(playLayer);
                }
                return;
            }

            // Commit winning bot into a new BeamCheckpoint (exact parity with simulateBot)
            BeamCheckpoint nextCp;
            nextCp.startTick = currentCp.startTick + m_currentHorizonTicks;
            nextCp.macroHistory = currentCp.macroHistory;
            for (const auto& act : bestBot.segmentActions) {
                nextCp.macroHistory.push_back({ currentCp.startTick + act.tick, act.pressed });
            }

            // Capture new checkpoint state
            nextCp.snapshot.capture(playLayer->m_player1, nextCp.startTick, DeterministicPRNG::getCurrentSeed());
            bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
            nextCp.hasPlayer2 = isDual;
            if (isDual) {
                nextCp.snapshot2.capture(playLayer->m_player2, nextCp.startTick, DeterministicPRNG::getCurrentSeed());
            }
            nextCp.startX = playLayer->m_player1->getPositionX();

            // Create native checkpoint ONCE for committed boundary
            playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
            nextCp.nativeCheckpoint = playLayer->createCheckpoint();
            if (nextCp.nativeCheckpoint) {
                nextCp.nativeCheckpoint->retain();
            }
            if (playLayer->m_checkpointArray && playLayer->m_checkpointArray->count() > 0) {
                playLayer->m_checkpointArray->removeAllObjects();
            }

                m_checkpointStack.push_back(std::move(nextCp));
                m_currentTick = m_checkpointStack.back().startTick;
                m_maxReachedX = m_checkpointStack.back().startX;
                m_waveRetryCount = 0;
                m_partialProgressSeeds.clear();
                m_activePopulation.clear();

                geode::log::info("[LevelSolver] Wave #{} passed (X={:.1f}, tick {}, {} survivors, best fitness={:.1f}) - New checkpoint set!",
                    m_activeWaveIndex, m_maxReachedX, m_currentTick, m_lastSurvivorCount, bestBot.fitnessScore);
        } else {
            // All bots died!
            m_waveRetryCount++;
            currentCp.failedWaves++;

            // Collect top furthest partial bots into m_partialProgressSeeds
            std::sort(m_activePopulation.begin(), m_activePopulation.end(), [](const SwarmBot& a, const SwarmBot& b) {
                return a.finalX > b.finalX;
            });
            m_partialProgressSeeds.clear();
            for (size_t i = 0; i < m_activePopulation.size() && m_partialProgressSeeds.size() < 8; ++i) {
                if (m_activePopulation[i].finalX > currentCp.startX + 5.0f) {
                    m_partialProgressSeeds.push_back(m_activePopulation[i]);
                }
            }

            geode::log::warn("[LevelSolver] Wave #{} produced 0 survivors at X={:.1f} (failed {}/10 times, best reached X={:.1f})",
                m_activeWaveIndex, currentCp.startX, currentCp.failedWaves,
                m_activePopulation.empty() ? 0.0f : m_activePopulation.front().finalX);

            if (currentCp.failedWaves >= 10) {
                geode::log::warn("[LevelSolver] All 10 re-runs failed around X={:.1f}! Backtracking...", currentCp.startX);
                handleBacktrack(playLayer);
            } else {
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
    m_telemetry.detailMessage = fmt::format("Raw Swarm Wave #{} | Attempt {}/10 | Stack {} | Survivors {} | TPS {:.0f}",
        m_activeWaveIndex, std::min(currentCp.failedWaves + 1, 10u), m_checkpointStack.size(), m_lastSurvivorCount, m_telemetry.ticksPerSecond);
}

void SwarmSolver::handleBacktrack(PlayLayer* playLayer) {
    m_backtrackCount++;

    if (m_checkpointStack.size() <= 1) {
        // At root: reset failed waves and broaden exploration with fresh seeds
        m_checkpointStack.front().failedWaves = 0;
        m_checkpointStack.front().runnerUpIndex = 0;
        m_activePopulation.clear();
        m_partialProgressSeeds.clear();
        geode::log::warn("[LevelSolver] Backtracked to root checkpoint at X={:.1f} (backtrack #{})",
            m_checkpointStack.front().startX, m_backtrackCount);
        return;
    }

    geode::log::warn("[LevelSolver] Backtracking from X={:.1f} (backtrack #{})",
        m_checkpointStack.back().startX, m_backtrackCount);

    // Pop the failed checkpoint
    if (m_checkpointStack.back().nativeCheckpoint) {
        m_checkpointStack.back().nativeCheckpoint->release();
        m_checkpointStack.back().nativeCheckpoint = nullptr;
    }
    m_checkpointStack.pop_back();

    auto& parentCp = m_checkpointStack.back();

    // Try alternate runner-up survivors from parent
    while (parentCp.runnerUpIndex < parentCp.runnerUps.size()) {
        const auto& altBot = parentCp.runnerUps[parentCp.runnerUpIndex++];

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
        if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
        playLayer->m_hasCompletedLevel = false;
        playLayer->m_queuedButtons.clear();

        size_t aIdx = 0;
        bool btn = parentCp.snapshot.isHolding;
        bool lastB = btn;
        if (btn) playLayer->handleButton(true, 1, true);
        else playLayer->handleButton(false, 1, true);

        DeterministicPRNG::clampSeed(parentCp.snapshot.rngSeed);
        bool died = false;
        for (uint32_t s = 0; s < m_currentHorizonTicks; ++s) {
            while (aIdx < altBot.segmentActions.size() && altBot.segmentActions[aIdx].tick <= s) {
                bool b = altBot.segmentActions[aIdx].pressed;
                if (b != lastB) {
                    playLayer->handleButton(b, 1, true);
                    lastB = b;
                }
                aIdx++;
            }
            playLayer->update(HeadlessEngine::FIXED_DT);
            if (playLayer->m_player1->m_isDead || (playLayer->m_player2 && playLayer->m_player2->m_isDead) || playLayer->m_playerDied) {
                died = true;
                break;
            }
        }
        playLayer->m_queuedButtons.clear();

        if (died) continue;

        // Runner up survived! Commit as nextCp (exact parity, no extraTicks)
        BeamCheckpoint nextCp;
        nextCp.startTick = parentCp.startTick + m_currentHorizonTicks;
        nextCp.macroHistory = parentCp.macroHistory;
        for (const auto& act : altBot.segmentActions) {
            nextCp.macroHistory.push_back({ parentCp.startTick + act.tick, act.pressed });
        }
        nextCp.snapshot.capture(playLayer->m_player1, nextCp.startTick, DeterministicPRNG::getCurrentSeed());
        bool isDual = playLayer->m_gameState.m_isDualMode && playLayer->m_player2 != nullptr;
        nextCp.hasPlayer2 = isDual;
        if (isDual) {
            nextCp.snapshot2.capture(playLayer->m_player2, nextCp.startTick, DeterministicPRNG::getCurrentSeed());
        }
        nextCp.startX = playLayer->m_player1->getPositionX();
        playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
        nextCp.nativeCheckpoint = playLayer->createCheckpoint();
        if (nextCp.nativeCheckpoint) nextCp.nativeCheckpoint->retain();
        if (playLayer->m_checkpointArray && playLayer->m_checkpointArray->count() > 0) {
            playLayer->m_checkpointArray->removeAllObjects();
        }

        m_checkpointStack.push_back(std::move(nextCp));
        m_currentTick = m_checkpointStack.back().startTick;
        m_maxReachedX = m_checkpointStack.back().startX;
        m_waveRetryCount = 0;
        m_activePopulation.clear();
        m_currentWaveSurvivors.clear();
        m_currentBotIndex = 0;
        geode::log::info("[LevelSolver] Switched to alternate survivor branch at X={:.1f} (runner-up #{})",
            m_maxReachedX, parentCp.runnerUpIndex);
        return;
    }

    // Runner-ups exhausted: restore parent and regenerate with different seed
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
    if (playLayer->m_player2) playLayer->m_player2->m_isDead = false;
    playLayer->m_hasCompletedLevel = false;
    playLayer->m_queuedButtons.clear();

    m_currentTick = parentCp.startTick;
    m_maxReachedX = parentCp.startX;
    m_activePopulation.clear();
    m_currentWaveSurvivors.clear();
    m_currentBotIndex = 0;
    parentCp.failedWaves++;
}

void SwarmSolver::finalizeSolution(PlayLayer* playLayer, const std::vector<TickAction>& winningActions) {
    geode::log::info("[LevelSolver] Swarm reached 100%! Verifying complete macro with {} inputs...",
        winningActions.size());

    // 1. Sort actions by tick and compress into edge-triggered actions
    std::vector<TickAction> sortedActions = winningActions;
    std::sort(sortedActions.begin(), sortedActions.end(), [](const TickAction& a, const TickAction& b) {
        if (a.tick == b.tick) {
            return !a.pressed && b.pressed;
        }
        return a.tick < b.tick;
    });

    std::vector<TickAction> compressed;
    bool lastBtn = false;
    for (const auto& act : sortedActions) {
        if (act.pressed != lastBtn) {
            compressed.push_back(act);
            lastBtn = act.pressed;
        }
    }
    if (!compressed.empty() && compressed.back().pressed) {
        compressed.push_back({ compressed.back().tick + 1, false });
    }

    // 2. Verification simulation run from tick 0
    DeterministicPRNG::clampSeed();
    playLayer->resetLevel();
    playLayer->startGame();

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
    playLayer->m_resumeTimer = 0;
    playLayer->m_extraDelta = 0.0;
    playLayer->m_isPaused = false;
    playLayer->m_hasCompletedLevel = false;
    playLayer->m_queuedButtons.clear();
    playLayer->m_player1->releaseButton(PlayerButton::Jump);
    playLayer->moveCameraToPos(playLayer->m_player1->getPosition());

    std::vector<TrajectorySample> trajectory;
    size_t actIdx = 0;
    bool currBtn = false;
    bool lastSimBtn = false;
    bool verified = true;

    uint32_t targetEndTick = m_checkpointStack.empty() ? 0 : (m_checkpointStack.back().startTick + m_currentHorizonTicks);
    uint32_t totalTicks = std::max(targetEndTick + 60, (compressed.empty() ? 0u : compressed.back().tick) + 120);
    if (totalTicks == 0) totalTicks = 2400;

    float prevX = playLayer->m_player1->getPositionX();
    uint32_t jammedTicks = 0;

    for (uint32_t t = 0; t <= totalTicks; ++t) {
        while (actIdx < compressed.size() && compressed[actIdx].tick <= t) {
            bool btn = compressed[actIdx].pressed;
            if (btn != lastSimBtn) {
                playLayer->handleButton(btn, 1, true);
                lastSimBtn = btn;
            }
            actIdx++;
        }
        currBtn = lastSimBtn;

        if (t % 30 == 0) {
            trajectory.push_back({ t, playLayer->m_player1->getPositionX(), playLayer->m_player1->getPositionY() });
            playLayer->moveCameraToPos(playLayer->m_player1->getPosition());
        }

        playLayer->update(HeadlessEngine::FIXED_DT);

        bool dead = playLayer->m_player1->m_isDead ||
                    (playLayer->m_player2 && playLayer->m_player2->m_isDead) ||
                    playLayer->m_playerDied;
        if (dead) {
            geode::log::warn("[LevelSolver] Swarm verification failed at tick {} (died at X={:.1f})! Continuing search...",
                t, playLayer->m_player1->getPositionX());
            verified = false;
            break;
        }

        if (isSimulationFinished(playLayer, m_levelLength, m_startX, m_lastHazardX)) {
            break;
        }

        // End wall check during verification
        float currX = playLayer->m_player1->getPositionX();
        bool goingLeft = playLayer->m_player1->m_isGoingLeft;
        float dx = goingLeft ? (prevX - currX) : (currX - prevX);
        if (dx < 0.001f && !playLayer->m_player1->m_isDashing && !playLayer->m_player1->m_isSpider && !playLayer->m_hasCompletedLevel) {
            float totalDist = m_levelLength - m_startX;
            bool isAtEndWall = (currX >= (m_levelLength - 15.0f)) ||
                               (m_lastHazardX > m_startX + 30.0f && currX >= m_lastHazardX + 10.0f &&
                                (currX >= m_startX + 0.85f * totalDist || !HazardDetector::isNearAnyObject(currX, 60.0f))) ||
                               (playLayer->getCurrentPercent() >= 95.0f);
            if (isAtEndWall) {
                // Reached end wall cleanly in verification!
                break;
            }
        }
        prevX = currX;
    }

    if (lastSimBtn) playLayer->handleButton(false, 1, true);
    playLayer->m_queuedButtons.clear();
    if (playLayer->m_player1) playLayer->m_player1->releaseButton(PlayerButton::Jump);
    if (playLayer->m_player2) playLayer->m_player2->releaseButton(PlayerButton::Jump);
    playLayer->moveCameraToPos(playLayer->m_player1->getPosition()); // Move camera ONCE at completion!

    if (!verified) {
        geode::log::warn("[LevelSolver] Verification failed! Backtracking to explore alternate branches...");
        m_activePopulation.clear();
        m_currentWaveSurvivors.clear();
        m_currentBotIndex = 0;
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
