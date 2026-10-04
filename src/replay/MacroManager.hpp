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

    std::filesystem::path getMacroPath(int levelID) const;
    bool saveMacro(int levelID);
    bool loadMacro(int levelID);
    bool hasMacro(int levelID) const;

    void startReplay(PlayLayer* playLayer);
    void stopReplay(PlayLayer* playLayer);
    void updateReplay(PlayLayer* playLayer);
    bool isReplaying() const;

    uint32_t getCurrentPlaybackTick() const;

private:
    MacroManager() = default;

    std::vector<TickAction> m_actions;
    bool m_isReplaying = false;
    uint32_t m_playbackTick = 0;
    size_t m_playbackIndex = 0;
    bool m_lastButtonState = false;
};

} // namespace solver
