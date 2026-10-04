#include "MacroManager.hpp"
#include "../core/CheatAPIIntegrator.hpp"
#include <matjson.hpp>
#include <fstream>

namespace solver {

MacroManager& MacroManager::get() {
    static MacroManager instance;
    return instance;
}

void MacroManager::clear() {
    m_actions.clear();
    m_accumulatedTime = 0.0f;
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_isReplaying = false;
    m_lastButtonState = false;
}

void MacroManager::setActions(const std::vector<TickAction>& actions) {
    m_actions = actions;
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
    std::filesystem::create_directories(dir);
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

        m_actions.clear();
        for (const auto& item : parsed.unwrap().asArray().unwrap()) {
            TickAction act;
            if (item.contains("tick")) {
                act.tick = static_cast<uint32_t>(item["tick"].asDouble().unwrapOr(0.0));
            }
            if (item.contains("pressed")) {
                act.pressed = item["pressed"].asBool().unwrapOr(false);
            }
            m_actions.push_back(act);
        }

        geode::log::info("[LevelSolver] Loaded {} inputs from {}", m_actions.size(), filePath.string());
        return true;
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception loading macro: {}", e.what());
        return false;
    }
}

bool MacroManager::hasMacro(int levelID, const std::string& levelName) const {
    return std::filesystem::exists(getMacroPath(levelID, levelName));
}

void MacroManager::startReplay(PlayLayer* playLayer) {
    if (!playLayer || m_actions.empty()) return;

    m_isReplaying = true;
    m_accumulatedTime = 0.0f;
    m_playbackTick = 0;
    m_playbackIndex = 0;
    m_lastButtonState = false;

    CheatAPIIntegrator::notifyCheatStarted();
    geode::log::info("[LevelSolver] Starting replay with {} recorded actions", m_actions.size());
}

void MacroManager::stopReplay(PlayLayer* playLayer) {
    if (m_isReplaying) {
        m_isReplaying = false;
        if (playLayer && m_lastButtonState) {
            playLayer->handleButton(false, 1, true);
            m_lastButtonState = false;
        }
        CheatAPIIntegrator::notifyCheatEnded();
        geode::log::info("[LevelSolver] Replay stopped");
    }
}

void MacroManager::updateReplay(PlayLayer* playLayer, float dt) {
    if (!m_isReplaying || !playLayer || !playLayer->m_player1) return;

    m_accumulatedTime += dt;
    m_playbackTick = static_cast<uint32_t>(std::round(m_accumulatedTime * 240.0f));

    while (m_playbackIndex < m_actions.size() && m_actions[m_playbackIndex].tick <= m_playbackTick) {
        const auto& act = m_actions[m_playbackIndex];
        if (act.pressed && !m_lastButtonState) {
            playLayer->handleButton(true, 1, true);
            playLayer->m_player1->pushButton(PlayerButton::Jump);
            m_lastButtonState = true;
        } else if (!act.pressed && m_lastButtonState) {
            playLayer->handleButton(false, 1, true);
            playLayer->m_player1->releaseButton(PlayerButton::Jump);
            m_lastButtonState = false;
        }
        m_playbackIndex++;
    }

    if (m_playbackIndex >= m_actions.size() && !m_lastButtonState) {
        // Replay finished
        geode::log::info("[LevelSolver] Macro replay completed all inputs");
    }
}

bool MacroManager::isReplaying() const {
    return m_isReplaying;
}

uint32_t MacroManager::getCurrentPlaybackTick() const {
    return m_playbackTick;
}

} // namespace solver
