#include "HazardDetector.hpp"
#include <cmath>
#include <algorithm>

namespace solver {

bool HazardDetector::isHazardObject(GameObject* obj) {
    if (!obj) return false;
    auto type = obj->m_objectType;
    if (type == GameObjectType::Hazard || type == GameObjectType::AnimatedHazard) {
        return true;
    }
    if (obj->m_slopeIsHazard) {
        return true;
    }
    return false;
}

bool HazardDetector::isInteractableOrbOrPad(GameObject* obj) {
    if (!obj) return false;
    auto type = obj->m_objectType;
    switch (type) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::DropRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
        case GameObjectType::SpiderOrb:
        case GameObjectType::TeleportOrb:
        case GameObjectType::YellowJumpPad:
        case GameObjectType::PinkJumpPad:
        case GameObjectType::GravityPad:
        case GameObjectType::RedJumpPad:
        case GameObjectType::SpiderPad:
            return true;
        default:
            return false;
    }
}

void HazardDetector::buildIndex(cocos2d::CCArray* objects) {
    clearIndex();
    if (!objects) return;

    for (unsigned int i = 0; i < objects->count(); ++i) {
        auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!obj) continue;

        float x = obj->getPositionX();
        int bucket = static_cast<int>(std::floor(x / BUCKET_WIDTH));

        if (isHazardObject(obj)) {
            s_hazardBuckets[bucket].push_back(obj);
        }
        if (isInteractableOrbOrPad(obj)) {
            s_interactableBuckets[bucket].push_back(obj);
        }
    }
    s_hasIndex = true;
}

void HazardDetector::clearIndex() {
    s_hazardBuckets.clear();
    s_interactableBuckets.clear();
    s_hasIndex = false;
}

float HazardDetector::calculateClearance(
    const cocos2d::CCPoint& playerPos,
    cocos2d::CCArray* objects,
    size_t& nearbyObstacleCountOut
) {
    nearbyObstacleCountOut = 0;
    float minDistanceSq = MAX_CLEARANCE * MAX_CLEARANCE;

    if (s_hasIndex) {
        int minBucket = static_cast<int>(std::floor((playerPos.x - EVALUATION_RADIUS_X) / BUCKET_WIDTH));
        int maxBucket = static_cast<int>(std::floor((playerPos.x + EVALUATION_RADIUS_X) / BUCKET_WIDTH));

        for (int b = minBucket; b <= maxBucket; ++b) {
            auto it = s_hazardBuckets.find(b);
            if (it == s_hazardBuckets.end()) continue;

            for (GameObject* obj : it->second) {
                if (!obj) continue;
                cocos2d::CCPoint objPos = obj->getPosition();
                float dx = objPos.x - playerPos.x;
                float dy = objPos.y - playerPos.y;

                if (std::abs(dx) <= EVALUATION_RADIUS_X && std::abs(dy) <= EVALUATION_RADIUS_Y) {
                    if (dx >= -25.0f) {
                        nearbyObstacleCountOut++;
                    }
                    float distSq = dx * dx + dy * dy;
                    if (distSq < minDistanceSq) {
                        minDistanceSq = distSq;
                    }
                }
            }
        }
        return std::sqrt(minDistanceSq);
    }

    if (!objects) return MAX_CLEARANCE;

    for (unsigned int i = 0; i < objects->count(); ++i) {
        auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!obj) continue;

        cocos2d::CCPoint objPos = obj->getPosition();
        float dx = objPos.x - playerPos.x;
        float dy = objPos.y - playerPos.y;

        if (std::abs(dx) <= EVALUATION_RADIUS_X && std::abs(dy) <= EVALUATION_RADIUS_Y) {
            if (isHazardObject(obj)) {
                if (dx >= -25.0f) {
                    nearbyObstacleCountOut++;
                }
                float distSq = dx * dx + dy * dy;
                if (distSq < minDistanceSq) {
                    minDistanceSq = distSq;
                }
            }
        }
    }

    return std::sqrt(minDistanceSq);
}

bool HazardDetector::isNearInteractable(
    const cocos2d::CCPoint& playerPos,
    cocos2d::CCArray* objects,
    float interactionRadius
) {
    float radiusSq = interactionRadius * interactionRadius;

    if (s_hasIndex) {
        int minBucket = static_cast<int>(std::floor((playerPos.x - interactionRadius) / BUCKET_WIDTH));
        int maxBucket = static_cast<int>(std::floor((playerPos.x + interactionRadius) / BUCKET_WIDTH));

        for (int b = minBucket; b <= maxBucket; ++b) {
            auto it = s_interactableBuckets.find(b);
            if (it == s_interactableBuckets.end()) continue;

            for (GameObject* obj : it->second) {
                if (!obj) continue;
                cocos2d::CCPoint objPos = obj->getPosition();
                float dx = objPos.x - playerPos.x;
                float dy = objPos.y - playerPos.y;
                if (dx >= -25.0f && (dx * dx + dy * dy <= radiusSq)) {
                    return true;
                }
            }
        }
        return false;
    }

    if (!objects) return false;

    for (unsigned int i = 0; i < objects->count(); ++i) {
        auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!obj) continue;

        if (isInteractableOrbOrPad(obj)) {
            cocos2d::CCPoint objPos = obj->getPosition();
            float dx = objPos.x - playerPos.x;
            float dy = objPos.y - playerPos.y;
            if (dx >= -25.0f && (dx * dx + dy * dy <= radiusSq)) {
                return true;
            }
        }
    }

    return false;
}

} // namespace solver
