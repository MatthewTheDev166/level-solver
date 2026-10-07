#include "MacroManager.hpp"
#include "GDRExporter.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include "../core/DeterministicPRNG.hpp"
#include <Geode/binding/PlayerObject.hpp>
#include <gdr/gdr.hpp>
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
    m_trajectorySamples.clear();
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_totalTicks = 0;
    m_completionTick = 0;
    m_state = ReplayState::Idle;
    m_lastButtonState = false;
    m_isDispatchingInput = false;
    m_replaySessionActive = false;
    m_hasDesync = false;
    m_desyncLogged = false;
    m_desyncTick = 0;
    m_armedLevelID = 0;
    m_armedLevelName.clear();
}

void MacroManager::setCompletionTick(uint32_t tick) {
    m_completionTick = tick;
    if (m_completionTick > m_totalTicks) {
        m_totalTicks = m_completionTick;
    }
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
    m_totalTicks = std::max(m_completionTick, m_actions.empty() ? 0u : m_actions.back().tick);
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

        matjson::Value root = matjson::Value::object();

        matjson::Value inputArray = matjson::Value::array();
        for (const auto& action : m_actions) {
            matjson::Value obj = matjson::Value::object();
            obj["tick"] = static_cast<double>(action.tick);
            obj["pressed"] = action.pressed;
            inputArray.push(obj);
        }

        matjson::Value trajArray = matjson::Value::array();
        for (const auto& sample : m_trajectorySamples) {
            matjson::Value obj = matjson::Value::object();
            obj["tick"] = static_cast<double>(sample.tick);
            obj["x"] = static_cast<double>(sample.x);
            obj["y"] = static_cast<double>(sample.y);
            trajArray.push(obj);
        }

        root["inputs"] = inputArray;
        root["trajectory"] = trajArray;
        root["completionTick"] = static_cast<double>(m_completionTick);
        root["totalTicks"] = static_cast<double>(m_totalTicks);
        root["duration"] = (m_totalTicks > 0) ? (static_cast<double>(m_totalTicks) / 240.0) : 1.0;

        std::ofstream file(filePath);
        if (!file.is_open()) {
            geode::log::error("[LevelSolver] Failed to open macro file for writing: {}", filePath.string());
            return false;
        }

        file << root.dump(matjson::NO_INDENTATION);
        file.close();
        geode::log::info("[LevelSolver] Successfully saved {} inputs (and {} trajectory points) to {}",
            m_actions.size(), m_trajectorySamples.size(), filePath.string());
        return true;
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception saving macro: {}", e.what());
        return false;
    }
}

bool MacroManager::loadMacro(int levelID, const std::string& levelName) {
    try {
        auto filePath = getMacroPath(levelID, levelName);
        std::error_code ec;

        if (!std::filesystem::exists(filePath, ec)) {
            // Fallback: search exported replay files across all replay directories
            std::string safeName = GDRExporter::sanitizeFilename(levelName, levelID);
            auto replayDirs = GDRExporter::getAllReplayDirectories();
            bool foundCandidate = false;

            for (const auto& dir : replayDirs) {
                // First try JSON macros by safeName
                for (const auto& suffix : { "-macro.json", ".json" }) {
                    auto cand = dir / (safeName + suffix);
                    if (std::filesystem::exists(cand, ec) && !ec && std::filesystem::file_size(cand, ec) > 0) {
                        filePath = cand;
                        foundCandidate = true;
                        break;
                    }
                }
                if (foundCandidate) break;

                // Try JSON macros by levelID
                if (levelID > 0) {
                    for (const auto& suffix : { "-macro.json", ".json" }) {
                        auto cand = dir / fmt::format("{}{}", levelID, suffix);
                        if (std::filesystem::exists(cand, ec) && !ec && std::filesystem::file_size(cand, ec) > 0) {
                            filePath = cand;
                            foundCandidate = true;
                            break;
                        }
                    }
                    if (foundCandidate) break;
                }

                // Next try GDR2/GDR binary macros by safeName
                for (const auto& suffix : { "-macro.gdr2", ".gdr2", "-macro.gdr", ".gdr" }) {
                    auto cand = dir / (safeName + suffix);
                    if (std::filesystem::exists(cand, ec) && !ec && std::filesystem::file_size(cand, ec) > 0) {
                        auto res = gdr::Replay<>::importData(cand);
                        if (res.isOk()) {
                            const auto& replay = res.unwrap();
                            std::vector<TickAction> loaded;
                            for (const auto& inp : replay.inputs) {
                                loaded.push_back({ static_cast<uint32_t>(inp.frame), inp.down });
                            }
                            m_completionTick = static_cast<uint32_t>(replay.duration * 240.0f);
                            setActions(loaded);
                            if (m_completionTick > m_totalTicks) {
                                m_totalTicks = m_completionTick;
                            }
                            m_trajectorySamples.clear();
                            geode::log::info("[LevelSolver] Loaded {} inputs from binary GDR file: {}",
                                m_actions.size(), cand.string());
                            return true;
                        }
                    }
                }

                // Try GDR2/GDR binary macros by levelID
                if (levelID > 0) {
                    for (const auto& suffix : { "-macro.gdr2", ".gdr2", "-macro.gdr", ".gdr" }) {
                        auto cand = dir / fmt::format("{}{}", levelID, suffix);
                        if (std::filesystem::exists(cand, ec) && !ec && std::filesystem::file_size(cand, ec) > 0) {
                            auto res = gdr::Replay<>::importData(cand);
                            if (res.isOk()) {
                                const auto& replay = res.unwrap();
                                std::vector<TickAction> loaded;
                                for (const auto& inp : replay.inputs) {
                                    loaded.push_back({ static_cast<uint32_t>(inp.frame), inp.down });
                                }
                                m_completionTick = static_cast<uint32_t>(replay.duration * 240.0f);
                                setActions(loaded);
                                if (m_completionTick > m_totalTicks) {
                                    m_totalTicks = m_completionTick;
                                }
                                m_trajectorySamples.clear();
                                geode::log::info("[LevelSolver] Loaded {} inputs from binary GDR file: {}",
                                    m_actions.size(), cand.string());
                                return true;
                            }
                        }
                    }
                }
            }

            if (!foundCandidate) {
                return false;
            }
        }

        std::ifstream file(filePath);
        if (!file.is_open()) return false;

        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        auto parsed = matjson::parse(content);
        if (!parsed.isOk()) {
            geode::log::error("[LevelSolver] Failed to parse macro JSON from {}", filePath.string());
            return false;
        }

        auto rootVal = parsed.unwrap();
        std::vector<TickAction> loaded;
        std::vector<TrajectorySample> loadedTraj;

        if (rootVal.isObject()) {
            if (rootVal.contains("completionTick")) {
                m_completionTick = static_cast<uint32_t>(rootVal["completionTick"].asDouble().unwrapOr(0.0));
            } else if (rootVal.contains("totalTicks")) {
                m_completionTick = static_cast<uint32_t>(rootVal["totalTicks"].asDouble().unwrapOr(0.0));
            } else if (rootVal.contains("duration")) {
                m_completionTick = static_cast<uint32_t>(rootVal["duration"].asDouble().unwrapOr(0.0) * 240.0);
            }
            if (rootVal.contains("inputs") && rootVal["inputs"].isArray()) {
                for (const auto& item : rootVal["inputs"].asArray().unwrap()) {
                    TickAction act;
                    if (item.contains("tick")) act.tick = static_cast<uint32_t>(item["tick"].asDouble().unwrapOr(0.0));
                    else if (item.contains("frame")) act.tick = static_cast<uint32_t>(item["frame"].asDouble().unwrapOr(0.0));
                    if (item.contains("pressed")) act.pressed = item["pressed"].asBool().unwrapOr(false);
                    else if (item.contains("down")) act.pressed = item["down"].asBool().unwrapOr(false);
                    loaded.push_back(act);
                }
            }
            if (rootVal.contains("trajectory") && rootVal["trajectory"].isArray()) {
                for (const auto& item : rootVal["trajectory"].asArray().unwrap()) {
                    TrajectorySample s;
                    if (item.contains("tick")) s.tick = static_cast<uint32_t>(item["tick"].asDouble().unwrapOr(0.0));
                    if (item.contains("x")) s.x = static_cast<float>(item["x"].asDouble().unwrapOr(0.0));
                    if (item.contains("y")) s.y = static_cast<float>(item["y"].asDouble().unwrapOr(0.0));
                    loadedTraj.push_back(s);
                }
            }
        } else if (rootVal.isArray()) {
            for (const auto& item : rootVal.asArray().unwrap()) {
                TickAction act;
                if (item.contains("tick")) act.tick = static_cast<uint32_t>(item["tick"].asDouble().unwrapOr(0.0));
                else if (item.contains("frame")) act.tick = static_cast<uint32_t>(item["frame"].asDouble().unwrapOr(0.0));
                if (item.contains("pressed")) act.pressed = item["pressed"].asBool().unwrapOr(false);
                else if (item.contains("down")) act.pressed = item["down"].asBool().unwrapOr(false);
                loaded.push_back(act);
            }
        }

        setActions(loaded);
        if (m_completionTick > m_totalTicks) {
            m_totalTicks = m_completionTick;
        }
        m_trajectorySamples = loadedTraj;
        geode::log::info("[LevelSolver] Loaded {} inputs (and {} trajectory points) from {}",
            m_actions.size(), m_trajectorySamples.size(), filePath.string());
        return true;
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception loading macro: {}", e.what());
        return false;
    }
}

bool MacroManager::hasMacro(int levelID, const std::string& levelName) const {
    auto filePath = getMacroPath(levelID, levelName);
    std::error_code ec;
    bool exists = std::filesystem::exists(filePath, ec);
    if (!ec && exists && std::filesystem::file_size(filePath, ec) > 0) {
        return true;
    }

    std::string safeName = GDRExporter::sanitizeFilename(levelName, levelID);
    auto replayDirs = GDRExporter::getAllReplayDirectories();
    for (const auto& dir : replayDirs) {
        for (const auto& suffix : { "-macro.gdr2", "-macro.json", "-macro.gdr", ".gdr2", ".json", ".gdr" }) {
            auto cand = dir / (safeName + suffix);
            if (std::filesystem::exists(cand, ec) && !ec && std::filesystem::file_size(cand, ec) > 0) {
                return true;
            }
        }
        if (levelID > 0) {
            for (const auto& suffix : { "-macro.gdr2", "-macro.json", ".json", ".gdr2" }) {
                auto candID = dir / fmt::format("{}{}", levelID, suffix);
                if (std::filesystem::exists(candID, ec) && !ec && std::filesystem::file_size(candID, ec) > 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

void MacroManager::armReplay(int levelID, const std::string& levelName) {
    if (m_armedLevelID != levelID || m_armedLevelName != levelName || m_actions.empty()) {
        if (!loadMacro(levelID, levelName)) {
            if (m_actions.empty() && !hasMacro(levelID, levelName)) {
                geode::log::warn("[LevelSolver] Cannot arm replay: macro file not found for level {} ('{}')", levelID, levelName);
                m_replaySessionActive = false;
                m_state = ReplayState::Idle;
                return;
            }
        }
    }

    if (m_actions.empty()) {
        geode::log::warn("[LevelSolver] Cannot arm replay: macro has 0 inputs for level {} ('{}')", levelID, levelName);
        m_replaySessionActive = false;
        m_state = ReplayState::Idle;
        return;
    }

    m_armedLevelID = levelID;
    m_armedLevelName = levelName;
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_lastButtonState = false;
    m_state = ReplayState::Armed;
    m_replaySessionActive = true;
    m_hasDesync = false;
    m_desyncLogged = false;
    m_desyncTick = 0;
    DeterministicPRNG::clampSeed();

    CheatAPIIntegrator::notifyCheatStarted();
    geode::log::info("[LevelSolver] Replay armed for level {} ('{}') with {} actions (total ticks: {})",
        levelID, levelName, m_actions.size(), m_totalTicks);
}

void MacroManager::onLevelReset(PlayLayer* playLayer) {
    if (!m_replaySessionActive) {
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
        if (playLayer->m_player2) {
            playLayer->m_player2->releaseButton(PlayerButton::Jump);
        }
        m_isDispatchingInput = false;
    }

    DeterministicPRNG::clampSeed();
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_lastButtonState = false;
    m_hasDesync = false;
    m_desyncLogged = false;
    m_desyncTick = 0;
    // Set directly to Playing so that subsequent attempts (respawns, restarts) start playing immediately
    m_state = ReplayState::Playing;

    CheatAPIIntegrator::notifyCheatStarted();
    geode::log::info("[LevelSolver] Replay session reset to tick 0 for new attempt ({} actions ready)", m_actions.size());
}

void MacroManager::onGameStart(PlayLayer* playLayer) {
    if (!m_replaySessionActive) return;

    DeterministicPRNG::clampSeed();
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_lastButtonState = false;
    m_hasDesync = false;
    m_desyncLogged = false;
    m_desyncTick = 0;
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
            if (playLayer->m_player1) {
                if (act.pressed) {
                    playLayer->m_player1->pushButton(PlayerButton::Jump);
                } else {
                    playLayer->m_player1->releaseButton(PlayerButton::Jump);
                }
            }
            m_isDispatchingInput = false;
            m_lastButtonState = act.pressed;
            geode::log::info("[LevelSolver] Replay dispatch tick {}: {} at X={:.1f}, Y={:.1f}",
                m_playbackTick, act.pressed ? "PRESS" : "RELEASE",
                playLayer->m_player1->getPositionX(), playLayer->m_player1->getPositionY());
        }
    }

    // Check position parity against recorded simulation trajectory
    if (!m_trajectorySamples.empty()) {
        for (const auto& sample : m_trajectorySamples) {
            if (sample.tick == m_playbackTick) {
                float dx = std::abs(playLayer->m_player1->getPositionX() - sample.x);
                float dy = std::abs(playLayer->m_player1->getPositionY() - sample.y);
                if (dx > 3.0f || dy > 3.0f) {
                    if (!m_desyncLogged) {
                        m_desyncLogged = true;
                        m_hasDesync = true;
                        m_desyncTick = m_playbackTick;
                        geode::log::warn("[LevelSolver] DESYNC at tick {}: expected ({:.1f}, {:.1f}) got ({:.1f}, {:.1f})",
                            m_playbackTick, sample.x, sample.y,
                            playLayer->m_player1->getPositionX(), playLayer->m_player1->getPositionY());
                    }
                }
                break;
            }
        }
    }

    m_playbackTick++;

    if (m_playbackIndex >= m_actions.size()) {
        if (m_lastButtonState && m_playbackTick > m_totalTicks + 60) {
            m_isDispatchingInput = true;
            playLayer->handleButton(false, 1, true);
            if (playLayer->m_player1) {
                playLayer->m_player1->releaseButton(PlayerButton::Jump);
            }
            if (playLayer->m_player2) {
                playLayer->m_player2->releaseButton(PlayerButton::Jump);
            }
            m_isDispatchingInput = false;
            m_lastButtonState = false;
        }

        if (m_playbackTick >= m_totalTicks) {
            if (m_lastButtonState) {
                m_isDispatchingInput = true;
                playLayer->handleButton(false, 1, true);
                if (playLayer->m_player1) {
                    playLayer->m_player1->releaseButton(PlayerButton::Jump);
                }
                if (playLayer->m_player2) {
                    playLayer->m_player2->releaseButton(PlayerButton::Jump);
                }
                m_isDispatchingInput = false;
                m_lastButtonState = false;
            }
            if (m_state != ReplayState::Finished) {
                m_state = ReplayState::Finished;
                geode::log::info("[LevelSolver] Macro replay completed all inputs and reached level end at tick {}", m_playbackTick);
            }
        }
    }
}

void MacroManager::stopReplay(PlayLayer* playLayer) {
    if (m_state == ReplayState::Idle && !m_replaySessionActive) return;

    if (playLayer && m_lastButtonState) {
        m_isDispatchingInput = true;
        playLayer->handleButton(false, 1, true);
        if (playLayer->m_player1) {
            playLayer->m_player1->releaseButton(PlayerButton::Jump);
        }
        if (playLayer->m_player2) {
            playLayer->m_player2->releaseButton(PlayerButton::Jump);
        }
        m_isDispatchingInput = false;
    }

    m_state = ReplayState::Idle;
    m_replaySessionActive = false;
    m_lastButtonState = false;
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_hasDesync = false;
    m_desyncLogged = false;
    m_desyncTick = 0;
    m_armedLevelID = 0;
    m_armedLevelName.clear();
    CheatAPIIntegrator::notifyCheatEnded();
    geode::log::info("[LevelSolver] Replay stopped and disarmed");
}

bool MacroManager::isArmed() const {
    return m_replaySessionActive && m_state == ReplayState::Armed;
}

bool MacroManager::isPlaying() const {
    return m_replaySessionActive && m_state == ReplayState::Playing;
}

bool MacroManager::isReplaying() const {
    return m_replaySessionActive && (m_state == ReplayState::Armed || m_state == ReplayState::Playing || m_state == ReplayState::Finished);
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
