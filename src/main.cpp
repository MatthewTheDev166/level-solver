#include <Geode/Geode.hpp>
#include "core/DeterministicPRNG.hpp"
#include "core/CheatAPIIntegrator.hpp"
#include "engine/HeadlessEngine.hpp"
#include "replay/MacroManager.hpp"

using namespace geode::prelude;

$on_mod(Loaded) {
    log::info("[LevelSolver] Autonomous In-Engine Level Solver initialized successfully.");
    log::info("[LevelSolver] Headless 240 TPS simulation engine ready.");
}
