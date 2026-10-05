#pragma once

#include <Geode/Geode.hpp>
#include <Geode/binding/PlayerObject.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/Enums.hpp>
#include <unordered_map>
#include <vector>

namespace solver {

class HazardDetector {
public:
    static constexpr float EVALUATION_RADIUS_X = 250.0f;
    static constexpr float EVALUATION_RADIUS_Y = 150.0f;
    static constexpr float MAX_CLEARANCE = 100.0f;
    static constexpr float BUCKET_WIDTH = 200.0f;

    static bool isHazardObject(GameObject* obj);
    static bool isInteractableOrbOrPad(GameObject* obj);
    static bool isOrb(GameObject* obj);
    static bool isDashOrb(GameObject* obj);

    static void buildIndex(cocos2d::CCArray* objects);
    static void clearIndex();

    static float calculateClearance(
        const cocos2d::CCPoint& playerPos,
        cocos2d::CCArray* objects,
        size_t& nearbyObstacleCountOut
    );

    static bool isNearInteractable(
        const cocos2d::CCPoint& playerPos,
        cocos2d::CCArray* objects,
        float interactionRadius = 80.0f
    );

    static std::vector<GameObject*> getInteractablesInWindow(
        float minX,
        float maxX,
        cocos2d::CCArray* objects
    );

private:
    static inline std::unordered_map<int, std::vector<GameObject*>> s_hazardBuckets;
    static inline std::unordered_map<int, std::vector<GameObject*>> s_interactableBuckets;
    static inline bool s_hasIndex = false;
};

} // namespace solver
