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

std::vector<std::filesystem::path> GDRExporter::getAllReplayDirectories() {
    std::vector<std::filesystem::path> dirs;
    std::error_code ec;

    // 1. Check relative to Geometry Dash directory
    try {
        auto gdDir = geode::dirs::getGameDir() / "replays";
        std::filesystem::create_directories(gdDir, ec);
        if (std::filesystem::exists(gdDir, ec)) {
            dirs.push_back(gdDir);
        }
    } catch (...) {}

    // 2. Check standard Steam install path on Windows
    std::filesystem::path standardPath = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Geometry Dash\\replays";
    try {
        if (std::filesystem::exists("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Geometry Dash", ec)) {
            std::filesystem::create_directories(standardPath, ec);
            bool alreadyAdded = false;
            for (const auto& d : dirs) {
                if (std::filesystem::equivalent(d, standardPath, ec)) {
                    alreadyAdded = true;
                    break;
                }
            }
            if (!alreadyAdded && std::filesystem::exists(standardPath, ec)) {
                dirs.push_back(standardPath);
            }
        }
    } catch (...) {}

    if (dirs.empty()) {
        dirs.push_back(standardPath);
    }
    return dirs;
}

std::filesystem::path GDRExporter::getMegaHackReplaysDir() {
    auto dirs = getAllReplayDirectories();
    return dirs.empty() ? std::filesystem::path("C:\\Program Files (x86)\\Steam\\steamapps\\common\\Geometry Dash\\replays") : dirs.front();
}

ExportResult GDRExporter::exportReplays(
    const std::string& levelName,
    int levelID,
    const std::vector<TickAction>& actions,
    bool isPlatformer
) {
    ExportResult result;

    std::string safeName = sanitizeFilename(levelName, levelID);
    auto replaysDir = getMegaHackReplaysDir();

    std::error_code ec;
    std::filesystem::create_directories(replaysDir, ec);

    result.gdr2Path = replaysDir / fmt::format("{}-macro.gdr2", safeName);
    result.gdrPath  = replaysDir / fmt::format("{}-macro.gdr", safeName);
    result.jsonPath = replaysDir / fmt::format("{}-macro.json", safeName);

    uint32_t validLevelID = (levelID > 0) ? static_cast<uint32_t>(levelID) : 0;
    std::string effectiveLevelName = levelName.empty() ? safeName : levelName;

    // 1. Export Native GDReplayFormat (.gdr2 and .gdr)
    try {
        gdr::Replay replay("LevelSolver", 1);
        replay.author = "LevelSolver";
        replay.description = "Solved by LevelSolver Autonomous AI";
        replay.gameVersion = 22081;
        replay.framerate = 240.0;
        replay.platformer = isPlatformer;
        replay.levelInfo = gdr::Level(effectiveLevelName, validLevelID);

        uint64_t maxTick = 0;
        for (const auto& act : actions) {
            replay.inputs.emplace_back(act.tick, 1, false, act.pressed);
            if (act.tick > maxTick) {
                maxTick = act.tick;
            }
        }
        replay.duration = (maxTick > 0) ? (static_cast<float>(maxTick) / 240.0f) : 1.0f;
        replay.sortInputs();

        auto exportBytes = replay.exportData();
        if (exportBytes.isOk()) {
            const auto& bytes = exportBytes.unwrap();
            auto allDirs = getAllReplayDirectories();

            for (const auto& dir : allDirs) {
                std::error_code dirEc;
                std::filesystem::create_directories(dir, dirEc);

                // Save {safeName}-macro.gdr2
                auto pathGdr2 = dir / fmt::format("{}-macro.gdr2", safeName);
                std::ofstream fGdr2(pathGdr2, std::ios::binary);
                if (fGdr2.is_open()) {
                    fGdr2.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }

                // Save {safeName}-macro.gdr
                auto pathGdr = dir / fmt::format("{}-macro.gdr", safeName);
                std::ofstream fGdr(pathGdr, std::ios::binary);
                if (fGdr.is_open()) {
                    fGdr.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }

                // Save plain {safeName}.gdr2
                auto plainGdr2 = dir / fmt::format("{}.gdr2", safeName);
                std::ofstream fPlainGdr2(plainGdr2, std::ios::binary);
                if (fPlainGdr2.is_open()) {
                    fPlainGdr2.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }

                // Save plain {safeName}.gdr
                auto plainGdr = dir / fmt::format("{}.gdr", safeName);
                std::ofstream fPlainGdr(plainGdr, std::ios::binary);
                if (fPlainGdr.is_open()) {
                    fPlainGdr.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
                }
            }

            geode::log::info("[LevelSolver] Successfully exported native GDR2/GDR macros to: {} and {}",
                result.gdr2Path.string(), result.gdrPath.string());
        } else {
            geode::log::error("[LevelSolver] Failed to serialize GDR data: {}", exportBytes.unwrapErr());
        }
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception exporting GDR macro: {}", e.what());
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
        root["duration"] = (maxTick > 0) ? (static_cast<double>(maxTick) / 240.0) : 1.0;
        root["gameVersion"] = 22081;
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
        levelObj["id"] = validLevelID;
        levelObj["name"] = effectiveLevelName;
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

        std::string jsonDump = root.dump(matjson::NO_INDENTATION);
        auto allDirs = getAllReplayDirectories();

        for (const auto& dir : allDirs) {
            std::error_code dirEc;
            std::filesystem::create_directories(dir, dirEc);

            auto jsonFileP = dir / fmt::format("{}-macro.json", safeName);
            std::ofstream jsonFile(jsonFileP);
            if (jsonFile.is_open()) {
                jsonFile << jsonDump;
                jsonFile.close();
            }

            auto plainJson = dir / fmt::format("{}.json", safeName);
            std::ofstream plainJsonFile(plainJson);
            if (plainJsonFile.is_open()) {
                plainJsonFile << jsonDump;
                plainJsonFile.close();
            }
        }
        geode::log::info("[LevelSolver] Successfully exported GDR JSON macro to: {}", result.jsonPath.string());
    } catch (const std::exception& e) {
        geode::log::error("[LevelSolver] Exception exporting GDR JSON macro: {}", e.what());
    }

    result.success = std::filesystem::exists(result.gdr2Path) ||
                     std::filesystem::exists(result.gdrPath) ||
                     std::filesystem::exists(result.jsonPath);
    return result;
}

} // namespace solver
