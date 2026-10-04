#include "DeterministicPRNG.hpp"

namespace solver {

void DeterministicPRNG::clampSeed(uint32_t seed) {
    s_seed = seed;
    std::srand(seed);
}

uint32_t DeterministicPRNG::getCurrentSeed() {
    return s_seed;
}

uint32_t DeterministicPRNG::nextRandom() {
    // 64-bit LCG step for high entropy and reproducibility
    s_seed = (static_cast<uint64_t>(s_seed) * 6364136223846793005ULL + 1442695040888963407ULL) & 0xFFFFFFFF;
    return s_seed;
}

float DeterministicPRNG::nextFloat() {
    return static_cast<float>(nextRandom()) / static_cast<float>(0xFFFFFFFF);
}

} // namespace solver
