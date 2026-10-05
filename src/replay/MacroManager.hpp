#pragma once

#include "../core/Types.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/PlayLayer.hpp>
#include <vector>
#include <filesystem>
#include <string>

namespace solver {

enum class ReplayState {
    Idle,
    Armed,
    Playing,
    Finished
};

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

    // High-level Replay Lifecycle
    void armReplay(int levelID, const std::string& levelName = "");
    void onLevelReset(PlayLayer* playLayer);
    void onGameStart(PlayLayer* playLayer);
    void stepReplay(PlayLayer* playLayer);
    void stopReplay(PlayLayer* playLayer = nullptr);

    // Queries
    bool isArmed() const;
    bool isPlaying() const;
    bool isReplaying() const;
    bool isDispatchingInput() const;
    bool isReplaySessionActive() const;
    void setReplaySessionActive(bool active);

    uint32_t getCurrentPlaybackTick() const;
    size_t getCurrentActionIndex() const;
    size_t getTotalActions() const;
    uint32_t getTotalTicks() const;

    // Compatibility helpers
    void queueReplay(int levelID, const std::string& levelName = "") { armReplay(levelID, levelName); }
    bool hasPendingReplay() const { return isArmed(); }
    void startReplay(PlayLayer* playLayer) { onGameStart(playLayer); }
    void stepReplaySubstep(PlayLayer* playLayer) { stepReplay(playLayer); }

private:
    MacroManager() = default;

    std::vector<TickAction> m_actions;
    ReplayState m_state = ReplayState::Idle;
    uint32_t m_playbackTick = 0;
    size_t m_playbackIndex = 0;
    uint32_t m_totalTicks = 0;
    bool m_lastButtonState = false;
    bool m_isDispatchingInput = false;
    bool m_replaySessionActive = false;

    int m_armedLevelID = 0;
    std::string m_armedLevelName;
};

} // namespace solver
