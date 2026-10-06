#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include "../solver/SwarmSolver.hpp"

namespace solver {

class TelemetryPopup : public geode::Popup {
public:
    static TelemetryPopup* create(GJGameLevel* level);

    ~TelemetryPopup() override;
    void onClose(cocos2d::CCObject* sender) override;

protected:
    bool init(float width, float height, GJGameLevel* level);
    void update(float dt) override;

    void onStartSolver(cocos2d::CCObject* sender);
    void onStopSolver(cocos2d::CCObject* sender);
    void onReplayMacro(cocos2d::CCObject* sender);
    void onExportMacro(cocos2d::CCObject* sender);
    void onToggleShowHUD(cocos2d::CCObject* sender);

    void cleanupHeadless();

private:
    GJGameLevel* m_level = nullptr;
    PlayLayer* m_headlessPlayLayer = nullptr;
    PlayLayer* m_previousPlayLayer = nullptr;
    cocos2d::CCScene* m_headlessScene = nullptr;

    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_horizonLabel = nullptr;
    cocos2d::CCLabelBMFont* m_waveLabel = nullptr;
    cocos2d::CCLabelBMFont* m_populationLabel = nullptr;
    cocos2d::CCLabelBMFont* m_backtrackLabel = nullptr;
    cocos2d::CCLabelBMFont* m_memoryLabel = nullptr;
    cocos2d::CCLabelBMFont* m_throughputLabel = nullptr;
    cocos2d::CCLabelBMFont* m_macroStatusLabel = nullptr;

    CCMenuItemSpriteExtra* m_startButton = nullptr;
    CCMenuItemSpriteExtra* m_stopButton = nullptr;
    CCMenuItemSpriteExtra* m_replayButton = nullptr;
    CCMenuItemSpriteExtra* m_exportButton = nullptr;
    CCMenuItemToggler* m_hudToggler = nullptr;
};

} // namespace solver
