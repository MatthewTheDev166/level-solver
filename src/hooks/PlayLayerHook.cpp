#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../solver/AStarSolver.hpp"
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
            if (solver::MacroManager::get().hasMacro(levelID)) {
                solver::MacroManager::get().loadMacro(levelID);
            }
        }

        return true;
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
            if (player) {
                player->m_isDead = true;
            }
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void onQuit() {
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
        if (solver::MacroManager::get().isReplaying()) {
            solver::MacroManager::get().startReplay(this);
        }
    }
};
