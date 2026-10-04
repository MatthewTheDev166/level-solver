#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include "../solver/AStarSolver.hpp"

namespace solver {

class TelemetryPopup : public geode::Popup {
public:
    static TelemetryPopup* create(GJGameLevel* level);
    static inline bool s_launchWithSolver = false;

    void onClose(cocos2d::CCObject* sender) override;

protected:
    bool init(float width, float height, GJGameLevel* level);
    void update(float dt) override;

    void onStartSolver(cocos2d::CCObject* sender);
    void onStopSolver(cocos2d::CCObject* sender);
    void onReplayMacro(cocos2d::CCObject* sender);

private:
    GJGameLevel* m_level = nullptr;

    cocos2d::CCLabelBMFont* m_statusLabel = nullptr;
    cocos2d::CCLabelBMFont* m_tickLabel = nullptr;
    cocos2d::CCLabelBMFont* m_horizonLabel = nullptr;
    cocos2d::CCLabelBMFont* m_openNodesLabel = nullptr;
    cocos2d::CCLabelBMFont* m_prunedStatesLabel = nullptr;
    cocos2d::CCLabelBMFont* m_memoryLabel = nullptr;
    cocos2d::CCLabelBMFont* m_throughputLabel = nullptr;
    cocos2d::CCLabelBMFont* m_macroStatusLabel = nullptr;

    CCMenuItemSpriteExtra* m_startButton = nullptr;
    CCMenuItemSpriteExtra* m_stopButton = nullptr;
    CCMenuItemSpriteExtra* m_replayButton = nullptr;
};

} // namespace solver
