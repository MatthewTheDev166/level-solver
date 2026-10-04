#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <cstdint>
#include <vector>
#include <string>

namespace solver {

enum class VehicleMode : uint8_t {
    Cube = 0,
    Ship = 1,
    Ball = 2,
    UFO = 3,
    Wave = 4,
    Robot = 5,
    Spider = 6,
    Swing = 7
};

enum class ActionType : uint8_t {
    None = 0,
    Jump = 1
};

struct TickAction {
    uint32_t tick = 0;
    bool pressed = false;

    bool operator==(const TickAction& other) const {
        return tick == other.tick && pressed == other.pressed;
    }
};

enum class SolverStatus {
    Idle,
    Searching,
    Solved,
    Replaying,
    Paused,
    Failed
};

struct TelemetryMetrics {
    uint32_t currentTick = 0;
    float explorationHorizon = 0.0f; // 0.0 to 100.0%
    float currentX = 0.0f;
    float targetEndX = 0.0f;
    size_t openNodes = 0;
    size_t exploredNodes = 0;
    size_t prunedStates = 0;
    size_t memoryFootprintBytes = 0;
    size_t nearbyObstacles = 0;
    float ticksPerSecond = 0.0f;
    SolverStatus status = SolverStatus::Idle;
    std::string detailMessage = "Ready";
};

struct PlayerSnapshot {
    cocos2d::CCPoint position = {0.0f, 0.0f};
    double yVelocity = 0.0;
    double fallSpeed = 0.0;
    float rotation = 0.0f;
    float vehicleSize = 1.0f;
    float playerSpeed = 1.0f;
    double gravity = 1.0;
    VehicleMode mode = VehicleMode::Cube;

    bool isUpsideDown = false;
    bool isOnGround = false;
    bool isOnGround2 = false;
    bool isOnGround3 = false;
    bool isOnGround4 = false;
    bool jumpBuffered = false;
    bool stateRingJump = false;
    bool touchedRing = false;
    bool touchedPad = false;
    bool isDashing = false;
    bool isDead = false;
    bool isHolding = false;

    uint32_t tick = 0;
    uint32_t rngSeed = 1337;

    static VehicleMode detectMode(PlayerObject* player) {
        if (!player) return VehicleMode::Cube;
        if (player->m_isShip) return VehicleMode::Ship;
        if (player->m_isBall) return VehicleMode::Ball;
        if (player->m_isBird) return VehicleMode::UFO;
        if (player->m_isDart) return VehicleMode::Wave;
        if (player->m_isRobot) return VehicleMode::Robot;
        if (player->m_isSpider) return VehicleMode::Spider;
        if (player->m_isSwing) return VehicleMode::Swing;
        return VehicleMode::Cube;
    }

    void capture(PlayerObject* player, uint32_t currentTick, uint32_t currentSeed = 1337) {
        if (!player) return;
        position = (player->m_position.x != 0.0f || player->m_position.y != 0.0f) ? player->m_position : player->getPosition();
        yVelocity = player->m_yVelocity;
        fallSpeed = player->m_fallSpeed;
        rotation = player->getRotation();
        vehicleSize = player->m_vehicleSize;
        playerSpeed = player->m_playerSpeed;
        gravity = player->m_gravity;
        mode = detectMode(player);

        isUpsideDown = player->m_isUpsideDown;
        isOnGround = player->m_isOnGround;
        isOnGround2 = player->m_isOnGround2;
        isOnGround3 = player->m_isOnGround3;
        isOnGround4 = player->m_isOnGround4;
        jumpBuffered = player->m_jumpBuffered;
        stateRingJump = player->m_stateRingJump;
        touchedRing = player->m_touchedRing;
        touchedPad = player->m_touchedPad;
        isDashing = player->m_isDashing;
        isDead = player->m_isDead;
        isHolding = player->buttonDown(PlayerButton::Jump);

        tick = currentTick;
        rngSeed = currentSeed;
    }

    void restore(PlayerObject* player) const {
        if (!player) return;
        player->setPosition(position);
        player->m_position = position;
        player->m_yVelocity = yVelocity;
        player->m_fallSpeed = fallSpeed;
        player->setRotation(rotation);
        player->m_vehicleSize = vehicleSize;
        player->m_playerSpeed = playerSpeed;
        player->m_gravity = gravity;

        if (vehicleSize < 0.9f) {
            player->togglePlayerScale(true, true);
        } else {
            player->togglePlayerScale(false, true);
        }

        if (player->m_isShip != (mode == VehicleMode::Ship)) player->toggleFlyMode(mode == VehicleMode::Ship, true);
        if (player->m_isBall != (mode == VehicleMode::Ball)) player->toggleRollMode(mode == VehicleMode::Ball, true);
        if (player->m_isBird != (mode == VehicleMode::UFO)) player->toggleBirdMode(mode == VehicleMode::UFO, true);
        if (player->m_isDart != (mode == VehicleMode::Wave)) player->toggleDartMode(mode == VehicleMode::Wave, true);
        if (player->m_isRobot != (mode == VehicleMode::Robot)) player->toggleRobotMode(mode == VehicleMode::Robot, true);
        if (player->m_isSpider != (mode == VehicleMode::Spider)) player->toggleSpiderMode(mode == VehicleMode::Spider, true);
        if (player->m_isSwing != (mode == VehicleMode::Swing)) player->toggleSwingMode(mode == VehicleMode::Swing, true);

        player->m_isUpsideDown = isUpsideDown;
        player->m_isOnGround = isOnGround;
        player->m_isOnGround2 = isOnGround2;
        player->m_isOnGround3 = isOnGround3;
        player->m_isOnGround4 = isOnGround4;
        player->m_jumpBuffered = jumpBuffered;
        player->m_stateRingJump = stateRingJump;
        player->m_touchedRing = touchedRing;
        player->m_touchedPad = touchedPad;
        player->m_isDashing = isDashing;
        player->m_isDead = false;

        if (isHolding) {
            player->pushButton(PlayerButton::Jump);
        } else {
            player->releaseButton(PlayerButton::Jump);
        }
    }
};

struct SearchNode {
    PlayerSnapshot snapshot;
    float gScore = 0.0f; // Traveled progress / cost
    float fScore = 0.0f; // Priority heuristic score
    float hazardClearance = 100.0f; // Distance to nearest hazard
    uint32_t parentIndex = UINT32_MAX;
    ActionType action = ActionType::None;
    uint32_t actionSwitches = 0;
    uint32_t holdDuration = 0;

    bool operator<(const SearchNode& other) const {
        // Max-priority queue: higher fScore has higher priority
        return fScore < other.fScore;
    }
};

} // namespace solver
