#pragma once

#include <cstdint>
#include <cstdlib>

namespace solver {

class DeterministicPRNG {
public:
    static constexpr uint32_t STATIC_SEED = 1337;

    static void clampSeed(uint32_t seed = STATIC_SEED);
    static uint32_t getCurrentSeed();
    static uint32_t nextRandom();
    static float nextFloat();

private:
    static inline uint32_t s_seed = STATIC_SEED;
};

} // namespace solver
