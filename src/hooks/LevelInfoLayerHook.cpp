#include <Geode/Geode.hpp>
#include <Geode/binding/LevelInfoLayer.hpp>
#include <alphalaneous.geode-utils/include/ObjectModify.hpp>
#include <alphalaneous.geode-utils/include/Utils.hpp>
#include "../ui/TelemetryPopup.hpp"

using namespace geode::prelude;

class $nodeModify(SolverLevelInfoLayer, LevelInfoLayer) {
    void modify() {
        auto levelInfo = reinterpret_cast<LevelInfoLayer*>(this);
        if (!levelInfo) return;

        // Find left-side-menu using geode node IDs or class lookup
        CCMenu* targetMenu = typeinfo_cast<CCMenu*>(levelInfo->getChildByID("left-side-menu"));
        if (!targetMenu) {
            auto menuNode = alpha::utils::cocos::getChildByClassName(levelInfo, "CCMenu", 0);
            if (menuNode.has_value()) {
                targetMenu = typeinfo_cast<CCMenu*>(menuNode.value());
            }
        }

        if (!targetMenu) {
            // Fallback: create menu if none found
            targetMenu = CCMenu::create();
            targetMenu->setPosition({ 30.0f, 150.0f });
            targetMenu->setID("left-side-menu");
            levelInfo->addChild(targetMenu);
        }

        // Create distinctive green solver trigger button
        auto spr = CCSprite::createWithSpriteFrameName("GJ_playBtn_001.png");
        if (spr) {
            spr->setScale(0.55f);
            spr->setColor({ 50, 255, 80 }); // Bright green solver tint
        }

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            levelInfo,
            menu_selector(SolverLevelInfoLayer::onOpenSolverTelemetry)
        );
        btn->setID("solver-trigger-button"_spr);

        targetMenu->addChild(btn);
        targetMenu->updateLayout();
    }

    void onOpenSolverTelemetry(CCObject* sender) {
        auto levelInfo = reinterpret_cast<LevelInfoLayer*>(this);
        if (!levelInfo || !levelInfo->m_level) return;

        auto popup = solver::TelemetryPopup::create(levelInfo->m_level);
        if (popup) {
            popup->show();
        }
    }
};
