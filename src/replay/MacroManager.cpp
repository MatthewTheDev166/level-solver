#include "MacroManager.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include <Geode/binding/PlayerObject.hpp>
#include <matjson.hpp>
#include <fstream>
#include <algorithm>

namespace solver {

MacroManager& MacroManager::get() {
    static MacroManager instance;
    return instance;
}

void MacroManager::clear() {
    m_actions.clear();
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_totalTicks = 0;
    m_state = ReplayState::Idle;
    m_lastButtonState = false;
    m_isDispatchingInput = false;
    m_replaySessionActive = false;
    m_armedLevelID = 0;
    m_armedLevelName.clear();
}

void MacroManager::setActions(const std::vector<TickAction>& actions) {
    m_actions = actions;

    // 1. Sort strictly by tick in ascending order
    std::sort(m_actions.begin(), m_actions.end(), [](const TickAction& a, const TickAction& b) {
        if (a.tick == b.tick) {
            return !a.pressed && b.pressed;
        }
        return a.tick < b.tick;
    });

    // 2. Deduplicate consecutive identical states
    std::vector<TickAction> clean;
    bool lastState = false;
    for (const auto& act : m_actions) {
        if (clean.empty()) {
            clean.push_back(act);
            lastState = act.pressed;
        } else if (act.pressed != lastState) {
            clean.push_back(act);
            lastState = act.pressed;
        }
    }
    m_actions = clean;
    m_totalTicks = m_actions.empty() ? 0 : m_actions.back().tick;
}

const std::vector<TickAction>& MacroManager::getActions() const {
    return m_actions;
}

static std::string sanitizeFilename(const std::string& input) {
    std::string out;
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-') {
            out += c;
        } else if (c == ' ') {
            out += '_';
        }
    }
    return out.empty() ? "unnamed" : out;
}

std::filesystem::path MacroManager::getMacroPath(int levelID, const std::string& levelName) const {
    auto dir = geode::Mod::get()->getSaveDir() / "macros";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (levelID > 0) {
        return dir / fmt::format("{}.json", levelID);
    }
    std::string safeName = sanitizeFilename(levelName);
    return dir / fmt::format("local_{}.json", safeName);
}

bool MacroManager::saveMacro(int levelID, const std::string& levelName) {
    try {
        auto filePath = getMacroPath(levelID, levelName);
        std::vector<matjson::Value> rootArray;

        for (const auto& action : m_actions) {
            matjson::Value obj;
            obj["tick"] = static_cast<double>(action.tick);
            obj["pressed"] = action.pressed;
            rootArray.push_back(std::move(obj));
        }

        matjson::Value root = rootArray;

        std::ofstream file(filePath);
        if (!file.is_open()) {
            geode::log::error("[LevelSolver] Failed to open macro file for writing: {}", filePath.string());
            return false;
        }

        file << root.dump(matjson::NO_INDENTATION);
        file.close();
        geode::log::info("[LevelSolver] Successfully saved {} inputs to {}", m_actions.size(), filePath.string());
        return true;
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception saving macro: {}", e.what());
        return false;
    }
}

bool MacroManager::loadMacro(int levelID, const std::string& levelName) {
    try {
        auto filePath = getMacroPath(levelID, levelName);
        if (!std::filesystem::exists(filePath)) {
            return false;
        }

        std::ifstream file(filePath);
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        auto parsed = matjson::parse(content);
        if (!parsed.isOk() || !parsed.unwrap().isArray()) {
            geode::log::error("[LevelSolver] Failed to parse macro JSON from {}", filePath.string());
            return false;
        }

        std::vector<TickAction> loaded;
        for (const auto& item : parsed.unwrap().asArray().unwrap()) {
            TickAction act;
            if (item.contains("tick")) {
                act.tick = static_cast<uint32_t>(item["tick"].asDouble().unwrapOr(0.0));
            }
            if (item.contains("pressed")) {
                act.pressed = item["pressed"].asBool().unwrapOr(false);
            }
            loaded.push_back(act);
        }

        setActions(loaded);
        geode::log::info("[LevelSolver] Loaded {} inputs from {}", m_actions.size(), filePath.string());
        return !m_actions.empty();
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception loading macro: {}", e.what());
        return false;
    }
}

bool MacroManager::hasMacro(int levelID, const std::string& levelName) const {
    auto filePath = getMacroPath(levelID, levelName);
    std::error_code ec;
    auto size = std::filesystem::file_size(filePath, ec);
    return !ec && size > 64;
}

void MacroManager::armReplay(int levelID, const std::string& levelName) {
    if (!loadMacro(levelID, levelName) || m_actions.empty()) {
        geode::log::warn("[LevelSolver] Cannot arm replay: macro empty or failed to load");
        m_replaySessionActive = false;
        return;
    }

    m_armedLevelID = levelID;
    m_armedLevelName = levelName;
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_lastButtonState = false;
    m_state = ReplayState::Armed;
    m_replaySessionActive = true;

    CheatAPIIntegrator::notifyCheatStarted();
    geode::log::info("[LevelSolver] Replay armed for level {} ('{}') with {} actions (total ticks: {})",
        levelID, levelName, m_actions.size(), m_totalTicks);
}

void MacroManager::onLevelReset(PlayLayer* playLayer) {
    if (!m_replaySessionActive || m_actions.empty()) {
        if (playLayer && m_lastButtonState) {
            m_isDispatchingInput = true;
            playLayer->handleButton(false, 1, true);
            if (playLayer->m_player1) {
                playLayer->m_player1->releaseButton(PlayerButton::Jump);
            }
            m_isDispatchingInput = false;
        }
        m_state = ReplayState::Idle;
        m_lastButtonState = false;
        m_playbackTick = 0;
        m_playbackIndex = 0;
        return;
    }

    // Safety release: ensure no lingering jump state across attempts
    if (playLayer) {
        m_isDispatchingInput = true;
        playLayer->handleButton(false, 1, true);
        if (playLayer->m_player1) {
            playLayer->m_player1->releaseButton(PlayerButton::Jump);
        }
        m_isDispatchingInput = false;
    }

    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_lastButtonState = false;
    // Set directly to Playing so that subsequent attempts (respawns, restarts) start playing immediately
    m_state = ReplayState::Playing;

    CheatAPIIntegrator::notifyCheatStarted();
    geode::log::info("[LevelSolver] Replay session reset to tick 0 for new attempt ({} actions ready)", m_actions.size());
}

void MacroManager::onGameStart(PlayLayer* playLayer) {
    if (!m_replaySessionActive || m_actions.empty()) return;

    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_lastButtonState = false;
    m_state = ReplayState::Playing;

    CheatAPIIntegrator::notifyCheatStarted();
    geode::log::info("[LevelSolver] Replay playback started at tick 0 with {} recorded actions", m_actions.size());
}

void MacroManager::stepReplay(PlayLayer* playLayer) {
    if (m_state != ReplayState::Playing || !playLayer || !playLayer->m_player1) return;

    // Safety: freeze playback during respawn transitions or death
    if (playLayer->m_playerDied || playLayer->m_player1->m_isDead || playLayer->m_inResetDelay) {
        return;
    }

    // Dispatch all scheduled actions up to and including the current physics tick
    while (m_playbackIndex < m_actions.size() && m_actions[m_playbackIndex].tick <= m_playbackTick) {
        const auto& act = m_actions[m_playbackIndex++];
        if (act.pressed != m_lastButtonState) {
            m_isDispatchingInput = true;
            playLayer->handleButton(act.pressed, 1, true);
            // Immediately flush button queue into physics engine so input takes effect on this exact 240Hz substep
            playLayer->processQueuedButtons(0.00416667f, false);
            if (playLayer->m_player1) {
                if (act.pressed) {
                    playLayer->m_player1->pushButton(PlayerButton::Jump);
                } else {
                    playLayer->m_player1->releaseButton(PlayerButton::Jump);
                }
            }
            m_isDispatchingInput = false;
            m_lastButtonState = act.pressed;
        }
    }

    m_playbackTick++;

    if (m_playbackIndex >= m_actions.size() && !m_lastButtonState) {
        m_state = ReplayState::Finished;
        geode::log::info("[LevelSolver] Macro replay completed all inputs at tick {}", m_playbackTick);
    }
}

void MacroManager::stopReplay(PlayLayer* playLayer) {
    if (m_state == ReplayState::Idle && !m_replaySessionActive) return;

    if (playLayer && m_lastButtonState) {
        m_isDispatchingInput = true;
        playLayer->handleButton(false, 1, true);
        playLayer->processQueuedButtons(0.00416667f, false);
        if (playLayer->m_player1) {
            playLayer->m_player1->releaseButton(PlayerButton::Jump);
        }
        m_isDispatchingInput = false;
    }

    m_state = ReplayState::Idle;
    m_replaySessionActive = false;
    m_lastButtonState = false;
    m_playbackTick = 0;
    m_playbackIndex = 0;
    CheatAPIIntegrator::notifyCheatEnded();
    geode::log::info("[LevelSolver] Replay stopped and disarmed");
}

bool MacroManager::isArmed() const {
    return m_state == ReplayState::Armed;
}

bool MacroManager::isPlaying() const {
    return m_state == ReplayState::Playing;
}

bool MacroManager::isReplaying() const {
    return m_state == ReplayState::Armed || m_state == ReplayState::Playing || m_state == ReplayState::Finished;
}

bool MacroManager::isDispatchingInput() const {
    return m_isDispatchingInput;
}

bool MacroManager::isReplaySessionActive() const {
    return m_replaySessionActive;
}

void MacroManager::setReplaySessionActive(bool active) {
    m_replaySessionActive = active;
}

uint32_t MacroManager::getCurrentPlaybackTick() const {
    return m_playbackTick;
}

size_t MacroManager::getCurrentActionIndex() const {
    return m_playbackIndex;
}

size_t MacroManager::getTotalActions() const {
    return m_actions.size();
}

uint32_t MacroManager::getTotalTicks() const {
    return m_totalTicks;
}

} // namespace solver
