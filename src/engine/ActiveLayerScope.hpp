#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/GameManager.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <Geode/binding/GJBaseGameLayer.hpp>

namespace solver {

/**
 * RAII scope guard to temporarily set GameManager's m_playLayer and m_gameLayer
 * while headless simulation logic is executing, preventing nullptr dereferences
 * in engine functions (such as PlayerObject::levelFlipping) and mod hooks.
 */
class ActiveLayerScope {
public:
    explicit ActiveLayerScope(PlayLayer* layer) {
        m_gm = GameManager::sharedState();
        if (m_gm) {
            m_prevPlay = m_gm->m_playLayer;
            m_prevGame = m_gm->m_gameLayer;
            m_gm->m_playLayer = layer;
            m_gm->m_gameLayer = layer;
        }
    }

    ~ActiveLayerScope() {
        if (m_gm) {
            m_gm->m_playLayer = m_prevPlay;
            m_gm->m_gameLayer = m_prevGame;
        }
    }

    ActiveLayerScope(const ActiveLayerScope&) = delete;
    ActiveLayerScope& operator=(const ActiveLayerScope&) = delete;

private:
    GameManager* m_gm = nullptr;
    PlayLayer* m_prevPlay = nullptr;
    GJBaseGameLayer* m_prevGame = nullptr;
};

} // namespace solver
