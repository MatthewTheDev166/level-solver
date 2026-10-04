#pragma once

#include "../core/Types.hpp"
#include <unordered_map>
#include <cmath>

namespace solver {

struct SpatialKey {
    int32_t x = 0;
    int32_t y = 0;
    int32_t vy = 0;
    uint8_t mode = 0;
    uint8_t gravity = 0;

    bool operator==(const SpatialKey& other) const {
        return x == other.x && y == other.y && vy == other.vy &&
               mode == other.mode && gravity == other.gravity;
    }
};

struct SpatialKeyHash {
    size_t operator()(const SpatialKey& k) const noexcept {
        size_t h = 2166136261u;
        h = (h ^ static_cast<size_t>(k.x)) * 16777619u;
        h = (h ^ static_cast<size_t>(k.y)) * 16777619u;
        h = (h ^ static_cast<size_t>(k.vy)) * 16777619u;
        h = (h ^ static_cast<size_t>(k.mode)) * 16777619u;
        h = (h ^ static_cast<size_t>(k.gravity)) * 16777619u;
        return h;
    }
};

class SpatialGrid {
public:
    static constexpr float CELL_SIZE_X = 1.5f;
    static constexpr float CELL_SIZE_Y = 1.5f;
    static constexpr float CELL_SIZE_VY = 0.5f;

    static SpatialKey computeKey(const PlayerSnapshot& snapshot) {
        SpatialKey key;
        key.x = static_cast<int32_t>(std::floor(snapshot.position.x / CELL_SIZE_X));
        key.y = static_cast<int32_t>(std::floor(snapshot.position.y / CELL_SIZE_Y));
        key.vy = static_cast<int32_t>(std::floor(snapshot.yVelocity / CELL_SIZE_VY));
        key.mode = static_cast<uint8_t>(snapshot.mode);
        key.gravity = snapshot.isUpsideDown ? 1 : 0;
        return key;
    }

    bool shouldPrune(const PlayerSnapshot& snapshot, float fScore) {
        SpatialKey key = computeKey(snapshot);
        auto it = m_visited.find(key);
        if (it != m_visited.end()) {
            if (fScore <= it->second) {
                m_prunedCount++;
                return true;
            }
            it->second = fScore;
            return false;
        }
        m_visited[key] = fScore;
        return false;
    }

    void clear() {
        m_visited.clear();
    }

    size_t size() const {
        return m_visited.size();
    }

    size_t getPrunedCount() const {
        return m_prunedCount;
    }

    void resetPrunedCount() {
        m_prunedCount = 0;
    }

private:
    std::unordered_map<SpatialKey, float, SpatialKeyHash> m_visited;
    size_t m_prunedCount = 0;
};

} // namespace solver
