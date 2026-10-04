#pragma once

#include <Geode/Geode.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <alphalaneous.alphas-ui-pack/include/API.hpp>
#include "../solver/AStarSolver.hpp"

using namespace alpha::prelude;

namespace solver {

class TelemetryPopup : public geode::Popup<GJGameLevel*> {
public:
    static TelemetryPopup* create(GJGameLevel* level);

protected:
    bool setup(GJGameLevel* level) override;
    void update(float dt) override;

    void onStartSolver(cocos2d::CCObject* sender);
    void onStopSolver(cocos2d::CCObject* sender);
    void onReplayMacro(cocos2d::CCObject* sender);

private:
    GJGameLevel* m_level = nullptr;
    AdvancedScrollLayer* m_scrollLayer = nullptr;
    TouchBlocker* m_touchBlocker = nullptr;

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
