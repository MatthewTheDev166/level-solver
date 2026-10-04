#include "CheatAPIIntegrator.hpp"

namespace solver {

void CheatAPIIntegrator::notifyCheatStarted() {
    if (!s_isCheatActive) {
        s_isCheatActive = true;
        geode::log::info("[LevelSolver] Marking cheat active via CheatAPI");
        cheatAPIEvents::setCheatingAll();
    }
}

void CheatAPIIntegrator::notifyCheatEnded() {
    if (s_isCheatActive) {
        s_isCheatActive = false;
        geode::log::info("[LevelSolver] Unmarking cheat via CheatAPI");
        cheatAPIEvents::endCheatingAll();
    }
}

bool CheatAPIIntegrator::isCheatActive() {
    return s_isCheatActive;
}

} // namespace solver
