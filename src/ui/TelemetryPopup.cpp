#include "TelemetryPopup.hpp"
#include "../replay/MacroManager.hpp"
#include "../engine/HeadlessEngine.hpp"
#include "../engine/ActiveLayerScope.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/PauseLayer.hpp>

using namespace geode::prelude;

namespace solver {

TelemetryPopup* TelemetryPopup::create(GJGameLevel* level) {
    auto ret = new TelemetryPopup();
    if (ret && ret->init(420.0f, 290.0f, level)) {
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
    this->setTitle("Genetic Swarm Solver");

    float yOffset = 215.0f;
    float lineHeight = 18.0f;
    float xOffset = 30.0f;

    // Status label
    m_statusLabel = CCLabelBMFont::create("Status: Idle", "bigFont.fnt");
    m_statusLabel->setScale(0.38f);
    m_statusLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_statusLabel->setPosition({ xOffset, yOffset });
    m_statusLabel->setColor({ 0, 255, 128 });
    m_mainLayer->addChild(m_statusLabel);
    yOffset -= lineHeight + 4.0f;

    // Exploration Horizon label
    m_horizonLabel = CCLabelBMFont::create("Exploration Horizon: 0.0%", "bigFont.fnt");
    m_horizonLabel->setScale(0.42f);
    m_horizonLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_horizonLabel->setPosition({ xOffset, yOffset });
    m_horizonLabel->setColor({ 255, 215, 0 });
    m_mainLayer->addChild(m_horizonLabel);
    yOffset -= lineHeight + 4.0f;

    // Wave / Checkpoint label
    m_waveLabel = CCLabelBMFont::create("Wave Generation: #0 | Checkpoint Depth: 0", "chatFont.fnt");
    m_waveLabel->setScale(0.80f);
    m_waveLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_waveLabel->setPosition({ xOffset, yOffset });
    m_mainLayer->addChild(m_waveLabel);
    yOffset -= lineHeight;

    // Population / Survivors label
    m_populationLabel = CCLabelBMFont::create("Swarm Population: 100 bots | Survivors: 0", "chatFont.fnt");
    m_populationLabel->setScale(0.80f);
    m_populationLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_populationLabel->setPosition({ xOffset, yOffset });
    m_mainLayer->addChild(m_populationLabel);
    yOffset -= lineHeight;

    // Backtrack count label
    m_backtrackLabel = CCLabelBMFont::create("Dead-End Backtracks: 0 rewinds", "chatFont.fnt");
    m_backtrackLabel->setScale(0.80f);
    m_backtrackLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_backtrackLabel->setPosition({ xOffset, yOffset });
    m_mainLayer->addChild(m_backtrackLabel);
    yOffset -= lineHeight;

    // Memory Footprint label
    m_memoryLabel = CCLabelBMFont::create("Memory Footprint: 0.00 MB", "chatFont.fnt");
    m_memoryLabel->setScale(0.75f);
    m_memoryLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_memoryLabel->setPosition({ xOffset, yOffset });
    m_mainLayer->addChild(m_memoryLabel);
    yOffset -= lineHeight;

    // Simulation Throughput label
    m_throughputLabel = CCLabelBMFont::create("Simulation Rate: 0 ticks/sec", "chatFont.fnt");
    m_throughputLabel->setScale(0.75f);
    m_throughputLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_throughputLabel->setPosition({ xOffset, yOffset });
    m_mainLayer->addChild(m_throughputLabel);
    yOffset -= lineHeight;

    // Macro status label
    int levelID = level ? level->m_levelID.value() : 0;
    bool hasSavedMacro = MacroManager::get().hasMacro(levelID);
    m_macroStatusLabel = CCLabelBMFont::create(
        hasSavedMacro ? "Saved Macro: Available on Disk (Ready to Replay)" : "Saved Macro: None Found",
        "chatFont.fnt"
    );
    m_macroStatusLabel->setScale(0.7f);
    m_macroStatusLabel->setAnchorPoint({ 0.0f, 0.5f });
    m_macroStatusLabel->setPosition({ xOffset, yOffset });
    m_macroStatusLabel->setColor(hasSavedMacro ? cocos2d::ccColor3B{100, 255, 100} : cocos2d::ccColor3B{180, 180, 180});
    m_mainLayer->addChild(m_macroStatusLabel);

    // Control buttons menu
    auto buttonMenu = CCMenu::create();
    buttonMenu->setPosition({ 210.0f, 35.0f });
    m_mainLayer->addChild(buttonMenu);

    // Start button
    auto startSpr = ButtonSprite::create("Start Solve", "goldFont.fnt", "GJ_button_01.png", 0.75f);
    m_startButton = CCMenuItemSpriteExtra::create(startSpr, this, menu_selector(TelemetryPopup::onStartSolver));
    m_startButton->setPosition({ -110.0f, 0.0f });
    buttonMenu->addChild(m_startButton);

    // Stop button
    auto stopSpr = ButtonSprite::create("Stop", "goldFont.fnt", "GJ_button_06.png", 0.75f);
    m_stopButton = CCMenuItemSpriteExtra::create(stopSpr, this, menu_selector(TelemetryPopup::onStopSolver));
    m_stopButton->setPosition({ 0.0f, 0.0f });
    buttonMenu->addChild(m_stopButton);

    // Replay button
    auto replaySpr = ButtonSprite::create("Replay", "goldFont.fnt", "GJ_button_02.png", 0.75f);
    m_replayButton = CCMenuItemSpriteExtra::create(replaySpr, this, menu_selector(TelemetryPopup::onReplayMacro));
    m_replayButton->setPosition({ 110.0f, 0.0f });
    m_replayButton->setEnabled(hasSavedMacro);
    buttonMenu->addChild(m_replayButton);

    this->scheduleUpdate();
    return true;
}


void TelemetryPopup::update(float dt) {
    // If running in background, advance headless swarm search batch
    if (SwarmSolver::get().isRunning() && m_headlessPlayLayer) {
        ActiveLayerScope scope(m_headlessPlayLayer);
        SwarmSolver::get().stepSwarmBatch(m_headlessPlayLayer, HeadlessEngine::get().getBatchSize());
    }

    auto telemetry = SwarmSolver::get().getTelemetry();

    // Update status & print errors clearly on the menu
    if (telemetry.status == SolverStatus::Solved) {
        m_statusLabel->setString("Status: Solved (100%) - Route saved to disk!");
        m_statusLabel->setColor({ 0, 255, 128 });
        m_startButton->setEnabled(true);
        m_stopButton->setEnabled(false);
        m_replayButton->setEnabled(true);
    } else if (telemetry.status == SolverStatus::Failed) {
        std::string err = "Status: Error - ";
        if (!telemetry.detailMessage.empty()) {
            err += telemetry.detailMessage;
        } else {
            err += "Search space exhausted";
        }
        m_statusLabel->setString(err.c_str());
        m_statusLabel->setColor({ 255, 60, 60 });
        m_startButton->setEnabled(true);
        m_stopButton->setEnabled(false);
    } else if (telemetry.status == SolverStatus::Searching) {
        std::string statusText = "Status: Swarm Searching";
        if (!telemetry.detailMessage.empty()) {
            statusText += " - " + telemetry.detailMessage;
        }
        m_statusLabel->setString(statusText.c_str());
        m_statusLabel->setColor({ 0, 255, 128 });
    } else if (telemetry.status == SolverStatus::Paused) {
        m_statusLabel->setString("Status: Paused");
        m_statusLabel->setColor({ 255, 200, 0 });
    }

    // Update horizon %
    m_horizonLabel->setString(fmt::format("Exploration Horizon: {:.1f}%", telemetry.explorationHorizon).c_str());

    // Update wave & checkpoints
    m_waveLabel->setString(fmt::format("Wave Generation: #{} | Checkpoint Depth: {}", telemetry.activeWave, telemetry.checkpointDepth).c_str());

    // Update population & survivors
    m_populationLabel->setString(fmt::format("Swarm Population: {} bots | Survivors: {}", telemetry.populationSize, telemetry.survivorCount).c_str());

    // Update backtracks
    m_backtrackLabel->setString(fmt::format("Dead-End Backtracks: {} rewinds", telemetry.backtrackCount).c_str());

    // Memory footprint
    float memMB = static_cast<float>(telemetry.memoryFootprintBytes) / (1024.0f * 1024.0f);
    m_memoryLabel->setString(fmt::format("Memory Footprint: {:.2f} MB", memMB).c_str());

    // Throughput
    m_throughputLabel->setString(fmt::format("Simulation Rate: {:.0f} ticks/sec", telemetry.ticksPerSecond).c_str());

    // Macro status
    int levelID = m_level ? m_level->m_levelID.value() : 0;
    bool hasSavedMacro = MacroManager::get().hasMacro(levelID) || SwarmSolver::get().isCompleted();
    if (m_macroStatusLabel) {
        m_macroStatusLabel->setString(hasSavedMacro ? "Saved Macro: Available on Disk (Ready to Replay)" : "Saved Macro: None Found");
        m_macroStatusLabel->setColor(hasSavedMacro ? cocos2d::ccColor3B{100, 255, 100} : cocos2d::ccColor3B{180, 180, 180});
    }

    // Button states
    bool isSearching = SwarmSolver::get().isRunning();
    m_startButton->setEnabled(!isSearching);
    m_stopButton->setEnabled(isSearching);
    m_replayButton->setEnabled(hasSavedMacro && !isSearching);
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
}

void TelemetryPopup::onClose(cocos2d::CCObject* sender) {
    cleanupHeadless();
    Popup::onClose(sender);
}

void TelemetryPopup::onStartSolver(cocos2d::CCObject* sender) {
    if (!m_level) return;

    if (SwarmSolver::get().isRunning()) return;

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
        if (m_headlessPlayLayer->m_player1) {
            m_headlessPlayLayer->moveCameraToPos(m_headlessPlayLayer->m_player1->getPosition());
            m_headlessPlayLayer->updateVisibility(0.0f);
        }
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

    m_startButton->setEnabled(false);
    m_stopButton->setEnabled(true);
    m_statusLabel->setString("Status: Swarm Searching");
    m_statusLabel->setColor({ 0, 255, 128 });
}

void TelemetryPopup::onStopSolver(cocos2d::CCObject* sender) {
    SwarmSolver::get().stop();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    m_statusLabel->setString("Status: Stopped by user");
    m_statusLabel->setColor({ 255, 200, 0 });
}

void TelemetryPopup::onReplayMacro(cocos2d::CCObject* sender) {
    if (!m_level) return;
    int levelID = m_level->m_levelID.value();

    if (!MacroManager::get().hasMacro(levelID) && !SwarmSolver::get().isCompleted()) {
        FLAlertLayer::create("No Macro", "No solved macro found for this level.", "OK")->show();
        return;
    }

    MacroManager::get().loadMacro(levelID);

    // If currently inside an active scene PlayLayer (e.g. from PauseLayer)
    if (m_previousPlayLayer && m_previousPlayLayer != m_headlessPlayLayer) {
        m_previousPlayLayer->resetLevel();
        MacroManager::get().startReplay(m_previousPlayLayer);
        this->onClose(nullptr);
        return;
    }

    cleanupHeadless();

    // Transition to gameplay scene to watch playback
    auto scene = PlayLayer::scene(m_level, false, false);
    CCDirector::sharedDirector()->pushScene(scene);

    geode::Loader::get()->queueInMainThread([]() {
        if (auto playLayer = PlayLayer::get()) {
            MacroManager::get().startReplay(playLayer);
        }
    });

    this->onClose(nullptr);
}

} // namespace solver
