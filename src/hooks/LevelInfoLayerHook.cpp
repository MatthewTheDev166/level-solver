#include <Geode/Geode.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include "../ui/TelemetryPopup.hpp"

using namespace geode::prelude;

class $modify(SolverLevelInfoLayer, LevelInfoLayer) {
    bool init(GJGameLevel* level, bool challenge) {
        if (!LevelInfoLayer::init(level, challenge)) {
            return false;
        }

        auto leftMenu = typeinfo_cast<CCMenu*>(this->getChildByID("left-side-menu"));
        if (!leftMenu) {
            leftMenu = CCMenu::create();
            leftMenu->setID("left-side-menu");
            leftMenu->setPosition({ 30.0f, 150.0f });
            this->addChild(leftMenu);
        }

        auto spr = ButtonSprite::create("Solve", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        if (!spr) {
            spr = ButtonSprite::create("Solve");
        }

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            this,
            menu_selector(SolverLevelInfoLayer::onOpenSolverTelemetry)
        );
        btn->setID("solver-trigger-button"_spr);

        leftMenu->addChild(btn);
        leftMenu->updateLayout();

        return true;
    }

    void onOpenSolverTelemetry(CCObject* sender) {
        if (!m_level) return;

        auto popup = solver::TelemetryPopup::create(m_level);
        if (popup) {
            popup->show();
        }
    }
};
