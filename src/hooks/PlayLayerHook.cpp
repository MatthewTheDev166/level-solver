#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../solver/AStarSolver.hpp"
#include "../replay/MacroManager.hpp"

using namespace geode::prelude;

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
        // If autonomous forward state search is active, execute headless batch
        if (solver::AStarSolver::get().isRunning()) {
            solver::AStarSolver::get().stepSearchBatch(this, solver::HeadlessEngine::get().getBatchSize());
            return;
        }

        // If macro replay is active, dispatch recorded inputs
        if (solver::MacroManager::get().isReplaying()) {
            solver::MacroManager::get().updateReplay(this);
        }

        PlayLayer::update(dt);
    }

    void visit() {
        // Bypass all Cocos2d-x rendering, particles, batch nodes, and shaders in headless mode
        if (solver::HeadlessEngine::get().isRenderingSuppressed()) {
            return;
        }
        PlayLayer::visit();
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
        // If replaying macro, restart playback
        if (solver::MacroManager::get().isReplaying()) {
            solver::MacroManager::get().startReplay(this);
        }
        PlayLayer::resetLevel();
    }
};
