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

float HazardDetector::calculateClearance(
    const cocos2d::CCPoint& playerPos,
    cocos2d::CCArray* objects,
    size_t& nearbyObstacleCountOut
) {
    nearbyObstacleCountOut = 0;
    if (!objects) return MAX_CLEARANCE;

    float minDistanceSq = MAX_CLEARANCE * MAX_CLEARANCE;

    for (unsigned int i = 0; i < objects->count(); ++i) {
        auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!obj) continue;

        cocos2d::CCPoint objPos = obj->getPosition();
        float dx = objPos.x - playerPos.x;
        float dy = objPos.y - playerPos.y;

        if (std::abs(dx) <= EVALUATION_RADIUS_X && std::abs(dy) <= EVALUATION_RADIUS_Y) {
            nearbyObstacleCountOut++;

            if (isHazardObject(obj)) {
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
    if (!objects) return false;
    float radiusSq = interactionRadius * interactionRadius;

    for (unsigned int i = 0; i < objects->count(); ++i) {
        auto obj = static_cast<GameObject*>(objects->objectAtIndex(i));
        if (!obj) continue;

        if (isInteractableOrbOrPad(obj)) {
            cocos2d::CCPoint objPos = obj->getPosition();
            float dx = objPos.x - playerPos.x;
            float dy = objPos.y - playerPos.y;
            if (dx * dx + dy * dy <= radiusSq) {
                return true;
            }
        }
    }

    return false;
}

} // namespace solver
