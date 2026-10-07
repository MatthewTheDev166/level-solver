#include <Geode/Geode.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
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

static void updateReplayBadge(PlayLayer* pl) {
    if (!pl || solver::HeadlessEngine::get().isHeadless() || pl->m_isSilent) return;

    bool showHudPref = geode::Mod::get()->getSavedValue<bool>("show-replay-hud", true);
    bool isReplaying = solver::MacroManager::get().isReplaySessionActive() &&
                       (solver::MacroManager::get().isPlaying() || solver::MacroManager::get().isArmed() || solver::MacroManager::get().isReplaying());

    // Wait until m_uiLayer is available so HUD badge is anchored in screen space and drawn above level graphics
    if (!pl->m_uiLayer) return;
    auto targetParent = pl->m_uiLayer;

    auto badge = static_cast<cocos2d::CCLabelBMFont*>(targetParent->getChildByTag(108492));

    if (!showHudPref || !isReplaying) {
        if (badge) {
            badge->removeFromParentAndCleanup(true);
        }
        if (auto oldBadge = static_cast<cocos2d::CCLabelBMFont*>(pl->getChildByTag(108492))) {
            oldBadge->removeFromParentAndCleanup(true);
        }
        return;
    }

    if (!badge) {
        badge = cocos2d::CCLabelBMFont::create("", "bigFont.fnt");
        if (!badge) return;
        badge->setTag(108492);
        badge->setScale(0.40f);
        badge->setAnchorPoint({ 0.0f, 1.0f });
        auto winSize = cocos2d::CCDirector::sharedDirector()->getWinSize();
        badge->setPosition({ 10.0f, winSize.height - 10.0f });
        badge->setZOrder(99999);
        targetParent->addChild(badge, 99999);
    }

    if (solver::MacroManager::get().isPlaying()) {
        badge->setVisible(true);
        uint32_t curTick = solver::MacroManager::get().getCurrentPlaybackTick();
        uint32_t totTick = solver::MacroManager::get().getTotalTicks();
        size_t curAct = solver::MacroManager::get().getCurrentActionIndex();
        size_t totAct = solver::MacroManager::get().getTotalActions();
        if (solver::MacroManager::get().hasDesync()) {
            badge->setString(fmt::format("[REPLAY BOT] DESYNC DETECTED at tick {} (Inputs: {}/{})",
                solver::MacroManager::get().getDesyncTick(), curAct, totAct).c_str());
            badge->setColor({ 255, 60, 60 });
        } else {
            badge->setString(fmt::format("[REPLAY BOT] 240 TPS | Tick: {}/{} | Inputs: {}/{}",
                curTick, totTick, curAct, totAct).c_str());
            badge->setColor({ 0, 255, 128 });
        }
    } else if (solver::MacroManager::get().isArmed()) {
        badge->setVisible(true);
        badge->setString(fmt::format("[REPLAY BOT] Armed ({} inputs ready on start)",
            solver::MacroManager::get().getTotalActions()).c_str());
        badge->setColor({ 255, 200, 0 });
    } else if (solver::MacroManager::get().isReplaying()) {
        badge->setVisible(true);
        badge->setString("[REPLAY BOT] Inputs Complete (Rolling to end)");
        badge->setColor({ 100, 220, 255 });
    } else {
        badge->setVisible(false);
    }
}

class $modify(SolverPlayerObject, PlayerObject) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayerObject::playerDestroyed", geode::Priority::First);
        (void)self.setHookPriority("PlayerObject::pushButton", geode::Priority::First);
        (void)self.setHookPriority("PlayerObject::releaseButton", geode::Priority::First);
    }

    void playerDestroyed(bool noEffects) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            this->m_isDead = true;
            if (auto pl = PlayLayer::get()) {
                pl->m_playerDied = true;
            }
            return;
        }
        PlayerObject::playerDestroyed(noEffects);
    }

    void pushButton(PlayerButton button) {
        if (solver::MacroManager::get().isReplaying() && !solver::MacroManager::get().isDispatchingInput()) {
            return;
        }
        PlayerObject::pushButton(button);
    }

    void releaseButton(PlayerButton button) {
        if (solver::MacroManager::get().isReplaying() && !solver::MacroManager::get().isDispatchingInput()) {
            return;
        }
        PlayerObject::releaseButton(button);
    }
};

class $modify(SolverBaseGameLayer, GJBaseGameLayer) {
    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!isHalfTick && solver::MacroManager::get().isPlaying()) {
            if (auto pl = typeinfo_cast<PlayLayer*>(this)) {
                if (!pl->m_inResetDelay && pl->m_started && !pl->m_playerDied && pl->m_player1 && !pl->m_player1->m_isDead) {
                    solver::MacroManager::get().stepReplay(pl);
                }
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (solver::MacroManager::get().isReplaying() && !solver::MacroManager::get().isDispatchingInput()) {
            return;
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void update(float dt) {
        GJBaseGameLayer::update(dt);
        if (auto pl = typeinfo_cast<PlayLayer*>(this)) {
            updateReplayBadge(pl);
        }
    }
};

class $modify(SolverFMODAudioEngine, FMODAudioEngine) {
    void playMusic(gd::string path, bool p1, float p2, int p3) {
        if (solver::HeadlessEngine::get().isAudioSuppressed()) {
            return;
        }
        FMODAudioEngine::playMusic(path, p1, p2, p3);
    }

    void playEffect(gd::string path) {
        if (solver::HeadlessEngine::get().isAudioSuppressed()) {
            return;
        }
        FMODAudioEngine::playEffect(path);
    }

    void playEffect(gd::string path, float speed, float p2, float volume) {
        if (solver::HeadlessEngine::get().isAudioSuppressed()) {
            return;
        }
        FMODAudioEngine::playEffect(path, speed, p2, volume);
    }
};

class $modify(SolverPlayLayer, PlayLayer) {
    static void onModify(auto& self) {
        (void)self.setHookPriority("PlayLayer::destroyPlayer", geode::Priority::First);
        (void)self.setHookPriority("PlayLayer::updateVisibility", geode::Priority::First);
        (void)self.setHookPriority("PlayLayer::postUpdate", geode::Priority::First);
    }

    ~SolverPlayLayer() {
        if (solver::HeadlessEngine::get().isHeadless() || this->m_isSilent) {
            return;
        }
        // If a new replay was armed for the upcoming scene, don't stop it!
        if (solver::MacroManager::get().isArmed()) {
            return;
        }
        if (solver::MacroManager::get().isReplaySessionActive()) {
            solver::MacroManager::get().stopReplay(this);
        }
    }

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) {
            return false;
        }

        if (solver::HeadlessEngine::get().isHeadless() || this->m_isSilent) {
            return true;
        }

        // Check if a replay session was explicitly requested for this level
        int levelID = level ? level->m_levelID.value() : 0;
        std::string levelName = level ? level->m_levelName : "";

        if (solver::MacroManager::get().isArmed()) {
            int armedID = solver::MacroManager::get().getArmedLevelID();
            std::string armedName = solver::MacroManager::get().getArmedLevelName();

            bool levelMatches = false;
            if (armedID > 0 && armedID == levelID) {
                levelMatches = true;
            } else if (armedID <= 0 && levelID <= 0) {
                levelMatches = (armedName == levelName || armedName.empty() || levelName.empty());
            }

            if (levelMatches) {
                solver::DeterministicPRNG::clampSeed();
                solver::MacroManager::get().onLevelReset(this);
            } else {
                solver::MacroManager::get().stopReplay(this);
            }
        } else {
            // Normal play: unconditionally ensure replay is completely disarmed and stopped
            solver::MacroManager::get().stopReplay(this);
            if (solver::SwarmSolver::get().isRunning()) {
                solver::SwarmSolver::get().stop();
            }
            if (solver::AStarSolver::get().isRunning()) {
                solver::AStarSolver::get().stop();
            }
        }

        updateReplayBadge(this);
        return true;
    }

    void startGame() {
        PlayLayer::startGame();
        if (!solver::HeadlessEngine::get().isHeadless() && !this->m_isSilent) {
            if (solver::MacroManager::get().isReplaySessionActive()) {
                solver::MacroManager::get().onGameStart(this);
            }
            updateReplayBadge(this);
        }
    }

    void updateVisibility(float dt) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            return;
        }
        PlayLayer::updateVisibility(dt);
    }

    void postUpdate(float dt) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            return;
        }
        PlayLayer::postUpdate(dt);
        updateReplayBadge(this);
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        if (solver::HeadlessEngine::get().isHeadless()) {
            // RobTop's initial spawn anti-cheat spike (m_anticheatSpike / Object ID 8 at X <= 30.0) must not kill headless simulation
            if (object && (object == this->m_anticheatSpike || (object->m_objectID == 8 && object->getPositionX() <= 30.0f))) {
                return;
            }
            // In headless simulation, the camera is detached.
            // Ignore camera culling / off-screen boundary deaths (object == nullptr) unless the player fell into the void.
            if (!object) {
                if (player && player->getPositionY() < -50.0f) {
                    player->m_isDead = true;
                    this->m_playerDied = true;
                }
                return;
            }
            static uint32_t s_deathLogCount = 0;
            if (++s_deathLogCount <= 50 || (s_deathLogCount % 200 == 0)) {
                geode::log::warn("[LevelSolver] Simulation death: Object={}, Player=({:.1f}, {:.1f}), yVel={:.2f}",
                    fmt::format("ID {} ({}) at ({:.1f}, {:.1f})", object->m_objectID, static_cast<int>(object->m_objectType), object->getPositionX(), object->getPositionY()),
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
            this->m_hasCompletedLevel = true;
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
            this->m_hasCompletedLevel = true;
            return;
        }
        PlayLayer::showEndLayer();
    }

    void levelComplete() {
        if (solver::HeadlessEngine::get().isHeadless()) {
            this->m_hasCompletedLevel = true;
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
        if (!solver::HeadlessEngine::get().isHeadless() && !this->m_isSilent) {
            if (solver::MacroManager::get().isReplaySessionActive()) {
                solver::DeterministicPRNG::clampSeed();
                solver::MacroManager::get().onLevelReset(this);
            }
            updateReplayBadge(this);
        }
    }
};
