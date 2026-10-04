#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../solver/AStarSolver.hpp"
#include "../solver/SwarmSolver.hpp"
#include "../replay/MacroManager.hpp"
#include "../ui/TelemetryPopup.hpp"

using namespace geode::prelude;

class $modify(SolverFMODAudioEngine, FMODAudioEngine) {
    void playMusic(gd::string path, bool p1, float p2, int p3) {
        if (solver::HeadlessEngine::get().isAudioSuppressed()) {
            return;
        }
        FMODAudioEngine::playMusic(path, p1, p2, p3);
    }

    void playEffect(gd::string path, float speed, float p2, float volume) {
        if (solver::HeadlessEngine::get().isAudioSuppressed()) {
            return;
        }
        FMODAudioEngine::playEffect(path, speed, p2, volume);
    }
};

class $modify(SolverPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        // Clamp engine pseudo-random number generator for determinism
        solver::DeterministicPRNG::clampSeed();

        // Check for existing solved macro for this level
        if (level) {
            int levelID = level->m_levelID.value();
            std::string levelName = level->m_levelName;
            if (solver::MacroManager::get().hasMacro(levelID, levelName)) {
                solver::MacroManager::get().loadMacro(levelID, levelName);
            }
        }

        return true;
    }

    void startGame() {
        PlayLayer::startGame();
        if (!solver::HeadlessEngine::get().isHeadless()) {
            if (solver::MacroManager::get().hasPendingReplay()) {
                solver::MacroManager::get().startReplay(this);
            }
        }
    }

    void update(float dt) {
        // If macro replay is active, dispatch recorded inputs synchronized to delta time
        if (solver::MacroManager::get().isReplaying()) {
            solver::MacroManager::get().updateReplay(this, dt);
        }

        PlayLayer::update(dt);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            // RobTop's initial spawn anti-cheat spike (m_anticheatSpike / Object ID 8 at X <= 30.0) must not kill headless simulation
            if (object && (object == this->m_anticheatSpike || (object->m_objectID == 8 && object->getPositionX() <= 30.0f))) {
                return;
            }
            static uint32_t s_deathLogCount = 0;
            if (++s_deathLogCount <= 50 || (s_deathLogCount % 200 == 0)) {
                geode::log::warn("[LevelSolver] Simulation death: Object={}, Player=({:.1f}, {:.1f}), yVel={:.2f}",
                    object ? fmt::format("ID {} ({}) at ({:.1f}, {:.1f})", object->m_objectID, static_cast<int>(object->m_objectType), object->getPositionX(), object->getPositionY()) : "NULL (Camera/Ground/Boundary)",
                    player ? player->getPositionX() : -1.0f,
                    player ? player->getPositionY() : -1.0f,
                    player ? player->m_yVelocity : 0.0
                );
            }
            if (player) {
                player->m_isDead = true;
            }
            this->m_playerDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            this->m_hasCompletedLevel = true;
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }

    void playPlatformerEndAnimationToPos(cocos2d::CCPoint position, bool instant) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            this->m_hasCompletedLevel = true;
            return;
        }
        PlayLayer::playPlatformerEndAnimationToPos(position, instant);
    }

    void showEndLayer() {
        if (solver::HeadlessEngine::get().isHeadless()) {
            this->m_hasCompletedLevel = true;
            return;
        }
        PlayLayer::showEndLayer();
    }

    void levelComplete() {
        if (solver::HeadlessEngine::get().isHeadless()) {
            this->m_hasCompletedLevel = true;
            return;
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (solver::SwarmSolver::get().isRunning()) {
            solver::SwarmSolver::get().stop();
        }
        if (solver::AStarSolver::get().isRunning()) {
            solver::AStarSolver::get().stop();
        }
        if (solver::MacroManager::get().isReplaying()) {
            solver::MacroManager::get().stopReplay(this);
        }
        solver::CheatAPIIntegrator::notifyCheatEnded();

        PlayLayer::onQuit();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        // If replaying macro, restart playback
        if (!solver::HeadlessEngine::get().isHeadless()) {
            if (solver::MacroManager::get().isReplaying() || solver::MacroManager::get().hasPendingReplay()) {
                solver::MacroManager::get().startReplay(this);
            }
        }
    }
};
