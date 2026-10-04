#pragma once

#include <Geode/Geode.hpp>
#include <legowiifun.cheatAPI/include/cheatAPI.hpp>

namespace solver {

class CheatAPIIntegrator {
public:
    static void notifyCheatStarted();
    static void notifyCheatEnded();
    static bool isCheatActive();

private:
    static inline bool s_isCheatActive = false;
};

} // namespace solver
