#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/FMODAudioEngine.hpp>
#include "../core/DeterministicPRNG.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../solver/AStarSolver.hpp"
#include "../solver/SwarmSolver.hpp"
#include "../replay/MacroManager.hpp"
#include "../ui/TelemetryPopup.hpp"

using namespace geode::prelude;

class $modify(SolverBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isHalfTick && solver::MacroManager::get().isPlaying()) {
            if (auto pl = PlayLayer::get()) {
                if (!pl->m_inResetDelay && pl->m_started && !pl->m_playerDied && pl->m_player1 && !pl->m_player1->m_isDead) {
                    solver::MacroManager::get().stepReplay(pl);
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (solver::MacroManager::get().isPlaying() && !solver::MacroManager::get().isDispatchingInput()) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(SolverFMODAudioEngine, FMODAudioEngine) {
    void playMusic(gd::string path, bool p1, float p2, int p3) {
        if (solver::HeadlessEngine::get().isAudioSuppressed()) {
            return;
        }
        FMODAudioEngine::playMusic(path, p1, p2, p3);
    }

    void playEffect(gd::string path, float speed, float p2, float volume) {
        if (solver::HeadlessEngine::get().isAudioSuppressed()) {
            return;
        }
        FMODAudioEngine::playEffect(path, speed, p2, volume);
    }
};

class $modify(SolverPlayLayer, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        // Clamp engine pseudo-random number generator for determinism
        solver::DeterministicPRNG::clampSeed();

        // Create on-screen replay status badge
        auto badge = cocos2d::CCLabelBMFont::create("", "chatFont.fnt");
        badge->setTag(108492);
        badge->setScale(0.55f);
        badge->setAnchorPoint({ 0.0f, 1.0f });
        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        badge->setPosition({ 10.0f, winSize.height - 10.0f });
        badge->setZOrder(999);
        badge->setVisible(false);
        this->addChild(badge);

        // Check for existing solved macro for this level
        if (level && !solver::MacroManager::get().isArmed()) {
            int levelID = level->m_levelID.value();
            std::string levelName = level->m_levelName;
            if (solver::MacroManager::get().hasMacro(levelID, levelName)) {
                solver::MacroManager::get().armReplay(levelID, levelName);
            }
        }

        if (solver::MacroManager::get().isArmed()) {
            solver::MacroManager::get().onLevelReset(this);
        }

        return true;
    }

    void startGame() {
        PlayLayer::startGame();
        if (!solver::HeadlessEngine::get().isHeadless()) {
            solver::MacroManager::get().onGameStart(this);
        }
    }

    void update(float dt) {
        PlayLayer::update(dt);

        if (!solver::HeadlessEngine::get().isHeadless()) {
            if (auto badge = static_cast<cocos2d::CCLabelBMFont*>(this->getChildByTag(108492))) {
                if (solver::MacroManager::get().isPlaying()) {
                    badge->setVisible(true);
                    uint32_t curTick = solver::MacroManager::get().getCurrentPlaybackTick();
                    uint32_t totTick = solver::MacroManager::get().getTotalTicks();
                    size_t curAct = solver::MacroManager::get().getCurrentActionIndex();
                    size_t totAct = solver::MacroManager::get().getTotalActions();
                    badge->setString(fmt::format("[REPLAY BOT] 240 TPS | Tick: {}/{} | Inputs: {}/{}",
                        curTick, totTick, curAct, totAct).c_str());
                    badge->setColor({ 0, 255, 128 });
                } else if (solver::MacroManager::get().isArmed()) {
                    badge->setVisible(true);
                    badge->setString(fmt::format("[REPLAY BOT] Armed ({} inputs ready on start)",
                        solver::MacroManager::get().getTotalActions()).c_str());
                    badge->setColor({ 255, 200, 0 });
                } else {
                    badge->setVisible(false);
                }
            }
        }
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            // RobTop's initial spawn anti-cheat spike (m_anticheatSpike / Object ID 8 at X <= 30.0) must not kill headless simulation
            if (object && (object == this->m_anticheatSpike || (object->m_objectID == 8 && object->getPositionX() <= 30.0f))) {
                return;
            }
            // Suppress spurious camera/boundary deaths while player is on valid spawn floor
            if (!object && player && player->getPositionX() <= 50.0f && player->getPositionY() >= 104.0f && !player->m_isUpsideDown) {
                return;
            }
            static uint32_t s_deathLogCount = 0;
            if (++s_deathLogCount <= 50 || (s_deathLogCount % 200 == 0)) {
                geode::log::warn("[LevelSolver] Simulation death: Object={}, Player=({:.1f}, {:.1f}), yVel={:.2f}",
                    object ? fmt::format("ID {} ({}) at ({:.1f}, {:.1f})", object->m_objectID, static_cast<int>(object->m_objectType), object->getPositionX(), object->getPositionY()) : "NULL (Camera/Ground/Boundary)",
                    player ? player->getPositionX() : -1.0f,
                    player ? player->getPositionY() : -1.0f,
                    player ? player->m_yVelocity : 0.0
                );
            }
            if (player) {
                player->m_isDead = true;
            }
            this->m_playerDied = true;
            return;
        }
        PlayLayer::destroyPlayer(player, object);
    }

    void playEndAnimationToPos(cocos2d::CCPoint position) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            bool isPlat = this->m_level && this->m_level->isPlatformer();
            if (isPlat || (this->m_player1 && this->m_player1->getPositionX() >= (this->m_endPosition.x - 150.0f))) {
                this->m_hasCompletedLevel = true;
            }
            return;
        }
        PlayLayer::playEndAnimationToPos(position);
    }

    void playPlatformerEndAnimationToPos(cocos2d::CCPoint position, bool instant) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            this->m_hasCompletedLevel = true;
            return;
        }
        PlayLayer::playPlatformerEndAnimationToPos(position, instant);
    }

    void showEndLayer() {
        if (solver::HeadlessEngine::get().isHeadless()) {
            bool isPlat = this->m_level && this->m_level->isPlatformer();
            if (isPlat || (this->m_player1 && this->m_player1->getPositionX() >= (this->m_endPosition.x - 150.0f))) {
                this->m_hasCompletedLevel = true;
            }
            return;
        }
        PlayLayer::showEndLayer();
    }

    void levelComplete() {
        if (solver::HeadlessEngine::get().isHeadless()) {
            bool isPlat = this->m_level && this->m_level->isPlatformer();
            if (isPlat || (this->m_player1 && this->m_player1->getPositionX() >= (this->m_endPosition.x - 150.0f))) {
                this->m_hasCompletedLevel = true;
            }
            return;
        }
        PlayLayer::levelComplete();
    }

    void onQuit() {
        if (solver::SwarmSolver::get().isRunning()) {
            solver::SwarmSolver::get().stop();
        }
        if (solver::AStarSolver::get().isRunning()) {
            solver::AStarSolver::get().stop();
        }
        solver::MacroManager::get().stopReplay(this);
        solver::CheatAPIIntegrator::notifyCheatEnded();

        PlayLayer::onQuit();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (!solver::HeadlessEngine::get().isHeadless()) {
            solver::MacroManager::get().onLevelReset(this);
        }
    }
};
