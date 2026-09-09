#include "pch.h"
#include "menu_overlay_base.h"
#include "menu_overlay_dx.h"

#include <Util.h>
#include <Logger.h>
#include <Config.h>

#include <imgui/imgui_impl_dx11.h>
#include <imgui/imgui_impl_dx12.h>
#include <imgui/imgui_impl_win32.h>

#include <atomic>
#include <chrono>
#include <cstdio>

// menu
static int const NUM_BACK_BUFFERS = 8;
static int const C119_MAX_ALLOCATOR_SLOTS = 16;
static int const SRV_HEAP_SIZE = 64;
static bool _dx11Device = false;
static bool _dx12Device = false;

// for dx11
static ID3D11Device* g_pd3dDevice = nullptr;
static ID3D11DeviceContext* g_pd3dDeviceContext = nullptr;
static ID3D11RenderTargetView* g_pd3dRenderTarget = nullptr;

// for dx12
static ID3D12Device* g_pd3dDeviceParam = nullptr;
static ID3D12DescriptorHeap* g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap* g_pd3dSrvDescHeap = nullptr;
static DescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;
static ID3D12CommandQueue* g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList* g_pd3dCommandList = nullptr;
static ID3D12CommandAllocator* g_commandAllocators[C119_MAX_ALLOCATOR_SLOTS] = {};
static ID3D12Resource* g_mainRenderTargetResource[NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE g_mainRenderTargetDescriptor[NUM_BACK_BUFFERS] = {};

// current command queue for dx12 swapchain
static IUnknown* currentSCCommandQueue = nullptr;

// status
static bool _isInited = false;
static bool _d3d12Captured = false;

// for showing
static bool _showRenderImGuiDebugOnce = true;

static bool C075DiagEnabledImpl()
{
    static const bool enabled = []
    {
        if (Util::ToLower(Util::ExePath().filename().wstring()) != L"monsterhunterworld.exe")
            return false;

        char envValue[8] = {};
        const DWORD envLen = GetEnvironmentVariableA("MHWFG_DIAG_MENU_ALLOCATOR_IDENTITY", envValue,
                                                     static_cast<DWORD>(sizeof(envValue)));
        return envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
    }();
    return enabled;
}

static bool C083DiagEnabledImpl()
{
    static const bool enabled = []
    {
        if (Util::ToLower(Util::ExePath().filename().wstring()) != L"monsterhunterworld.exe")
            return false;

#ifdef MHWFG_PRODUCTION
        LOG_INFO("[MHWFG] production MHW defaults: menu_completion_guard=on allocator_slots=16 skip_reason_diag=off");
        return true;
#else
        char envValue[8] = {};
        const DWORD envLen = GetEnvironmentVariableA("MHWFG_DIAG_MENU_COMPLETION_GUARD", envValue,
                                                     static_cast<DWORD>(sizeof(envValue)));
        return envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
#endif
    }();
    return enabled;
}

static bool C114DiagEnabledImpl()
{
#ifdef MHWFG_PRODUCTION
    return false;
#else
    static const bool enabled = []
    {
        if (Util::ToLower(Util::ExePath().filename().wstring()) != L"monsterhunterworld.exe")
            return false;

        char envValue[8] = {};
        const DWORD envLen = GetEnvironmentVariableA("MHWFG_DIAG_MENU_SKIP_REASONS", envValue,
                                                     static_cast<DWORD>(sizeof(envValue)));
        return envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
    }();
    return enabled;
#endif
}

static bool C114DiagActive() { return C083DiagEnabledImpl() && C114DiagEnabledImpl(); }

static uint32_t C119EffectiveAllocatorSlots()
{
    static const uint32_t slots = []
    {
        if (!C083DiagEnabledImpl())
            return 8u;

        if (Util::ToLower(Util::ExePath().filename().wstring()) != L"monsterhunterworld.exe")
            return 8u;

#ifdef MHWFG_PRODUCTION
        LOG_INFO("[C119] effective_allocator_slots=16 production=1");
        return 16u;
#else
        char envValue[8] = {};
        const DWORD envLen =
            GetEnvironmentVariableA("MHWFG_DIAG_MENU_POOL_16", envValue, static_cast<DWORD>(sizeof(envValue)));
        const bool pool16 = envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
        const uint32_t effective = pool16 ? 16u : 8u;
        LOG_INFO("[C119] effective_allocator_slots={} experimental={}", effective, pool16 ? 1u : 0u);
        return effective;
#endif
    }();
    return slots;
}

enum class C114TerminalReason : uint8_t
{
    NotReadyOrOther,
    SubmittedSignalOk,
    NoFreeSlot,
    RenderMenuFalse,
    CleanupOrGeneration,
    GpuFrozen,
    ApiFailure,
};

struct C114PresentRecord
{
    C114TerminalReason terminal = C114TerminalReason::NotReadyOrOther;
    bool sampled = false;
};

struct C114PeriodStats
{
    uint32_t visibleTrue = 0;
    uint32_t visibleFalse = 0;
    uint32_t submittedSignalOk = 0;
    uint32_t noFreeSlot = 0;
    uint32_t renderMenuFalse = 0;
    uint32_t cleanupOrGeneration = 0;
    uint32_t notReadyOrOther = 0;
    uint32_t gpuFrozen = 0;
    uint32_t apiFailure = 0;
    bool hasNoSlotSample = false;
    bool noSlotCompletedKnown = false;
    uint64_t noSlotCompleted = 0;
    uint64_t noSlotLastSignaled = 0;
    uint64_t noSlotSlotFences[C119_MAX_ALLOCATOR_SLOTS] {};
};

struct C114SessionState
{
    bool started = false;
    bool ended = false;
    uint64_t sessionStartMonoUs = 0;
    uint64_t sessionDeadlineMonoUs = 0;
    uint64_t periodStartMonoUs = 0;
    uint32_t summaryCount = 0;
    uint64_t lastSummaryMonoUs = 0;
    C114PeriodStats period {};
};

static constexpr uint64_t C114SessionLifetimeUs = 90'000'000ULL;
static constexpr uint64_t C114SummaryIntervalUs = 1'000'000ULL;

static std::atomic<bool> g_c114WindowOpen { false };
static std::atomic<uint32_t> g_c114BusyCounter { 0 };
static C114SessionState g_c114Session {};

static uint64_t C114MonoUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

static void C114SetTerminal(C114PresentRecord* record, C114TerminalReason reason)
{
    if (record != nullptr)
        record->terminal = reason;
}

static void C114CaptureNoSlotSample(C114PeriodStats& period, uint64_t completedValue, bool completedKnown);

static void C114NoteTerminal(C114PeriodStats& period, C114TerminalReason reason)
{
    switch (reason)
    {
    case C114TerminalReason::SubmittedSignalOk:
        ++period.submittedSignalOk;
        break;
    case C114TerminalReason::NoFreeSlot:
        ++period.noFreeSlot;
        break;
    case C114TerminalReason::RenderMenuFalse:
        ++period.renderMenuFalse;
        break;
    case C114TerminalReason::CleanupOrGeneration:
        ++period.cleanupOrGeneration;
        break;
    case C114TerminalReason::GpuFrozen:
        ++period.gpuFrozen;
        break;
    case C114TerminalReason::ApiFailure:
        ++period.apiFailure;
        break;
    case C114TerminalReason::NotReadyOrOther:
    default:
        ++period.notReadyOrOther;
        break;
    }
}

static void C114CloseSession()
{
    g_c114Session.ended = true;
    g_c114WindowOpen.store(false, std::memory_order_release);
}

static bool C114SessionExpired(uint64_t nowUs)
{
    return g_c114Session.started && nowUs >= g_c114Session.sessionDeadlineMonoUs;
}

static void C114MaybeStartSession(uint64_t nowUs)
{
    if (g_c114Session.started || g_c114Session.ended)
        return;

    if (!MenuOverlayBase::IsVisible())
        return;

    g_c114Session.started = true;
    g_c114Session.sessionStartMonoUs = nowUs;
    g_c114Session.sessionDeadlineMonoUs = nowUs + C114SessionLifetimeUs;
    g_c114Session.periodStartMonoUs = nowUs;
    g_c114Session.lastSummaryMonoUs = nowUs;
    g_c114WindowOpen.store(true, std::memory_order_release);
}

static void C114OwnerPresentBegin(C114PresentRecord* record)
{
    if (g_c114Session.ended)
        return;

    const uint64_t nowUs = C114MonoUs();

    if (!g_c114Session.started)
    {
        C114MaybeStartSession(nowUs);
        if (!g_c114Session.started)
            return;
    }

    if (C114SessionExpired(nowUs))
    {
        C114CloseSession();
        return;
    }

    if (record != nullptr)
    {
        record->sampled = true;
        record->terminal = C114TerminalReason::NotReadyOrOther;
    }

    if (MenuOverlayBase::IsVisible())
        ++g_c114Session.period.visibleTrue;
    else
        ++g_c114Session.period.visibleFalse;
}

static void C114OwnerPresentEnd(C114PresentRecord* record)
{
    if (!g_c114Session.started || g_c114Session.ended)
        return;

    const uint64_t nowUs = C114MonoUs();

    if (record != nullptr && record->sampled)
        C114NoteTerminal(g_c114Session.period, record->terminal);

    const bool sessionExpired = C114SessionExpired(nowUs);
    const bool due = g_c114Session.summaryCount < 90 &&
                     nowUs - g_c114Session.lastSummaryMonoUs >= C114SummaryIntervalUs;

    if (!due)
    {
        if (sessionExpired)
            C114CloseSession();
        return;
    }

    const uint32_t busyAsync = g_c114BusyCounter.exchange(0, std::memory_order_acq_rel);
    const uint64_t windowStartUs = g_c114Session.periodStartMonoUs;
    const uint64_t windowEndUs = nowUs;
    const uint64_t sessionStartUs = g_c114Session.sessionStartMonoUs;
    const uint64_t sessionDeadlineUs = g_c114Session.sessionDeadlineMonoUs;
    const DWORD summaryThreadId = GetCurrentThreadId();
    const C114PeriodStats period = g_c114Session.period;
    const uint32_t summaryIndex = g_c114Session.summaryCount + 1;
    const uint32_t allocatorSlots = C119EffectiveAllocatorSlots();

    if (period.hasNoSlotSample)
    {
        LOG_INFO("[C114] summary={}/90 session_mono_us={}-{} window_mono_us={}-{} summary_thread_id={} "
                 "visible_true={} visible_false={} busy_async={} submitted_signal_ok={} no_free_slot={} "
                 "render_menu_false={} cleanup_or_generation={} gpu_frozen={} api_failure={} not_ready_or_other={} "
                 "no_slot_sample=cpu_observed_fence_values allocator_slots={} completed_known={} completed={} "
                 "last_signaled={} slot_fences=[{},{},{},{},{},{},{},{}] extra_slot_fences=[{},{},{},{},{},{},{},{}]",
                 summaryIndex, sessionStartUs, sessionDeadlineUs, windowStartUs, windowEndUs, summaryThreadId,
                 period.visibleTrue, period.visibleFalse, busyAsync, period.submittedSignalOk, period.noFreeSlot,
                 period.renderMenuFalse, period.cleanupOrGeneration, period.gpuFrozen, period.apiFailure,
                 period.notReadyOrOther, allocatorSlots, period.noSlotCompletedKnown ? 1u : 0u, period.noSlotCompleted,
                 period.noSlotLastSignaled, period.noSlotSlotFences[0], period.noSlotSlotFences[1],
                 period.noSlotSlotFences[2], period.noSlotSlotFences[3], period.noSlotSlotFences[4],
                 period.noSlotSlotFences[5], period.noSlotSlotFences[6], period.noSlotSlotFences[7],
                 period.noSlotSlotFences[8], period.noSlotSlotFences[9], period.noSlotSlotFences[10],
                 period.noSlotSlotFences[11], period.noSlotSlotFences[12], period.noSlotSlotFences[13],
                 period.noSlotSlotFences[14], period.noSlotSlotFences[15]);
    }
    else
    {
        LOG_INFO("[C114] summary={}/90 session_mono_us={}-{} window_mono_us={}-{} summary_thread_id={} "
                 "visible_true={} visible_false={} busy_async={} submitted_signal_ok={} no_free_slot={} "
                 "render_menu_false={} cleanup_or_generation={} gpu_frozen={} api_failure={} not_ready_or_other={} "
                 "no_slot_sample=unknown allocator_slots={}",
                 summaryIndex, sessionStartUs, sessionDeadlineUs, windowStartUs, windowEndUs, summaryThreadId,
                 period.visibleTrue, period.visibleFalse, busyAsync, period.submittedSignalOk, period.noFreeSlot,
                 period.renderMenuFalse, period.cleanupOrGeneration, period.gpuFrozen, period.apiFailure,
                 period.notReadyOrOther, allocatorSlots);
    }

    g_c114Session.period = {};
    const uint64_t afterLogUs = C114MonoUs();
    g_c114Session.periodStartMonoUs = afterLogUs;
    g_c114Session.lastSummaryMonoUs = afterLogUs;
    g_c114Session.summaryCount = summaryIndex;

    if (g_c114Session.summaryCount >= 90 || sessionExpired)
        C114CloseSession();
}

enum class C083CleanupResult : uint8_t
{
    NoCleanup,
    Deferred,
    Performed,
    Frozen,
};

enum C083CleanupMailboxBits : uint32_t
{
    C083CleanupNone = 0,
    C083CleanupClearQueueFalse = 1u << 0,
    C083CleanupClearQueueTrue = 1u << 1,
};

struct C083GenerationState
{
    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* executeQueue = nullptr;
    void* swapchain = nullptr;
    HWND hwnd = nullptr;
    ID3D12Fence* fence = nullptr;
    uint64_t nextFenceValue = 1;
    uint64_t lastSignaledValue = 0;
    uint64_t allocatorSlotFence[C119_MAX_ALLOCATOR_SLOTS] {};
    uint32_t allocatorSlotCursor = 0;
    bool gpuFrozen = false;
    bool inflightUnknown = false;
    bool ownerPendingCleanup = false;
    bool ownerPendingClearQueue = false;
};

static C083GenerationState g_c083Gen {};
static std::atomic_flag g_c083MenuOwnerFlag = ATOMIC_FLAG_INIT;

static void C114CaptureNoSlotSample(C114PeriodStats& period, uint64_t completedValue, bool completedKnown)
{
    if (period.hasNoSlotSample)
        return;

    period.hasNoSlotSample = true;
    period.noSlotCompletedKnown = completedKnown;
    period.noSlotCompleted = completedValue;
    period.noSlotLastSignaled = g_c083Gen.lastSignaledValue;
    const uint32_t slotCount = C119EffectiveAllocatorSlots();
    for (uint32_t i = 0; i < slotCount; ++i)
        period.noSlotSlotFences[i] = g_c083Gen.allocatorSlotFence[i];
}
static std::atomic<uint32_t> g_c083CleanupMailbox {};
static std::atomic<bool> g_c083ThemeMailbox {};
static std::atomic<void*> g_c083MenuCommandListMirror {};

class C083MenuOwnerGuard
{
    bool acquired_ = false;

  public:
    explicit C083MenuOwnerGuard(bool enabled)
    {
        if (enabled && !g_c083MenuOwnerFlag.test_and_set(std::memory_order_acquire))
            acquired_ = true;
    }

    ~C083MenuOwnerGuard()
    {
        if (acquired_)
            g_c083MenuOwnerFlag.clear(std::memory_order_release);
    }

    bool Owns() const { return acquired_; }
};


static void C083PublishCleanupRequest(bool clearQueue)
{
    const uint32_t bit = clearQueue ? C083CleanupClearQueueTrue : C083CleanupClearQueueFalse;
    g_c083CleanupMailbox.fetch_or(bit, std::memory_order_release);
}

static void C083MergeMailboxIntoOwnerPending(uint32_t mailboxBits)
{
    if ((mailboxBits & C083CleanupClearQueueTrue) != 0)
    {
        g_c083Gen.ownerPendingCleanup = true;
        g_c083Gen.ownerPendingClearQueue = true;
    }
    else if ((mailboxBits & C083CleanupClearQueueFalse) != 0)
    {
        g_c083Gen.ownerPendingCleanup = true;
    }
}

static bool C083QueryInflightComplete()
{
    if (g_c083Gen.gpuFrozen || g_c083Gen.inflightUnknown)
        return false;

    if (g_c083Gen.lastSignaledValue == 0 || g_c083Gen.fence == nullptr)
        return true;

    const uint64_t completedValue = g_c083Gen.fence->GetCompletedValue();
    if (completedValue == UINT64_MAX)
    {
        g_c083Gen.gpuFrozen = true;
        g_c083Gen.inflightUnknown = true;
        LOG_INFO("[C083] device_removed device={} execute_queue={} last_signaled={}", static_cast<void*>(g_c083Gen.device),
                 static_cast<void*>(g_c083Gen.executeQueue), g_c083Gen.lastSignaledValue);
        return false;
    }

    return completedValue >= g_c083Gen.lastSignaledValue;
}

static void C092ClearAllocatorSlotCredentials()
{
    for (UINT i = 0; i < C119_MAX_ALLOCATOR_SLOTS; ++i)
        g_c083Gen.allocatorSlotFence[i] = 0;
    g_c083Gen.allocatorSlotCursor = 0;
}

struct C092SlotPick
{
    bool selected = false;
    uint32_t slot = 0;
    bool completedKnown = false;
    uint64_t completedValue = 0;
    bool slotFenceTargetKnown = false;
    uint64_t slotFenceTarget = 0;
};

static bool C092TryPickAllocatorSlot(uint64_t completedValue, bool completedKnown, C092SlotPick& out)
{
    out = {};
    out.completedKnown = completedKnown;
    out.completedValue = completedValue;

    if (g_c083Gen.gpuFrozen || g_c083Gen.inflightUnknown)
        return false;

    const uint32_t slotCount = C119EffectiveAllocatorSlots();
    const uint32_t start = g_c083Gen.allocatorSlotCursor;
    for (uint32_t scan = 0; scan < slotCount; ++scan)
    {
        const uint32_t slot = (start + scan) % slotCount;
        const uint64_t slotFence = g_c083Gen.allocatorSlotFence[slot];
        const bool available = slotFence == 0 || (completedKnown && completedValue >= slotFence);
        if (available)
        {
            out.selected = true;
            out.slot = slot;
            out.slotFenceTargetKnown = true;
            out.slotFenceTarget = slotFence;
            g_c083Gen.allocatorSlotCursor = (slot + 1) % slotCount;
            return true;
        }
    }

    out.slotFenceTargetKnown = true;
    out.slotFenceTarget = g_c083Gen.allocatorSlotFence[start];
    return false;
}

static void C083ReleaseGenerationGpuTracking(bool releaseExecuteQueue)
{
    SAFE_RELEASE(g_c083Gen.fence);
    if (releaseExecuteQueue && g_c083Gen.executeQueue != nullptr)
    {
        g_c083Gen.executeQueue->Release();
        g_c083Gen.executeQueue = nullptr;
    }
    g_c083Gen.nextFenceValue = 1;
    g_c083Gen.lastSignaledValue = 0;
    g_c083Gen.inflightUnknown = false;
    C092ClearAllocatorSlotCredentials();
}

static void C083PublishMenuCommandListMirror()
{
    g_c083MenuCommandListMirror.store(static_cast<void*>(g_pd3dCommandList), std::memory_order_release);
}

static void C083ClearMenuCommandListMirror()
{
    g_c083MenuCommandListMirror.store(nullptr, std::memory_order_release);
}

static void CleanupRenderTargetDx12Impl(bool clearQueue);

static void C083OwnerReleaseResources(bool clearQueue)
{
    if (clearQueue)
        C083ClearMenuCommandListMirror();

    CleanupRenderTargetDx12Impl(clearQueue);

    if (clearQueue)
    {
        C083ReleaseGenerationGpuTracking(true);
        g_c083Gen.device = nullptr;
        g_c083Gen.swapchain = nullptr;
        g_c083Gen.hwnd = nullptr;
        g_c083Gen.ownerPendingCleanup = false;
        g_c083Gen.ownerPendingClearQueue = false;
    }
}

static C083CleanupResult C083OwnerProcessCleanupRequests()
{
    const uint32_t mailboxBits = g_c083CleanupMailbox.exchange(0, std::memory_order_acq_rel);
    C083MergeMailboxIntoOwnerPending(mailboxBits);

    if (!g_c083Gen.ownerPendingCleanup)
        return C083CleanupResult::NoCleanup;

    if (g_c083Gen.gpuFrozen)
        return C083CleanupResult::Frozen;

    if (!C083QueryInflightComplete())
    {
        return C083CleanupResult::Deferred;
    }

    const bool clearQueue = g_c083Gen.ownerPendingClearQueue;
    g_c083Gen.ownerPendingCleanup = false;
    g_c083Gen.ownerPendingClearQueue = false;
    C083OwnerReleaseResources(clearQueue);
    return C083CleanupResult::Performed;
}

static bool C083MaybeCreateGenerationFence(ID3D12Device* device, ID3D12CommandQueue* executeQueue)
{
    if (device == nullptr || executeQueue == nullptr || g_c083Gen.gpuFrozen)
        return false;

    if (g_c083Gen.fence != nullptr)
        return g_c083Gen.executeQueue == executeQueue && g_c083Gen.device == device;

    ID3D12Fence* fence = nullptr;
    const HRESULT createFenceHr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (createFenceHr != S_OK || fence == nullptr)
    {
        LOG_INFO("[C083] fence_create_failed hr=0x{:08X} device={}", static_cast<uint32_t>(createFenceHr),
                 static_cast<void*>(device));
        return false;
    }

    g_c083Gen.fence = fence;
    g_c083Gen.nextFenceValue = 1;
    g_c083Gen.lastSignaledValue = 0;
    g_c083Gen.inflightUnknown = false;
    C092ClearAllocatorSlotCredentials();

    {
        LOG_INFO("[C083] fence_created fence={} device={} execute_queue={}", static_cast<void*>(g_c083Gen.fence),
                 static_cast<void*>(g_c083Gen.device), static_cast<void*>(g_c083Gen.executeQueue));
    }

    return true;
}

static void C083FreezeGenerationUnknown(const char* reason)
{
    g_c083Gen.gpuFrozen = true;
    g_c083Gen.inflightUnknown = true;
        LOG_INFO("[C083] generation_frozen reason={}", reason);
}

static bool C083SignalAfterExecute(ID3D12CommandQueue* executedQueue, uint32_t allocatorSlot)
{
    if (g_c083Gen.gpuFrozen || g_c083Gen.fence == nullptr || g_c083Gen.executeQueue == nullptr)
        return false;

    if (executedQueue != g_c083Gen.executeQueue)
    {
        C083FreezeGenerationUnknown("execute_queue_mismatch");
        return false;
    }

    const uint64_t signalValue = g_c083Gen.nextFenceValue++;
    const HRESULT signalHr = g_c083Gen.executeQueue->Signal(g_c083Gen.fence, signalValue);
    if (signalHr != S_OK)
    {
        C083FreezeGenerationUnknown("signal_failed");
        {
            LOG_INFO("[C083] signal_failed hr=0x{:08X} fence={} execute_queue={} signal_value={}",
                     static_cast<uint32_t>(signalHr), static_cast<void*>(g_c083Gen.fence),
                     static_cast<void*>(g_c083Gen.executeQueue), signalValue);
        }
        return false;
    }

    g_c083Gen.lastSignaledValue = signalValue;
    g_c083Gen.allocatorSlotFence[allocatorSlot] = signalValue;
    return true;
}

static bool C083GenerationMatches(ID3D12Device* device, ID3D12CommandQueue* executeQueue, HWND hwnd, void* swapchain)
{
    if (g_c083Gen.executeQueue == nullptr && g_c083Gen.device == nullptr && g_c083Gen.fence == nullptr &&
        g_pd3dCommandList == nullptr)
        return true;

    return g_c083Gen.device == device && g_c083Gen.executeQueue == executeQueue && g_c083Gen.hwnd == hwnd &&
           g_c083Gen.swapchain == swapchain;
}

static bool C083ShouldEndPresentAfterCleanup(C083CleanupResult result)
{
    return result == C083CleanupResult::Performed || result == C083CleanupResult::Deferred ||
           result == C083CleanupResult::Frozen;
}

static bool C083MailboxHasRequest()
{
    return g_c083CleanupMailbox.load(std::memory_order_acquire) != C083CleanupNone;
}

static void C083AssignGenerationQueueGlobals(ID3D12CommandQueue* executeQueue, IUnknown* queueIdentity)
{
    g_pd3dCommandQueue = executeQueue;
    currentSCCommandQueue = queueIdentity;
}

static void C083BindGeneration(ID3D12Device* device, ID3D12CommandQueue* executeQueue, HWND hwnd, void* swapchain)
{
    if (g_c083Gen.executeQueue != nullptr && g_c083Gen.executeQueue != executeQueue)
        g_c083Gen.executeQueue->Release();

    if (executeQueue != nullptr)
        executeQueue->AddRef();

    g_c083Gen.device = device;
    g_c083Gen.executeQueue = executeQueue;
    g_c083Gen.swapchain = swapchain;
    g_c083Gen.hwnd = hwnd;
}

static void C083OwnerCleanupInitFailure()
{
    if (!C083QueryInflightComplete())
    {
        g_c083Gen.ownerPendingCleanup = true;
        g_c083Gen.ownerPendingClearQueue = true;
        return;
    }

    C083OwnerReleaseResources(true);
}

static bool C083CanRecordMenuGpu()
{
    if (g_c083Gen.gpuFrozen || g_c083Gen.inflightUnknown)
        return false;

    if (!C083QueryInflightComplete())
        return false;

    if (g_c083Gen.fence == nullptr || g_c083Gen.executeQueue == nullptr)
        return false;

    return true;
}

static void C083PresentReleaseLocals(ID3D12CommandQueue* cq, ID3D11Device* device, ID3D12Device* device12)
{
    if (cq != nullptr)
        cq->Release();

    if (device != nullptr)
        device->Release();

    if (device12 != nullptr)
        device12->Release();
}

static void PresentLegacy(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                          const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP);
static void PresentC083Owned(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP,
                             C114PresentRecord* c114Record);
static void RenderImGui_DX12C083(IDXGISwapChain* pSwapChainPlain, ID3D12Device* device, ID3D12CommandQueue* executeQueue,
                                 IUnknown* queueIdentity, C114PresentRecord* c114Record);

static IID streamlineRiid {};
static bool CheckForRealObject(std::string functionName, IUnknown* pObject, IUnknown** ppRealObject)
{
    if (streamlineRiid.Data1 == 0)
    {
        auto iidResult = IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &streamlineRiid);

        if (iidResult != S_OK)
            return false;
    }

    auto qResult = pObject->QueryInterface(streamlineRiid, (void**) ppRealObject);

    if (qResult == S_OK && *ppRealObject != nullptr)
    {
        LOG_INFO("{} Streamline proxy found!", functionName);
        (*ppRealObject)->Release();
        return true;
    }

    return false;
}

static int GetCorrectDXGIFormat(int eCurrentFormat)
{
    switch (eCurrentFormat)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    }

    return eCurrentFormat;
}

static void CreateRenderTargetDx12(ID3D12Device* device, IDXGISwapChain* pSwapChain)
{
    LOG_FUNC();

    DXGI_SWAP_CHAIN_DESC sd;
    HRESULT hr = pSwapChain->GetDesc(&sd);

    if (hr != S_OK)
    {
        LOG_ERROR("pSwapChain->GetDesc: {0:X}", (unsigned long) hr);
        return;
    }

    for (UINT i = 0; i < sd.BufferCount; ++i)
    {
        ID3D12Resource* pBackBuffer = nullptr;
        auto result = pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));

        if (result != S_OK)
        {
            LOG_ERROR("pSwapChain->GetBuffer: {:X}", (unsigned long) result);
            return;
        }

        if (pBackBuffer != nullptr)
        {
            D3D12_RENDER_TARGET_VIEW_DESC desc = {};
            desc.Format = static_cast<DXGI_FORMAT>(GetCorrectDXGIFormat(sd.BufferDesc.Format));
            desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;

            device->CreateRenderTargetView(pBackBuffer, &desc, g_mainRenderTargetDescriptor[i]);
            g_mainRenderTargetResource[i] = pBackBuffer;
        }
    }

    LOG_INFO("done!");
}

static void CleanupRenderTargetDx12Impl(bool clearQueue)
{
    for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
    {
        SAFE_RELEASE(g_mainRenderTargetResource[i]);
    }

    LOG_TRACE("clearQueue: {}", clearQueue);

    if (clearQueue)
    {
        if (MenuOverlayBase::IsInited() && g_pd3dDeviceParam != nullptr && g_pd3dSrvDescHeap != nullptr &&
            ImGui::GetIO().BackendRendererUserData)
        {
            // std::this_thread::sleep_for(std::chrono::milliseconds(500));
            ImGui_ImplDX12_Shutdown(false);
        }

        SAFE_RELEASE(g_pd3dRtvDescHeap);
        SAFE_RELEASE(g_pd3dSrvDescHeap);

        for (UINT i = 0; i < C119_MAX_ALLOCATOR_SLOTS; ++i)
        {
            SAFE_RELEASE(g_commandAllocators[i]);
        }

        SAFE_RELEASE(g_pd3dCommandList);

        if (g_pd3dCommandQueue != nullptr)
            g_pd3dCommandQueue = nullptr;

        g_pd3dSrvDescHeapAlloc.Destroy();

        // SAFE_RELEASE(g_pd3dDeviceParam);

        _dx12Device = false;
        _isInited = false;
    }
}

static void CleanupRenderTargetDx12(bool clearQueue)
{
    if (!_isInited || !_dx12Device || State::Instance().isShuttingDown)
        return;

    CleanupRenderTargetDx12Impl(clearQueue);
}

static void CreateRenderTargetDx11(IDXGISwapChain* pSwapChain)
{
    ID3D11Texture2D* pBackBuffer = NULL;
    pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));

    if (pBackBuffer)
    {
        DXGI_SWAP_CHAIN_DESC sd;
        pSwapChain->GetDesc(&sd);

        D3D11_RENDER_TARGET_VIEW_DESC desc = {};
        desc.Format = static_cast<DXGI_FORMAT>(GetCorrectDXGIFormat(sd.BufferDesc.Format));
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, &desc, &g_pd3dRenderTarget);
        pBackBuffer->Release();
    }
}

static void CleanupRenderTargetDx11(bool shutDown)
{
    if (!_isInited || !_dx11Device || State::Instance().isShuttingDown)
        return;

    if (!shutDown)
        LOG_FUNC();

    SAFE_RELEASE(g_pd3dRenderTarget);

    if (g_pd3dDevice != nullptr)
        g_pd3dDevice = nullptr;

    _dx11Device = false;
    _isInited = false;
}

static void RenderImGui_DX11(IDXGISwapChain* pSwapChain)
{
    bool drawMenu = false;

    do
    {
        if (!MenuOverlayBase::IsInited())
            break;

        // Draw only when menu activated
        // if (!MenuOverlayBase::IsVisible())
        //    break;

        if (!_dx11Device || g_pd3dDevice == nullptr)
            break;

        drawMenu = true;

    } while (false);

    if (!drawMenu)
    {
        MenuOverlayBase::HideMenu();
        return;
    }

    LOG_FUNC();

    ImGuiIO& io = ImGui::GetIO();
    (void) io;

    if (io.BackendRendererUserData == nullptr)
    {
        if (pSwapChain->GetDevice(IID_PPV_ARGS(&g_pd3dDevice)) == S_OK)
        {
            g_pd3dDevice->Release();
            g_pd3dDevice->GetImmediateContext(&g_pd3dDeviceContext);
            g_pd3dDeviceContext->Release();
            ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);
        }
    }

    if (_isInited)
    {
        if (!g_pd3dRenderTarget)
            CreateRenderTargetDx11(pSwapChain);

        if (ImGui::GetCurrentContext() && g_pd3dRenderTarget)
        {
            ImGui_ImplDX11_NewFrame();
            ImGui_ImplWin32_NewFrame();

            if (MenuOverlayBase::RenderMenu())
            {
                ImGui::Render();

                g_pd3dDeviceContext->OMSetRenderTargets(1, &g_pd3dRenderTarget, NULL);
                ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            }
        }
    }
}

static void RenderImGui_DX12Legacy(IDXGISwapChain* pSwapChainPlain)
{
    bool drawMenu = false;
    IDXGISwapChain3* pSwapChain = nullptr;

    do
    {
        if (pSwapChainPlain->QueryInterface(IID_PPV_ARGS(&pSwapChain)) != S_OK || pSwapChain == nullptr)
            return;

        if (!MenuOverlayBase::IsInited())
            break;

        if (!_dx12Device || currentSCCommandQueue == nullptr || g_pd3dDeviceParam == nullptr)
            break;

        drawMenu = true;

    } while (false);

    if (!drawMenu)
    {
        MenuOverlayBase::HideMenu();
        pSwapChain->Release();
        return;
    }

    LOG_FUNC();

    ID3D12Device* device = g_pd3dDeviceParam;

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    if (!io.BackendRendererUserData && currentSCCommandQueue != nullptr)
    {
        LOG_DEBUG("ImGui::GetIO().BackendRendererUserData == nullptr");

        HRESULT result;

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = NUM_BACK_BUFFERS;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 1;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dRtvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }

            SIZE_T rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

            for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
            {
                g_mainRenderTargetDescriptor[i] = rtvHandle;
                rtvHandle.ptr += rtvDescriptorSize;
            }
        }

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = SRV_HEAP_SIZE;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dSrvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }

            g_pd3dSrvDescHeapAlloc.Create(device, g_pd3dSrvDescHeap);
        }

        for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
        {
            result =
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator[{0}]: {1:X}", i, (unsigned long) result);
                MenuOverlayBase::HideMenu();
                CleanupRenderTargetDx12(true);
                pSwapChain->Release();
                return;
            }

            if (C075DiagEnabledImpl() && g_commandAllocators[i] != nullptr)
            {
                const HRESULT setNameHr =
                    g_commandAllocators[i]->SetName(std::format(L"C075.MenuOverlay.Allocator[{}]", i).c_str());
                LOG_INFO("[C075] type=allocator index={} allocator={} device={} swapchain={} queue={} setName_hr=0x{:08X}",
                         i, static_cast<void*>(g_commandAllocators[i]), static_cast<void*>(device),
                         static_cast<void*>(pSwapChain), static_cast<void*>(currentSCCommandQueue),
                         static_cast<uint32_t>(setNameHr));
            }
        }

        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0], NULL,
                                           IID_PPV_ARGS(&g_pd3dCommandList));
        if (result != S_OK)
        {
            LOG_ERROR("CreateCommandList: {0:X}", (unsigned long) result);
            MenuOverlayBase::HideMenu();
            CleanupRenderTargetDx12(true);
            pSwapChain->Release();
            return;
        }

        if (C075DiagEnabledImpl() && g_pd3dCommandList != nullptr)
        {
            const HRESULT setNameHr = g_pd3dCommandList->SetName(L"C075.MenuOverlay.CommandList");
            LOG_INFO("[C075] type=commandList commandList={} setName_hr=0x{:08X}", static_cast<void*>(g_pd3dCommandList),
                     static_cast<uint32_t>(setNameHr));
        }

        result = g_pd3dCommandList->Close();
        if (result != S_OK)
        {
            LOG_ERROR("g_pd3dCommandList->Close: {0:X}", (unsigned long) result);
            MenuOverlayBase::HideMenu();
            CleanupRenderTargetDx12(false);
            pSwapChain->Release();
            return;
        }

        DXGI_SWAP_CHAIN_DESC scDesc;
        pSwapChain->GetDesc(&scDesc);

        ImGui_ImplDX12_InitInfo initInfo {};
        initInfo.Device = device;
        initInfo.CommandQueue = (ID3D12CommandQueue*) currentSCCommandQueue;
        initInfo.NumFramesInFlight = NUM_BACK_BUFFERS;
        initInfo.RTVFormat = scDesc.BufferDesc.Format;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = g_pd3dSrvDescHeap;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
        initInfo.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };

        ImGui_ImplDX12_Init(&initInfo);

        pSwapChain->Release();
        return;
    }

    if (_isInited && currentSCCommandQueue != nullptr)
    {
        if (!g_mainRenderTargetResource[0])
        {
            CreateRenderTargetDx12(device, pSwapChain);
            pSwapChain->Release();
            return;
        }

        if (ImGui::GetCurrentContext() && g_mainRenderTargetResource[0])
        {
            _showRenderImGuiDebugOnce = true;

            ImGui_ImplDX12_NewFrame();

            if (MenuOverlayBase::RenderMenu())
            {
                ImGui::Render();

                UINT backBufferIdx = pSwapChain->GetCurrentBackBufferIndex();
                ID3D12CommandAllocator* commandAllocator = g_commandAllocators[backBufferIdx];

                auto result = commandAllocator->Reset();
                if (result != S_OK)
                {
                    LOG_ERROR("commandAllocator->Reset: {0:X}", (unsigned long) result);
                    CleanupRenderTargetDx12(false);
                    pSwapChain->Release();
                    return;
                }

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

                result = g_pd3dCommandList->Reset(commandAllocator, nullptr);
                if (result != S_OK)
                {
                    LOG_ERROR("g_pd3dCommandList->Reset: {0:X}", (unsigned long) result);
                    pSwapChain->Release();
                    return;
                }

                g_pd3dCommandList->ResourceBarrier(1, &barrier);
                g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, NULL);
                g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);

                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                g_pd3dCommandList->ResourceBarrier(1, &barrier);

                result = g_pd3dCommandList->Close();
                if (result != S_OK)
                {
                    LOG_ERROR("g_pd3dCommandList->Close: {0:X}", (unsigned long) result);
                    CleanupRenderTargetDx12(true);
                    pSwapChain->Release();
                    return;
                }

                ID3D12CommandList* ppCommandLists[] = { g_pd3dCommandList };
                ((ID3D12CommandQueue*) currentSCCommandQueue)->ExecuteCommandLists(1, ppCommandLists);
            }
        }
        else
        {
            if (_showRenderImGuiDebugOnce)
                LOG_INFO("!(ImGui::GetCurrentContext() && currentSCCommandQueue && g_mainRenderTargetResource[0])");

            MenuOverlayBase::HideMenu();
            _showRenderImGuiDebugOnce = false;
        }
    }

    pSwapChain->Release();
}

static void RenderImGui_DX12C083(IDXGISwapChain* pSwapChainPlain, ID3D12Device* device, ID3D12CommandQueue* executeQueue,
                                 IUnknown* queueIdentity, C114PresentRecord* c114Record)
{
    bool drawMenu = false;
    IDXGISwapChain3* pSwapChain = nullptr;

    do
    {
        if (pSwapChainPlain->QueryInterface(IID_PPV_ARGS(&pSwapChain)) != S_OK || pSwapChain == nullptr)
        {
            C114SetTerminal(c114Record, C114TerminalReason::NotReadyOrOther);
            return;
        }

        if (!MenuOverlayBase::IsInited())
            break;

        if (!_dx12Device || executeQueue == nullptr || device == nullptr)
            break;

        drawMenu = true;

    } while (false);

    if (!drawMenu)
    {
        MenuOverlayBase::HideMenu();
        pSwapChain->Release();
        C114SetTerminal(c114Record, C114TerminalReason::NotReadyOrOther);
        return;
    }

    LOG_FUNC();

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    if (!io.BackendRendererUserData && executeQueue != nullptr)
    {
        LOG_DEBUG("ImGui::GetIO().BackendRendererUserData == nullptr");

        if (!C083CanRecordMenuGpu() && g_c083Gen.fence != nullptr)
        {
            pSwapChain->Release();
            C114SetTerminal(c114Record, C114TerminalReason::NotReadyOrOther);
            return;
        }

        HRESULT result;

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            desc.NumDescriptors = NUM_BACK_BUFFERS;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NodeMask = 1;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dRtvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                C083OwnerCleanupInitFailure();
                C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
                pSwapChain->Release();
                return;
            }

            SIZE_T rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

            for (UINT i = 0; i < NUM_BACK_BUFFERS; ++i)
            {
                g_mainRenderTargetDescriptor[i] = rtvHandle;
                rtvHandle.ptr += rtvDescriptorSize;
            }
        }

        {
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = SRV_HEAP_SIZE;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

            {
                ScopedSkipHeapCapture skipHeapCapture {};
                result = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap));
            }

            if (result != S_OK)
            {
                LOG_ERROR("CreateDescriptorHeap(g_pd3dSrvDescHeap): {0:X}", (unsigned long) result);
                MenuOverlayBase::HideMenu();
                C083OwnerCleanupInitFailure();
                C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
                pSwapChain->Release();
                return;
            }

            g_pd3dSrvDescHeapAlloc.Create(device, g_pd3dSrvDescHeap);
        }

        const uint32_t allocatorSlots = C119EffectiveAllocatorSlots();
        for (UINT i = 0; i < allocatorSlots; ++i)
        {
            result =
                device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_commandAllocators[i]));

            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocator[{0}]: {1:X}", i, (unsigned long) result);
                MenuOverlayBase::HideMenu();
                C083OwnerCleanupInitFailure();
                C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
                pSwapChain->Release();
                return;
            }

            if (C075DiagEnabledImpl() && g_commandAllocators[i] != nullptr)
            {
                const HRESULT setNameHr =
                    g_commandAllocators[i]->SetName(std::format(L"C075.MenuOverlay.Allocator[{}]", i).c_str());
                LOG_INFO("[C075] type=allocator index={} allocator={} device={} swapchain={} queue={} setName_hr=0x{:08X}",
                         i, static_cast<void*>(g_commandAllocators[i]), static_cast<void*>(device),
                         static_cast<void*>(pSwapChain), static_cast<void*>(queueIdentity),
                         static_cast<uint32_t>(setNameHr));
            }
        }

        if (!C083MaybeCreateGenerationFence(device, executeQueue))
        {
            MenuOverlayBase::HideMenu();
            C083OwnerCleanupInitFailure();
            C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
            pSwapChain->Release();
            return;
        }

        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_commandAllocators[0], NULL,
                                           IID_PPV_ARGS(&g_pd3dCommandList));
        if (result != S_OK)
        {
            LOG_ERROR("CreateCommandList: {0:X}", (unsigned long) result);
            MenuOverlayBase::HideMenu();
            C083OwnerCleanupInitFailure();
            C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
            pSwapChain->Release();
            return;
        }

        C083PublishMenuCommandListMirror();

        if (C075DiagEnabledImpl() && g_pd3dCommandList != nullptr)
        {
            const HRESULT setNameHr = g_pd3dCommandList->SetName(L"C075.MenuOverlay.CommandList");
            LOG_INFO("[C075] type=commandList commandList={} setName_hr=0x{:08X}", static_cast<void*>(g_pd3dCommandList),
                     static_cast<uint32_t>(setNameHr));
        }

        result = g_pd3dCommandList->Close();
        if (result != S_OK)
        {
            LOG_ERROR("g_pd3dCommandList->Close: {0:X}", (unsigned long) result);
            MenuOverlayBase::HideMenu();
            C083OwnerCleanupInitFailure();
            C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
            pSwapChain->Release();
            return;
        }

        DXGI_SWAP_CHAIN_DESC scDesc;
        pSwapChain->GetDesc(&scDesc);

        ImGui_ImplDX12_InitInfo initInfo {};
        initInfo.Device = device;
        initInfo.CommandQueue = executeQueue;
        initInfo.NumFramesInFlight = allocatorSlots;
        initInfo.RTVFormat = scDesc.BufferDesc.Format;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = g_pd3dSrvDescHeap;
        initInfo.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle,
                                           D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
        initInfo.SrvDescriptorFreeFn =
            [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)
        { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };

        ImGui_ImplDX12_Init(&initInfo);

        pSwapChain->Release();
        C114SetTerminal(c114Record, C114TerminalReason::NotReadyOrOther);
        return;
    }

    if (_isInited && executeQueue != nullptr)
    {
        if (static_cast<void*>(pSwapChainPlain) != g_c083Gen.swapchain)
        {
            pSwapChain->Release();
            C114SetTerminal(c114Record, C114TerminalReason::CleanupOrGeneration);
            return;
        }

        if (!g_mainRenderTargetResource[0])
        {
            CreateRenderTargetDx12(device, pSwapChain);
            pSwapChain->Release();
            C114SetTerminal(c114Record, C114TerminalReason::NotReadyOrOther);
            return;
        }

        if (ImGui::GetCurrentContext() && g_mainRenderTargetResource[0])
        {
            _showRenderImGuiDebugOnce = true;

            if (g_c083Gen.gpuFrozen || g_c083Gen.inflightUnknown)
            {
                pSwapChain->Release();
                C114SetTerminal(c114Record, C114TerminalReason::GpuFrozen);
                return;
            }

            if (g_c083Gen.fence == nullptr || g_c083Gen.executeQueue == nullptr)
            {
                pSwapChain->Release();
                C114SetTerminal(c114Record, C114TerminalReason::NotReadyOrOther);
                return;
            }

            const UINT backBufferIdx = pSwapChain->GetCurrentBackBufferIndex();

            const uint64_t completedValue = g_c083Gen.fence->GetCompletedValue();
            if (completedValue == UINT64_MAX)
            {
                g_c083Gen.gpuFrozen = true;
                g_c083Gen.inflightUnknown = true;
                LOG_INFO("[C083] device_removed device={} execute_queue={} last_signaled={}",
                         static_cast<void*>(g_c083Gen.device), static_cast<void*>(g_c083Gen.executeQueue),
                         g_c083Gen.lastSignaledValue);
                pSwapChain->Release();
                C114SetTerminal(c114Record, C114TerminalReason::GpuFrozen);
                return;
            }

            C092SlotPick slotPick {};
            if (!C092TryPickAllocatorSlot(completedValue, true, slotPick))
            {
                if (c114Record != nullptr && c114Record->sampled && C114DiagActive() && g_c114Session.started &&
                    !g_c114Session.ended)
                    C114CaptureNoSlotSample(g_c114Session.period, completedValue, true);
                pSwapChain->Release();
                C114SetTerminal(c114Record, C114TerminalReason::NoFreeSlot);
                return;
            }

            const uint32_t allocatorSlot = slotPick.slot;

            ImGui_ImplDX12_NewFrame();

            const bool renderMenuResult = MenuOverlayBase::RenderMenu();

            if (renderMenuResult)
            {
                ImGui::Render();

                if (C083MailboxHasRequest() || g_c083Gen.ownerPendingCleanup)
                {
                    pSwapChain->Release();
                    C114SetTerminal(c114Record, C114TerminalReason::CleanupOrGeneration);
                    return;
                }

                if (g_c083Gen.gpuFrozen || g_c083Gen.inflightUnknown || executeQueue != g_c083Gen.executeQueue)
                {
                    if (executeQueue != g_c083Gen.executeQueue)
                        C083FreezeGenerationUnknown("pre_record_generation_invalid");
                    pSwapChain->Release();
                    C114SetTerminal(c114Record, C114TerminalReason::GpuFrozen);
                    return;
                }

                ID3D12CommandAllocator* commandAllocator = g_commandAllocators[allocatorSlot];

                auto result = commandAllocator->Reset();
                if (result != S_OK)
                {
                    LOG_ERROR("commandAllocator->Reset: {0:X}", (unsigned long) result);
                    g_c083Gen.ownerPendingCleanup = true;
                    pSwapChain->Release();
                    C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
                    return;
                }

                D3D12_RESOURCE_BARRIER barrier = {};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
                barrier.Transition.pResource = g_mainRenderTargetResource[backBufferIdx];
                barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

                result = g_pd3dCommandList->Reset(commandAllocator, nullptr);
                if (result != S_OK)
                {
                    LOG_ERROR("g_pd3dCommandList->Reset: {0:X}", (unsigned long) result);
                    pSwapChain->Release();
                    C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
                    return;
                }

                g_pd3dCommandList->ResourceBarrier(1, &barrier);
                g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, NULL);
                g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);

                ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);

                barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
                barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
                g_pd3dCommandList->ResourceBarrier(1, &barrier);

                result = g_pd3dCommandList->Close();
                if (result != S_OK)
                {
                    LOG_ERROR("g_pd3dCommandList->Close: {0:X}", (unsigned long) result);
                    g_c083Gen.ownerPendingCleanup = true;
                    g_c083Gen.ownerPendingClearQueue = true;
                    pSwapChain->Release();
                    C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
                    return;
                }

                ID3D12CommandList* ppCommandLists[] = { g_pd3dCommandList };
                executeQueue->ExecuteCommandLists(1, ppCommandLists);
                if (C083SignalAfterExecute(executeQueue, allocatorSlot))
                    C114SetTerminal(c114Record, C114TerminalReason::SubmittedSignalOk);
                else
                    C114SetTerminal(c114Record, C114TerminalReason::ApiFailure);
            }
            else
            {
                C114SetTerminal(c114Record, C114TerminalReason::RenderMenuFalse);
            }
        }
        else
        {
            if (_showRenderImGuiDebugOnce)
                LOG_INFO("!(ImGui::GetCurrentContext() && currentSCCommandQueue && g_mainRenderTargetResource[0])");

            MenuOverlayBase::HideMenu();
            _showRenderImGuiDebugOnce = false;
            C114SetTerminal(c114Record, C114TerminalReason::NotReadyOrOther);
        }
    }


    pSwapChain->Release();
}

ID3D12GraphicsCommandList* MenuOverlayDx::MenuCommandList()
{
    if (C083DiagEnabledImpl())
        return static_cast<ID3D12GraphicsCommandList*>(g_c083MenuCommandListMirror.load(std::memory_order_acquire));

    return g_pd3dCommandList;
}

void MenuOverlayDx::CleanupRenderTarget(bool clearQueue, HWND hWnd)
{
    LOG_FUNC();

    auto fg = State::Instance().currentFG;
    if (fg != nullptr && fg->FrameGenerationContext() != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    if (!C083DiagEnabledImpl())
    {
        if (_dx11Device)
            CleanupRenderTargetDx11(false);
        else
            CleanupRenderTargetDx12(clearQueue);
        return;
    }

    C083MenuOwnerGuard guard(true);
    if (!guard.Owns())
    {
        C083PublishCleanupRequest(clearQueue);
        return;
    }

    if (_dx11Device)
    {
        CleanupRenderTargetDx11(false);
        return;
    }

    if (clearQueue)
        g_c083Gen.ownerPendingClearQueue = true;
    g_c083Gen.ownerPendingCleanup = true;
    C083OwnerProcessCleanupRequests();
}

static void PresentLegacy(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                          const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP)
{
    LOG_DEBUG("");

    ID3D12CommandQueue* cq = nullptr;
    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;

    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        if (!_dx11Device)
            LOG_DEBUG("D3D11Device captured");

        _dx11Device = true;
    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        if (!_dx12Device)
            LOG_DEBUG("D3D12CommandQueue captured");

        if (!CheckForRealObject(__FUNCTION__, pDevice, &currentSCCommandQueue))
            currentSCCommandQueue = pDevice;

        if (((ID3D12CommandQueue*) currentSCCommandQueue)->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
        {
            if (!_dx12Device)
                LOG_DEBUG("D3D12Device captured");

            _dx12Device = true;
        }
    }

    if (MenuOverlayBase::Handle() != hWnd)
    {
        LOG_DEBUG("Handle changed {:X} -> {:X}", (size_t) MenuOverlayBase::Handle(), (size_t) hWnd);

        if (MenuOverlayBase::IsInited())
            MenuOverlayBase::Shutdown();

        MenuOverlayBase::Init(hWnd, isUWP);

        _isInited = false;
    }

    if (!_isInited)
    {
        if (_dx11Device)
        {
            CleanupRenderTargetDx11(false);

            g_pd3dDevice = device;

            CreateRenderTargetDx11(pSwapChain);
            MenuOverlayBase::Dx11Ready();
            _isInited = true;
        }
        else if (_dx12Device && (g_pd3dDeviceParam != nullptr || device12 != nullptr))
        {
            if (g_pd3dDeviceParam != nullptr && device12 == nullptr)
                device12 = g_pd3dDeviceParam;

            CleanupRenderTargetDx12(true);

            g_pd3dCommandQueue = cq;
            g_pd3dDeviceParam = device12;

            MenuOverlayBase::Dx12Ready();
            _isInited = true;
        }
    }

    {
        ScopedSkipHeapCapture skipHeapCapture {};

        if (_dx11Device)
            RenderImGui_DX11(pSwapChain);
        else if (_dx12Device)
            RenderImGui_DX12Legacy(pSwapChain);
    }

    if (cq != nullptr)
        cq->Release();

    if (device != nullptr)
        device->Release();

    if (device12 != nullptr)
        device12->Release();
}

static void PresentC083Owned(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP,
                             C114PresentRecord* c114Record)
{

    ID3D12CommandQueue* cq = nullptr;
    ID3D11Device* device = nullptr;
    ID3D12Device* device12 = nullptr;
    IUnknown* localQueueIdentity = nullptr;
    void* swapchainIdentity = static_cast<void*>(pSwapChain);

    if (pDevice->QueryInterface(IID_PPV_ARGS(&device)) == S_OK)
    {
        _dx11Device = true;
    }
    else if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) == S_OK)
    {
        IUnknown* proxyCheck = nullptr;
        if (!CheckForRealObject(__FUNCTION__, pDevice, &proxyCheck))
            localQueueIdentity = pDevice;
        else
            localQueueIdentity = currentSCCommandQueue != nullptr ? currentSCCommandQueue : pDevice;

        if (cq->GetDevice(IID_PPV_ARGS(&device12)) == S_OK)
            _dx12Device = true;
    }


    C083CleanupResult cleanupResult = C083OwnerProcessCleanupRequests();
    if (C083ShouldEndPresentAfterCleanup(cleanupResult))
    {
        C114SetTerminal(c114Record, C114TerminalReason::CleanupOrGeneration);
        C083PresentReleaseLocals(cq, device, device12);
        return;
    }

    if (_dx12Device && cq != nullptr && device12 != nullptr && g_c083Gen.executeQueue != nullptr &&
        !C083GenerationMatches(device12, cq, hWnd, swapchainIdentity))
    {
        C083PublishCleanupRequest(true);
        cleanupResult = C083OwnerProcessCleanupRequests();
        C114SetTerminal(c114Record, C114TerminalReason::CleanupOrGeneration);
        C083PresentReleaseLocals(cq, device, device12);
        return;
    }

    if (MenuOverlayBase::Handle() != hWnd)
    {
        if (_isInited || g_c083Gen.executeQueue != nullptr || g_pd3dCommandList != nullptr)
        {
            C083PublishCleanupRequest(true);
            cleanupResult = C083OwnerProcessCleanupRequests();
            C114SetTerminal(c114Record, C114TerminalReason::CleanupOrGeneration);
            C083PresentReleaseLocals(cq, device, device12);
            return;
        }

        if (MenuOverlayBase::IsInited())
            MenuOverlayBase::Shutdown();

        MenuOverlayBase::Init(hWnd, isUWP);
        _isInited = false;
    }

    if (g_c083ThemeMailbox.exchange(false, std::memory_order_acq_rel))
        MenuOverlayBase::ApplyThemeStyle();

    if (!_isInited)
    {
        if (_dx11Device)
        {
            CleanupRenderTargetDx11(false);
            g_pd3dDevice = device;
            CreateRenderTargetDx11(pSwapChain);
            MenuOverlayBase::Dx11Ready();
            _isInited = true;
        }
        else if (_dx12Device && device12 != nullptr && cq != nullptr)
        {
            C083BindGeneration(device12, cq, hWnd, swapchainIdentity);
            C083AssignGenerationQueueGlobals(cq, localQueueIdentity);
            g_pd3dDeviceParam = device12;
            MenuOverlayBase::Dx12Ready();
            _isInited = true;
        }
    }

    {
        ScopedSkipHeapCapture skipHeapCapture {};

        if (_dx11Device)
            RenderImGui_DX11(pSwapChain);
        else if (_dx12Device && device12 != nullptr && cq != nullptr)
            RenderImGui_DX12C083(pSwapChain, device12, cq, localQueueIdentity, c114Record);
    }

    C083PresentReleaseLocals(cq, device, device12);
}

void MenuOverlayDx::Present(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                            const DXGI_PRESENT_PARAMETERS* pPresentParameters, IUnknown* pDevice, HWND hWnd, bool isUWP)
{
    if (!C083DiagEnabledImpl())
    {
        if (!Config::Instance()->OverlayMenu.value_or_default())
        {
            MenuOverlayBase::Present();
            return;
        }

        PresentLegacy(pSwapChain, SyncInterval, Flags, pPresentParameters, pDevice, hWnd, isUWP);
        return;
    }

    C083MenuOwnerGuard guard(true);
    if (!guard.Owns())
    {
        if (C114DiagActive() && g_c114WindowOpen.load(std::memory_order_acquire))
            g_c114BusyCounter.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (!Config::Instance()->OverlayMenu.value_or_default())
    {
        MenuOverlayBase::Present();
        return;
    }

    C114PresentRecord c114Record {};
    if (C114DiagActive())
        C114OwnerPresentBegin(&c114Record);

    PresentC083Owned(pSwapChain, SyncInterval, Flags, pPresentParameters, pDevice, hWnd, isUWP,
                     C114DiagActive() ? &c114Record : nullptr);

    if (C114DiagActive())
        C114OwnerPresentEnd(&c114Record);
}

void MenuOverlayDx::ApplyThemeStyle()
{
    if (!C083DiagEnabledImpl())
    {
        MenuOverlayBase::ApplyThemeStyle();
        return;
    }

    C083MenuOwnerGuard guard(true);
    if (!guard.Owns())
    {
        g_c083ThemeMailbox.store(true, std::memory_order_release);
        return;
    }

    MenuOverlayBase::ApplyThemeStyle();
}
