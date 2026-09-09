#include "pch.h"
#include "FG_Hooks.h"
#include <Config.h>

#include <framegen/ffx/FSRFG_Dx12.h>
#include <framegen/xefg/XeFG_Dx12.h>
#include <framegen/dlssg/DLSSG_Dx12.h>

#include <inputs/FG/FSR3_Dx12_FG.h>
#include <inputs/FG/FfxApi_Dx12_FG.h>

#include <hudfix/Hudfix_Dx12.h>
#include <resource_tracking/ResTrack_Dx12.h>

#include <misc/FrameLimit.h>

#include <misc/IdentifyGpu.h>
#include <misc/MhwFgFailure.h>
#include <hooks/Reflex_Hooks.h>
#include <menu/menu_overlay_dx.h>

#include <d3d12.h>
#include <detours/detours.h>

#include <chrono>
#include <cstring>
#include <optional>
#include <array>

namespace
{
static uint32_t FgPresentResolveFrameLimitOutputMultiplier(bool fgActiveForLimit, IFGFeature* fg, FGOutput fgOutput,
                                                           bool forceDmfg)
{
    if (!fgActiveForLimit)
        return 2;

    if (fgOutput == FGOutput::DLSSG && fg != nullptr && !forceDmfg)
    {
        const uint32_t requestedOutputFrames = fg->GetInterpolatedFrameCount() + 1;
        if (requestedOutputFrames >= 2 && requestedOutputFrames <= 6)
            return requestedOutputFrames;
    }

    return 2;
}

static bool FgPresentIsMhwProcess()
{
    static thread_local bool resolved = false;
    static thread_local bool isMhw = false;
    if (!resolved)
    {
        resolved = true;
        isMhw = Util::ToLower(Util::ExePath().filename().wstring()) == L"monsterhunterworld.exe";
    }
    return isMhw;
}

using MhwSwapchainWaitHelperFn = void(__fastcall*)(void* thisObject);

static constexpr uint8_t kMhwSwapchainWaitHelperSignature[71] = {
    0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x80, 0xb9, 0xd9, 0x10, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xd9, 0x74, 0x2f, 0x80,
    0xb9, 0xda, 0x10, 0x00, 0x00, 0x00, 0x75, 0x26, 0x48, 0x8b, 0x89, 0xf0, 0x10, 0x00, 0x00, 0xba, 0xe8, 0x03, 0x00,
    0x00, 0x41, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xff, 0x15, 0x75, 0x54, 0xa6, 0x00, 0x3d, 0x02, 0x01, 0x00, 0x00, 0x75,
    0x07, 0xc6, 0x83, 0xda, 0x10, 0x00, 0x00, 0x01, 0x48, 0x83, 0xc4, 0x20, 0x5b, 0xc3,
};

static constexpr uintptr_t kMhwSwapchainWaitHelperRva = 0x23ade30;
static constexpr uintptr_t kMhwSwapchainWaitCallerReturnRva = 0x229b078;

static bool fgC024HookInstalled = false;
static void* fgC024ExpectedCallerReturn = nullptr;
static MhwSwapchainWaitHelperFn o_MhwSwapchainWaitHelper = nullptr;

static bool FgC024IsMonsterHunterWorldProcess()
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

static bool FgC024SkipSwapchainWaitEnabled()
{
    static bool resolved = false;
    static bool enabled = false;
    if (!resolved)
    {
        resolved = true;
        if (FgC024IsMonsterHunterWorldProcess())
        {
#ifdef MHWFG_PRODUCTION
            enabled = true;
            LOG_INFO("[MHWFG] production MHW defaults: skip_swapchain_wait=on skip_cold_off_wait=on");
#else
            char envValue[8] = {};
            const DWORD envLen =
                GetEnvironmentVariableA("MHWFG_DIAG_SKIP_SWAPCHAIN_WAIT", envValue, static_cast<DWORD>(sizeof(envValue)));
            enabled = envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
#endif
        }
    }
    return enabled;
}

static bool FgC036SkipColdOffWaitEnabled()
{
    static bool resolved = false;
    static bool enabled = false;
    if (!resolved)
    {
        resolved = true;
        if (FgC024IsMonsterHunterWorldProcess())
        {
#ifdef MHWFG_PRODUCTION
            enabled = true;
#else
            char envValue[8] = {};
            const DWORD envLen =
                GetEnvironmentVariableA("MHWFG_DIAG_SKIP_COLD_OFF_WAIT", envValue, static_cast<DWORD>(sizeof(envValue)));
            enabled = envLen == 1 && envValue[0] == '1' && envValue[1] == '\0';
            if (enabled)
            {
                LOG_INFO("[C036] experimental cold-off wait bypass enabled (requires hook installed via "
                         "MHWFG_DIAG_SKIP_SWAPCHAIN_WAIT=1; not a production optimization)");
            }
#endif
        }
    }
    return enabled;
}

static size_t FgC024GetMainModuleSizeOfImage(HMODULE gameModule)
{
    const auto* dosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(gameModule);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;

    const auto* ntHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(
        reinterpret_cast<const uint8_t*>(gameModule) + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    return ntHeaders->OptionalHeader.SizeOfImage;
}

static bool FgC024IsOffsetWithinMainModule(size_t moduleSize, uintptr_t offset, size_t span)
{
    return offset <= moduleSize && span <= moduleSize - offset;
}

static bool FgC024ProtectionIsReadable(DWORD protect)
{
    if (protect & PAGE_GUARD)
        return false;

    switch (protect & 0xFF)
    {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

static bool FgC024IsReadableMemory(const void* address, size_t size)
{
    MEMORY_BASIC_INFORMATION memoryInfo {};
    if (VirtualQuery(address, &memoryInfo, sizeof(memoryInfo)) == 0)
        return false;
    if (memoryInfo.State != MEM_COMMIT)
        return false;
    if (!FgC024ProtectionIsReadable(memoryInfo.Protect))
        return false;

    const uintptr_t regionStart = reinterpret_cast<uintptr_t>(memoryInfo.BaseAddress);
    const uintptr_t regionEnd = regionStart + memoryInfo.RegionSize;
    const uintptr_t rangeStart = reinterpret_cast<uintptr_t>(address);
    return rangeStart + size <= regionEnd;
}

static bool FgC024ShouldBypassWait(void* callerReturn)
{
    if (!fgC024HookInstalled || o_MhwSwapchainWaitHelper == nullptr)
        return false;

    if (callerReturn != fgC024ExpectedCallerReturn)
        return false;

    auto& state = State::Instance();
    if (state.isShuttingDown)
        return false;
    if (state.activeFgOutput != FGOutput::DLSSG)
        return false;

    if (!Config::Instance()->FGEnabled.value_or_default())
        return false;

    IFGFeature* fg = state.currentFG;
    if (fg == nullptr || !fg->IsActive() || fg->IsPaused())
        return false;

    if (fg->IsWaitingNewFrameData())
        return false;

    return true;
}

static bool FgC036ShouldBypassColdOffWait(void* callerReturn)
{
    if (!FgC036SkipColdOffWaitEnabled())
        return false;
    if (!fgC024HookInstalled || o_MhwSwapchainWaitHelper == nullptr)
        return false;
    if (callerReturn != fgC024ExpectedCallerReturn)
        return false;

    auto& state = State::Instance();
    if (state.isShuttingDown)
        return false;
    if (state.activeFgOutput != FGOutput::DLSSG)
        return false;
    if (Config::Instance()->FGEnabled.value_or_default())
        return false;

    IFGFeature* fg = state.currentFG;
    if (fg == nullptr)
        return false;
    if (fg->IsActive())
        return false;
    if (fg->IsPaused())
        return false;
    if (fg->IsWaitingNewFrameData())
        return false;

    return true;
}

static void __fastcall hkMhwSwapchainWaitHelper(void* thisObject)
{
    void* const callerReturn = _ReturnAddress();
    if (FgC024ShouldBypassWait(callerReturn))
        return;
    if (FgC036ShouldBypassColdOffWait(callerReturn))
        return;
    o_MhwSwapchainWaitHelper(thisObject);
}

static void FgC024TryInstallSwapchainWaitHook()
{
    static bool installAttempted = false;
    if (installAttempted)
        return;
    installAttempted = true;

    if (!FgC024IsMonsterHunterWorldProcess())
    {
        LOG_INFO("[C024] skipped: not monsterhunterworld.exe");
        return;
    }

    if (!FgC024SkipSwapchainWaitEnabled())
    {
        LOG_INFO("[C024] skipped: MHWFG_DIAG_SKIP_SWAPCHAIN_WAIT unset or not 1");
        return;
    }

    if (State::Instance().activeFgOutput != FGOutput::DLSSG)
    {
        LOG_INFO("[C024] skipped: active FG output is not DLSSG");
        return;
    }

    const HMODULE gameModule = GetModuleHandle(nullptr);
    if (gameModule == nullptr)
    {
        LOG_ERROR("[C024] rejected: main module handle unavailable");
        return;
    }

    const size_t moduleSize = FgC024GetMainModuleSizeOfImage(gameModule);
    if (moduleSize == 0)
    {
        LOG_ERROR("[C024] rejected: main module SizeOfImage unavailable");
        return;
    }

    const uintptr_t gameBase = reinterpret_cast<uintptr_t>(gameModule);
    auto* target = reinterpret_cast<uint8_t*>(gameBase + kMhwSwapchainWaitHelperRva);
    fgC024ExpectedCallerReturn = reinterpret_cast<void*>(gameBase + kMhwSwapchainWaitCallerReturnRva);

    if (!FgC024IsOffsetWithinMainModule(moduleSize, kMhwSwapchainWaitHelperRva,
                                       sizeof(kMhwSwapchainWaitHelperSignature)))
    {
        LOG_ERROR("[C024] rejected: target outside main module image at base+0x{:X}", kMhwSwapchainWaitHelperRva);
        return;
    }

    if (!FgC024IsOffsetWithinMainModule(moduleSize, kMhwSwapchainWaitCallerReturnRva, 1))
    {
        LOG_ERROR("[C024] rejected: caller return outside main module at base+0x{:X}",
                  kMhwSwapchainWaitCallerReturnRva);
        return;
    }

    if (!FgC024IsReadableMemory(target, sizeof(kMhwSwapchainWaitHelperSignature)))
    {
        LOG_ERROR("[C024] rejected: target unreadable at base+0x{:X}", kMhwSwapchainWaitHelperRva);
        return;
    }

    if (memcmp(target, kMhwSwapchainWaitHelperSignature, sizeof(kMhwSwapchainWaitHelperSignature)) != 0)
    {
        LOG_ERROR("[C024] rejected: signature mismatch at base+0x{:X}", kMhwSwapchainWaitHelperRva);
        return;
    }

    o_MhwSwapchainWaitHelper = reinterpret_cast<MhwSwapchainWaitHelperFn>(target);

    const LONG beginResult = DetourTransactionBegin();
    if (beginResult != NO_ERROR)
    {
        LOG_ERROR("[C024] rejected: DetourTransactionBegin failed 0x{:08X}", static_cast<uint32_t>(beginResult));
        o_MhwSwapchainWaitHelper = nullptr;
        fgC024ExpectedCallerReturn = nullptr;
        return;
    }

    const LONG updateResult = DetourUpdateThread(GetCurrentThread());
    if (updateResult != NO_ERROR)
    {
        DetourTransactionAbort();
        LOG_ERROR("[C024] rejected: DetourUpdateThread failed 0x{:08X}", static_cast<uint32_t>(updateResult));
        o_MhwSwapchainWaitHelper = nullptr;
        fgC024ExpectedCallerReturn = nullptr;
        return;
    }

    const LONG attachResult =
        DetourAttach(reinterpret_cast<PVOID*>(&o_MhwSwapchainWaitHelper), hkMhwSwapchainWaitHelper);
    if (attachResult != NO_ERROR)
    {
        DetourTransactionAbort();
        LOG_ERROR("[C024] rejected: DetourAttach failed 0x{:08X}", static_cast<uint32_t>(attachResult));
        o_MhwSwapchainWaitHelper = nullptr;
        fgC024ExpectedCallerReturn = nullptr;
        return;
    }

    const LONG commitResult = DetourTransactionCommit();
    if (commitResult != NO_ERROR)
    {
        LOG_ERROR("[C024] rejected: DetourTransactionCommit failed 0x{:08X}", static_cast<uint32_t>(commitResult));
        o_MhwSwapchainWaitHelper = nullptr;
        fgC024ExpectedCallerReturn = nullptr;
        return;
    }

    fgC024HookInstalled = true;
#ifdef MHWFG_PRODUCTION
    LOG_INFO("[C024] installed target=base+0x{:X} callerReturn=base+0x{:X} production wait bypass (DLSSG active only)",
             kMhwSwapchainWaitHelperRva, kMhwSwapchainWaitCallerReturnRva);
#else
    LOG_INFO("[C024] installed target=base+0x{:X} callerReturn=base+0x{:X} experimental wait bypass (DLSSG active "
             "only; not a production sync fix)",
             kMhwSwapchainWaitHelperRva, kMhwSwapchainWaitCallerReturnRva);
#endif
}
} // namespace

#define XEFG_RESOURCE_REF_LIMIT 1

static ID3D12Fence* resizeFence = nullptr;
static UINT64 resizeFenceValue = 0;
static HANDLE resizeFenceEvent = nullptr;
static IUnknown* oldSwapChain = nullptr;
static ID3D12CommandQueue* currentCommandQueue = nullptr;
static bool _forcedHdrForXeFG = false;
static HANDLE _semaphore = nullptr;

#if (XEFG_RESOURCE_REF_LIMIT == 0)
inline static std::vector<void*> oldBackBuffers;
#endif

static bool CheckForFGStatus()
{
    // Need to check overlay menu parameter, goes to places it shouldn't go
    // if (!Config::Instance()->OverlayMenu.value_or_default())
    //    return false;

    if (State::Instance().activeFgInput == FGInput::NoFG || State::Instance().activeFgInput == FGInput::NvngxFG)
        return false;

    // Disable FG if amd dll is not found
    if (State::Instance().activeFgOutput == FGOutput::FSRFG)
    {
        FfxApiProxy::InitFfxDx12();
        if (!FfxApiProxy::IsFGReady())
        {
            ImGui::InsertNotification(
                { ImGuiToastType::Error, 20000, "Can't init FSR FG\nAre you missing the required DLLs?" });

            LOG_DEBUG("Can't init FfxApiProxy, disabling FGOutput");
            Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
            State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
        }
    }
    else if (State::Instance().activeFgOutput == FGOutput::XeFG && !XeFGProxy::InitXeFG())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init XeFG\nAre you missing the required DLLs?" });

        LOG_DEBUG("Can't init XeFGProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();
    }
    else if (State::Instance().activeFgOutput == FGOutput::DLSSG && !StreamlineProxy::LoadStreamline())
    {
        ImGui::InsertNotification(
            { ImGuiToastType::Error, 20000, "Can't init DLSSG Output\nAre you missing the streamline folder?" });

        LOG_DEBUG("Can't init StreamlineProxy, disabling FGOutput");
        Config::Instance()->FGOutput.set_volatile_value(FGOutput::NoFG);
        State::Instance().activeFgOutput = Config::Instance()->FGOutput.value_or_default();

        Config::Instance()->FGNvngxReplacement.set_volatile_value(FGNvngxReplacement::None);
        State::Instance().activeFgNvngx = Config::Instance()->FGNvngxReplacement.value_or_default();
    }

    if (State::Instance().activeFgOutput == FGOutput::NoFG)
    {
        LOG_WARN("FGOutput doesn't have any FG active");
        return false;
    }

    return true;
}

HRESULT FGHooks::CreateSwapChain(IDXGIFactory* pFactory, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                 IDXGISwapChain** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && !pDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (State::Instance().currentFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

                resizeFenceValue++;
                State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);

                if (resizeFence->GetCompletedValue() < resizeFenceValue)
                {
                    resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
                    // Max 5 sec
                    auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
                    LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
                }
            }

            oldSwapChain = State::Instance().currentFGSwapchain;
        }

        scResult = fg->CreateSwapchain(pFactory, cq, pDesc, ppSwapChain, true);

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            SAFE_RELEASE(resizeFence);
            SAFE_CLOSE_HANDLE(resizeFenceEvent);

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        SetFGSwapchain(*ppSwapChain, pDesc->OutputWindow);

        return S_OK;
    }

    return E_INVALIDARG;
}

HRESULT FGHooks::CreateSwapChainForHwnd(IDXGIFactory* pFactory, IUnknown* pDevice, HWND hWnd,
                                        DXGI_SWAP_CHAIN_DESC1* pDesc, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFullscreenDesc,
                                        IDXGIOutput* pRestrictToOutput, IDXGISwapChain1** ppSwapChain)
{
    if (!CheckForFGStatus())
    {
        LOG_WARN("Can't init FG Feature or invalid FGOutput setting!");
        return E_NOINTERFACE;
    }

    // Check if it's Dx12
    ID3D12CommandQueue* cq = nullptr;
    if (pDevice->QueryInterface(IID_PPV_ARGS(&cq)) != S_OK)
    {
        LOG_ERROR("FG Feature requires D3D12 Command Queue!");
        return E_INVALIDARG;
    }

    if (State::Instance().currentFG == nullptr)
    {
        // FG Init
        if (State::Instance().activeFgOutput == FGOutput::FSRFG)
        {
            State::Instance().currentFG = new FSRFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            State::Instance().currentFG = new XeFG_Dx12();
        }
        else if (State::Instance().activeFgOutput == FGOutput::DLSSG)
        {
            State::Instance().currentFG = new DLSSG_Dx12();
        }
    }

    // Create FG swapchain
    auto fg = State::Instance().currentFG;
    bool scResult = false;

    {
        ScopedSkipDxgiLoadChecks skipDxgiLoadChecks {};

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = true;

        if (State::Instance().activeFgOutput == FGOutput::XeFG && pFullscreenDesc != nullptr &&
            !pFullscreenDesc->Windowed)
            LOG_WARN("Using exclusive fullscreen with XeFG!!!");

        // These effects are not supported in DX12
        if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_SEQUENTIAL)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_SEQUENTIAL is not supported in DX12, changing to FLIP_SEQUENTIAL");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        }
        else if (pDesc->SwapEffect == DXGI_SWAP_EFFECT_DISCARD)
        {
            LOG_WARN("DXGI_SWAP_EFFECT_DISCARD is not supported in DX12, changing to FLIP_DISCARD");
            pDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        }

        // Looks like game is creating new swapchain,
        // without releasing old one, be sure gpu is in idle state
        if (State::Instance().currentFGSwapchain != nullptr)
        {
            LOG_WARN("Looks like game is creating new swapchain, without releasing old one!");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

                resizeFenceValue++;
                State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);

                if (resizeFence->GetCompletedValue() < resizeFenceValue)
                {
                    resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
                    // Max 5 sec
                    auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
                    LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
                }
            }

            oldSwapChain = State::Instance().currentFGSwapchain;
        }

        scResult = fg->CreateSwapchain1(pFactory, cq, hWnd, pDesc, pFullscreenDesc, ppSwapChain, true);

        if (Config::Instance()->FGDontUseSwapchainBuffers.value_or_default())
            State::Instance().skipHeapCapture = false;
    }

    if (scResult)
    {
        if (State::Instance().currentD3D12Device != nullptr)
        {
            SAFE_RELEASE(resizeFence);
            SAFE_CLOSE_HANDLE(resizeFenceEvent);

            State::Instance().currentD3D12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&resizeFence));
            resizeFenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        }

        SetFGSwapchain((IDXGISwapChain*) *ppSwapChain, hWnd);

        return S_OK;
    }

    return E_INVALIDARG;
}

void FGHooks::SetFGSwapchain(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _hwnd = hWnd;

    if (_dx12InteropPresentSC == pSwapChain)
    {
        _dx12InteropPresentSC = nullptr;
        _dx12InteropPresentHwnd = nullptr;
    }

    State::Instance().currentFGSwapchain = pSwapChain;
    State::Instance().currentSwapchain = pSwapChain;

    HookFGSwapchain(pSwapChain);
}

void FGHooks::SetDx12InteropPresentSC(IDXGISwapChain* pSwapChain, HWND hWnd)
{
    if (pSwapChain == nullptr)
        return;

    _dx12InteropPresentSC = pSwapChain;
    _dx12InteropPresentHwnd = hWnd;

    LOG_INFO("Set DX12 presenting swapchain: {:X}, hWnd: {:X}", (size_t) pSwapChain, (size_t) hWnd);
}

void FGHooks::ClearDx12InteropPresentSC(IUnknown* pSwapChain)
{
    if (pSwapChain == nullptr || pSwapChain != _dx12InteropPresentSC)
        return;

    LOG_INFO("Clearing DX12 presenting swapchain: {:X}", (size_t) pSwapChain);

    _dx12InteropPresentSC = nullptr;
    _dx12InteropPresentHwnd = nullptr;
}

bool FGHooks::IsDx12InteropPresentSC(IUnknown* pSwapChain)
{
    return pSwapChain != nullptr && pSwapChain == _dx12InteropPresentSC;
}

void FGHooks::HookFGSwapchain(IDXGISwapChain* pSwapChain)
{
    if (o_FGSCPresent != nullptr || pSwapChain == nullptr)
        return;

    void** pFactoryVTable = *reinterpret_cast<void***>(pSwapChain);

    o_FGRelease = (PFN_Release) pFactoryVTable[2];
    o_FGSCPresent = (PFN_Present) pFactoryVTable[8];
    o_FGSCSetFullscreenState = (PFN_SetFullscreenState) pFactoryVTable[10];
    o_FGSCGetFullscreenState = (PFN_GetFullscreenState) pFactoryVTable[11];
    o_FGSCResizeBuffers = (PFN_ResizeBuffers) pFactoryVTable[13];
    o_FGSCResizeTarget = (PFN_ResizeTarget) pFactoryVTable[14];
    o_FGSCGetFullscreenDesc = (PFN_GetFullscreenDesc) pFactoryVTable[19];
    o_FGSCPresent1 = (PFN_Present1) pFactoryVTable[22];
    o_FGSCGetFrameLatencyWaitableObject = (PFN_GetFrameLatencyWaitableObject) pFactoryVTable[33];
    o_FGSCResizeBuffers1 = (PFN_ResizeBuffers1) pFactoryVTable[39];

    if (o_FGSCPresent != nullptr)
    {
        LOG_INFO("Hooking FG SwapChain present");
        LOG_TRACE("FGRelease: {:X}", (size_t) o_FGRelease);
        LOG_TRACE("FGSCPresent: {:X}", (size_t) o_FGSCPresent);
        LOG_TRACE("FGSCSetFullscreenState: {:X}", (size_t) o_FGSCSetFullscreenState);
        LOG_TRACE("FGSCGetFullscreenState: {:X}", (size_t) o_FGSCGetFullscreenState);
        LOG_TRACE("FGSCResizeBuffers: {:X}", (size_t) o_FGSCResizeBuffers);
        LOG_TRACE("FGSCResizeTarget: {:X}", (size_t) o_FGSCResizeTarget);
        LOG_TRACE("FGSCGetFullscreenDesc: {:X}", (size_t) o_FGSCGetFullscreenDesc);
        LOG_TRACE("FGSCPresent1: {:X}", (size_t) o_FGSCPresent1);
        LOG_TRACE("FGSCResizeBuffers1: {:X}", (size_t) o_FGSCResizeBuffers1);
        LOG_TRACE("FGSCGetFrameLatencyWaitableObject: {:X}", (size_t) o_FGSCGetFrameLatencyWaitableObject);

        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());

        DetourAttach(&(PVOID&) o_FGRelease, hkFGRelease);
        DetourAttach(&(PVOID&) o_FGSCPresent, hkFGPresent);
        DetourAttach(&(PVOID&) o_FGSCResizeTarget, hkResizeTarget);
        DetourAttach(&(PVOID&) o_FGSCResizeBuffers, hkResizeBuffers);
        DetourAttach(&(PVOID&) o_FGSCSetFullscreenState, hkSetFullscreenState);

        if (o_FGSCPresent1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCPresent1, hkFGPresent1);

        if (o_FGSCResizeBuffers1 != nullptr)
            DetourAttach(&(PVOID&) o_FGSCResizeBuffers1, hkResizeBuffers1);

        if (State::Instance().activeFgOutput == FGOutput::XeFG)
        {
            if (o_FGSCGetFullscreenState != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenState, hkGetFullscreenState);

            if (o_FGSCGetFullscreenDesc != nullptr)
                DetourAttach(&(PVOID&) o_FGSCGetFullscreenDesc, hkGetFullscreenDesc);

            if ((Config::Instance()->SimulateWaitableObject.value_or_default() ||
                 (State::Instance().gameEngine == GameEngineType::Unity &&
                  State::Instance().activeFgOutput == FGOutput::XeFG)) &&
                o_FGSCGetFrameLatencyWaitableObject != nullptr)
            {
                DetourAttach(&(PVOID&) o_FGSCGetFrameLatencyWaitableObject, hkGetFrameLatencyWaitableObject);
            }
        }

        auto detourResult = DetourTransactionCommit();
        if (detourResult != NO_ERROR)
        {
            LOG_ERROR("Failed to attach detour: {:X}", detourResult);
            o_FGRelease = nullptr;
            o_FGSCPresent = nullptr;
            o_FGSCSetFullscreenState = nullptr;
            o_FGSCGetFullscreenState = nullptr;
            o_FGSCResizeBuffers = nullptr;
            o_FGSCResizeTarget = nullptr;
            o_FGSCGetFullscreenDesc = nullptr;
            o_FGSCPresent1 = nullptr;
            o_FGSCResizeBuffers1 = nullptr;
            o_FGSCGetFrameLatencyWaitableObject = nullptr;
        }
        else
        {
            FgC024TryInstallSwapchainWaitHook();
        }
    }
}

HRESULT FGHooks::hkSetFullscreenState(IDXGISwapChain* This, BOOL Fullscreen, IDXGIOutput* pTarget)
{
    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    bool modeChanged = false;
    bool orgFS = Fullscreen;
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        if (Fullscreen)
        {
            Fullscreen = false;

            if (!State::Instance().SCExclusiveFullscreen)
            {
                State::Instance().SCExclusiveFullscreen = true;
                modeChanged = true;
            }

            LOG_DEBUG("Prevented exclusive fullscreen");
        }
        else
        {
            if (State::Instance().SCExclusiveFullscreen)
            {
                modeChanged = true;
                State::Instance().SCExclusiveFullscreen = false;
            }
        }
    }

    State::Instance().realExclusiveFullscreen = Fullscreen;

    if (State::Instance().activeFgOutput == FGOutput::XeFG && Fullscreen)
        LOG_WARN("Using exclusive fullscreen with XeFG!!!");

    auto result = S_OK;

    if (!Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        result = o_FGSCSetFullscreenState(This, Fullscreen, pTarget);
        LOG_DEBUG("Fullscreen: {}, pTarget: {:X}, Result: {:X}", Fullscreen, (size_t) pTarget, (UINT) result);
    }

    if (result == S_OK && modeChanged)
    {
        LOG_DEBUG("Mode changed");

        DXGI_SWAP_CHAIN_DESC scDesc {};
        This->GetDesc(&scDesc);

        if (State::Instance().SCExclusiveFullscreen)
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

            Util::MonitorInfo info;

            if (pTarget != nullptr)
                info = Util::GetMonitorInfoForOutput(pTarget);
            else
                info = Util::GetMonitorInfoForWindow(_hwnd);

            LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                      info.y, wstring_to_string(info.name));

            SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        else
        {
            SetWindowLongPtr(_hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_OVERLAPPEDWINDOW);
            SetWindowPos(_hwnd, nullptr, 0, 0, scDesc.BufferDesc.Width, scDesc.BufferDesc.Height,
                         SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    return result;
}

HANDLE FGHooks::hkGetFrameLatencyWaitableObject(IDXGISwapChain2* This)
{
    if (State::Instance().activeFgOutput != FGOutput::XeFG)
        return o_FGSCGetFrameLatencyWaitableObject(This);

    if (_semaphore == nullptr)
    {
        _semaphore = CreateSemaphore(nullptr, 16, 16, nullptr);

        if (_semaphore == nullptr)
            return nullptr;
    }

    HANDLE duplicatedHandle = nullptr;

    if (!DuplicateHandle(GetCurrentProcess(), _semaphore, GetCurrentProcess(), &duplicatedHandle, 0, FALSE,
                         DUPLICATE_SAME_ACCESS))
    {
        return nullptr;
    }

    return duplicatedHandle;
}

HRESULT FGHooks::hkGetFullscreenDesc(IDXGISwapChain1* This, DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc)
{
    auto result = o_FGSCGetFullscreenDesc(This, pDesc);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        pDesc->Windowed = false;
    }

    return result;
}

HRESULT FGHooks::hkGetFullscreenState(IDXGISwapChain* This, BOOL* pFullscreen, IDXGIOutput** ppTarget)
{
    auto result = o_FGSCGetFullscreenState(This, pFullscreen, ppTarget);

    if (result == S_OK && State::Instance().SCExclusiveFullscreen &&
        Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        *pFullscreen = true;
    }

    return result;
}

HRESULT FGHooks::hkResizeBuffers(IDXGISwapChain* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                 UINT SwapChainFlags)
{
    // Skip XeFG's internal call
    if (_skipResize)
    {
        LOG_DEBUG("XeFG call skipping");
        _skipResize = false;

        IDXGISwapChain* sc = nullptr;

        if (sc != nullptr)
        {
            auto result = sc->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
            LOG_DEBUG("XeFG internal ResizeBuffers result: {:X}", (UINT) result);
            return result;
        }

        return o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", BufferCount, Width, Height,
              (UINT) NewFormat, SwapChainFlags);

    auto fgDx12 = State::Instance().currentFG;
    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (NewFormat == desc.BufferDesc.Format || NewFormat == 0) &&
                State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();

                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // Docs say you cannot do that
        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    _skipResize1 = true;

    // Release swapchain backbuffers to prevent errors when resizing
    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        for (UINT i = 0; i < 8; i++)
        {
            ID3D12Resource* backBuffer = nullptr;
            auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

            if (bbResult == S_OK)
            {
                LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
                auto refCount = backBuffer->Release();
                while (refCount > XEFG_RESOURCE_REF_LIMIT)
                {
                    LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
                    refCount = backBuffer->Release();
                }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
                oldBackBuffers.push_back(backBuffer);
#endif
            }
            else
            {
                LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                break;
            }
        }
    }

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_FGSCResizeBuffers(This, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }

    _skipResize1 = false;

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    return result;
}

HRESULT FGHooks::hkResizeTarget(IDXGISwapChain* This, const DXGI_MODE_DESC* pNewTargetParameters)
{
    if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
    {
        LOG_DEBUG("Skipping resize target.");
        return S_OK;
    }

    IFGFeature* fg = State::Instance().currentFG;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    return o_FGSCResizeTarget(This, pNewTargetParameters);
}

HRESULT FGHooks::hkResizeBuffers1(IDXGISwapChain3* This, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                  UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue)
{
    // Skip XeFG's internal call
    if (_skipResize1)
    {
        LOG_DEBUG("XeFG call skipping");
        _skipResize1 = false;

        IDXGISwapChain3* sc = nullptr;

        if (sc != nullptr)
        {
            auto result = sc->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                             ppPresentQueue);

            LOG_DEBUG("XeFG internal ResizeBuffers1 result: {:X}", (UINT) result);
            return result;
        }

        return o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                    ppPresentQueue);
    }

    if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)
    {
        LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

        resizeFenceValue++;
        State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);

        if (resizeFence->GetCompletedValue() < resizeFenceValue)
        {
            resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
            // Max 5 sec
            auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
            LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
        }
    }

    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        if (Config::Instance()->FGXeFGForceBorderless.value_or_default())
        {
            SwapChainFlags &= ~DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        }

        if (State::Instance().SCLastFlags != SwapChainFlags)
        {
            LOG_WARN("SwapChainFlags changed from {:X} to {:X}", State::Instance().SCLastFlags, SwapChainFlags);

            if (State::Instance().activeFgOutput == FGOutput::XeFG)
            {
                LOG_WARN("Preventing flag change for XeFG!");
                SwapChainFlags = State::Instance().SCLastFlags;
            }
        }
    }

    LOG_DEBUG("BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}, Caller: {}", BufferCount,
              Width, Height, (UINT) Format, SwapChainFlags, Util::WhoIsTheCaller(_ReturnAddress()));

    auto fgDx12 = State::Instance().currentFG;
    IFGFeature* fg = fgDx12;

    if (!State::Instance().SCExclusiveFullscreen && Config::Instance()->FGSkipResizeBuffers.value_or_default())
    {
        DXGI_SWAP_CHAIN_DESC desc {};
        if (This->GetDesc(&desc) == S_OK)
        {
            LOG_DEBUG("SC BufferCount: {}, Width: {}, Height: {}, NewFormat:{}, SwapChainFlags: {:X}", desc.BufferCount,
                      desc.BufferDesc.Width, desc.BufferDesc.Height, (UINT) desc.BufferDesc.Format,
                      State::Instance().SCLastFlags);

            if (BufferCount == 0)
                BufferCount = desc.BufferCount;

            if ((desc.BufferDesc.Width == Width || Width == 0) && (desc.BufferDesc.Height == Height || Height == 0) &&
                (Format == desc.BufferDesc.Format || Format == 0) && State::Instance().SCLastFlags == SwapChainFlags &&
                (BufferCount == desc.BufferCount || BufferCount == 0))
            {
                LOG_DEBUG("Skipping resize");

                if (Config::Instance()->FGModifyBufferState.value_or_default() ||
                    Config::Instance()->FGModifySCIndex.value_or_default())
                {
                    auto swapchain = ((IDXGISwapChain3*) This);
                    auto swapchainIndex = swapchain->GetCurrentBackBufferIndex();

                    if (fgDx12 != nullptr && Config::Instance()->FGModifyBufferState.value_or_default())
                    {
                        LOG_INFO("Trying to change backbuffer state to COMMON");

                        auto cmdList = fgDx12->GetUICommandList();
                        if (cmdList != nullptr)
                        {
                            for (size_t i = 0; i < desc.BufferCount; i++)
                            {
                                ID3D12Resource* backBuffer = nullptr;
                                if (swapchain->GetBuffer(swapchainIndex, IID_PPV_ARGS(&backBuffer)) == S_OK)
                                {
                                    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
                                        backBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);

                                    cmdList->ResourceBarrier(1, &barrier);

                                    backBuffer->Release();
                                }
                            }
                        }
                    }

                    if (swapchainIndex != 0 && Config ::Instance()->FGModifySCIndex.value_or_default())
                    {
                        auto presents = desc.BufferCount - swapchainIndex;

                        LOG_DEBUG("Trying to reset backbuffer index: {} with {} present calls", swapchainIndex,
                                  presents);

                        for (size_t i = 0; i < presents; i++)
                        {
                            swapchain->Present(0, 0);
                        }
                    }
                }

                return S_OK;
            }
        }

        // SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }

    State::Instance().SCAllowTearing = (SwapChainFlags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) > 0;

    State::Instance().SCLastFlags = SwapChainFlags;

    if (fg != nullptr && fg->IsActive())
    {
        State::Instance().fgChanged = true;
        fg->UpdateTarget();
        fg->Deactivate();
    }

    // Release menu render targets
    if (Config::Instance()->OverlayMenu.value_or_default())
        MenuOverlayDx::CleanupRenderTarget(false, NULL);

    // Release swapchain backbuffers to prevent errors when resizing
    if (State::Instance().activeFgOutput == FGOutput::XeFG)
    {
        for (UINT i = 0; i < 8; i++)
        {
            ID3D12Resource* backBuffer = nullptr;
            auto bbResult = This->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

            if (bbResult == S_OK)
            {
                LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
                auto refCount = backBuffer->Release();
                while (refCount > XEFG_RESOURCE_REF_LIMIT)
                {
                    LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
                    refCount = backBuffer->Release();
                }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
                oldBackBuffers.push_back(backBuffer);
#endif
            }
            else
            {
                LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                break;
            }
        }
    }

    HRESULT result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        _skipResize = true;

        result = o_FGSCResizeBuffers1(This, BufferCount, Width, Height, Format, SwapChainFlags, pCreationNodeMask,
                                      ppPresentQueue);

        _skipResize = false;
    }

    LOG_DEBUG("Result: {:X}, Caller: {}", (UINT) result, Util::WhoIsTheCaller(_ReturnAddress()));

    if (result == S_OK)
    {
        if (fg != nullptr)
        {
            State::Instance().fgChanged = true;
            fg->Deactivate();
            fg->UpdateTarget();
        }
    }

    // Resize window to cover the screen
    if (result == S_OK && Config::Instance()->FGXeFGForceBorderless.value_or_default() &&
        State::Instance().SCExclusiveFullscreen)
    {
        SetWindowLongPtr(_hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowLongPtr(_hwnd, GWL_EXSTYLE, WS_EX_APPWINDOW);

        Util::MonitorInfo info;
        info = Util::GetMonitorInfoForWindow(_hwnd);

        LOG_DEBUG("Overriding window size: {}x{}, and pos: {}x{} at monitor: {}", info.width, info.height, info.x,
                  info.y, wstring_to_string(info.name));

        SetWindowPos(_hwnd, HWND_TOP, info.x, info.y, info.width, info.height, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    return result;
}

HRESULT FGHooks::hkFGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags)
{
    // Skip XeFG's internal call
    if (_skipPresent)
    {
        LOG_DEBUG("XeFG call skipping");

        IDXGISwapChain* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present(SyncInterval, Flags);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent(This, SyncInterval, Flags);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);

    _skipPresent1 = true;
    auto result = FGPresent(This, SyncInterval, Flags, nullptr);
    _skipPresent1 = false;

    return result;
}

HRESULT FGHooks::hkFGPresent1(IDXGISwapChain1* This, UINT SyncInterval, UINT Flags,
                              const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    // Skip XeFG's internal call
    if (_skipPresent1)
    {
        LOG_DEBUG("XeFG call skipping");

        IDXGISwapChain3* sc = nullptr;

        HRESULT result;

        if (sc != nullptr)
        {
            result = sc->Present1(SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("sc->Present result: {:X}", (UINT) result);
        }
        else
        {
            result = o_FGSCPresent1(This, SyncInterval, Flags, pPresentParameters);
            LOG_DEBUG("o_FGSCPresent result: {:X}", (UINT) result);
        }

        return result;
    }

    LOG_DEBUG("SyncInterval: {}, Flags: {:X}", SyncInterval, Flags);
    _skipPresent = true;
    auto result = FGPresent(This, SyncInterval, Flags, pPresentParameters);
    _skipPresent = false;

    return result;
}

HRESULT FGHooks::FGPresent(IDXGISwapChain* This, UINT SyncInterval, UINT Flags,
                           const DXGI_PRESENT_PARAMETERS* pPresentParameters)
{
    bool pendingMhwPresentStart = false;

    _lastPresentFlags = Flags;

    auto& state = State::Instance();
    auto config = Config::Instance();

    if (state.isShuttingDown)
    {
        if (pPresentParameters == nullptr)
            return o_FGSCPresent(This, SyncInterval, Flags);
        return o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);
    }

    auto willPresent = (Flags & DXGI_PRESENT_TEST) == 0;

    if (willPresent)
    {
        state.fgLastFrame++;

        double ftDelta = 0.0f;
        auto now = Util::MillisecondsNow();

        if (_lastFGFrameTime > 0.0)
            ftDelta = now - _lastFGFrameTime;

        _lastFGFrameTime = now;
        state.lastFGFrameTime = ftDelta;

        LOG_DEBUG("flags: {:X}, Frametime: {}", Flags, ftDelta);

#ifdef LOW_LATENCY_INPUTS
        IUnknown* device = state.currentD3D12Device ? state.currentD3D12Device : (IUnknown*) state.currentD3D11Device;
        InputCommon::mark_present_start(device);
#endif
    }

    IFGFeature* fg = state.currentFG;

    if (MhwFgFailure::BlocksFgActivation())
    {
        config->FGEnabled.set_volatile_value(false);

        IFGFeature* fgForFailure = state.currentFG;
        if (fgForFailure != nullptr)
        {
            OwnedMutex& failureMutex = fgForFailure->Mutex;
            const bool acquiredHere = !failureMutex.currentThreadHoldsLock();

            if (acquiredHere)
                failureMutex.lock(2);

            if (state.currentFG == fgForFailure && fgForFailure->IsActive())
            {
                state.fgChanged = true;
                fgForFailure->Deactivate();
            }

            if (acquiredHere)
                failureMutex.unlockThis(2);
        }
    }

    if (fg != nullptr && willPresent && fg->IsActive() && !fg->IsPaused())
    {
        if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
        {
            std::optional<double> upscalerTimeOpt {};

            if (state.swapchainInteropApi == SwapchainInteropApi::Dx11wDx12 && state.currentD3D11Device != nullptr)
            {
                ID3D11DeviceContext* context = nullptr;
                state.currentD3D11Device->GetImmediateContext(&context);

                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(context); upscalerTimeOpt.has_value())
                    currentFeature->ReadDetailedGpuTimes(context, State::Instance().detailedGpuTimes);

                context->Release();
            }
            else if (state.swapchainInteropApi == SwapchainInteropApi::None && state.currentCommandQueue != nullptr)
            {
                if (upscalerTimeOpt = currentFeature->ReadUpscalerTime(state.currentCommandQueue);
                    upscalerTimeOpt.has_value())
                {
                    currentFeature->ReadDetailedGpuTimes(state.currentCommandQueue, state.detailedGpuTimes);
                }
            }

            if (upscalerTimeOpt.has_value())
            {
                auto upscalerTime = upscalerTimeOpt.value();
                if (upscalerTime < 100.0)
                {
                    State::Instance().frameTimeMutex.lock();
                    State::Instance().upscaleTimes.push_back(upscalerTime);
                    State::Instance().upscaleTimes.pop_front();
                    State::Instance().frameTimeMutex.unlock();
                }
            }
        }
    }

    bool mutexUsed = false;
    if (willPresent && fg != nullptr && fg->IsActive() && !fg->IsPaused() &&
        config->FGUseMutexForSwapchain.value_or_default() && fg->Mutex.getOwner() != 2)
    {
        LOG_TRACE("Waiting FG->Mutex 2, current: {}", fg->Mutex.getOwner());
        fg->Mutex.lock(2);
        mutexUsed = true;
        LOG_TRACE("Accuired FG->Mutex: {}", fg->Mutex.getOwner());
    }

    const bool fgFeatureActive = fg != nullptr && fg->IsActive() && !fg->IsPaused();

    sl::FrameToken* localToken = nullptr;
    sl::Result tokenResult = sl::Result::eErrorReflexAPI;
    if (willPresent && fg != nullptr && !fgFeatureActive)
        state.dlssgDetectedInterpolationCount = 0;

    if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::DLSSG)
    {
        if ((!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default()))
        {
            if (StreamlineProxy::PCLSetMarker() != nullptr)
            {
                ((IDXGISwapChain4*) This)->GetCurrentBackBufferIndex();
                const uint32_t frameId = (uint32_t) fg->FrameCount();
                tokenResult = StreamlineProxy::GetNewFrameToken()(localToken, &frameId);

                if (tokenResult == sl::Result::eOk)
                {
                    if (FgPresentIsMhwProcess())
                        pendingMhwPresentStart = true;
                    else
                        StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentStart, *localToken);
                }
            }
        }
    }

    if (willPresent && fgFeatureActive)
    {
        if (state.activeFgInput == FGInput::FSRFG)
            ffxPresentCallback();
        else if (state.activeFgInput == FGInput::FSRFG30)
            FSR3FG::ffxPresentCallback();

        fg->Present();
    }
    else if (willPresent && fg != nullptr)
    {
        LOG_TRACE("FGHooks::FGPresent: FG feature exists but is inactive/paused; pass-through present only");
    }

    if (pendingMhwPresentStart && tokenResult == sl::Result::eOk && localToken != nullptr &&
        StreamlineProxy::PCLSetMarker() != nullptr)
    {
        StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentStart, *localToken);
    }

    if (willPresent && state.swapchainInteropApi == SwapchainInteropApi::None)
    {
        ResTrack_Dx12::ClearPossibleHudless();
        Hudfix_Dx12::PresentStart();
    }

    if (willPresent && config->ForceVsync.has_value())
    {
        LOG_DEBUG("ForceVsync: {}, VsyncInterval: {}, SCAllowTearing: {}, realExclusiveFullscreen: {}",
                  config->ForceVsync.value(), config->VsyncInterval.value_or_default(), state.SCAllowTearing,
                  state.realExclusiveFullscreen);

        if (!config->ForceVsync.value())
        {
            SyncInterval = 0;

            if (state.SCAllowTearing && !state.realExclusiveFullscreen)
            {
                LOG_DEBUG("Adding DXGI_PRESENT_ALLOW_TEARING");
                Flags |= DXGI_PRESENT_ALLOW_TEARING;
            }
        }
        else
        {
            SyncInterval = config->VsyncInterval.value_or_default();

            if (SyncInterval < 1)
                SyncInterval = 1;

            LOG_DEBUG("Removing DXGI_PRESENT_ALLOW_TEARING");
            Flags &= ~DXGI_PRESENT_ALLOW_TEARING;
        }

        LOG_DEBUG("Final SyncInterval: {}", SyncInterval);
    }

    if (willPresent)
        state.fgPresentIsCalled = true;

    HRESULT result;
    if (pPresentParameters == nullptr)
        result = o_FGSCPresent(This, SyncInterval, Flags);
    else
        result = o_FGSCPresent1((IDXGISwapChain1*) This, SyncInterval, Flags, pPresentParameters);

    if (result == S_OK)
    {
        LOG_DEBUG("Result: {:X}", result);
    }
    else
    {
        if (result == DXGI_ERROR_DEVICE_REMOVED && state.currentD3D12Device != nullptr)
            Util::GetDeviceRemovedReason(state.currentD3D12Device);
    }

    if (tokenResult == sl::Result::eOk && localToken != nullptr && fgFeatureActive &&
        (!ReflexHooks::gameIsSendingMarkers() || !config->FGDLSSGUseGamesReflexMarkers.value_or_default()) &&
        willPresent && state.activeFgOutput == FGOutput::DLSSG)
    {
        if (StreamlineProxy::PCLSetMarker() != nullptr)
            StreamlineProxy::PCLSetMarker()(sl::PCLMarker::ePresentEnd, *localToken);

        LOG_DEBUG("Calling ReflexSleep");
        StreamlineProxy::ReflexSleep()(*localToken);
    }

    if (state.swapchainInteropApi == SwapchainInteropApi::None)
        Hudfix_Dx12::PresentEnd();

    const bool fgActiveForLimit = fg != nullptr ? fg->IsActive() && !fg->IsPaused() : false;
    const bool frameLimitPath =
        willPresent && !state.reflexLimitsFps && state.activeFgOutput != FGOutput::NoFG &&
        !IdentifyGpu::getPrimaryGpu().usesDxvk && !XellHooks::canLimit();
    const uint32_t frameLimitOutputMultiplier =
        FgPresentResolveFrameLimitOutputMultiplier(fgActiveForLimit, fg, state.activeFgOutput,
                                                 config->FGDLSSGForceDMFG.value_or_default());

    if (frameLimitPath)
        FrameLimit::sleep(fgActiveForLimit, frameLimitOutputMultiplier);

    if ((config->SimulateWaitableObject.value_or_default() ||
         (state.gameEngine == GameEngineType::Unity && state.activeFgOutput == FGOutput::XeFG)) &&
        _semaphore != nullptr)
    {
        ReleaseSemaphore(_semaphore, 1, nullptr);
    }

    if (mutexUsed && fg != nullptr)
    {
        LOG_TRACE("Releasing FG->Mutex: {}", fg->Mutex.getOwner());
        fg->Mutex.unlockThis(2);
    }

    LOG_DEBUG("Present finished");

    return result;
}

ULONG FGHooks::hkFGRelease(IUnknown* This)
{
    // We already released this one, prevent crashes
    if (This == oldSwapChain)
    {
        LOG_DEBUG("Release called on old swapchain, skipping release and returning 0");
        return 0;
    }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
    // find if this resource in oldBackBuffers, if it is, skip release and return 0
    if (oldBackBuffers.size() > 0)
    {
        for (auto it = oldBackBuffers.begin(); it != oldBackBuffers.end(); ++it)
        {
            if (*it == This)
            {
                LOG_DEBUG("Release called on old backbuffer, skipping release and returning 0");
                return 0;
            }
        }
    }
#endif // (XEFG_RESOURCE_REF_LIMIT == 0)

    static bool skipReleaseChecks = false;

    if (skipReleaseChecks || State::Instance().currentFGSwapchain != This || State::Instance().isShuttingDown)
        return o_FGRelease(This);

    This->AddRef();

    if (!Config::Instance()->FGPreserveSwapChain.value_or_default())
    {
        if (o_FGRelease(This) == 1)
        {
            LOG_DEBUG("");

            if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr &&
                resizeFenceEvent != nullptr)
            {
                LOG_DEBUG("Waiting for GPU to finish before resizing buffers");

                resizeFenceValue++;
                State::Instance().currentCommandQueue->Signal(resizeFence, resizeFenceValue);

                if (resizeFence->GetCompletedValue() < resizeFenceValue)
                {
                    resizeFence->SetEventOnCompletion(resizeFenceValue, resizeFenceEvent);
                    // Max 5 sec
                    auto waitResult = WaitForSingleObject(resizeFenceEvent, 5000);
                    LOG_DEBUG("WaitForSingleObject result: {:X}", waitResult);
                }
            }

            DXGI_SWAP_CHAIN_DESC scDesc {};
            ((IDXGISwapChain*) This)->GetDesc(&scDesc);

            // Release swapchain backbuffers to prevent errors when releasing FG swapchain
            {
                for (UINT i = 0; i < scDesc.BufferCount; i++)
                {
                    ID3D12Resource* backBuffer = nullptr;
                    auto bbResult = ((IDXGISwapChain*) This)->GetBuffer(i, IID_PPV_ARGS(&backBuffer));

                    if (bbResult == S_OK)
                    {
                        LOG_DEBUG("Backbuffer {}: {:X}", i, (size_t) backBuffer);
                        auto refCount = backBuffer->Release();
                        while (refCount > XEFG_RESOURCE_REF_LIMIT)
                        {
                            LOG_DEBUG("Releasing backbuffer {}: RefCount {}", i, refCount);
                            refCount = backBuffer->Release();
                        }

#if (XEFG_RESOURCE_REF_LIMIT == 0)
                        oldBackBuffers.push_back(backBuffer);
#endif
                    }
                    else
                    {
                        LOG_DEBUG("GetBuffer failed for index {}: {:X}", i, (UINT) bbResult);
                        break;
                    }
                }
            }

            // To prevent deadlock when FG release the swapchain
            skipReleaseChecks = true;

            if (State::Instance().currentFG != nullptr)
            {
                LOG_DEBUG("FG Swapchain released, release FG & swapchain context");
                State::Instance().currentFG->ReleaseSwapchain(_hwnd);
            }

            LOG_DEBUG("FG Swapchain released, clearing currentFGSwapchain");
            State::Instance().currentFGSwapchain = nullptr;

            if (State::Instance().currentWrappedSwapchain != nullptr &&
                State::Instance().currentSwapchainDesc.OutputWindow == _hwnd)
            {
                auto refCount = State::Instance().currentWrappedSwapchain->Release();

                while (refCount > 0 && refCount < 0xffffff00)
                {
                    refCount = State::Instance().currentWrappedSwapchain->Release();
                }

                State::Instance().currentWrappedSwapchain = nullptr;
            }

            skipReleaseChecks = false;

            return 0;
        }
    }
    else
    {
        if (o_FGRelease(This) == 1)
        {
            LOG_INFO("Preserving FG Swapchain from release");
            return 0;
        }
    }

    return o_FGRelease(This);
}
