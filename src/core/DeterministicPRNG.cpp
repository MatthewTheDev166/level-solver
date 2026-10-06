#include "DeterministicPRNG.hpp"
#include <Geode/binding/GameToolbox.hpp>

namespace solver {

void DeterministicPRNG::clampSeed(uint32_t seed) {
    s_seed = seed;
    std::srand(seed);
    GameToolbox::fast_srand(static_cast<uint64_t>(seed));
}

uint32_t DeterministicPRNG::getCurrentSeed() {
    uint32_t gdSeed = static_cast<uint32_t>(GameToolbox::getfast_srand());
    s_seed = gdSeed;
    return gdSeed;
}

uint32_t DeterministicPRNG::nextRandom() {
    return static_cast<uint32_t>(GameToolbox::fast_rand());
}

float DeterministicPRNG::nextFloat() {
    return GameToolbox::fast_rand_0_1();
}

} // namespace solver
