#include "GDRExporter.hpp"
#include <gdr/gdr.hpp>
#include <Geode/Geode.hpp>
#include <fstream>
#include <regex>

namespace solver {

std::string GDRExporter::sanitizeFilename(const std::string& name, int levelID) {
    if (name.empty()) {
        return fmt::format("level_{}", levelID);
    }

    // Replace filesystem illegal characters with underscores
    std::string safe = name;
    for (char& c : safe) {
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
            c = '_';
        }
    }

    // Trim trailing spaces or dots (invalid in Windows filenames)
    while (!safe.empty() && (safe.back() == ' ' || safe.back() == '.')) {
        safe.pop_back();
    }

    if (safe.empty()) {
        return fmt::format("level_{}", levelID);
    }
    return safe;
}

std::filesystem::path GDRExporter::getMegaHackReplaysDir() {
    // 1. Check relative to Geometry Dash directory
    try {
        auto gdDir = geode::dirs::getGameDir();
        auto candidate = gdDir / "replays";
        if (std::filesystem::exists(candidate)) {
            return candidate;
        }
    } catch (...) {}

    // 2. Check standard Steam install path on Windows
    std::filesystem::path standardPath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Geometry Dash\\replays";
    if (std::filesystem::exists(standardPath)) {
        return standardPath;
    }

    // 3. Fallback: create in GD game directory
    try {
        auto fallback = geode::dirs::getGameDir() / "replays";
        std::filesystem::create_directories(fallback);
        return fallback;
    } catch (...) {}

    return standardPath;
}

ExportResult GDRExporter::exportReplays(
    const std::string& levelName,
    int levelID,
    const std::vector<TickAction>& actions,
    bool isPlatformer
) {
    ExportResult result;
    if (actions.empty()) {
        result.errorMessage = "No actions to export";
        return result;
    }

    std::string safeName = sanitizeFilename(levelName, levelID);
    auto replaysDir = getMegaHackReplaysDir();

    std::error_code ec;
    std::filesystem::create_directories(replaysDir, ec);

    result.gdr2Path = replaysDir / fmt::format("{}-macro.gdr2", safeName);
    result.jsonPath = replaysDir / fmt::format("{}-macro.json", safeName);

    // 1. Export Native GDReplayFormat v2 (.gdr2)
    try {
        gdr::Replay replay("LevelSolver", 1);
        replay.author = "LevelSolver";
        replay.description = "Solved by LevelSolver Autonomous AI";
        replay.gameVersion = 22074;
        replay.framerate = 240.0;
        replay.platformer = isPlatformer;
        replay.levelInfo = gdr::Level(levelName.empty() ? safeName : levelName, static_cast<uint32_t>(levelID));

        uint64_t maxTick = 0;
        for (const auto& act : actions) {
            replay.inputs.emplace_back(act.tick, 1, false, act.pressed);
            if (act.tick > maxTick) {
                maxTick = act.tick;
            }
        }
        replay.duration = static_cast<float>(maxTick) / 240.0f;
        replay.sortInputs();

        auto exportRes = replay.exportData(result.gdr2Path);
        if (exportRes.isOk()) {
            geode::log::info("[LevelSolver] Successfully exported native GDR2 macro to: {}", result.gdr2Path.string());
        } else {
            geode::log::error("[LevelSolver] Failed to export GDR2 macro: {}", exportRes.unwrapErr());
        }
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception exporting GDR2 macro: {}", e.what());
    }

    // 2. Export GDR JSON (.json) for Mega Hack Converter tool
    try {
        matjson::Value root = matjson::Value::object();
        root["author"] = "LevelSolver";
        root["description"] = "Solved by LevelSolver Autonomous AI";
        
        uint64_t maxTick = 0;
        for (const auto& act : actions) {
            if (act.tick > maxTick) maxTick = act.tick;
        }
        root["duration"] = static_cast<double>(maxTick) / 240.0;
        root["gameVersion"] = 22074;
        root["framerate"] = 240.0;
        root["seed"] = 1337;
        root["coins"] = 0;
        root["ldm"] = false;
        root["platformer"] = isPlatformer;

        matjson::Value botObj = matjson::Value::object();
        botObj["name"] = "LevelSolver";
        botObj["version"] = "1";
        root["bot"] = botObj;

        matjson::Value levelObj = matjson::Value::object();
        levelObj["id"] = levelID;
        levelObj["name"] = levelName.empty() ? safeName : levelName;
        root["level"] = levelObj;

        matjson::Value inputsArr = matjson::Value::array();
        for (const auto& act : actions) {
            matjson::Value item = matjson::Value::object();
            item["frame"] = static_cast<double>(act.tick);
            item["button"] = 1;
            item["player2"] = false;
            item["down"] = act.pressed;
            inputsArr.push(item);
        }
        root["inputs"] = inputsArr;

        std::ofstream jsonFile(result.jsonPath);
        if (jsonFile.is_open()) {
            jsonFile << root.dump(matjson::NO_INDENTATION);
            jsonFile.close();
            geode::log::info("[LevelSolver] Successfully exported GDR JSON macro to: {}", result.jsonPath.string());
        }
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception exporting GDR JSON macro: {}", e.what());
    }

    result.success = std::filesystem::exists(result.gdr2Path) || std::filesystem::exists(result.jsonPath);
    return result;
}

} // namespace solver
