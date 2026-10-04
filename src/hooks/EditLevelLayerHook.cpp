#include <Geode/Geode.hpp>
#include <Geode/modify/EditLevelLayer.hpp>
#include "../ui/TelemetryPopup.hpp"

using namespace geode::prelude;

class $modify(SolverEditLevelLayer, EditLevelLayer) {
    bool init(GJGameLevel* level) {
        if (!EditLevelLayer::init(level)) {
            return false;
        }

        auto spr = ButtonSprite::create("Solve", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        if (!spr) {
            spr = ButtonSprite::create("Solve");
        }

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            this,
            menu_selector(SolverEditLevelLayer::onOpenSolverTelemetry)
        );
        btn->setID("solver-trigger-button"_spr);

        // Try placing on left-side-menu or level-actions-menu if provided by node-ids
        auto leftMenu = typeinfo_cast<CCMenu*>(this->getChildByID("left-side-menu"));
        if (!leftMenu) {
            leftMenu = typeinfo_cast<CCMenu*>(this->getChildByID("level-actions-menu"));
        }
        if (!leftMenu && m_buttonMenu) {
            leftMenu = m_buttonMenu;
        }

        if (leftMenu) {
            leftMenu->addChild(btn);
            leftMenu->updateLayout();
        } else {
            // Standalone menu on the left side of EditLevelLayer
            auto solveMenu = CCMenu::create();
            solveMenu->setID("level-solver-menu"_spr);
            solveMenu->setPosition({ 40.0f, 130.0f });
            solveMenu->addChild(btn);
            this->addChild(solveMenu);
        }

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
