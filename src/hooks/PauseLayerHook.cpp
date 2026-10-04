#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include "../ui/TelemetryPopup.hpp"

using namespace geode::prelude;

class $modify(SolverPauseLayer, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();

        auto menu = typeinfo_cast<CCMenu*>(this->getChildByID("right-button-menu"));
        if (!menu) {
            menu = typeinfo_cast<CCMenu*>(this->getChildByID("center-button-menu"));
        }
        if (!menu) return;

        auto spr = ButtonSprite::create("Solve", "goldFont.fnt", "GJ_button_01.png", 0.7f);
        if (!spr) {
            spr = ButtonSprite::create("Solve");
        }

        auto btn = CCMenuItemSpriteExtra::create(
            spr,
            this,
            menu_selector(SolverPauseLayer::onOpenSolver)
        );
        btn->setID("solver-pause-button"_spr);

        menu->addChild(btn);
        menu->updateLayout();
    }

    void onOpenSolver(CCObject* sender) {
        auto playLayer = PlayLayer::get();
        if (!playLayer || !playLayer->m_level) return;

        auto popup = solver::TelemetryPopup::create(playLayer->m_level);
        if (popup) {
            popup->show();
        }
    }
};
