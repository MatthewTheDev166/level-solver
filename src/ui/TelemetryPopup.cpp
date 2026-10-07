#include "TelemetryPopup.hpp"
#include "../solver/SwarmSolver.hpp"
#include "../replay/MacroManager.hpp"
#include "../replay/GDRExporter.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../engine/ActiveLayerScope.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>

using namespace geode::prelude;

namespace solver {

static void fitLabel(CCLabelBMFont* label, float maxWidth, float defaultScale) {
    if (!label) return;
    label->setScale(defaultScale);
    float width = label->getContentWidth() * defaultScale;
    if (width > maxWidth && width > 0.001f) {
        label->setScale(defaultScale * (maxWidth / width));
    }
}

static void setButtonVisualState(CCMenuItemSpriteExtra* btn, bool enabled) {
    if (!btn) return;
    btn->setEnabled(enabled);
    if (auto sprite = btn->getNormalImage()) {
        cocos2d::ccColor3B col = enabled ? cocos2d::ccColor3B{255, 255, 255} : cocos2d::ccColor3B{120, 120, 120};
        GLubyte op = enabled ? 255 : 130;
        if (auto rgba = typeinfo_cast<CCRGBAProtocol*>(sprite)) {
            rgba->setColor(col);
            rgba->setOpacity(op);
        }
        if (auto btnSpr = typeinfo_cast<ButtonSprite*>(sprite)) {
            if (auto lbl = btnSpr->m_label) {
                lbl->setColor(col);
                lbl->setOpacity(op);
            }
        }
        for (unsigned int i = 0; i < sprite->getChildrenCount(); ++i) {
            if (auto child = typeinfo_cast<CCRGBAProtocol*>(sprite->getChildren()->objectAtIndex(i))) {
                child->setColor(col);
                child->setOpacity(op);
            }
        }
    }
}

TelemetryPopup* TelemetryPopup::create(GJGameLevel* level) {
    auto ret = new TelemetryPopup();
    if (ret && ret->init(485.0f, 290.0f, level)) {
        ret->autorelease();
        return ret;
    }
    CC_SAFE_DELETE(ret);
    return nullptr;
}

bool TelemetryPopup::init(float width, float height, GJGameLevel* level) {
    if (!Popup::init(width, height)) {
        return false;
    }

    m_level = level;
    std::string verStr = "v1.5.3";
    this->setTitle(fmt::format("Level Solver {}", verStr));

    // Display version in upper corner of stats panel
    auto verBadge = CCLabelBMFont::create(verStr.c_str(), "chatFont.fnt");
    verBadge->setScale(0.70f);
    verBadge->setAnchorPoint({ 1.0f, 1.0f });
    verBadge->setPosition({ width - 35.0f, height - 12.0f });
    verBadge->setColor({ 140, 190, 255 });
    m_mainLayer->addChild(verBadge);

    float yOffset = 216.0f;
    float lineHeight = 19.0f;
    float xOffset = 30.0f;

    // 1. Status label
    m_statusLabel = CCLabelBMFont::create("Status: Idle", "bigFont.fnt");
    m_statusLabel->setScale(0.38f);
    m_statusLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_statusLabel->setPosition({ xOffset, yOffset });
    m_statusLabel->setColor({ 180, 200, 220 });
    m_mainLayer->addChild(m_statusLabel);
    yOffset -= lineHeight + 2.0f;

    // 2. Exploration Progress label
    m_progressLabel = CCLabelBMFont::create("Exploration Progress: 0.0%", "bigFont.fnt");
    m_progressLabel->setScale(0.40f);
    m_progressLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_progressLabel->setPosition({ xOffset, yOffset });
    m_progressLabel->setColor({ 255, 215, 0 });
    m_mainLayer->addChild(m_progressLabel);
    yOffset -= lineHeight + 2.0f;

    // 3. Real-time Bots Alive label
    m_aliveBotsLabel = CCLabelBMFont::create("Bots Alive: 160 / 160", "bigFont.fnt");
    m_aliveBotsLabel->setScale(0.38f);
    m_aliveBotsLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_aliveBotsLabel->setPosition({ xOffset, yOffset });
    m_aliveBotsLabel->setColor({ 80, 220, 255 });
    m_mainLayer->addChild(m_aliveBotsLabel);
    yOffset -= lineHeight + 1.0f;

    // 4. Wave Info label
    m_waveLabel = CCLabelBMFont::create("Wave: #1 (Attempt 1/10)", "chatFont.fnt");
    m_waveLabel->setScale(0.80f);
    m_waveLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_waveLabel->setPosition({ xOffset, yOffset });
    m_waveLabel->setColor({ 240, 240, 240 });
    m_mainLayer->addChild(m_waveLabel);
    yOffset -= lineHeight;

    // 5. Checkpoints & Backtracks label
    m_checkpointLabel = CCLabelBMFont::create("Checkpoints: 1 | Backtracks: 0", "chatFont.fnt");
    m_checkpointLabel->setScale(0.80f);
    m_checkpointLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_checkpointLabel->setPosition({ xOffset, yOffset });
    m_checkpointLabel->setColor({ 200, 225, 255 });
    m_mainLayer->addChild(m_checkpointLabel);
    yOffset -= lineHeight;

    // 6. Solving Speed label
    m_speedLabel = CCLabelBMFont::create("Speed: 0 ticks/sec", "chatFont.fnt");
    m_speedLabel->setScale(0.80f);
    m_speedLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_speedLabel->setPosition({ xOffset, yOffset });
    m_speedLabel->setColor({ 160, 255, 160 });
    m_mainLayer->addChild(m_speedLabel);
    yOffset -= lineHeight;

    // 7. Saved Replay label
    int levelID = level ? level->m_levelID.value() : 0;
    std::string levelName = level ? level->m_levelName : "";
    bool hasSavedMacro = MacroManager::get().hasMacro(levelID, levelName);
    m_macroStatusLabel = CCLabelBMFont::create(
        hasSavedMacro ? "Saved Replay: Available (Ready to Replay)" : "Saved Replay: None",
        "chatFont.fnt"
    );
    m_macroStatusLabel->setScale(0.75f);
    m_macroStatusLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_macroStatusLabel->setPosition({ xOffset, yOffset });
    m_macroStatusLabel->setColor(hasSavedMacro ? cocos2d::ccColor3B{100, 255, 100} : cocos2d::ccColor3B{180, 180, 180});
    m_mainLayer->addChild(m_macroStatusLabel);

    // Checkbox for toggling HUD display during replay playback
    auto hudMenu = CCMenu::create();
    hudMenu->setPosition({ xOffset, 62.0f });
    m_mainLayer->addChild(hudMenu);

    m_hudToggler = CCMenuItemToggler::createWithStandardSprites(
        this,
        menu_selector(TelemetryPopup::onToggleShowHUD),
        0.65f
    );
    bool showHUD = Mod::get()->getSavedValue<bool>("show-replay-hud", true);
    m_hudToggler->toggle(showHUD);
    m_hudToggler->setPosition({ 10.0f, 0.0f });
    hudMenu->addChild(m_hudToggler);

    auto hudLabel = CCLabelBMFont::create("Show HUD on Replay", "chatFont.fnt");
    hudLabel->setScale(0.70f);
    hudLabel->setAnchorPoint({ 0.0f, 0.5f });
    hudLabel->setPosition({ 26.0f, 0.0f });
    hudMenu->addChild(hudLabel);

    // Control buttons menu
    auto buttonMenu = CCMenu::create();
    buttonMenu->setPosition({ 242.5f, 35.0f });
    m_mainLayer->addChild(buttonMenu);

    // 4 bottom action buttons: Start, Stop, Replay, Export Macro
    auto startSpr = ButtonSprite::create("Start", "goldFont.fnt", "GJ_button_01.png", 0.70f);
    m_startButton = CCMenuItemSpriteExtra::create(startSpr, this, menu_selector(TelemetryPopup::onStartSolver));
    m_startButton->setPosition({ -165.0f, 0.0f });
    buttonMenu->addChild(m_startButton);

    auto stopSpr = ButtonSprite::create("Stop", "goldFont.fnt", "GJ_button_06.png", 0.70f);
    m_stopButton = CCMenuItemSpriteExtra::create(stopSpr, this, menu_selector(TelemetryPopup::onStopSolver));
    m_stopButton->setPosition({ -55.0f, 0.0f });
    buttonMenu->addChild(m_stopButton);

    auto replaySpr = ButtonSprite::create("Replay", "goldFont.fnt", "GJ_button_02.png", 0.70f);
    m_replayButton = CCMenuItemSpriteExtra::create(replaySpr, this, menu_selector(TelemetryPopup::onReplayMacro));
    m_replayButton->setPosition({ 55.0f, 0.0f });
    buttonMenu->addChild(m_replayButton);

    auto exportSpr = ButtonSprite::create("Export", "goldFont.fnt", "GJ_button_04.png", 0.70f);
    m_exportButton = CCMenuItemSpriteExtra::create(exportSpr, this, menu_selector(TelemetryPopup::onExportMacro));
    m_exportButton->setPosition({ 165.0f, 0.0f });
    buttonMenu->addChild(m_exportButton);

    setButtonVisualState(m_startButton, true);
    setButtonVisualState(m_stopButton, false);
    setButtonVisualState(m_replayButton, hasSavedMacro);
    setButtonVisualState(m_exportButton, hasSavedMacro);

    this->scheduleUpdate();
    return true;
}


void TelemetryPopup::update(float dt) {
    if (auto eglView = cocos2d::CCEGLView::sharedOpenGLView()) {
        eglView->showCursor(true);
    }

    // Ensure mouse cursor remains visible while popup is open
    if (auto eglView = cocos2d::CCEGLView::sharedOpenGLView()) {
        eglView->showCursor(true);
    }

    // If running in background, advance headless swarm batch
    if (SwarmSolver::get().isRunning() && m_headlessPlayLayer) {
        ActiveLayerScope scope(m_headlessPlayLayer);
        SwarmSolver::get().stepSwarmBatch(m_headlessPlayLayer, HeadlessEngine::get().getBatchSize());
    }

    auto telemetry = SwarmSolver::get().getTelemetry();

    // Update status & print errors clearly on the menu
    if (telemetry.status == SolverStatus::Solved) {
        m_statusLabel->setString(telemetry.isVerified ? "Status: Solved (100% Verified)!" : "Status: Solved (100%)");
        m_statusLabel->setColor({ 0, 255, 128 });
        m_startButton->setEnabled(true);
        m_stopButton->setEnabled(false);
        m_replayButton->setEnabled(true);
    } else if (telemetry.status == SolverStatus::Failed) {
        std::string err = "Status: Error - ";
        if (!telemetry.detailMessage.empty()) {
            err += telemetry.detailMessage;
        } else {
            err += "Search failed";
        }
        m_statusLabel->setString(err.c_str());
        m_statusLabel->setColor({ 255, 60, 60 });
        m_startButton->setEnabled(true);
        m_stopButton->setEnabled(false);
    } else if (telemetry.status == SolverStatus::Searching) {
        m_statusLabel->setString("Status: Searching");
        m_statusLabel->setColor({ 0, 255, 128 });
    } else if (telemetry.status == SolverStatus::Paused) {
        m_statusLabel->setString("Status: Paused");
        m_statusLabel->setColor({ 255, 200, 0 });
    } else {
        m_statusLabel->setString("Status: Idle");
        m_statusLabel->setColor({ 180, 200, 220 });
    }

    // Update Exploration Progress
    m_progressLabel->setString(fmt::format("Exploration Progress: {:.1f}%", telemetry.explorationHorizon).c_str());

    // Update Real-time Bots Alive
    m_aliveBotsLabel->setString(fmt::format("Bots Alive: {} / {}", telemetry.aliveBots, telemetry.totalBots).c_str());

    // Update Wave Info
    uint32_t displayWave = telemetry.activeWave == 0 ? 1 : telemetry.activeWave;
    uint32_t displayAttempt = telemetry.waveAttempt == 0 ? 1 : telemetry.waveAttempt;
    m_waveLabel->setString(fmt::format("Wave: #{} (Attempt {}/10)", displayWave, displayAttempt).c_str());

    // Update Checkpoints & Backtracks
    m_checkpointLabel->setString(fmt::format("Checkpoints: {} | Backtracks: {}",
        telemetry.frontierSize, telemetry.rewindCount).c_str());

    // Update Solving Speed
    m_speedLabel->setString(fmt::format("Speed: {:.0f} ticks/sec", telemetry.ticksPerSecond).c_str());

    // Macro / Replay status
    int levelID = m_level ? m_level->m_levelID.value() : 0;
    std::string levelName = m_level ? m_level->m_levelName : "";
    bool hasSavedMacro = MacroManager::get().hasMacro(levelID, levelName) || SwarmSolver::get().isCompleted();
    if (m_macroStatusLabel) {
        if (hasSavedMacro) {
            m_macroStatusLabel->setString(telemetry.isVerified ? "Saved Replay: Available (100% Verified)" : "Saved Replay: Available (Ready to Replay)");
            m_macroStatusLabel->setColor({ 100, 255, 100 });
        } else {
            m_macroStatusLabel->setString("Saved Replay: None");
            m_macroStatusLabel->setColor({ 180, 180, 180 });
        }
    }

    // Ensure all labels fit cleanly inside the popup box without overflowing
    const float maxLabelW = 425.0f;
    fitLabel(m_statusLabel, maxLabelW, 0.38f);
    fitLabel(m_progressLabel, maxLabelW, 0.40f);
    fitLabel(m_aliveBotsLabel, maxLabelW, 0.38f);
    fitLabel(m_waveLabel, maxLabelW, 0.80f);
    fitLabel(m_checkpointLabel, maxLabelW, 0.80f);
    fitLabel(m_speedLabel, maxLabelW, 0.80f);
    fitLabel(m_macroStatusLabel, maxLabelW, 0.75f);

    // Button states
    bool isSearching = SwarmSolver::get().isRunning();
    setButtonVisualState(m_startButton, !isSearching);
    setButtonVisualState(m_stopButton, isSearching);
    setButtonVisualState(m_replayButton, hasSavedMacro && !isSearching);
    setButtonVisualState(m_exportButton, hasSavedMacro && !isSearching);
}

TelemetryPopup::~TelemetryPopup() {
    cleanupHeadless();
}

void TelemetryPopup::cleanupHeadless() {
    if (SwarmSolver::get().isRunning()) {
        SwarmSolver::get().stop();
    }
    if (m_headlessPlayLayer) {
        if (auto gm = GameManager::sharedState()) {
            if (gm->m_playLayer == m_headlessPlayLayer) {
                gm->m_playLayer = m_previousPlayLayer;
            }
            if (gm->m_gameLayer == m_headlessPlayLayer) {
                gm->m_gameLayer = m_previousPlayLayer;
            }
        }
        m_headlessPlayLayer->removeFromParentAndCleanup(true);
        m_headlessPlayLayer->release();
        m_headlessPlayLayer = nullptr;
    }
    if (m_headlessScene) {
        m_headlessScene->release();
        m_headlessScene = nullptr;
    }
    HeadlessEngine::get().disableHeadless();
    if (auto eglView = cocos2d::CCEGLView::sharedOpenGLView()) {
        eglView->showCursor(true);
    }
}

void TelemetryPopup::onClose(cocos2d::CCObject* sender) {
    cleanupHeadless();
    Popup::onClose(sender);
}

void TelemetryPopup::onStartSolver(cocos2d::CCObject* sender) {
    if (!m_level) return;

    if (SwarmSolver::get().isRunning()) return;

    if (m_level->isPlatformer()) {
        m_statusLabel->setString("Status: Platformer unsupported");
        m_statusLabel->setColor({ 255, 60, 60 });
        FLAlertLayer::create("Unsupported Mode", "Platformer mode is not supported by Level Solver.", "OK")->show();
        return;
    }

    if (m_level->m_twoPlayerMode) {
        m_statusLabel->setString("Status: 2-Player unsupported");
        m_statusLabel->setColor({ 255, 60, 60 });
        FLAlertLayer::create("Unsupported Mode", "2-Player mode is not supported by Level Solver.", "OK")->show();
        return;
    }

    // Immediately notify CheatAPI to safeguard leaderboards
    CheatAPIIntegrator::notifyCheatStarted();

    cleanupHeadless();

    // Cache the previous active PlayLayer (e.g. if opened from PauseLayer, or nullptr if from menu)
    m_previousPlayLayer = GameManager::sharedState() ? GameManager::sharedState()->m_playLayer : nullptr;

    // Enable headless engine and silence audio before initializing PlayLayer
    HeadlessEngine::get().enableHeadless();

    // Create an off-screen scene and PlayLayer purely for background simulation
    m_headlessScene = cocos2d::CCScene::create();
    m_headlessScene->retain();

    m_headlessPlayLayer = PlayLayer::create(m_level, false, false);
    if (!m_headlessPlayLayer) {
        cleanupHeadless();
        m_statusLabel->setString("Status: Error - Failed to initialize simulation");
        m_statusLabel->setColor({ 255, 60, 60 });
        return;
    }

    m_headlessPlayLayer->retain();
    m_headlessScene->addChild(m_headlessPlayLayer);
    m_headlessPlayLayer->m_isSilent = true;
    m_headlessPlayLayer->m_isPracticeMode = true;

    // Disable all input reception on headless layer to avoid intercepting user keys/clicks
    m_headlessPlayLayer->setKeypadEnabled(false);
    m_headlessPlayLayer->setTouchEnabled(false);
    m_headlessPlayLayer->setMouseEnabled(false);

    {
        // Scope active layer while setting up objects and initializing start state
        ActiveLayerScope scope(m_headlessPlayLayer);

        // Complete object creation synchronously if chunked across frames
        int safetyLimit = 10000;
        while (m_headlessPlayLayer->m_loadingProgress < 1.0f && --safetyLimit > 0) {
            m_headlessPlayLayer->processCreateObjectsFromSetup();
        }

        m_headlessPlayLayer->resetLevel();
        m_headlessPlayLayer->startGame();
        m_headlessPlayLayer->m_isPaused = false;
        m_headlessPlayLayer->m_hasCompletedLevel = false;
        if (m_headlessPlayLayer->m_checkpointArray) {
            m_headlessPlayLayer->m_checkpointArray->removeAllObjects();
        }
        if (m_headlessPlayLayer->m_player1) {
            m_headlessPlayLayer->moveCameraToPos(m_headlessPlayLayer->m_player1->getPosition());
        }
    }

    // Force cursor to stay visible after startGame() hides it
    if (auto eglView = cocos2d::CCEGLView::sharedOpenGLView()) {
        eglView->showCursor(true);
    }

    if (!m_headlessPlayLayer->m_player1 || !m_headlessPlayLayer->m_objects) {
        cleanupHeadless();
        m_statusLabel->setString("Status: Error - Level setup incomplete");
        m_statusLabel->setColor({ 255, 60, 60 });
        return;
    }

    geode::log::info("[LevelSolver] Headless PlayLayer ready. Object count: {}, Player pos: ({}, {})",
        m_headlessPlayLayer->m_objects ? m_headlessPlayLayer->m_objects->count() : 0,
        m_headlessPlayLayer->m_player1 ? m_headlessPlayLayer->m_player1->getPositionX() : -1.0f,
        m_headlessPlayLayer->m_player1 ? m_headlessPlayLayer->m_player1->getPositionY() : -1.0f
    );

    // Start SwarmSolver on the headless playLayer with active layer scope
    {
        ActiveLayerScope scope(m_headlessPlayLayer);
        SwarmSolver::get().start(m_headlessPlayLayer);
    }

    m_statusLabel->setString("Status: Searching");
    m_statusLabel->setColor({ 0, 255, 128 });
    setButtonVisualState(m_startButton, false);
    setButtonVisualState(m_stopButton, true);
    setButtonVisualState(m_replayButton, false);
    setButtonVisualState(m_exportButton, false);
}

void TelemetryPopup::onStopSolver(cocos2d::CCObject* sender) {
    SwarmSolver::get().stop();
    m_statusLabel->setString("Status: Stopped by user");
    m_statusLabel->setColor({ 255, 200, 0 });

    int levelID = m_level ? m_level->m_levelID.value() : 0;
    std::string levelName = m_level ? m_level->m_levelName : "";
    bool hasSavedMacro = MacroManager::get().hasMacro(levelID, levelName) || SwarmSolver::get().isCompleted();
    setButtonVisualState(m_startButton, true);
    setButtonVisualState(m_stopButton, false);
    setButtonVisualState(m_replayButton, hasSavedMacro);
    setButtonVisualState(m_exportButton, hasSavedMacro);
}

void TelemetryPopup::onExportMacro(cocos2d::CCObject* sender) {
    if (!m_level) return;
    int levelID = m_level->m_levelID.value();
    std::string levelName = m_level->m_levelName;

    if (!MacroManager::get().hasMacro(levelID, levelName) && !SwarmSolver::get().isCompleted()) {
        FLAlertLayer::create("No Macro", "No solved macro found for this level to export.", "OK")->show();
        return;
    }

    if (!MacroManager::get().hasMacro(levelID, levelName) && SwarmSolver::get().isCompleted()) {
        MacroManager::get().setActions(SwarmSolver::get().getResolvedMacro());
        MacroManager::get().setTrajectory(SwarmSolver::get().getTrajectory());
        MacroManager::get().saveMacro(levelID, levelName);
    } else {
        MacroManager::get().loadMacro(levelID, levelName);
    }

    const auto& actions = MacroManager::get().getActions();
    if (actions.empty()) {
        FLAlertLayer::create("No Actions", "Macro contains no recorded actions to export.", "OK")->show();
        return;
    }

    auto exportRes = GDRExporter::exportReplays(levelName, levelID, actions);
    std::string safeName = GDRExporter::sanitizeFilename(levelName, levelID);

    if (exportRes.success) {
        std::string alertMsg = fmt::format(
            "Macro exported to Mega Hack!\n\n"
            "File: replays/{}-macro.gdr2\n"
            "(also replays/{}-macro.json)\n\n"
            "To play via Mega Hack:\n"
            "Press Tab -> Replay -> select macro -> Load\n\n"
            "Replay is also saved for built-in playback!",
            safeName, safeName
        );
        FLAlertLayer::create("Macro Exported", alertMsg.c_str(), "OK")->show();
    } else {
        FLAlertLayer::create("Export Failed", "Failed to export macro files to replays directory.", "OK")->show();
    }
}

void TelemetryPopup::onToggleShowHUD(cocos2d::CCObject* sender) {
    auto toggler = typeinfo_cast<CCMenuItemToggler*>(sender);
    if (!toggler) return;
    bool showHUD = toggler->isOn();
    Mod::get()->setSavedValue("show-replay-hud", showHUD);
    geode::log::info("[LevelSolver] Replay HUD visibility toggled: {}", showHUD);
}

void TelemetryPopup::onReplayMacro(cocos2d::CCObject* sender) {
    if (!m_level) return;
    int levelID = m_level->m_levelID.value();
    std::string levelName = m_level->m_levelName;

    if (!MacroManager::get().hasMacro(levelID, levelName) && !SwarmSolver::get().isCompleted()) {
        FLAlertLayer::create("No Macro", "No solved macro found for this level.", "OK")->show();
        return;
    }

    if (!MacroManager::get().hasMacro(levelID, levelName) && SwarmSolver::get().isCompleted()) {
        MacroManager::get().setActions(SwarmSolver::get().getResolvedMacro());
        MacroManager::get().setTrajectory(SwarmSolver::get().getTrajectory());
        MacroManager::get().saveMacro(levelID, levelName);
    } else {
        MacroManager::get().loadMacro(levelID, levelName);
    }

    // Explicitly activate replay session so it persists across respawns and restarts
    MacroManager::get().setReplaySessionActive(true);
    MacroManager::get().armReplay(levelID, levelName);

    // If currently inside an active scene PlayLayer
    if (m_previousPlayLayer && m_previousPlayLayer != m_headlessPlayLayer) {
        cleanupHeadless();
        if (auto scene = cocos2d::CCDirector::sharedDirector()->getRunningScene()) {
            if (auto pauseLayer = scene->getChildByType<PauseLayer>(0)) {
                pauseLayer->onResume(nullptr);
            }
        }
        m_previousPlayLayer->resetLevel();
        this->onClose(nullptr);
        return;
    }

    cleanupHeadless();

    // Transition to gameplay scene to watch playback
    auto scene = PlayLayer::scene(m_level, false, false);
    CCDirector::sharedDirector()->pushScene(scene);

    this->onClose(nullptr);
}

} // namespace solver
