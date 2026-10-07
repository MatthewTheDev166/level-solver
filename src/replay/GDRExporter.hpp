#pragma once

#include "../core/Types.hpp"
#include <string>
#include <vector>
#include <filesystem>

namespace solver {

struct ExportResult {
    bool success = false;
    std::filesystem::path gdrPath;
    std::filesystem::path gdr2Path;
    std::filesystem::path jsonPath;
    std::string errorMessage;
};

class GDRExporter {
public:
    /// @brief Export replay actions to both .gdr2 and .json in the Mega Hack replays directory
    static ExportResult exportReplays(
        const std::string& levelName,
        int levelID,
        const std::vector<TickAction>& actions,
        bool isPlatformer = false
    );

    /// @brief Get the Mega Hack replays directory path
    static std::filesystem::path getMegaHackReplaysDir();

    /// @brief Get all valid replays directories across standard and game installations
    static std::vector<std::filesystem::path> getAllReplayDirectories();

    /// @brief Sanitize a level name for safe filesystem usage
    static std::string sanitizeFilename(const std::string& name, int levelID);
};

} // namespace solver
