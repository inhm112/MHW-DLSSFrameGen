#pragma once

#include <Logger.h>
#include <SysUtils.h>
#include <Util.h>

#include <atomic>
#include <cstdint>

enum class MhwFgFailureStage : uint8_t
{
    None = 0,
    ModuleMissing,
    FeatureCreate,
    InitFailed,
    RebuildFailed,
};

namespace MhwFgFailure
{
inline std::atomic<MhwFgFailureStage>& LatchedStageAtomic()
{
    static std::atomic<MhwFgFailureStage> stage { MhwFgFailureStage::None };
    return stage;
}

inline bool IsMhwProcess()
{
    static const bool isMhw = Util::ToLower(Util::ExePath().filename().wstring()) == L"monsterhunterworld.exe";
    return isMhw;
}

inline bool PolicyActive()
{
#ifdef MHWFG_PRODUCTION
    return IsMhwProcess();
#else
    return false;
#endif
}

inline bool IsLatched() { return LatchedStageAtomic().load(std::memory_order_acquire) != MhwFgFailureStage::None; }

inline MhwFgFailureStage Stage() { return LatchedStageAtomic().load(std::memory_order_acquire); }

inline const char* StageName(MhwFgFailureStage stage)
{
    switch (stage)
    {
    case MhwFgFailureStage::ModuleMissing:
        return "module load";
    case MhwFgFailureStage::FeatureCreate:
        return "feature create";
    case MhwFgFailureStage::InitFailed:
        return "initialization";
    case MhwFgFailureStage::RebuildFailed:
        return "rebuild";
    default:
        return "unknown";
    }
}

inline const char* UserMessage()
{
    return "DLSS creation/initialization/rebuild failed; FG is disabled. Fix configuration and restart the game. "
           "Rendering may not recover until restart.";
}

inline bool BlocksFgActivation() { return PolicyActive() && IsLatched(); }

inline void Latch(MhwFgFailureStage stage)
{
    if (!PolicyActive() || stage == MhwFgFailureStage::None)
        return;

    static std::atomic<bool> logged { false };

    auto& latchedStage = LatchedStageAtomic();
    const MhwFgFailureStage prior = latchedStage.load(std::memory_order_acquire);
    if (prior != MhwFgFailureStage::None)
        return;

    latchedStage.store(stage, std::memory_order_release);

    if (!logged.exchange(true, std::memory_order_acq_rel))
    {
        LOG_ERROR("[MHWFG] terminal DLSS failure stage={} ({})", static_cast<uint32_t>(stage), StageName(stage));
    }
}

} // namespace MhwFgFailure
