#pragma once

#include <cstdint>
#include <chrono>

namespace solver {

class HeadlessEngine {
public:
    static constexpr float FIXED_DT = 1.0f / 240.0f;
    static constexpr uint32_t DEFAULT_BATCH_TICKS = 5000;

    static HeadlessEngine& get();

    void enableHeadless();
    void disableHeadless();

    bool isHeadless() const;
    bool isRenderingSuppressed() const;
    bool isAudioSuppressed() const;

    void setRenderingSuppressed(bool suppressed);
    void setAudioSuppressed(bool suppressed);

    void recordStepBatch(uint32_t stepCount, double elapsedSeconds);
    float getTicksPerSecond() const;
    uint32_t getBatchSize() const;
    void setBatchSize(uint32_t batchSize);

private:
    HeadlessEngine() = default;

    bool m_isHeadless = false;
    bool m_suppressRendering = false;
    bool m_suppressAudio = false;
    uint32_t m_batchSize = DEFAULT_BATCH_TICKS;
    float m_ticksPerSecond = 0.0f;
};

} // namespace solver
