#include "HeadlessEngine.hpp"
#include <Geode/Geode.hpp>
#include <Geode/binding/FMODAudioEngine.hpp>

namespace solver {

HeadlessEngine& HeadlessEngine::get() {
    static HeadlessEngine instance;
    return instance;
}

void HeadlessEngine::enableHeadless() {
    m_isHeadless = true;
    m_suppressRendering = true;
    setAudioSuppressed(true);
}

void HeadlessEngine::disableHeadless() {
    m_isHeadless = false;
    m_suppressRendering = false;
    setAudioSuppressed(false);
    m_ticksPerSecond = 0.0f;
}

bool HeadlessEngine::isHeadless() const {
    return m_isHeadless;
}

bool HeadlessEngine::isRenderingSuppressed() const {
    return m_suppressRendering;
}

bool HeadlessEngine::isAudioSuppressed() const {
    return m_suppressAudio;
}

void HeadlessEngine::setRenderingSuppressed(bool suppressed) {
    m_suppressRendering = suppressed;
}

void HeadlessEngine::setAudioSuppressed(bool suppressed) {
    m_suppressAudio = suppressed;
    if (auto engine = FMODAudioEngine::sharedEngine()) {
        if (engine->m_system) {
            FMOD::ChannelGroup* masterGroup = nullptr;
            if (engine->m_system->getMasterChannelGroup(&masterGroup) == FMOD_OK && masterGroup) {
                masterGroup->setMute(suppressed);
                masterGroup->setVolume(suppressed ? 0.0f : 1.0f);
            }
        }
    }
}

void HeadlessEngine::recordStepBatch(uint32_t stepCount, double elapsedSeconds) {
    if (elapsedSeconds > 0.0001) {
        float currentRate = static_cast<float>(stepCount / elapsedSeconds);
        // Exponential moving average for smooth display
        if (m_ticksPerSecond <= 0.0f) {
            m_ticksPerSecond = currentRate;
        } else {
            m_ticksPerSecond = 0.8f * m_ticksPerSecond + 0.2f * currentRate;
        }
    }
}

float HeadlessEngine::getTicksPerSecond() const {
    return m_ticksPerSecond;
}

uint32_t HeadlessEngine::getBatchSize() const {
    return m_batchSize;
}

void HeadlessEngine::setBatchSize(uint32_t batchSize) {
    m_batchSize = batchSize;
}

} // namespace solver
