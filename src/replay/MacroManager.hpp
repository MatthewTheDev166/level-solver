#pragma once

#include "../core/Types.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <vector>
#include <filesystem>

namespace solver {

class MacroManager {
public:
    static MacroManager& get();

    void clear();
    void setActions(const std::vector<TickAction>& actions);
    const std::vector<TickAction>& getActions() const;

    std::filesystem::path getMacroPath(int levelID, const std::string& levelName = "") const;
    bool saveMacro(int levelID, const std::string& levelName = "");
    bool loadMacro(int levelID, const std::string& levelName = "");
    bool hasMacro(int levelID, const std::string& levelName = "") const;

    void queueReplay(int levelID, const std::string& levelName = "");
    bool hasPendingReplay() const;

    void startReplay(PlayLayer* playLayer);
    void stopReplay(PlayLayer* playLayer);
    void updateReplay(PlayLayer* playLayer, float dt);
    bool isReplaying() const;

    uint32_t getCurrentPlaybackTick() const;

private:
    MacroManager() = default;

    std::vector<TickAction> m_actions;
    bool m_isReplaying = false;
    bool m_pendingReplay = false;
    float m_accumulatedTime = 0.0f;
    uint32_t m_playbackTick = 0;
    size_t m_playbackIndex = 0;
    bool m_lastButtonState = false;

};

} // namespace solver
