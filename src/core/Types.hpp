#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/CheckpointObject.hpp>
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

struct TrajectorySample {
    uint32_t tick = 0;
    float x = 0.0f;
    float y = 0.0f;
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
    size_t frontierSize = 0;
    size_t currentWidth = 96;
    size_t rewindCount = 0;
    uint32_t deepestTick = 0;
    float stuckX = 0.0f;
    float ticksPerSecond = 0.0f;
    bool isVerified = false;

    // Compatibility fields
    size_t openNodes = 0;
    size_t exploredNodes = 0;
    size_t prunedStates = 0;
    size_t memoryFootprintBytes = 0;
    size_t nearbyObstacles = 0;
    uint32_t activeWave = 0;
    size_t populationSize = 96;
    size_t survivorCount = 0;
    size_t currentBotIndex = 0;
    size_t currentSurvivors = 0;
    size_t checkpointDepth = 0;
    size_t backtrackCount = 0;

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
    cocos2d::CCPoint lastGroundedPos = {0.0f, 0.0f};
    bool hasEverJumped = false;
    bool isGoingLeft = false;
    bool isOnSlope = false;
    float slopeVelocity = 0.0f;

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
        position = player->getPosition();
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
        lastGroundedPos = player->m_lastGroundedPos;
        hasEverJumped = player->m_hasEverJumped;
        isGoingLeft = player->m_isGoingLeft;
        isOnSlope = player->m_isOnSlope;
        slopeVelocity = player->m_slopeVelocity;

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

        bool isMini = (vehicleSize < 0.9f);
        bool currentMini = (player->m_vehicleSize < 0.9f);
        if (isMini != currentMini) {
            player->togglePlayerScale(isMini, true);
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
        player->m_lastGroundedPos = lastGroundedPos;
        player->m_hasEverJumped = hasEverJumped;
        player->m_isGoingLeft = isGoingLeft;
        player->m_isOnSlope = isOnSlope;
        player->m_slopeVelocity = slopeVelocity;

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

struct SwarmBot {
    std::vector<TickAction> segmentActions; // Input timeline for this segment
    float finalX = 0.0f;
    uint32_t deathTick = 0;
    bool survived = false;
    float clearance = 100.0f;
    float fitnessScore = 0.0f;
};

struct BeamCheckpoint {
    CheckpointObject* nativeCheckpoint = nullptr; // RobTop's full practice mode checkpoint
    PlayerSnapshot snapshot;                      // Native physics snapshot at segment start
    PlayerSnapshot snapshot2;                     // Native physics snapshot for Player 2 (dual mode)
    bool hasPlayer2 = false;
    uint32_t startTick = 0;                       // Starting tick of this checkpoint
    float startX = 0.0f;                          // X coordinate at segment start
    std::vector<TickAction> macroHistory;         // Global inputs accumulated to this point
    std::vector<SwarmBot> runnerUps;              // Top alternate surviving paths
    uint32_t runnerUpIndex = 0;                   // Currently tested alternate branch
    uint32_t failedWaves = 0;                     // Consecutive failed generations

    BeamCheckpoint() = default;

    ~BeamCheckpoint() {
        if (nativeCheckpoint) {
            nativeCheckpoint->release();
            nativeCheckpoint = nullptr;
        }
    }

    BeamCheckpoint(const BeamCheckpoint& other) {
        *this = other;
    }

    BeamCheckpoint& operator=(const BeamCheckpoint& other) {
        if (this != &other) {
            if (nativeCheckpoint) nativeCheckpoint->release();
            nativeCheckpoint = other.nativeCheckpoint;
            if (nativeCheckpoint) nativeCheckpoint->retain();
            snapshot = other.snapshot;
            snapshot2 = other.snapshot2;
            hasPlayer2 = other.hasPlayer2;
            startTick = other.startTick;
            startX = other.startX;
            macroHistory = other.macroHistory;
            runnerUps = other.runnerUps;
            runnerUpIndex = other.runnerUpIndex;
            failedWaves = other.failedWaves;
        }
        return *this;
    }

    BeamCheckpoint(BeamCheckpoint&& other) noexcept {
        *this = std::move(other);
    }

    BeamCheckpoint& operator=(BeamCheckpoint&& other) noexcept {
        if (this != &other) {
            if (nativeCheckpoint) nativeCheckpoint->release();
            nativeCheckpoint = other.nativeCheckpoint;
            other.nativeCheckpoint = nullptr;
            snapshot = other.snapshot;
            snapshot2 = other.snapshot2;
            hasPlayer2 = other.hasPlayer2;
            startTick = other.startTick;
            startX = other.startX;
            macroHistory = std::move(other.macroHistory);
            runnerUps = std::move(other.runnerUps);
            runnerUpIndex = other.runnerUpIndex;
            failedWaves = other.failedWaves;
        }
        return *this;
    }
};

struct ArenaNode {
    int32_t parentIndex = -1;
    uint32_t tick = 0;
    bool buttonDown = false;
};

struct BeamNode {
    CheckpointObject* checkpoint = nullptr;
    PlayerSnapshot p1;
    PlayerSnapshot p2;
    bool hasP2 = false;
    uint32_t tick = 0;
    int32_t arenaIndex = -1;
    bool buttonDown = false;
    float clearance = 100.0f;
    float x = 0.0f;
    float y = 0.0f;

    BeamNode() = default;

    ~BeamNode() {
        if (checkpoint) {
            checkpoint->release();
            checkpoint = nullptr;
        }
    }

    BeamNode(const BeamNode& other) {
        *this = other;
    }

    BeamNode& operator=(const BeamNode& other) {
        if (this != &other) {
            if (checkpoint) checkpoint->release();
            checkpoint = other.checkpoint;
            if (checkpoint) checkpoint->retain();
            p1 = other.p1;
            p2 = other.p2;
            hasP2 = other.hasP2;
            tick = other.tick;
            arenaIndex = other.arenaIndex;
            buttonDown = other.buttonDown;
            clearance = other.clearance;
            x = other.x;
            y = other.y;
        }
        return *this;
    }

    BeamNode(BeamNode&& other) noexcept {
        *this = std::move(other);
    }

    BeamNode& operator=(BeamNode&& other) noexcept {
        if (this != &other) {
            if (checkpoint) checkpoint->release();
            checkpoint = other.checkpoint;
            other.checkpoint = nullptr;
            p1 = other.p1;
            p2 = other.p2;
            hasP2 = other.hasP2;
            tick = other.tick;
            arenaIndex = other.arenaIndex;
            buttonDown = other.buttonDown;
            clearance = other.clearance;
            x = other.x;
            y = other.y;
        }
        return *this;
    }
};

struct SavedLayer {
    uint32_t layerTick = 0;
    float maxReachedX = 0.0f;
    std::vector<BeamNode> nodes;
};

} // namespace solver
