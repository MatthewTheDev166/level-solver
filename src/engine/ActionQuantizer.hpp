#pragma once

#include "../core/Types.hpp"
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/cocos/cocoa/CCArray.h>
#include <vector>

namespace solver {

class ActionQuantizer {
public:
    static constexpr uint32_t CONTINUOUS_BLOCK_SIZE = 4; // Quantize to 4 ticks (60Hz equivalent decision points)

    static bool isContinuousMode(VehicleMode mode);
    static bool isDiscreteMode(VehicleMode mode);

    static std::vector<ActionType> getCandidateActions(
        PlayerObject* player,
        cocos2d::CCArray* objects,
        uint32_t currentTick,
        ActionType currentAction,
        uint32_t currentHoldTicks
    );
};

} // namespace solver
