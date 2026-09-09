#include "pch.h"

#include "DLSSG_Dx12.h"

#include <hudfix/Hudfix_Dx12.h>
#include <menu/menu_overlay_dx.h>
#include <resource_tracking/ResTrack_dx12.h>

#include <hooks/Reflex_Hooks.h>
#include <hooks/DxgiFactory_Hooks.h>
#include <misc/MhwFgFailure.h>

#include <magic_enum.hpp>

#include <DirectXMath.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>

using namespace DirectX;

namespace
{
using DlssgDiagClock = std::chrono::steady_clock;

struct DlssgC013ReflexDiagState
{
    bool resolved = false;
    bool isMonsterHunterWorld = false;
    bool requestReflexOff = false;
    sl::ReflexMode dispatchMode { sl::ReflexMode::eLowLatency };
};

static DlssgC013ReflexDiagState& GetDlssgC013ReflexDiagState()
{
    static DlssgC013ReflexDiagState state;
    return state;
}

static bool DlssgIsMonsterHunterWorldProcess()
{
    static bool resolved = false;
    static bool isMhw = false;
    if (!resolved)
    {
        resolved = true;
        isMhw = Util::ToLower(Util::ExePath().filename().wstring()) == L"monsterhunterworld.exe";
    }
    return isMhw;
}

static bool C066DiagEnabledImpl()
{
    static const bool enabled = []
    {
        if (!DlssgIsMonsterHunterWorldProcess())
            return false;

        char envValue[8] = {};
        const DWORD envLen =
            GetEnvironmentVariableA("MHWFG_DIAG_DEVICE_INFOQUEUE", envValue, static_cast<DWORD>(sizeof(envValue)));
        return envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
    }();
    return enabled;
}

struct C069AddressModuleInfo
{
    void* address = nullptr;
    std::string modulePath {};
    bool modulePathKnown = false;
    bool pathTruncated = false;
    bool rvaKnown = false;
    uintptr_t rva = 0;
    DWORD win32Error = 0;
};

static C069AddressModuleInfo C069ResolveAddressModule(void* address)
{
    C069AddressModuleInfo info {};
    info.address = address;
    if (address == nullptr)
        return info;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(address), &module))
    {
        info.win32Error = GetLastError();
        return info;
    }

    wchar_t modulePath[32768] = {};
    const DWORD pathLen = GetModuleFileNameW(module, modulePath, static_cast<DWORD>(std::size(modulePath)));
    if (pathLen == 0)
    {
        info.win32Error = GetLastError();
        return info;
    }

    info.pathTruncated = pathLen >= std::size(modulePath) - 1;
    info.modulePath = wstring_to_string(std::wstring(modulePath, pathLen));
    info.modulePathKnown = true;
    info.rva = reinterpret_cast<uintptr_t>(address) - reinterpret_cast<uintptr_t>(module);
    info.rvaKnown = true;
    return info;
}

static const char* C069ModulePathOrUnknown(const C069AddressModuleInfo& info)
{
    return info.modulePathKnown ? info.modulePath.c_str() : "unknown";
}

static std::string C069RvaOrUnknown(const C069AddressModuleInfo& info)
{
    if (!info.rvaKnown)
        return "unknown";
    return std::format("0x{:X}", info.rva);
}

static void C069DiagLogDeviceProxyIdentity(ID3D12Device* device)
{
    void* const deviceIdentity = static_cast<void*>(device);
    void** const vtable = *reinterpret_cast<void***>(device);
    void* const vtableIdentity = static_cast<void*>(vtable);
    void* const qiFunctionIdentity = vtable != nullptr ? vtable[0] : nullptr;

    const C069AddressModuleInfo deviceModule = C069ResolveAddressModule(deviceIdentity);
    const C069AddressModuleInfo vtableModule = C069ResolveAddressModule(vtableIdentity);
    const C069AddressModuleInfo qiModule = C069ResolveAddressModule(qiFunctionIdentity);

    LOG_INFO("[C069][DLSSG] device_identity={} device_module={} device_rva={} device_module_win32={} "
             "device_path_truncated={} vtable_identity={} vtable_module={} vtable_rva={} vtable_module_win32={} "
             "vtable_path_truncated={} qi_function_identity={} qi_function_module={} qi_function_rva={} "
             "qi_function_module_win32={} qi_function_path_truncated={}",
             deviceIdentity, C069ModulePathOrUnknown(deviceModule), C069RvaOrUnknown(deviceModule), deviceModule.win32Error,
             deviceModule.pathTruncated, vtableIdentity, C069ModulePathOrUnknown(vtableModule),
             C069RvaOrUnknown(vtableModule), vtableModule.win32Error, vtableModule.pathTruncated, qiFunctionIdentity,
             C069ModulePathOrUnknown(qiModule), C069RvaOrUnknown(qiModule), qiModule.win32Error, qiModule.pathTruncated);
}

static void C066DiagProbeDeviceInfoQueue(ID3D12Device* device)
{
    if (!C066DiagEnabledImpl())
        return;

    if (device == nullptr)
    {
        LOG_INFO("[C066][DLSSG] device_infoqueue_probe skipped device=null");
        return;
    }

    void* const deviceIdentity = static_cast<void*>(device);
    C069DiagLogDeviceProxyIdentity(device);

    ID3D12InfoQueue* infoQueue = nullptr;
    const HRESULT qiResult = device->QueryInterface(IID_PPV_ARGS(&infoQueue));
    const bool interfaceAvailable = qiResult == S_OK && infoQueue != nullptr;

    if (!interfaceAvailable)
    {
        LOG_INFO("[C066][DLSSG] device_identity={} query_hr=0x{:08X} interface_available=false "
                 "note=interface_unavailable_on_supplied_device",
                 deviceIdentity, static_cast<uint32_t>(qiResult));
        if (infoQueue != nullptr)
            infoQueue->Release();
        return;
    }

    const BOOL muteDebugOutput = infoQueue->GetMuteDebugOutput();
    const UINT64 numStoredMessages = infoQueue->GetNumStoredMessages();
    const UINT64 numAllowedByFilter = infoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
    const UINT64 numDiscardedByLimit = infoQueue->GetNumMessagesDiscardedByMessageCountLimit();

    LOG_INFO("[C066][DLSSG] device_identity={} query_hr=0x{:08X} interface_available=true mute_debug_output={} "
             "num_stored_messages={} num_allowed_by_retrieval_filter={} "
             "num_discarded_by_message_count_limit={}",
             deviceIdentity, static_cast<uint32_t>(qiResult), muteDebugOutput != FALSE, numStoredMessages,
             numAllowedByFilter, numDiscardedByLimit);

    infoQueue->Release();
}

static sl::ReflexMode DlssgC013ResolveDispatchReflexMode()
{
    auto& state = GetDlssgC013ReflexDiagState();
    if (state.resolved)
        return state.dispatchMode;

    state.isMonsterHunterWorld = DlssgIsMonsterHunterWorldProcess();

    if (state.isMonsterHunterWorld)
    {
        char envValue[8] = {};
        const DWORD envLen =
            GetEnvironmentVariableA("MHWFG_DIAG_REFLEX_OFF", envValue, static_cast<DWORD>(sizeof(envValue)));
        state.requestReflexOff = envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
        state.dispatchMode =
            state.requestReflexOff ? sl::ReflexMode::eOff : sl::ReflexMode::eLowLatency;

        LOG_INFO("[C013][DLSSG] experimental A/B diagnostic only; requested Reflex dispatch mode={} "
                 "(MHWFG_DIAG_REFLEX_OFF={}; request only, not driver-confirmed)",
                 magic_enum::enum_name(state.dispatchMode), state.requestReflexOff ? "1" : "unset/other");
    }
    else
    {
        state.dispatchMode = sl::ReflexMode::eLowLatency;
    }

    state.resolved = true;
    return state.dispatchMode;
}

constexpr size_t C048_PENDING_CAPACITY = 128;
constexpr size_t C048_SAMPLE_CAPACITY = 4;
constexpr size_t C048_SNAPSHOT_STACK_DEPTH = 8;
constexpr size_t C048_INFLIGHT_CAPACITY = 64;
constexpr size_t C048_CALL_CAPACITY = 64;

struct C048PendingRecord
{
    bool occupied = false;
    uint32_t frameId = 0;
    FG_ResourceType resourceType = FG_ResourceType::Depth;
    void* resourceIdentity = nullptr;
    void* cmdListIdentity = nullptr;
    uint64_t tagTimeUs = 0;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON;
    uint32_t lifecycle = 0;
    void* creationQueueIdentity = nullptr;
};

struct C048MatchSample
{
    uint32_t frameId = 0;
    FG_ResourceType resourceType = FG_ResourceType::Depth;
    void* cmdListIdentity = nullptr;
    void* submitQueueIdentity = nullptr;
    void* creationQueueIdentity = nullptr;
    int8_t queueCompare = 0;
    D3D12_RESOURCE_STATES resourceState = D3D12_RESOURCE_STATE_COMMON;
    uint32_t lifecycle = 0;
    uint64_t tagTimeUs = 0;
    uint64_t submitEntryUs = 0;
    uint64_t submitExitUs = 0;
};

enum class C048QueueCompareResult : int8_t
{
    Unknown = 0,
    SameRaw = 1,
    DifferentRaw = 2,
};

struct C048ExecuteCandidate
{
    bool ambiguous = false;
    C048PendingRecord record {};
};

struct C048ExecuteSnapshot
{
    bool valid = false;
    void* submitQueueIdentity = nullptr;
    uint64_t ownerGeneration = 0;
    uint32_t callId = 0;
    uint16_t candidateCount = 0;
    std::array<C048ExecuteCandidate, C048_PENDING_CAPACITY> candidates {};
};

struct C048InFlightRecord
{
    bool active = false;
    void* cmdListIdentity = nullptr;
    void* submitQueueIdentity = nullptr;
    uint64_t ownerGeneration = 0;
    uint32_t callId = 0;
};

struct C048CallRecord
{
    bool active = false;
    uint32_t callId = 0;
    uint64_t ownerGeneration = 0;
    bool polluted = false;
};

struct C048TlsExecuteState
{
    std::array<C048ExecuteSnapshot, C048_SNAPSHOT_STACK_DEPTH> stack {};
    uint8_t depth = 0;
};

struct C048GlobalState
{
    std::mutex mutex;
    DLSSG_Dx12* owner = nullptr;
    uint64_t ownerGeneration = 1;
    std::array<C048PendingRecord, C048_PENDING_CAPACITY> pending {};
    std::array<C048InFlightRecord, C048_INFLIGHT_CAPACITY> inFlight {};
    std::array<C048CallRecord, C048_CALL_CAPACITY> calls {};
    uint32_t nextCallId = 0;
    std::array<C048MatchSample, C048_SAMPLE_CAPACITY> samples {};
    size_t sampleCount = 0;
    uint64_t periodMatched = 0;
    uint64_t periodDropped = 0;
    uint64_t periodAmbiguous = 0;
    uint64_t periodStale = 0;
    uint64_t periodInflightDropped = 0;
    DlssgDiagClock::time_point periodStart { DlssgDiagClock::now() };
};

static C048GlobalState g_c048;
static thread_local C048TlsExecuteState g_c048Tls;

static void C048SetOwnerLocked(DLSSG_Dx12* newOwner)
{
    if (g_c048.owner == newOwner)
        return;

    g_c048.owner = newOwner;
    ++g_c048.ownerGeneration;
    for (auto& slot : g_c048.pending)
        slot.occupied = false;
    for (auto& inflight : g_c048.inFlight)
        inflight.active = false;
    for (auto& call : g_c048.calls)
        call.active = false;
}

static uint32_t C048AllocateCallLocked(uint64_t ownerGeneration)
{
    const uint32_t callId = ++g_c048.nextCallId;
    for (auto& slot : g_c048.calls)
    {
        if (slot.active)
            continue;

        slot.active = true;
        slot.callId = callId;
        slot.ownerGeneration = ownerGeneration;
        slot.polluted = false;
        return callId;
    }

    ++g_c048.periodInflightDropped;
    return 0;
}

static void C048ReleaseCallLocked(uint32_t callId)
{
    for (auto& slot : g_c048.calls)
    {
        if (!slot.active || slot.callId != callId)
            continue;

        slot.active = false;
        slot.polluted = false;
        return;
    }
}

static bool C048IsCallPollutedLocked(uint32_t callId)
{
    for (const auto& slot : g_c048.calls)
    {
        if (slot.active && slot.callId == callId)
            return slot.polluted;
    }

    return true;
}

static void C048SetCallPollutedLocked(uint32_t callId)
{
    for (auto& slot : g_c048.calls)
    {
        if (slot.active && slot.callId == callId)
        {
            slot.polluted = true;
            return;
        }
    }
}

static void C048PolluteCallsForCmdListLocked(void* cmdListIdentity, uint64_t ownerGeneration)
{
    for (const auto& inflight : g_c048.inFlight)
    {
        if (!inflight.active || inflight.cmdListIdentity != cmdListIdentity)
            continue;
        if (inflight.ownerGeneration != ownerGeneration)
            continue;

        C048SetCallPollutedLocked(inflight.callId);
    }
}

static bool C048DiagEnabledImpl()
{
    static const bool enabled = []
    {
        if (Util::ToLower(Util::ExePath().filename().wstring()) != L"monsterhunterworld.exe")
            return false;

        char envValue[8] = {};
        const DWORD envLen =
            GetEnvironmentVariableA("MHWFG_DIAG_QUEUE_IDENTITY", envValue, static_cast<DWORD>(sizeof(envValue)));
        const bool diagEnabled = envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
        if (diagEnabled)
        {
            LOG_INFO("[C048] queue identity diagnostic enabled (experimental; "
                     "cpu_submission_not_gpu_completion,raw_identity_only)");
        }
        return diagEnabled;
    }();
    return enabled;
}

static uint64_t C048NowUs()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(DlssgDiagClock::now().time_since_epoch()).count());
}

static C048QueueCompareResult C048CompareQueues(void* submitQueue, void* creationQueue)
{
    if (submitQueue == nullptr || creationQueue == nullptr)
        return C048QueueCompareResult::Unknown;
    if (submitQueue == creationQueue)
        return C048QueueCompareResult::SameRaw;
    return C048QueueCompareResult::DifferentRaw;
}

static const char* C048QueueCompareName(C048QueueCompareResult result)
{
    switch (result)
    {
    case C048QueueCompareResult::SameRaw:
        return "sameRaw";
    case C048QueueCompareResult::DifferentRaw:
        return "differentRaw";
    default:
        return "unknown";
    }
}

static void C048CommitCandidate(const C048ExecuteCandidate& candidate, void* submitQueue, uint64_t entryUs,
                                uint64_t exitUs)
{
    if (candidate.ambiguous)
    {
        ++g_c048.periodAmbiguous;
        return;
    }

    const auto& record = candidate.record;
    C048MatchSample sample {};
    sample.frameId = record.frameId;
    sample.resourceType = record.resourceType;
    sample.cmdListIdentity = record.cmdListIdentity;
    sample.submitQueueIdentity = submitQueue;
    sample.creationQueueIdentity = record.creationQueueIdentity;
    sample.queueCompare = static_cast<int8_t>(C048CompareQueues(submitQueue, record.creationQueueIdentity));
    sample.resourceState = record.resourceState;
    sample.lifecycle = record.lifecycle;
    sample.tagTimeUs = record.tagTimeUs;
    sample.submitEntryUs = entryUs;
    sample.submitExitUs = exitUs;

    if (g_c048.sampleCount < C048_SAMPLE_CAPACITY)
        g_c048.samples[g_c048.sampleCount++] = sample;

    ++g_c048.periodMatched;
}

static bool C048HasOverlappingInFlightLocked(void* cmdListIdentity, uint64_t ownerGeneration, uint32_t callId)
{
    for (const auto& inflight : g_c048.inFlight)
    {
        if (!inflight.active || inflight.cmdListIdentity != cmdListIdentity)
            continue;

        if (inflight.ownerGeneration != ownerGeneration)
            return true;

        if (inflight.callId != callId)
            return true;
    }

    return false;
}

static bool C048RegisterInFlightLocked(void* cmdListIdentity, void* submitQueue, uint64_t ownerGeneration,
                                       uint32_t callId)
{
    for (auto& inflight : g_c048.inFlight)
    {
        if (inflight.active)
            continue;

        inflight.active = true;
        inflight.cmdListIdentity = cmdListIdentity;
        inflight.submitQueueIdentity = submitQueue;
        inflight.ownerGeneration = ownerGeneration;
        inflight.callId = callId;
        return true;
    }

    ++g_c048.periodInflightDropped;
    return false;
}

static void C048ClearInFlightForCallIdLocked(uint32_t callId)
{
    for (auto& inflight : g_c048.inFlight)
    {
        if (!inflight.active || inflight.callId != callId)
            continue;

        inflight.active = false;
        inflight.cmdListIdentity = nullptr;
        inflight.submitQueueIdentity = nullptr;
        inflight.callId = 0;
    }
}

static void C048NoteCmdListOverlapLocked(void* cmdListIdentity, uint64_t ownerGeneration, uint32_t callId)
{
    if (!C048HasOverlappingInFlightLocked(cmdListIdentity, ownerGeneration, callId))
        return;

    C048PolluteCallsForCmdListLocked(cmdListIdentity, ownerGeneration);
    C048SetCallPollutedLocked(callId);
}

static void C048PrepareExecuteLocked(UINT numCommandLists, ID3D12CommandList* const* ppCommandLists,
                                     C048ExecuteSnapshot& snapshot, void* submitQueue, uint32_t callId)
{
    snapshot.candidateCount = 0;

    for (UINT listIndex = 0; listIndex < numCommandLists; ++listIndex)
    {
        void* const cmdListIdentity = ppCommandLists[listIndex];
        std::array<size_t, static_cast<size_t>(FG_ResourceType::ResourceTypeCOUNT)> typeHits {};
        std::array<size_t, C048_PENDING_CAPACITY> slotIndices {};
        size_t slotIndexCount = 0;

        for (size_t slotIndex = 0; slotIndex < g_c048.pending.size(); ++slotIndex)
        {
            auto& record = g_c048.pending[slotIndex];
            if (!record.occupied || record.cmdListIdentity != cmdListIdentity)
                continue;

            const size_t typeIndex = static_cast<size_t>(record.resourceType);
            if (typeIndex < typeHits.size())
                ++typeHits[typeIndex];

            if (slotIndexCount < slotIndices.size())
                slotIndices[slotIndexCount++] = slotIndex;
        }

        const bool duplicateTypeAmbiguous =
            std::any_of(typeHits.begin(), typeHits.end(), [](size_t hits) { return hits > 1; });
        const bool inflightOverlap =
            C048HasOverlappingInFlightLocked(cmdListIdentity, snapshot.ownerGeneration, callId);
        if (inflightOverlap)
            C048NoteCmdListOverlapLocked(cmdListIdentity, snapshot.ownerGeneration, callId);

        const bool ambiguous = duplicateTypeAmbiguous || inflightOverlap || C048IsCallPollutedLocked(callId);

        const uint16_t baseCandidate = snapshot.candidateCount;
        for (size_t i = 0; i < slotIndexCount; ++i)
        {
            if (snapshot.candidateCount >= snapshot.candidates.size())
            {
                ++g_c048.periodInflightDropped;
                break;
            }

            auto& record = g_c048.pending[slotIndices[i]];
            auto& candidate = snapshot.candidates[snapshot.candidateCount++];
            candidate.ambiguous = ambiguous;
            candidate.record = record;
            record.occupied = false;
        }

        if (!C048RegisterInFlightLocked(cmdListIdentity, submitQueue, snapshot.ownerGeneration, callId))
        {
            C048SetCallPollutedLocked(callId);
            for (uint16_t ci = baseCandidate; ci < snapshot.candidateCount; ++ci)
                snapshot.candidates[ci].ambiguous = true;
        }
    }
}

static void C048CompleteExecuteLocked(const C048ExecuteSnapshot& snapshot, uint64_t entryUs, uint64_t exitUs)
{
    const bool callPolluted = C048IsCallPollutedLocked(snapshot.callId);

    for (uint16_t i = 0; i < snapshot.candidateCount; ++i)
    {
        C048ExecuteCandidate candidate = snapshot.candidates[i];
        if (callPolluted)
            candidate.ambiguous = true;

        C048CommitCandidate(candidate, snapshot.submitQueueIdentity, entryUs, exitUs);
    }
}

static void C048MaybeReport()
{
    uint64_t periodMatched = 0;
    uint64_t periodDropped = 0;
    uint64_t periodAmbiguous = 0;
    uint64_t periodStale = 0;
    uint64_t periodInflightDropped = 0;
    size_t pendingUnmatched = 0;
    size_t sampleCount = 0;
    std::array<C048MatchSample, C048_SAMPLE_CAPACITY> samples {};

    {
        std::lock_guard<std::mutex> lock(g_c048.mutex);
        const auto now = DlssgDiagClock::now();
        if (now - g_c048.periodStart < std::chrono::seconds(1))
            return;

        for (const auto& record : g_c048.pending)
        {
            if (record.occupied)
                ++pendingUnmatched;
        }

        periodMatched = g_c048.periodMatched;
        periodDropped = g_c048.periodDropped;
        periodAmbiguous = g_c048.periodAmbiguous;
        periodStale = g_c048.periodStale;
        periodInflightDropped = g_c048.periodInflightDropped;
        sampleCount = g_c048.sampleCount;
        samples = g_c048.samples;

        g_c048.periodMatched = 0;
        g_c048.periodDropped = 0;
        g_c048.periodAmbiguous = 0;
        g_c048.periodStale = 0;
        g_c048.periodInflightDropped = 0;
        g_c048.sampleCount = 0;
        g_c048.periodStart = now;
    }

    LOG_INFO("[C048] matched={} ambiguous={} stale={} dropped={} inflightDropped={} pendingUnmatched={} "
             "note=cpu_submission_not_gpu_completion,raw_identity_only,matched_is_submit_assoc",
             periodMatched, periodAmbiguous, periodStale, periodDropped, periodInflightDropped, pendingUnmatched);

    for (size_t i = 0; i < sampleCount; ++i)
    {
        const auto& sample = samples[i];
        LOG_INFO("[C048][sample] frameId={} type={} cmdList={} submitQ={} creationQ={} compare={} state={} "
                 "lifecycle={} tagUs={} submitEntryUs={} submitExitUs={}",
                 sample.frameId, magic_enum::enum_name(sample.resourceType), sample.cmdListIdentity,
                 sample.submitQueueIdentity, sample.creationQueueIdentity,
                 C048QueueCompareName(static_cast<C048QueueCompareResult>(sample.queueCompare)),
                 static_cast<uint32_t>(sample.resourceState), sample.lifecycle, sample.tagTimeUs,
                 sample.submitEntryUs, sample.submitExitUs);
    }
}
} // namespace

feature_version DLSSG_Dx12::Version()
{
    if (StreamlineProxy::LoadStreamline())
    {
        auto ver = StreamlineProxy::Version();
        return ver;
    }

    return { 0, 0, 0 };
}

HWND DLSSG_Dx12::Hwnd() { return _hwnd; }

bool DLSSG_Dx12::DispatchDlssgOptionsMatchSubmitted(const sl::DLSSGOptions& options) const
{
    if (!_dispatchDlssgOptionsCache.valid)
        return false;

    return options.mode == _dispatchDlssgOptionsCache.mode &&
           options.numFramesToGenerate == _dispatchDlssgOptionsCache.numFramesToGenerate &&
           options.queueParallelismMode == _dispatchDlssgOptionsCache.queueParallelismMode &&
           options.dynamicTargetFrameRate == _dispatchDlssgOptionsCache.dynamicTargetFrameRate;
}

void DLSSG_Dx12::StoreDispatchDlssgOptionsCache(const sl::DLSSGOptions& options)
{
    _dispatchDlssgOptionsCache.valid = true;
    _dispatchDlssgOptionsCache.mode = options.mode;
    _dispatchDlssgOptionsCache.numFramesToGenerate = options.numFramesToGenerate;
    _dispatchDlssgOptionsCache.queueParallelismMode = options.queueParallelismMode;
    _dispatchDlssgOptionsCache.dynamicTargetFrameRate = options.dynamicTargetFrameRate;
}

bool DLSSG_Dx12::DispatchReflexOptionsMatchSubmitted(const sl::ReflexOptions& options) const
{
    if (!_dispatchReflexOptionsCache.valid)
        return false;

    return options.mode == _dispatchReflexOptionsCache.mode &&
           options.useMarkersToOptimize == _dispatchReflexOptionsCache.useMarkersToOptimize &&
           options.frameLimitUs == _dispatchReflexOptionsCache.frameLimitUs;
}

void DLSSG_Dx12::StoreDispatchReflexOptionsCache(const sl::ReflexOptions& options)
{
    _dispatchReflexOptionsCache.valid = true;
    _dispatchReflexOptionsCache.mode = options.mode;
    _dispatchReflexOptionsCache.useMarkersToOptimize = options.useMarkersToOptimize;
    _dispatchReflexOptionsCache.frameLimitUs = options.frameLimitUs;
}

bool DLSSG_Dx12::CreateSwapchain(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, DXGI_SWAP_CHAIN_DESC* desc,
                                 IDXGISwapChain** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == desc->OutputWindow)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->BufferDesc.Width, desc->BufferDesc.Height,
                              desc->BufferDesc.Format, desc->Flags) == S_OK;

            *swapChain = State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (StreamlineProxy::Module() == nullptr)
    {
        LOG_ERROR("Streamline proxy can't find sl.interposer.dll!");
        return false;
    }

    if (!StreamlineProxy::IsD3D12Inited())
    {
        if (State::Instance().currentD3D12Device != nullptr &&
            !StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device))
        {
            return false;
        }
    }

    _width = desc->BufferDesc.Width;
    _height = desc->BufferDesc.Height;

    IDXGIFactory* slFactory = nullptr;
    if (!Util::CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &slFactory))
    {
        StreamlineProxy::UpgradeInterface()((void**) &factory);
        DxgiFactoryHooks::HookToDLSSGFactory(factory);
    }

    StreamlineProxy::SetFeatureLoaded()(sl::kFeatureDLSS_G, true);

    desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

    auto result = S_FALSE;

    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = factory->CreateSwapChain(cmdQueue, desc, swapChain);
    }

    if (result != S_OK)
    {
        LOG_ERROR("CreateSwapChain error: {:X}", (UINT) result);
        return false;
    }

    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    if (StreamlineProxy::DLSSGGetState()(viewport, dlssgState, &dlssgOptions) == sl::Result::eOk)
    {
        _maxInterpolationCount = dlssgState.numFramesToGenerateMax;
        LOG_INFO("Max supported interpolations: {}", dlssgState.numFramesToGenerateMax);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }

    _gameCommandQueue = cmdQueue;
    _swapChain = *swapChain;
    _hwnd = desc->OutputWindow;
    _c048PresentQueueIdentity = cmdQueue;
    if (C048DiagEnabled())
    {
        std::lock_guard<std::mutex> lock(g_c048.mutex);
        C048SetOwnerLocked(this);
    }

    return true;
}

bool DLSSG_Dx12::CreateSwapchain1(IDXGIFactory* factory, ID3D12CommandQueue* cmdQueue, HWND hwnd,
                                  DXGI_SWAP_CHAIN_DESC1* desc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                  IDXGISwapChain1** swapChain, bool readyToRelease)
{
    if (State::Instance().currentFGSwapchain != nullptr && _hwnd == hwnd)
    {
        if (Config::Instance()->FGPreserveSwapChain.value_or_default())
        {
            LOG_WARN("FG swapchain already created for the same output window!");
            auto result = State::Instance().currentFGSwapchain->ResizeBuffers(
                              desc->BufferCount, desc->Width, desc->Height, desc->Format, desc->Flags) == S_OK;

            *swapChain = (IDXGISwapChain1*) State::Instance().currentFGSwapchain;
            return result;
        }
        // Game is creating new swapchain without releasing old one,
        // we need to release it to avoid errors
        else if (readyToRelease)
        {
            LOG_INFO("Releasing old swapchain");
            ReleaseSwapchain(_hwnd);

            // Not sure why but XeFG sometimes doesn't release the swapchain properly
            // so we force release it here to be able to recreate swapchain for same hwnd
            if (State::Instance().currentRealSwapchain != nullptr)
            {
                UINT release = 0;
                do
                {
                    release = State::Instance().currentRealSwapchain->Release();
                    LOG_DEBUG("Releasing swapchain, ref count: {}", release);
                } while (release > 0);
            }
        }
        else
        {
            LOG_WARN("FG swapchain already exists for the same output window and is not ready to release!");
            return false;
        }
    }

    if (StreamlineProxy::Module() == nullptr)
    {
        LOG_ERROR("Streamline proxy can't find sl.interposer.dll!");
        return false;
    }

    if (!StreamlineProxy::IsD3D12Inited())
    {
        if (State::Instance().currentD3D12Device != nullptr &&
            !StreamlineProxy::InitWithD3D12(State::Instance().currentD3D12Device))
        {
            return false;
        }
    }

    _width = desc->Width;
    _height = desc->Height;

    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

        IDXGIFactory* slFactory = nullptr;
        if (!Util::CheckForRealObject(__FUNCTION__, factory, (IUnknown**) &slFactory))
        {
            StreamlineProxy::UpgradeInterface()((void**) &factory);
            DxgiFactoryHooks::HookToDLSSGFactory(factory);
        }

        IDXGIFactory2* factory2 = nullptr;
        if (factory->QueryInterface(IID_PPV_ARGS(&factory2)) != S_OK)
            return false;

        StreamlineProxy::SetFeatureLoaded()(sl::kFeatureDLSS_G, true);

        desc->Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
        auto result = factory2->CreateSwapChainForHwnd(cmdQueue, hwnd, desc, pFullscreenDesc, nullptr, swapChain);

        factory2->Release();
        factory2 = nullptr;

        if (result != S_OK)
        {
            LOG_ERROR("CreateSwapChain error: {:X}", (UINT) result);
            return false;
        }
    }

    sl::DLSSGState dlssgState {};
    sl::DLSSGOptions dlssgOptions {};
    if (StreamlineProxy::DLSSGGetState()(viewport, dlssgState, &dlssgOptions) == sl::Result::eOk)
    {
        _maxInterpolationCount = dlssgState.numFramesToGenerateMax;
        LOG_INFO("Max supported interpolations: {}", dlssgState.numFramesToGenerateMax);

        _supportsDMFG = dlssgState.bIsDynamicMFGSupported == sl::Boolean::eTrue;
    }

    _gameCommandQueue = cmdQueue;
    _swapChain = *swapChain;
    _hwnd = hwnd;
    _c048PresentQueueIdentity = cmdQueue;
    if (C048DiagEnabled())
    {
        std::lock_guard<std::mutex> lock(g_c048.mutex);
        C048SetOwnerLocked(this);
    }

    return true;
}

void DLSSG_Dx12::CreateContext(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_DEBUG("");

    if (_device != nullptr)
        return;

    _device = device;
    CreateObjects(device);

    if (_isActive)
    {
        LOG_INFO("FG context recreated while active, pausing");
        State::Instance().fgChanged = true;
        UpdateTarget();
        Deactivate();
    }
}

void DLSSG_Dx12::Activate()
{
    LOG_DEBUG("");

    if (!_isActive)
    {

        InvalidateDispatchDlssgOptionsCache();
        InvalidateDispatchReflexOptionsCache();
        UpdateTarget();
        _isActive = true;
    }
}

void DLSSG_Dx12::Deactivate()
{
    LOG_DEBUG("");

    if (_isActive)
    {
        InvalidateDispatchDlssgOptionsCache();
        InvalidateDispatchReflexOptionsCache();

        sl::DLSSGOptions options {};
        options.mode = sl::DLSSGMode::eOff;
        options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;
        const auto dlssgSetOptionsResult = StreamlineProxy::DLSSGSetOptions()(viewport, options);

        sl::ReflexOptions reflexConst = {};
        reflexConst.mode = sl::ReflexMode::eOff;
        reflexConst.useMarkersToOptimize = false;
        const auto reflexSetOptionsResult = StreamlineProxy::ReflexSetOptions()(reflexConst);

        if (dlssgSetOptionsResult != sl::Result::eOk)
            LOG_ERROR("Couldn't set DLSSG options off, error: {}", magic_enum::enum_name(dlssgSetOptionsResult));
        if (reflexSetOptionsResult != sl::Result::eOk)
            LOG_ERROR("Couldn't set Reflex options off, error: {}", magic_enum::enum_name(reflexSetOptionsResult));

        _isActive = false;
    }
}

void DLSSG_Dx12::DestroyFGContext()
{
    Deactivate();
    ReleaseObjects();
}

bool DLSSG_Dx12::Shutdown()
{
    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    DestroyFGContext();

    if (State::Instance().isShuttingDown)
        StreamlineProxy::Shutdown()();

    return true;
}

bool DLSSG_Dx12::Dispatch()
{
    LOG_FUNC();

    UINT64 willDispatchFrame = 0;
    auto fIndex = GetDispatchIndex(willDispatchFrame);
    if (fIndex < 0)
        return false;

    if (!IsActive() || IsPaused())
        return false;

    LOG_DEBUG("_frameCount: {}, willDispatchFrame: {}, fIndex: {}", _frameCount, willDispatchFrame, fIndex);

    if (!_resourceReady[fIndex].contains(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Depth) ||
        !_resourceReady[fIndex].contains(FG_ResourceType::Velocity) ||
        !_resourceReady[fIndex].at(FG_ResourceType::Velocity))
    {
        LOG_WARN("Depth or Velocity is not ready, skipping");
        return false;
    }

    auto& state = State::Instance();

    if (Config::Instance()->FGDLSSGInterpolationCount.value_or_default() > _maxInterpolationCount)
    {
        Config::Instance()->FGDLSSGInterpolationCount = _maxInterpolationCount;
        LOG_WARN("Requested interpolation count is higher than max supported, setting to max: {}",
                 _maxInterpolationCount);
    }

    if (_framesToInterpolate != Config::Instance()->FGDLSSGInterpolationCount.value_or_default())
    {
        LOG_INFO("Interpolation count changed {} -> {}", _framesToInterpolate,
                 Config::Instance()->FGDLSSGInterpolationCount.value_or_default());

        _framesToInterpolate = Config::Instance()->FGDLSSGInterpolationCount.value_or_default();
    }

    sl::DLSSGOptions options {};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = _framesToInterpolate;
    options.queueParallelismMode = sl::DLSSGQueueParallelismMode::eBlockPresentingClientQueue;

    if (Config::Instance()->FGDLSSGForceDMFG.value_or_default())
    {
        options.mode = sl::DLSSGMode::eDynamic;
        options.dynamicTargetFrameRate = Config::Instance()->FGDLSSGFramerateTargetDMFG.value_or_default();
    }

    const bool dlssgOptionsUnchanged = DispatchDlssgOptionsMatchSubmitted(options);
    sl::Result dlssgSetOptionsResult = sl::Result::eOk;
    if (!dlssgOptionsUnchanged)
    {
        dlssgSetOptionsResult = StreamlineProxy::DLSSGSetOptions()(viewport, options);

        if (dlssgSetOptionsResult == sl::Result::eOk)
            StoreDispatchDlssgOptionsCache(options);
        else
            LOG_ERROR("Couldn't set DLSSG options, error: {}", magic_enum::enum_name(dlssgSetOptionsResult));
    }

    sl::ReflexOptions reflexConst = {};
    reflexConst.mode = DlssgC013ResolveDispatchReflexMode();
    reflexConst.useMarkersToOptimize = ReflexHooks::gameIsSendingMarkers();

    if (DlssgIsMonsterHunterWorldProcess())
    {
        const bool reflexOptionsUnchanged = DispatchReflexOptionsMatchSubmitted(reflexConst);
        if (!reflexOptionsUnchanged)
        {
            const auto reflexSetOptionsResult = StreamlineProxy::ReflexSetOptions()(reflexConst);

            if (reflexSetOptionsResult == sl::Result::eOk)
                StoreDispatchReflexOptionsCache(reflexConst);
            else
                LOG_ERROR("Couldn't set Reflex options, error: {}", magic_enum::enum_name(reflexSetOptionsResult));
        }
    }
    else
    {
        const auto reflexSetOptionsResult = StreamlineProxy::ReflexSetOptions()(reflexConst);

        if (reflexSetOptionsResult != sl::Result::eOk)
            LOG_ERROR("Couldn't set Reflex options, error: {}", magic_enum::enum_name(reflexSetOptionsResult));
    }

    if (!_haveHudless.has_value())
    {
        _haveHudless = IsUsingHudless(fIndex);
    }

    if (!_noHudless[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::HudlessColor];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    if (!_noDistortionField[fIndex])
    {
        auto res = &_frameResources[fIndex][FG_ResourceType::Distortion];
        if (res->validity != FG_ResourceValidity::ValidNow)
        {
            res->validity = FG_ResourceValidity::UntilPresentFromDispatch;
            res->frameIndex = fIndex;
            SetResource(res);
        }
    }

    sl::Constants constData = {};

    if (IsInfiniteDepth() && _cameraFar[fIndex] > _cameraNear[fIndex])
        _cameraFar[fIndex] = std::numeric_limits<float>::infinity();
    else if (IsInfiniteDepth() && _cameraNear[fIndex] > _cameraFar[fIndex])
        _cameraNear[fIndex] = std::numeric_limits<float>::infinity();

    if (_cameraPosition[fIndex][0] != 0.0f || _cameraPosition[fIndex][1] != 0.0f || _cameraPosition[fIndex][2] != 0.0f)
    {
        constData.cameraPos.x = _cameraPosition[fIndex][0];
        constData.cameraPos.y = _cameraPosition[fIndex][1];
        constData.cameraPos.z = _cameraPosition[fIndex][2];

        constData.cameraUp.x = _cameraUp[fIndex][0];
        constData.cameraUp.y = _cameraUp[fIndex][1];
        constData.cameraUp.z = _cameraUp[fIndex][2];

        constData.cameraFwd.x = _cameraForward[fIndex][0];
        constData.cameraFwd.y = _cameraForward[fIndex][1];
        constData.cameraFwd.z = _cameraForward[fIndex][2];

        constData.cameraRight.x = _cameraRight[fIndex][0];
        constData.cameraRight.y = _cameraRight[fIndex][1];
        constData.cameraRight.z = _cameraRight[fIndex][2];
    }
    else
    {
        constData.cameraPos = { 0.0f, 0.0f, 0.0f };
        constData.cameraUp = { 0.0f, 0.0f, 1.0f };
        constData.cameraRight = { 0.0f, 1.0f, 0.0f };
        constData.cameraFwd = { 1.0f, 0.0f, 0.0f };
        constData.cameraPinholeOffset = { 0.0f, 0.0f };

        XMMATRIX cameraViewToClip {};

        // XMMatrixPerspectiveFovRH will fail if input values are incorrect
        if (_cameraNear[fIndex] > 0.f && _cameraFar[fIndex] > 0.f &&
            !XMScalarNearEqual(_cameraVFov[fIndex], 0.0f, 0.00001f) &&
            !XMScalarNearEqual(_cameraAspectRatio[fIndex], 0.0f, 0.00001f))
        {
            if (XMScalarNearEqual(_cameraNear[fIndex], _cameraFar[fIndex], 0.00001f))
                _cameraFar[fIndex]++;

            cameraViewToClip = XMMatrixPerspectiveFovRH(_cameraVFov[fIndex], _cameraAspectRatio[fIndex],
                                                        _cameraNear[fIndex], _cameraFar[fIndex]);
        }
        else
        {
            LOG_WARN("Can't calculate projectionMatrix");
        }

        XMMATRIX clipToCameraView = XMMatrixInverse(nullptr, cameraViewToClip);

        auto prev = XMMatrixIdentity();

        // Convert to sl::float4x4 for Streamline
        XMFLOAT4X4 temp;
        XMStoreFloat4x4(&temp, cameraViewToClip);
        memcpy(&constData.cameraViewToClip, &temp, sizeof(sl::float4x4));
        XMStoreFloat4x4(&temp, clipToCameraView);
        memcpy(&constData.clipToCameraView, &temp, sizeof(sl::float4x4));

        XMStoreFloat4x4(&temp, prev);
        memcpy(&constData.clipToLensClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.clipToPrevClip, &temp, sizeof(sl::float4x4));
        memcpy(&constData.prevClipToClip, &temp, sizeof(sl::float4x4));
    }

    constData.cameraAspectRatio = _cameraAspectRatio[fIndex];
    constData.cameraFOV = _cameraVFov[fIndex];
    constData.cameraNear = _cameraNear[fIndex];
    constData.cameraFar = _cameraFar[fIndex];

    constData.jitterOffset.x = _jitterX[fIndex];
    constData.jitterOffset.y = _jitterY[fIndex];

    {
        auto mv = GetResource(FG_ResourceType::Velocity, fIndex);

        if (!mv)
        {
            LOG_ERROR("Motion vectors missing for: {}", fIndex);

            return false;
        }

        constData.mvecScale.x = _mvScaleX[fIndex] / (float) mv->width;
        constData.mvecScale.y = _mvScaleY[fIndex] / (float) mv->height;
    }

    // LOG_DEBUG("MvRes: {}x{}, Games MvScale : {}x{}, SL MvScale: {}x{}", mv->width, mv->height, _mvScaleX[fIndex],
    //           _mvScaleY[fIndex], constData.mvecScale.x, constData.mvecScale.y);

    // if (State::Instance().currentFeature != nullptr)
    //{
    //     auto fResX = State::Instance().currentFeature->RenderWidth();
    //     auto fResY = State::Instance().currentFeature->RenderHeight();

    //    LOG_DEBUG("Feature LowResMV: {} RenderRes : {}x{}, CMvScale: {}x{}",
    //              State::Instance().currentFeature->LowResMV(), fResX, fResY, 1.0f / (float) fResX,
    //              1.0f / (float) fResY);
    //}

    if (!Config::Instance()->FGSkipReset.value_or_default())
        constData.reset = _reset[fIndex] != 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    else
        constData.reset = sl::Boolean::eFalse;

    constData.depthInverted = IsInvertedDepth() ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constData.cameraMotionIncluded = sl::Boolean::eTrue;
    constData.motionVectors3D = sl::Boolean::eFalse;
    // constData.motionVectorsInvalidValue = 0.0f;
    constData.orthographicProjection = sl::Boolean::eFalse;
    constData.motionVectorsDilated = IsLowResMV() ? sl::Boolean::eFalse : sl::Boolean::eTrue;
    constData.motionVectorsJittered = IsJitteredMVs() ? sl::Boolean::eTrue : sl::Boolean::eFalse;

    auto frameId = static_cast<uint32_t>(willDispatchFrame);

    auto tokenResult = StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
    if (tokenResult != sl::Result::eOk)
    {
        LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    auto result = StreamlineProxy::SetConstants()(constData, *frameToken, viewport);
    if (result != sl::Result::eOk)
    {
        LOG_ERROR("SetConstants error: {} ({})", magic_enum::enum_name(result), (UINT) result);

        state.fgChanged = true;
        UpdateTarget();
        Deactivate();

        return false;
    }

    LOG_DEBUG("Result: Ok");

    return true;
}

void* DLSSG_Dx12::FrameGenerationContext() { return (void*) 0x13371337; }

void* DLSSG_Dx12::SwapchainContext() { return (void*) 0x23372337; }

DLSSG_Dx12::~DLSSG_Dx12()
{
    C048ClearDiagState();
    Shutdown();
}

bool DLSSG_Dx12::SetInterpolatedFrameCount(UINT interpolatedFrameCount) { return true; }

void DLSSG_Dx12::EvaluateState(ID3D12Device* device, FG_Constants& fgConstants)
{
    LOG_FUNC();

    OwnedLockGuard lock(Mutex, 555);

    auto& state = State::Instance();

    if (MhwFgFailure::BlocksFgActivation())
    {
        Config::Instance()->FGEnabled.set_volatile_value(false);

        if (IsActive())
        {
            state.fgChanged = true;
            Deactivate();
        }

        state.clearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();

        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);

        return;
    }

    // If needed hooks are missing or XeFG proxy is not inited or FG swapchain is not created
    if (!StreamlineProxy::LoadStreamline() || state.currentFGSwapchain == nullptr)
        return;

    if (state.isShuttingDown)
    {
        return;
    }

    _constants = fgConstants;

    // If FG Enabled from menu
    if (Config::Instance()->FGEnabled.value_or_default())
    {
        if (_device == nullptr)
        {
            // Create it again
            CreateContext(device, fgConstants);
        }
        else if (state.fgChanged)
        {
            LOG_DEBUG("FGChanged");
            Deactivate();

            // Pause for 10 frames
            UpdateTarget();
        }

        if (State::Instance().activeFgInput == FGInput::Upscaler && !IsPaused() && !IsActive())
            Activate();
    }
    else
    {
        LOG_DEBUG("!FGEnabled");
        Deactivate();

        state.clearCapturedHudlesses = true;
        Hudfix_Dx12::ResetCounters();
    }

    if (state.fgChanged)
    {
        LOG_DEBUG("FGchanged");

        state.fgChanged = false;

        Hudfix_Dx12::ResetCounters();

        // Pause for 10 frames
        UpdateTarget();

        // Release FG mutex
        if (Mutex.getOwner() == 2)
            Mutex.unlockThis(2);
    }

    state.scChanged = false;
}

void DLSSG_Dx12::ReleaseObjects()
{
    C048ClearDiagState();

    InvalidateDispatchDlssgOptionsCache();
    InvalidateDispatchReflexOptionsCache();

    for (size_t i = 0; i < BUFFER_COUNT; i++)
    {
        SAFE_RELEASE(_uiCommandAllocator[i]);
        SAFE_RELEASE(_uiCommandList[i]);
        SAFE_RELEASE(_scCommandAllocator[i]);
        SAFE_RELEASE(_scCommandList[i]);
        SAFE_RELEASE(dlssgFence[i]);

        // Reset command list state
        _scCommandListResetted[i] = false;
        _scAllocatorFenceValues[i] = 0;

        _uiCommandListResetted[i] = false;
        _uiAllocatorFenceValues[i] = 0;
    }

    _renderUI.reset();
    _hudlessCompare.reset();
    _mvFlip.reset();
    _depthFlip.reset();
}

void DLSSG_Dx12::CreateObjects(ID3D12Device* InDevice)
{
    _device = InDevice;

    if (_uiCommandAllocator[0] != nullptr)
        return;

    C066DiagProbeDeviceInfoQueue(InDevice);

    LOG_DEBUG("");

    do
    {
        HRESULT result;
        ID3D12CommandAllocator* allocator = nullptr;
        ID3D12GraphicsCommandList* cmdList = nullptr;
        ID3D12CommandQueue* cmdQueue = nullptr;

        // FG
        for (size_t i = 0; i < BUFFER_COUNT; i++)
        {
            // Reset command list state
            _scCommandListResetted[i] = false;
            _scAllocatorFenceValues[i] = 0;

            _uiCommandListResetted[i] = false;
            _uiAllocatorFenceValues[i] = 0;

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_uiCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _uiCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _uiCommandAllocator[i]->SetName(std::format(L"_uiCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandAllocator[i], (IUnknown**) &allocator))
                _uiCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _uiCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_uiCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _uiCommandList[i]->SetName(std::format(L"_uiCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _uiCommandList[i], (IUnknown**) &cmdList))
                _uiCommandList[i] = cmdList;

            result = _uiCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_uiCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_uiFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_uiFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create UI fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_uiFenceEvent == nullptr)
            {
                _uiFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_uiFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for UI fence failed");
                    break;
                }
            }

            result =
                InDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_scCommandAllocator[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandAllocators _scCommandAllocator[{}]: {:X}", i, (unsigned long) result);
                break;
            }

            _scCommandAllocator[i]->SetName(std::format(L"_scCommandAllocator[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandAllocator[i], (IUnknown**) &allocator))
                _scCommandAllocator[i] = allocator;

            result = InDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, _scCommandAllocator[i], NULL,
                                                 IID_PPV_ARGS(&_scCommandList[i]));
            if (result != S_OK)
            {
                LOG_ERROR("CreateCommandList _hudlessCommandList[{}]: {:X}", i, (unsigned long) result);
                break;
            }
            _scCommandList[i]->SetName(std::format(L"_scCommandList[{}]", i).c_str());
            if (CheckForRealObject(__FUNCTION__, _scCommandList[i], (IUnknown**) &cmdList))
                _scCommandList[i] = cmdList;

            result = _scCommandList[i]->Close();
            if (result != S_OK)
            {
                LOG_ERROR("_scCommandList[{}]->Close: {:X}", i, (unsigned long) result);
                break;
            }

            if (_scFence == nullptr)
            {
                result = InDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_scFence));
                if (FAILED(result))
                {
                    LOG_ERROR("Create SC fence failed: {:X}", (UINT) result);
                    break;
                }
            }

            if (_scFenceEvent == nullptr)
            {
                _scFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
                if (_scFenceEvent == nullptr)
                {
                    LOG_ERROR("CreateEvent for SC fence failed");
                    break;
                }
            }
        }

    } while (false);
}

bool DLSSG_Dx12::Present()
{
    auto fIndex = GetIndexWillBeDispatched();
    LOG_DEBUG("fIndex: {}", fIndex);

    if (Config::Instance()->FGDrawUIOverFG.value_or_default())
    {
        auto ui = GetResource(FG_ResourceType::UIColor, fIndex);
        if (ui && (ui->validity == FG_ResourceValidity::UntilPresent ||
                   ui->validity == FG_ResourceValidity::JustTrackCmdlist ||
                   ui->validity == FG_ResourceValidity::UntilPresentFromDispatch))
        {
            LOG_DEBUG("UI[{}] resource: {:X}, copy: {}", fIndex, (size_t) ui->resource, (size_t) ui->copy);
            if (_renderUI.get() == nullptr)
            {
                _renderUI = std::make_unique<RUI_Dx12>("RenderUI", _device,
                                                       Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
            }
            else
            {
                if (Config::Instance()->FGUIPremultipliedAlpha.value_or_default() != _renderUI->IsPreMultipliedAlpha())
                {
                    LOG_INFO("UI premultiplied alpha changed, recreating RenderUI");
                    _renderUI = std::make_unique<RUI_Dx12>(
                        "RenderUI", _device, Config::Instance()->FGUIPremultipliedAlpha.value_or_default());
                }
                else if (_renderUI->IsInit())
                {
                    auto commandList = GetSCCommandList(fIndex);
                    _renderUI->Dispatch((IDXGISwapChain3*) _swapChain, commandList, ui->GetResource(), ui->state);
                }
            }
        }
        else if (!ui)
        {
            LOG_WARN("UI resource is nullptr");
        }
    }

    if (IsActive() && !IsPaused())
    {
        if (State::Instance().fgHudlessCompare)
        {
            auto hudless = GetResource(FG_ResourceType::HudlessColor, fIndex);
            if (hudless && (hudless->validity == FG_ResourceValidity::UntilPresent ||
                            hudless->validity == FG_ResourceValidity::JustTrackCmdlist ||
                            hudless->validity == FG_ResourceValidity::UntilPresentFromDispatch))
            {
                LOG_DEBUG("Hudless[{}] resource: {:X}, copy: {}", fIndex, (size_t) hudless->resource,
                          (size_t) hudless->copy);
                if (_hudlessCompare.get() == nullptr)
                {
                    _hudlessCompare = std::make_unique<HC_Dx12>("HudlessCompare", _device);
                }
                else
                {
                    if (_hudlessCompare->IsInit())
                    {
                        auto commandList = GetSCCommandList(fIndex);
                        _hudlessCompare->Dispatch((IDXGISwapChain3*) _swapChain, commandList, hudless->GetResource(),
                                                  hudless->state);
                    }
                }
            }
            else if (!hudless)
            {
                LOG_WARN("Hudless resource is nullptr");
            }
        }
    }

    bool result = false;

    // if (IsActive() && !IsPaused())
    {
        if (_uiCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _uiCommandList[{}]: {:X}", fIndex, (size_t) _uiCommandList[fIndex]);
            auto closeResult = _uiCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_uiCommandList[fIndex]);
            else
                LOG_ERROR("_uiCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _gameCommandQueue->Signal(_uiFence, _uiAllocatorFenceValues[fIndex]);

            _uiCommandListResetted[fIndex] = false;
        }

        if (_scCommandListResetted[fIndex])
        {
            LOG_DEBUG("Executing _scCommandList[{}]: {:X}", fIndex, (size_t) _scCommandList[fIndex]);
            auto closeResult = _scCommandList[fIndex]->Close();

            if (closeResult == S_OK)
                _gameCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**) &_scCommandList[fIndex]);
            else
                LOG_ERROR("_scCommandList[{}]->Close() error: {:X}", fIndex, (UINT) closeResult);

            _scCommandListResetted[fIndex] = false;
        }
    }

    if ((_fgFramePresentId - _lastFGFramePresentId) > 3 && IsActive() && !_waitingNewFrameData)
    {
        LOG_DEBUG("Pausing FG");

        Deactivate();
        _waitingNewFrameData = true;
        return false;
    }

    _fgFramePresentId++;

    return Dispatch();
}

struct DLSSG_Dx12::C048DeferredTag
{
    bool valid = false;
    uint32_t frameId = 0;
    FG_ResourceType resourceType = FG_ResourceType::Depth;
    ID3D12Resource* resource = nullptr;
    ID3D12GraphicsCommandList* cmdList = nullptr;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    sl::ResourceLifecycle lifecycle = sl::ResourceLifecycle::eOnlyValidNow;
};

bool DLSSG_Dx12::SetResource(Dx12Resource* inputResource)
{
    if (inputResource == nullptr || inputResource->resource == nullptr ||
        (inputResource->type != FG_ResourceType::UIColor && (!IsActive() || IsPaused())))
    {
        return false;
    }

    // For late sent SL resources
    // we use provided frame index
    auto fIndex = inputResource->frameIndex;
    if (fIndex < 0)
        fIndex = GetIndex();

    auto& type = inputResource->type;

    C048DeferredTag c048Deferred;

    std::unique_lock<std::shared_mutex> lock(_resourceMutex[fIndex]);

    if (type == FG_ResourceType::HudlessColor)
    {
        if (Config::Instance()->FGDisableHudless.value_or_default())
            return false;

        // Making a copy if it's just valid now to be able to use it later
        if (State::Instance().fgHudlessCompare && inputResource->validity == FG_ResourceValidity::ValidNow)
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;

        if (!_noHudless[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }

        if (!_noHudless[fIndex] && Config::Instance()->FGOnlyAcceptFirstHudless.value_or_default() &&
            inputResource->validity != FG_ResourceValidity::UntilPresentFromDispatch)
        {
            return false;
        }
    }

    if (type == FG_ResourceType::UIColor)
    {
        if (Config::Instance()->FGDisableUI.value_or_default())
            return false;

        // Making a copy if it's just valid now
        if (Config::Instance()->FGDrawUIOverFG.value_or_default() &&
            inputResource->validity == FG_ResourceValidity::ValidNow)
        {
            inputResource->validity = FG_ResourceValidity::ValidButMakeCopy;
        }

        if (!_noUi[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if (type == FG_ResourceType::Distortion)
    {
        if (!_noDistortionField[fIndex] && (_frameResources[fIndex][type].validity == FG_ResourceValidity::ValidNow))
        {
            return false;
        }
    }

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) && _frameResources[fIndex].contains(type))
    {
        return false;
    }

    if (inputResource->cmdList == nullptr && inputResource->validity == FG_ResourceValidity::ValidNow)
    {
        LOG_ERROR("{}, validity == ValidNow but cmdList is nullptr!", magic_enum::enum_name(type));
        return false;
    }

    if (type == FG_ResourceType::Distortion)
    {
        LOG_TRACE("Distortion field is not supported by XeFG");
        return false;
    }

    auto fResource = &_frameResources[fIndex][type];
    fResource->type = type;
    fResource->state = inputResource->state;
    fResource->validity = inputResource->validity;
    fResource->resource = inputResource->resource;
    fResource->top = inputResource->top;
    fResource->left = inputResource->left;
    fResource->width = inputResource->width;
    fResource->height = inputResource->height;
    fResource->cmdList = inputResource->cmdList;

    auto willFlip = State::Instance().activeFgInput == FGInput::Upscaler &&
                    Config::Instance()->FGResourceFlip.value_or_default() &&
                    (type == FG_ResourceType::Velocity || type == FG_ResourceType::Depth);

    // Resource flipping
    if (willFlip && _device != nullptr)
        FlipResource(fResource);

    // We usually don't copy any resources for DLSSG, the ones with this tag are the exception
    if (inputResource->cmdList != nullptr && fResource->validity == FG_ResourceValidity::ValidButMakeCopy)
    {
        LOG_DEBUG("Making a resource copy of: {}", magic_enum::enum_name(type));

        ID3D12Resource* copyOutput = nullptr;

        if (_resourceCopy[fIndex].contains(type))
            copyOutput = _resourceCopy[fIndex][type];

        if (!CopyResource(inputResource->cmdList, inputResource->resource, &copyOutput, inputResource->state))
        {
            LOG_ERROR("{}, CopyResource error!", magic_enum::enum_name(type));
            return false;
        }

        _resourceCopy[fIndex][type] = copyOutput;
        _resourceCopy[fIndex][type]->SetName(std::format(L"_resourceCopy[{}][{}]", fIndex, (UINT) type).c_str());
        fResource->copy = copyOutput;
        fResource->state = D3D12_RESOURCE_STATE_COPY_DEST;

        fResource->validity = FG_ResourceValidity::UntilPresent;
    }

    if (type == FG_ResourceType::UIColor)
        _noUi[fIndex] = false;
    else if (type == FG_ResourceType::Distortion)
        _noDistortionField[fIndex] = false;
    else if (type == FG_ResourceType::HudlessColor)
        _noHudless[fIndex] = false;

    if ((type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity) ||
        (fResource->validity != FG_ResourceValidity::UntilPresent &&
         fResource->validity != FG_ResourceValidity::JustTrackCmdlist))
    {
        fResource->validity = (fResource->validity != FG_ResourceValidity::ValidNow || willFlip)
                                  ? FG_ResourceValidity::UntilPresent
                                  : FG_ResourceValidity::ValidNow;

        if (type == FG_ResourceType::HudlessColor)
        {
            static DXGI_FORMAT lastFormat[BUFFER_COUNT] = {};
            auto desc = fResource->GetResource()->GetDesc();

            if (lastFormat[fIndex] != DXGI_FORMAT_UNKNOWN && lastFormat[fIndex] != desc.Format)
            {
                State::Instance().fgChanged = true;
                return false;
            }

            lastFormat[fIndex] = desc.Format;
        }

        sl::Resource resource {};
        resource.height = fResource->height;
        resource.native = fResource->GetResource();
        resource.state = fResource->state;
        resource.type = sl::ResourceType::eTex2d;
        resource.width = (uint32_t) fResource->width;

        sl::ResourceTag resourceTag {};
        resourceTag.resource = &resource;

        switch (fResource->type)
        {
        case FG_ResourceType::Depth:
            resourceTag.type = sl::kBufferTypeDepth;
            break;

        case FG_ResourceType::HudlessColor:
            resourceTag.type = sl::kBufferTypeHUDLessColor;
            break;

        case FG_ResourceType::UIColor:
            resourceTag.type = sl::kBufferTypeUIColorAndAlpha;
            break;

        case FG_ResourceType::Velocity:
            resourceTag.type = sl::kBufferTypeMotionVectors;
            break;

        default:
            return false;
        }

        resourceTag.lifecycle = fResource->validity == FG_ResourceValidity::UntilPresent
                                    ? ::sl::ResourceLifecycle::eValidUntilPresent
                                    : sl::ResourceLifecycle::eOnlyValidNow;

        resourceTag.extent.left = fResource->left;
        resourceTag.extent.top = fResource->top;
        resourceTag.extent.width = (uint32_t) fResource->width;
        resourceTag.extent.height = fResource->height;

        int indexDiff = GetIndex() - fIndex;
        if (indexDiff < 0)
            indexDiff += BUFFER_COUNT;

        // We will us UI color later with Render UI
        {
            auto frameId = static_cast<uint32_t>(_frameCount - indexDiff);

            auto tokenResult = StreamlineProxy::GetNewFrameToken()(frameToken, &frameId);
            if (tokenResult != sl::Result::eOk)
            {
                LOG_ERROR("GetNewFrameToken error: {} ({})", magic_enum::enum_name(tokenResult), (UINT) tokenResult);
                return false;
            }

            auto result = StreamlineProxy::SetTagForFrame()(*frameToken, viewport, &resourceTag, 1, fResource->cmdList);
            LOG_DEBUG("SetTagForFrame, frameId: {}, type: {} result: {} ({})", frameId, magic_enum::enum_name(type),
                      magic_enum::enum_name(result), (int32_t) result);

            if (result != sl::Result::eOk)
            {
                State::Instance().fgChanged = true;
                UpdateTarget();
                Deactivate();

                return false;
            }

            if (type == FG_ResourceType::Depth || type == FG_ResourceType::Velocity)
            {
                c048Deferred.valid = true;
                c048Deferred.frameId = frameId;
                c048Deferred.resourceType = type;
                c048Deferred.resource = fResource->GetResource();
                c048Deferred.cmdList = fResource->cmdList;
                c048Deferred.state = fResource->state;
                c048Deferred.lifecycle = resourceTag.lifecycle;
            }
        }

        // Potentially we don't need to restore but do it just to be safe
        if (inputResource->state == D3D12_RESOURCE_STATE_COPY_SOURCE)
        {
            ResourceBarrier(inputResource->cmdList, inputResource->resource, D3D12_RESOURCE_STATE_COPY_DEST,
                            inputResource->state);
        }

        SetResourceReady(type, fIndex);
    }

    LOG_TRACE("_frameResources[{}][{}]: {:X}", fIndex, magic_enum::enum_name(type), (size_t) fResource->GetResource());

    lock.unlock();

    if (c048Deferred.valid)
        C048RegisterDeferredTag(c048Deferred);

    return true;
}

void DLSSG_Dx12::SetCommandQueue(FG_ResourceType type, ID3D12CommandQueue* queue) { _gameCommandQueue = queue; }

bool DLSSG_Dx12::ReleaseSwapchain(HWND hwnd)
{
    if (hwnd != _hwnd || _hwnd == NULL)
        return false;

    LOG_DEBUG("");

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        if (Mutex.getOwner() == 1)
        {
            LOG_WARN("Skipping Mutex we are already in ReleaseSwapchain");
            return true;
        }

        LOG_TRACE("Waiting Mutex 1, current: {}", Mutex.getOwner());
        Mutex.lock(1);
        LOG_TRACE("Accuired Mutex: {}", Mutex.getOwner());
    }

    MenuOverlayDx::CleanupRenderTarget(true, NULL);

    // if (_fgContext != nullptr)
    //     DestroyFGContext();

    // if (!State::Instance().isShuttingDown)
    //{
    //     if (_swapChainContext != nullptr)
    //         DestroySwapchainContext();

    //    _swapChainContext = nullptr;
    //    State::Instance().currentFGSwapchain = nullptr;
    //}

    ReleaseObjects();

    if (Config::Instance()->FGUseMutexForSwapchain.value_or_default())
    {
        LOG_TRACE("Releasing Mutex: {}", Mutex.getOwner());
        Mutex.unlockThis(1);
    }

    return true;
}

bool DLSSG_Dx12::C048DiagEnabled() { return C048DiagEnabledImpl(); }

uint64_t DLSSG_Dx12::C048DiagNowUs() { return C048NowUs(); }

uint32_t DLSSG_Dx12::C048DiagPrepareExecuteCommandLists(ID3D12CommandQueue* queue, UINT numCommandLists,
                                                        ID3D12CommandList* const* ppCommandLists)
{
    if (!C048DiagEnabledImpl())
        return 0;

    std::lock_guard<std::mutex> lock(g_c048.mutex);
    if (g_c048.owner == nullptr)
        return 0;

    if (g_c048Tls.depth >= C048_SNAPSHOT_STACK_DEPTH)
    {
        ++g_c048.periodStale;
        return 0;
    }

    auto& snap = g_c048Tls.stack[g_c048Tls.depth];
    snap = {};
    snap.valid = true;
    snap.submitQueueIdentity = queue;
    snap.ownerGeneration = g_c048.ownerGeneration;

    const uint32_t callId = C048AllocateCallLocked(snap.ownerGeneration);
    if (callId == 0)
    {
        ++g_c048.periodStale;
        return 0;
    }

    snap.callId = callId;
    C048PrepareExecuteLocked(numCommandLists, ppCommandLists, snap, queue, callId);

    ++g_c048Tls.depth;
    return static_cast<uint32_t>(g_c048Tls.depth);
}

void DLSSG_Dx12::C048DiagCompleteExecuteCommandLists(uint32_t snapshotToken, ID3D12CommandQueue* queue,
                                                     uint64_t originalEntryUs, uint64_t originalExitUs)
{
    if (!C048DiagEnabledImpl() || snapshotToken == 0)
        return;

    std::lock_guard<std::mutex> lock(g_c048.mutex);

    if (snapshotToken != g_c048Tls.depth || snapshotToken > C048_SNAPSHOT_STACK_DEPTH)
    {
        ++g_c048.periodStale;
        return;
    }

    auto& snap = g_c048Tls.stack[snapshotToken - 1];
    if (!snap.valid || snap.submitQueueIdentity != queue)
    {
        ++g_c048.periodStale;
        C048ClearInFlightForCallIdLocked(snap.callId);
        C048ReleaseCallLocked(snap.callId);
        snap = {};
        if (g_c048Tls.depth > 0)
            --g_c048Tls.depth;
        return;
    }

    if (snap.ownerGeneration != g_c048.ownerGeneration || g_c048.owner == nullptr)
    {
        ++g_c048.periodStale;
        C048ClearInFlightForCallIdLocked(snap.callId);
        C048ReleaseCallLocked(snap.callId);
        snap = {};
        --g_c048Tls.depth;
        return;
    }

    C048CompleteExecuteLocked(snap, originalEntryUs, originalExitUs);
    C048ClearInFlightForCallIdLocked(snap.callId);
    C048ReleaseCallLocked(snap.callId);
    snap = {};
    --g_c048Tls.depth;
}

void DLSSG_Dx12::C048DiagMaybeReport() { C048MaybeReport(); }

void DLSSG_Dx12::C048RegisterDeferredTag(const C048DeferredTag& tag)
{
    if (!C048DiagEnabledImpl())
        return;

    std::lock_guard<std::mutex> lock(g_c048.mutex);
    if (g_c048.owner != this)
        C048SetOwnerLocked(this);

    for (auto& slot : g_c048.pending)
    {
        if (slot.occupied)
            continue;

        slot.occupied = true;
        slot.frameId = tag.frameId;
        slot.resourceType = tag.resourceType;
        slot.resourceIdentity = tag.resource;
        slot.cmdListIdentity = tag.cmdList;
        slot.tagTimeUs = C048NowUs();
        slot.resourceState = tag.state;
        slot.lifecycle = static_cast<uint32_t>(tag.lifecycle);
        slot.creationQueueIdentity = _c048PresentQueueIdentity;
        return;
    }

    ++g_c048.periodDropped;
}

void DLSSG_Dx12::C048ClearDiagState()
{
    if (!C048DiagEnabledImpl())
        return;

    {
        std::lock_guard<std::mutex> lock(g_c048.mutex);
        if (g_c048.owner == this)
        {
            for (auto& slot : g_c048.pending)
                slot.occupied = false;
            for (auto& inflight : g_c048.inFlight)
                inflight.active = false;
            for (auto& call : g_c048.calls)
                call.active = false;

            g_c048.sampleCount = 0;
            g_c048.samples.fill({});
            g_c048.periodMatched = 0;
            g_c048.periodDropped = 0;
            g_c048.periodAmbiguous = 0;
            g_c048.periodStale = 0;
            g_c048.periodInflightDropped = 0;
            g_c048.owner = nullptr;
            ++g_c048.ownerGeneration;
        }
    }

    _c048PresentQueueIdentity = nullptr;
}
