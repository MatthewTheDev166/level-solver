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
        // Decision points at CONTINUOUS_BLOCK_SIZE intervals (4 ticks = 1/60s at 240Hz)
        if ((currentTick % CONTINUOUS_BLOCK_SIZE) != 0) {
            return { currentAction };
        }

        // At decision boundaries, branch into holding or releasing
        return { ActionType::None, ActionType::Jump };
    }

    // Discrete modes: Cube, Ball, UFO, Spider, Robot
    bool isGrounded = player->m_isOnGround || player->m_isOnGround2 || player->m_isOnGround3 || player->m_isOnGround4;
    bool isNearOrbOrPad = HazardDetector::isNearInteractable(player->getPosition(), objects) ||
                          player->m_touchedRing || player->m_touchedPad;

    if (mode == VehicleMode::UFO) {
        // UFO flaps upward periodically (every 6 ticks) or near orbs
        if (isNearOrbOrPad || (currentTick % 6 == 0)) {
            return { ActionType::None, ActionType::Jump };
        }
        return { ActionType::None };
    }

    if (mode == VehicleMode::Ball || mode == VehicleMode::Spider) {
        // Ball and Spider switch gravity/teleport on single click when grounded or near orbs
        if (isGrounded || isNearOrbOrPad) {
            return { ActionType::None, ActionType::Jump };
        }
        return { ActionType::None };
    }

    if (mode == VehicleMode::Robot) {
        // Robot can hold jump while grounded or within takeoff window
        if (isGrounded) {
            return { ActionType::None, ActionType::Jump };
        }
        if (currentAction == ActionType::Jump && currentHoldTicks < 40) {
            if (currentHoldTicks % 6 == 0) {
                return { ActionType::None, ActionType::Jump };
            }
            return { ActionType::Jump };
        }
        if (isNearOrbOrPad) {
            return { ActionType::None, ActionType::Jump };
        }
        return { ActionType::None };
    }

    // Cube mode:
    if (isGrounded) {
        return { ActionType::None, ActionType::Jump };
    }

    // While airborne in Cube mode:
    if (currentAction == ActionType::Jump) {
        // Full standard jump in Geometry Dash at 240Hz requires holding for ~20-22 ticks.
        // Maintain hold for at least 8 ticks to gain sufficient height over basic obstacles.
        if (currentHoldTicks < 8) {
            return { ActionType::Jump };
        }
        // At tick 8: short hop (release) vs full jump (hold) branching point
        if (currentHoldTicks == 8) {
            return { ActionType::None, ActionType::Jump };
        }
        // If continuing full jump, hold up to 22 ticks
        if (currentHoldTicks < 22) {
            return { ActionType::Jump };
        }
        // Maximum jump hold duration reached, must release
        return { ActionType::None };
    }

    // Near an interactable orb or pad in mid-air
    if (isNearOrbOrPad) {
        return { ActionType::None, ActionType::Jump };
    }

    // Completely mid-air with nothing interactable: deterministic 0-branching
    return { ActionType::None };
}

} // namespace solver
