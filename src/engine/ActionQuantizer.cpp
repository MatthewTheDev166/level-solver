#include "ActionQuantizer.hpp"
#include "HazardDetector.hpp"

namespace solver {

bool ActionQuantizer::isContinuousMode(VehicleMode mode) {
    return mode == VehicleMode::Ship || mode == VehicleMode::Wave || mode == VehicleMode::Swing;
}

bool ActionQuantizer::isDiscreteMode(VehicleMode mode) {
    return !isContinuousMode(mode);
}

std::vector<ActionType> ActionQuantizer::getCandidateActions(
    PlayerObject* player,
    cocos2d::CCArray* objects,
    uint32_t currentTick,
    ActionType currentAction,
    uint32_t currentHoldTicks
) {
    if (!player) {
        return { ActionType::None };
    }

    VehicleMode mode = PlayerSnapshot::detectMode(player);

    if (isContinuousMode(mode)) {
        // Continuous modes: Ship, Wave, Swing
        // Only allow decisions at block boundary multiples to prevent 2^240 branching
        if ((currentTick % CONTINUOUS_BLOCK_SIZE) != 0) {
            return { currentAction };
        }

        // At block boundaries, branch into releasing or holding
        return { ActionType::None, ActionType::Jump };
    }

    // Discrete modes: Cube, Ball, UFO, Spider, Robot
    bool isGrounded = player->m_isOnGround || player->m_isOnGround2 || player->m_isOnGround3 || player->m_isOnGround4;
    bool isNearOrbOrPad = HazardDetector::isNearInteractable(player->getPosition(), objects) ||
                          player->m_touchedRing || player->m_touchedPad;

    if (mode == VehicleMode::UFO) {
        // UFO can flap in mid-air, but limit branching to periodic intervals or near orbs
        if (isNearOrbOrPad || (currentTick % 8 == 0)) {
            return { ActionType::None, ActionType::Jump };
        }
        return { ActionType::None };
    }

    if (mode == VehicleMode::Robot) {
        // Robot can hold jump while grounded or within 60 ticks (~0.25s at 240 TPS) of takeoff
        if (isGrounded || (currentAction == ActionType::Jump && currentHoldTicks < 60)) {
            return { ActionType::None, ActionType::Jump };
        }
        if (isNearOrbOrPad) {
            return { ActionType::None, ActionType::Jump };
        }
        return { ActionType::None };
    }

    // Cube, Ball, Spider: branching ONLY permitted when grounded or touching/near interactables
    if (isGrounded || isNearOrbOrPad) {
        return { ActionType::None, ActionType::Jump };
    }

    // Completely mid-air with nothing interactable: deterministic 0-branching
    return { ActionType::None };
}

} // namespace solver
