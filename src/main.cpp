#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <imgui-node-editor/imgui_node_editor.h>

#include "node_graph.h"
#include "obj_exporter.h"
#include "ui/UiTheme.h"
#include "Version.h"

using Microsoft::WRL::ComPtr;
namespace ed = ax::NodeEditor;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
constexpr int kFrameCount = 2;
constexpr int kSrvDescriptorCount = 64;

struct FrameContext
{
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    UINT64 fenceValue = 0;
};

HWND g_hwnd = nullptr;
UINT g_width = 1600;
UINT g_height = 900;
bool g_running = true;

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_commandQueue;
ComPtr<IDXGISwapChain3> g_swapChain;
ComPtr<ID3D12DescriptorHeap> g_rtvHeap;
ComPtr<ID3D12DescriptorHeap> g_srvHeap;
ComPtr<ID3D12GraphicsCommandList> g_commandList;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_fenceEvent = nullptr;
UINT64 g_fenceLastSignaledValue = 0;
UINT g_frameIndex = 0;
UINT g_rtvDescriptorSize = 0;
UINT g_srvDescriptorSize = 0;

std::array<FrameContext, kFrameCount> g_frameContexts;
std::array<ComPtr<ID3D12Resource>, kFrameCount> g_renderTargets;
std::array<bool, kSrvDescriptorCount> g_srvDescriptorUsed{};
ed::EditorContext* g_nodeEditor = nullptr;
bool g_nodePositionsInitialized = false;
rock::NodeGraph g_graph = rock::NodeGraph::CreateDefaultRockGraph();
std::string g_exportStatus = "No export yet";
rock::UiThemeManager g_themeManager;
rock::GraphId g_selectedNodeId = 0;

struct UiState
{
    int primitive = 0;
    float noiseAmplitude = 0.35f;
    float noiseFrequency = 2.0f;
    int noiseOctaves = 4;
    float crackWidth = 0.035f;
    float crackDepth = 0.42f;
    float crackRoughness = 0.65f;
    bool meshPreview = true;
    bool sdfPreview = false;
};

UiState g_ui;

struct ViewportState
{
    float yaw = 0.65f;
    float pitch = 0.42f;
    float zoom = 1.0f;
    ImVec2 pan = ImVec2(0.0f, 0.0f);
};

ViewportState g_viewport;

std::wstring MakeWindowTitle()
{
    std::wstring title = L"Rock Generator ";
    for (const char c : std::string(ROCK_GENERATOR_VERSION_STRING))
    {
        title.push_back(static_cast<wchar_t>(c));
    }
    return title;
}

void ThrowIfFailed(HRESULT hr, const char* message)
{
    if (FAILED(hr))
    {
        throw std::runtime_error(message);
    }
}

void AllocateSrvDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* outCpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE* outGpuHandle)
{
    for (int i = 0; i < kSrvDescriptorCount; ++i)
    {
        if (!g_srvDescriptorUsed[i])
        {
            g_srvDescriptorUsed[i] = true;
            *outCpuHandle = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
            *outGpuHandle = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
            outCpuHandle->ptr += static_cast<SIZE_T>(i) * g_srvDescriptorSize;
            outGpuHandle->ptr += static_cast<UINT64>(i) * g_srvDescriptorSize;
            return;
        }
    }

    throw std::runtime_error("No free ImGui SRV descriptors");
}

void FreeSrvDescriptor(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    const D3D12_CPU_DESCRIPTOR_HANDLE start = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    const SIZE_T offset = cpuHandle.ptr - start.ptr;
    const int index = static_cast<int>(offset / g_srvDescriptorSize);
    if (index >= 0 && index < kSrvDescriptorCount)
    {
        g_srvDescriptorUsed[index] = false;
    }
}

void WaitForFenceValue(UINT64 value)
{
    if (g_fence->GetCompletedValue() >= value)
    {
        return;
    }

    ThrowIfFailed(g_fence->SetEventOnCompletion(value, g_fenceEvent), "SetEventOnCompletion failed");
    WaitForSingleObject(g_fenceEvent, INFINITE);
}

void WaitForLastSubmittedFrame()
{
    FrameContext& frameContext = g_frameContexts[g_frameIndex % kFrameCount];
    if (frameContext.fenceValue != 0)
    {
        WaitForFenceValue(frameContext.fenceValue);
        frameContext.fenceValue = 0;
    }
}

FrameContext& WaitForNextFrameResources()
{
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    FrameContext& frameContext = g_frameContexts[g_frameIndex % kFrameCount];
    if (frameContext.fenceValue != 0)
    {
        WaitForFenceValue(frameContext.fenceValue);
        frameContext.fenceValue = 0;
    }
    return frameContext;
}

void CreateRenderTarget()
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kFrameCount; ++i)
    {
        ThrowIfFailed(g_swapChain->GetBuffer(i, IID_PPV_ARGS(&g_renderTargets[i])), "GetBuffer failed");
        g_device->CreateRenderTargetView(g_renderTargets[i].Get(), nullptr, handle);
        handle.ptr += g_rtvDescriptorSize;
    }
}

void CleanupRenderTarget()
{
    WaitForLastSubmittedFrame();
    for (auto& target : g_renderTargets)
    {
        target.Reset();
    }
}

void ResizeSwapChain(UINT width, UINT height)
{
    if (!g_swapChain || width == 0 || height == 0)
    {
        return;
    }

    g_width = width;
    g_height = height;
    CleanupRenderTarget();
    ThrowIfFailed(g_swapChain->ResizeBuffers(kFrameCount, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT), "ResizeBuffers failed");
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();
    CreateRenderTarget();
}

void InitD3D(HWND hwnd)
{
    UINT dxgiFactoryFlags = 0;
#if defined(_DEBUG)
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
    {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif

    ComPtr<IDXGIFactory4> factory;
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2 failed");

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT adapterIndex = 0; factory->EnumAdapters1(adapterIndex, &adapter) != DXGI_ERROR_NOT_FOUND; ++adapterIndex)
    {
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0)
        {
            continue;
        }
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device))))
        {
            break;
        }
    }

    if (!g_device)
    {
        ThrowIfFailed(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_device)), "D3D12CreateDevice failed");
    }

    D3D12_COMMAND_QUEUE_DESC queueDesc{};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ThrowIfFailed(g_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_commandQueue)), "CreateCommandQueue failed");

    for (FrameContext& frameContext : g_frameContexts)
    {
        ThrowIfFailed(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frameContext.commandAllocator)), "CreateCommandAllocator failed");
    }

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
    swapChainDesc.BufferCount = kFrameCount;
    swapChainDesc.Width = g_width;
    swapChainDesc.Height = g_height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain;
    ThrowIfFailed(factory->CreateSwapChainForHwnd(g_commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain), "CreateSwapChainForHwnd failed");
    ThrowIfFailed(factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER), "MakeWindowAssociation failed");
    ThrowIfFailed(swapChain.As(&g_swapChain), "SwapChain cast failed");
    g_frameIndex = g_swapChain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc{};
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.NumDescriptors = kFrameCount;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_rtvHeap)), "CreateDescriptorHeap RTV failed");
    g_rtvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc{};
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.NumDescriptors = kSrvDescriptorCount;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(g_device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&g_srvHeap)), "CreateDescriptorHeap SRV failed");
    g_srvDescriptorSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    ThrowIfFailed(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContexts[0].commandAllocator.Get(), nullptr, IID_PPV_ARGS(&g_commandList)), "CreateCommandList failed");
    ThrowIfFailed(g_commandList->Close(), "CommandList close failed");

    ThrowIfFailed(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)), "CreateFence failed");
    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_fenceEvent)
    {
        throw std::runtime_error("CreateEvent failed");
    }

    CreateRenderTarget();
}

void CleanupD3D()
{
    WaitForLastSubmittedFrame();
    CleanupRenderTarget();
    if (g_fenceEvent)
    {
        CloseHandle(g_fenceEvent);
        g_fenceEvent = nullptr;
    }
}

void ResetViewport()
{
    g_viewport = {};
    g_viewport.yaw = 0.65f;
    g_viewport.pitch = 0.42f;
    g_viewport.zoom = 1.0f;
}

void UpdateViewportInteraction(const ImVec2& min, const ImVec2& max)
{
    ImGuiIO& io = ImGui::GetIO();
    const bool hovered = ImGui::IsMouseHoveringRect(min, max);
    if (!hovered && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) && !ImGui::IsMouseDragging(ImGuiMouseButton_Right) && !ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
    {
        return;
    }

    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
    {
        ResetViewport();
        return;
    }

    if (hovered && io.MouseWheel != 0.0f)
    {
        g_viewport.zoom *= std::pow(1.12f, io.MouseWheel);
        g_viewport.zoom = std::clamp(g_viewport.zoom, 0.35f, 4.0f);
    }

    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) && hovered)
    {
        g_viewport.yaw += io.MouseDelta.x * 0.010f;
        g_viewport.pitch += io.MouseDelta.y * 0.010f;
        g_viewport.pitch = std::clamp(g_viewport.pitch, -1.25f, 1.25f);
    }

    if ((ImGui::IsMouseDragging(ImGuiMouseButton_Right) || ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) && hovered)
    {
        g_viewport.pan.x += io.MouseDelta.x;
        g_viewport.pan.y += io.MouseDelta.y;
    }
}

ImVec2 RotatePoint(float x, float y, float z, float yaw, float pitch)
{
    const float ca = std::cos(yaw);
    const float sa = std::sin(yaw);
    const float xz = x * ca - z * sa;
    const float zz = x * sa + z * ca;
    const float cy = std::cos(pitch);
    const float sy = std::sin(pitch);
    const float yz = y * cy - zz * sy;
    const float depth = y * sy + zz * cy + 3.5f;
    const float perspective = 1.0f / depth;
    return ImVec2(xz * perspective, yz * perspective);
}

ImU32 ColorToU32(const ImVec4& color);
ImU32 ThemeColor(const std::string& name, const ImVec4& fallback);

ImU32 SdfSliceColor(float sdf)
{
    const float band = std::clamp(1.0f - std::fabs(sdf) / 0.045f, 0.0f, 1.0f);
    if (sdf < 0.0f)
    {
        const float depth = std::clamp(-sdf / 0.28f, 0.0f, 1.0f);
        const int r = static_cast<int>(58.0f + depth * 90.0f + band * 90.0f);
        const int g = static_cast<int>(85.0f + depth * 100.0f + band * 55.0f);
        const int b = static_cast<int>(78.0f + depth * 62.0f);
        return IM_COL32(r, g, b, 255);
    }

    const float distance = std::clamp(sdf / 0.35f, 0.0f, 1.0f);
    const int v = static_cast<int>(28.0f + (1.0f - distance) * 42.0f + band * 120.0f);
    return IM_COL32(v, v + 4, v + 2, 255);
}

void DrawSdfSliceOverlay(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::SdfPreviewStats& sdf)
{
    if (sdf.centerSlice.empty() || sdf.sliceResolution <= 0)
    {
        return;
    }

    const float viewportWidth = max.x - min.x;
    const float viewportHeight = max.y - min.y;
    const float panelSize = std::clamp(std::min(viewportWidth, viewportHeight) * 0.30f, 120.0f, 220.0f);
    const ImVec2 panelMin(max.x - panelSize - 18.0f, max.y - panelSize - 42.0f);
    const ImVec2 panelMax(panelMin.x + panelSize, panelMin.y + panelSize);
    const float cellSize = panelSize / static_cast<float>(sdf.sliceResolution);

    drawList->AddRectFilled(ImVec2(panelMin.x - 8.0f, panelMin.y - 28.0f), ImVec2(panelMax.x + 8.0f, panelMax.y + 8.0f), ThemeColor("panelBg", ImVec4(0.07f, 0.08f, 0.08f, 0.86f)), 6.0f);
    drawList->AddText(ImVec2(panelMin.x, panelMin.y - 22.0f), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), "SDF Center Slice");

    for (int y = 0; y < sdf.sliceResolution; ++y)
    {
        for (int x = 0; x < sdf.sliceResolution; ++x)
        {
            const float value = sdf.centerSlice[static_cast<size_t>(y * sdf.sliceResolution + x)];
            const ImVec2 a(panelMin.x + static_cast<float>(x) * cellSize, panelMin.y + static_cast<float>(y) * cellSize);
            const ImVec2 b(a.x + cellSize + 0.25f, a.y + cellSize + 0.25f);
            drawList->AddRectFilled(a, b, SdfSliceColor(value));
        }
    }

    drawList->AddRect(panelMin, panelMax, ThemeColor("viewportGrid", ImVec4(0.56f, 0.59f, 0.57f, 1.0f)), 0.0f, 0, 1.0f);
}

void DrawSurfacePointPreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::SdfPreviewStats& sdf)
{
    if (sdf.surfacePoints.empty())
    {
        return;
    }

    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.52f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.18f * g_viewport.zoom;

    for (const rock::SurfacePoint& point : sdf.surfacePoints)
    {
        ImVec2 p = RotatePoint(point.x, point.y, point.z, g_viewport.yaw, g_viewport.pitch);
        const ImVec2 screen(center.x + p.x * scale, center.y + p.y * scale);
        if (screen.x < min.x + 8.0f || screen.x > max.x - 8.0f || screen.y < min.y + 8.0f || screen.y > max.y - 8.0f)
        {
            continue;
        }

        const float nearSurface = std::clamp(1.0f - std::fabs(point.sdf) / std::max(sdf.voxelSize, 0.0001f), 0.0f, 1.0f);
        const ImVec4 base = g_themeManager.AppColor("surfacePoint", ImVec4(0.78f, 0.84f, 0.72f, 0.82f));
        drawList->AddCircleFilled(screen, 1.35f, ColorToU32(ImVec4(base.x, base.y, base.z, std::clamp(base.w + nearSurface * 0.12f, 0.0f, 1.0f))));
    }
}

void DrawSurfaceWirePreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::SdfPreviewStats& sdf)
{
    if (sdf.surfaceSegments.empty())
    {
        return;
    }

    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.52f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.18f * g_viewport.zoom;

    for (const rock::SurfaceSegment& segment : sdf.surfaceSegments)
    {
        ImVec2 a = RotatePoint(segment.ax, segment.ay, segment.az, g_viewport.yaw, g_viewport.pitch);
        ImVec2 b = RotatePoint(segment.bx, segment.by, segment.bz, g_viewport.yaw, g_viewport.pitch);
        a = ImVec2(center.x + a.x * scale, center.y + a.y * scale);
        b = ImVec2(center.x + b.x * scale, center.y + b.y * scale);

        if ((a.x < min.x && b.x < min.x) || (a.x > max.x && b.x > max.x) || (a.y < min.y && b.y < min.y) || (a.y > max.y && b.y > max.y))
        {
            continue;
        }

        drawList->AddLine(a, b, ThemeColor("surfaceWire", ImVec4(0.80f, 0.84f, 0.75f, 0.53f)), 1.0f);
    }
}

ImVec2 ProjectPreviewPoint(float x, float y, float z, const ImVec2& center, float scale)
{
    ImVec2 p = RotatePoint(x, y, z, g_viewport.yaw, g_viewport.pitch);
    return ImVec2(center.x + p.x * scale, center.y + p.y * scale);
}

void DrawSurfaceTrianglePreview(ImDrawList* drawList, const ImVec2& min, const ImVec2& max, const rock::SdfPreviewStats& sdf)
{
    if (sdf.surfaceTriangles.empty())
    {
        return;
    }

    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.52f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.18f * g_viewport.zoom;

    for (const rock::SurfaceTriangle& triangle : sdf.surfaceTriangles)
    {
        const ImVec2 a = ProjectPreviewPoint(triangle.ax, triangle.ay, triangle.az, center, scale);
        const ImVec2 b = ProjectPreviewPoint(triangle.bx, triangle.by, triangle.bz, center, scale);
        const ImVec2 c = ProjectPreviewPoint(triangle.cx, triangle.cy, triangle.cz, center, scale);

        if ((a.x < min.x && b.x < min.x && c.x < min.x) || (a.x > max.x && b.x > max.x && c.x > max.x) ||
            (a.y < min.y && b.y < min.y && c.y < min.y) || (a.y > max.y && b.y > max.y && c.y > max.y))
        {
            continue;
        }

        drawList->AddTriangleFilled(a, b, c, ThemeColor("surfaceFill", ImVec4(0.38f, 0.48f, 0.40f, 0.29f)));
        drawList->AddTriangle(a, b, c, ThemeColor("surfaceWire", ImVec4(0.80f, 0.84f, 0.75f, 0.53f)), 0.8f);
    }
}

void DrawViewportCube(const ImVec2& min, const ImVec2& max, float timeSeconds)
{
    (void)timeSeconds;
    UpdateViewportInteraction(min, max);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 center((min.x + max.x) * 0.5f + g_viewport.pan.x, (min.y + max.y) * 0.5f + g_viewport.pan.y);
    const float viewportSize = std::min(max.x - min.x, max.y - min.y);
    const float scale = viewportSize * 1.35f * g_viewport.zoom;

    drawList->AddRectFilled(min, max, ThemeColor("viewportBg", ImVec4(0.09f, 0.10f, 0.11f, 1.0f)));

    const ImU32 gridColor = ThemeColor("viewportGrid", ImVec4(0.24f, 0.27f, 0.25f, 0.35f));
    for (int i = 1; i < 8; ++i)
    {
        const float x = min.x + (max.x - min.x) * (static_cast<float>(i) / 8.0f);
        const float y = min.y + (max.y - min.y) * (static_cast<float>(i) / 8.0f);
        drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), gridColor);
        drawList->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), gridColor);
    }

    DrawSurfaceTrianglePreview(drawList, min, max, g_graph.Evaluation().previewSdf);
    DrawSurfacePointPreview(drawList, min, max, g_graph.Evaluation().previewSdf);
    DrawSurfaceWirePreview(drawList, min, max, g_graph.Evaluation().previewSdf);

    const std::array<std::array<float, 3>, 8> vertices{{
        {{-0.6f, -0.6f, -0.6f}},
        {{ 0.6f, -0.6f, -0.6f}},
        {{ 0.6f,  0.6f, -0.6f}},
        {{-0.6f,  0.6f, -0.6f}},
        {{-0.6f, -0.6f,  0.6f}},
        {{ 0.6f, -0.6f,  0.6f}},
        {{ 0.6f,  0.6f,  0.6f}},
        {{-0.6f,  0.6f,  0.6f}},
    }};

    std::array<ImVec2, 8> projected{};
    for (size_t i = 0; i < vertices.size(); ++i)
    {
        ImVec2 p = RotatePoint(vertices[i][0], vertices[i][1], vertices[i][2], g_viewport.yaw, g_viewport.pitch);
        projected[i] = ImVec2(center.x + p.x * scale, center.y + p.y * scale);
    }

    const std::array<std::array<int, 2>, 12> edges{{
        {{0, 1}}, {{1, 2}}, {{2, 3}}, {{3, 0}},
        {{4, 5}}, {{5, 6}}, {{6, 7}}, {{7, 4}},
        {{0, 4}}, {{1, 5}}, {{2, 6}}, {{3, 7}},
    }};

    for (const auto& edge : edges)
    {
        drawList->AddLine(projected[edge[0]], projected[edge[1]], ThemeColor("surfaceWire", ImVec4(0.80f, 0.84f, 0.75f, 0.53f)), 2.0f);
    }

    const std::string title = "SDF Preview: " + std::string(rock::ToString(g_graph.Preview()));
    drawList->AddText(ImVec2(min.x + 16.0f, min.y + 14.0f), ThemeColor("accentText", ImVec4(0.86f, 0.88f, 0.85f, 1.0f)), title.c_str());
    drawList->AddText(ImVec2(min.x + 16.0f, min.y + 36.0f), ThemeColor("mutedText", ImVec4(0.54f, 0.59f, 0.56f, 1.0f)), "Debug triangles from dense SDF");
    DrawSdfSliceOverlay(drawList, min, max, g_graph.Evaluation().previewSdf);
}

ImVec4 NodeAccentColor(rock::NodeKind kind)
{
    switch (kind)
    {
    case rock::NodeKind::PrimitiveSdf:
        return ImVec4(0.53f, 0.71f, 0.61f, 1.0f);
    case rock::NodeKind::NoiseWarp:
        return ImVec4(0.46f, 0.65f, 0.76f, 1.0f);
    case rock::NodeKind::CrackField:
        return ImVec4(0.77f, 0.61f, 0.43f, 1.0f);
    case rock::NodeKind::OutputMesh:
        return ImVec4(0.70f, 0.52f, 0.62f, 1.0f);
    default:
        return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    }
}

ImVec2 InitialNodePosition(rock::NodeKind kind)
{
    switch (kind)
    {
    case rock::NodeKind::PrimitiveSdf:
        return ImVec2(24.0f, 48.0f);
    case rock::NodeKind::NoiseWarp:
        return ImVec2(24.0f, 178.0f);
    case rock::NodeKind::CrackField:
        return ImVec2(24.0f, 308.0f);
    case rock::NodeKind::OutputMesh:
        return ImVec2(24.0f, 438.0f);
    default:
        return ImVec2(24.0f, 48.0f);
    }
}

int ToGraphId(uintptr_t id)
{
    return static_cast<int>(id);
}

void EvaluateWhenParameterEditEnds()
{
    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        g_graph.Evaluate();
    }
}

void LoadJapaneseFont(ImGuiIO& io)
{
    const char* fontPaths[] = {
        "C:\\Windows\\Fonts\\meiryo.ttc",
        "C:\\Windows\\Fonts\\YuGothM.ttc",
        "C:\\Windows\\Fonts\\msgothic.ttc",
    };

    for (const char* fontPath : fontPaths)
    {
        if (!std::filesystem::exists(fontPath))
        {
            continue;
        }

        ImFont* font = io.Fonts->AddFontFromFileTTF(fontPath, 18.0f, nullptr, io.Fonts->GetGlyphRangesJapanese());
        if (font != nullptr)
        {
            return;
        }
    }

    io.Fonts->AddFontDefault();
}

ImU32 ColorToU32(const ImVec4& color)
{
    return ImGui::ColorConvertFloat4ToU32(color);
}

ImU32 ThemeColor(const std::string& name, const ImVec4& fallback)
{
    return ColorToU32(g_themeManager.AppColor(name, fallback));
}

void DrawPinLabel(const rock::Pin& pin)
{
    const bool connected = g_graph.PinHasLink(pin.id);
    const ImVec4 color = connected ? ImVec4(0.80f, 0.88f, 0.82f, 1.0f) : ImVec4(0.54f, 0.59f, 0.57f, 1.0f);
    ImGui::TextColored(color, "%s", pin.label.c_str());
}

void DrawRockNode(const rock::Node& node)
{
    ed::BeginNode(ed::NodeId(node.id));
    ImGui::PushStyleColor(ImGuiCol_Text, NodeAccentColor(node.kind));
    ImGui::TextUnformatted(node.title.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();

    if (!node.inputs.empty())
    {
        ed::BeginPin(ed::PinId(node.inputs.front().id), ed::PinKind::Input);
        DrawPinLabel(node.inputs.front());
        ed::EndPin();
    }
    else
    {
        ImGui::Dummy(ImVec2(62.0f, ImGui::GetTextLineHeight()));
    }

    if (!node.outputs.empty())
    {
        ImGui::SameLine(104.0f);
        ed::BeginPin(ed::PinId(node.outputs.front().id), ed::PinKind::Output);
        DrawPinLabel(node.outputs.front());
        ed::EndPin();
    }

    ed::EndNode();
}

void DrawNodeGraph()
{
    ed::SetCurrentEditor(g_nodeEditor);
    ed::Begin("Rock Node Graph", ImGui::GetContentRegionAvail());

    for (const rock::Node& node : g_graph.Nodes())
    {
        DrawRockNode(node);
        if (!g_nodePositionsInitialized)
        {
            ed::SetNodePosition(ed::NodeId(node.id), InitialNodePosition(node.kind));
        }
    }
    g_nodePositionsInitialized = true;

    for (const rock::Link& link : g_graph.Links())
    {
        ed::Link(ed::LinkId(link.id), ed::PinId(link.startPin), ed::PinId(link.endPin), ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f);
    }

    if (ed::BeginCreate(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 2.5f))
    {
        ed::PinId startPinId;
        ed::PinId endPinId;
        if (ed::QueryNewLink(&startPinId, &endPinId))
        {
            int startPin = ToGraphId(startPinId.Get());
            int endPin = ToGraphId(endPinId.Get());
            if (g_graph.CanCreateLink(startPin, endPin))
            {
                if (ed::AcceptNewItem(ImVec4(0.52f, 0.70f, 0.59f, 1.0f), 3.0f))
                {
                    g_graph.CreateLink(startPin, endPin);
                }
            }
            else
            {
                ed::RejectNewItem(ImVec4(0.78f, 0.28f, 0.24f, 1.0f), 2.0f);
            }
        }
    }
    ed::EndCreate();

    if (ed::BeginDelete())
    {
        ed::LinkId deletedLinkId;
        while (ed::QueryDeletedLink(&deletedLinkId))
        {
            if (ed::AcceptDeletedItem())
            {
                const int linkId = ToGraphId(deletedLinkId.Get());
                g_graph.DeleteLink(linkId);
            }
        }
    }
    ed::EndDelete();

    ed::NodeId selectedNodes[1];
    if (ed::GetSelectedNodes(selectedNodes, 1) > 0)
    {
        const rock::GraphId selectedNodeId = ToGraphId(selectedNodes[0].Get());
        g_selectedNodeId = selectedNodeId;
        if (const rock::Node* selectedNode = g_graph.FindNode(selectedNodeId))
        {
            if (g_graph.SetPreviewStage(rock::PreviewStageFor(selectedNode->kind)))
            {
                g_graph.Evaluate();
            }
        }
    }
    else
    {
        g_selectedNodeId = 0;
    }

    ed::End();
    ed::SetCurrentEditor(nullptr);
}

bool DrawPropertyComboRow(const char* label, const char* id, int* value, const char* items)
{
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);
    ImGui::PushID(id);
    ImGui::SetNextItemWidth(-1.0f);
    const bool changed = ImGui::Combo("##value", value, items);
    ImGui::PopID();
    return changed;
}

bool DrawPropertyFloatRow(const char* label, const char* id, float* value, float minValue, float maxValue, const char* dirtyReason)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 76.0f;
    const float sliderWidth = std::max(80.0f, ImGui::GetContentRegionAvail().x - inputWidth - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderFloat("##slider", value, minValue, maxValue, "%.3f"))
    {
        g_graph.MarkDirty(dirtyReason);
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (ImGui::InputFloat("##number", value, 0.0f, 0.0f, "%.3f"))
    {
        *value = std::clamp(*value, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    return editEnded;
}

bool DrawPropertyIntRow(const char* label, const char* id, int* value, int minValue, int maxValue, const char* dirtyReason)
{
    bool editEnded = false;
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableSetColumnIndex(1);

    ImGui::PushID(id);
    const float inputWidth = 58.0f;
    const float sliderWidth = std::max(80.0f, ImGui::GetContentRegionAvail().x - inputWidth - ImGui::GetStyle().ItemInnerSpacing.x);
    ImGui::SetNextItemWidth(sliderWidth);
    if (ImGui::SliderInt("##slider", value, minValue, maxValue))
    {
        g_graph.MarkDirty(dirtyReason);
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(inputWidth);
    if (ImGui::InputInt("##number", value, 0, 0))
    {
        *value = std::clamp(*value, minValue, maxValue);
        g_graph.MarkDirty(dirtyReason);
    }
    editEnded = editEnded || ImGui::IsItemDeactivatedAfterEdit();
    ImGui::PopID();
    return editEnded;
}

void DrawPropertiesPanel()
{
    const rock::Node* selectedNode = g_graph.FindNode(g_selectedNodeId);
    if (selectedNode == nullptr)
    {
        ImGui::TextDisabled("ノードを選択してください");
        ImGui::Spacing();
        ImGui::TextWrapped("選択したノードの設定だけをここに表示します。");
        return;
    }

    ImGui::TextUnformatted(selectedNode->title.c_str());
    ImGui::TextDisabled("%s", rock::ToString(selectedNode->kind).data());
    ImGui::Separator();

    rock::GraphSettings& settings = g_graph.Settings();
    if (selectedNode->kind == rock::NodeKind::PrimitiveSdf && ImGui::BeginTable("PrimitivePropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        int primitive = static_cast<int>(settings.primitive.kind);
        if (DrawPropertyComboRow("Primitive", "Primitive", &primitive, "Sphere\0Box\0Capsule\0Ellipsoid\0Rock Blob\0"))
        {
            settings.primitive.kind = static_cast<rock::PrimitiveKind>(primitive);
            g_graph.MarkDirty("Primitive changed");
            g_graph.Evaluate();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::NoiseWarp && ImGui::BeginTable("NoisePropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (DrawPropertyFloatRow("Amplitude", "NoiseAmplitude", &settings.noise.amplitude, 0.0f, 2.0f, "Noise amplitude changed"))
        {
            g_graph.Evaluate();
        }
        if (DrawPropertyFloatRow("Frequency", "NoiseFrequency", &settings.noise.frequency, 0.1f, 12.0f, "Noise frequency changed"))
        {
            g_graph.Evaluate();
        }
        if (DrawPropertyIntRow("Octaves", "NoiseOctaves", &settings.noise.octaves, 1, 8, "Noise octaves changed"))
        {
            g_graph.Evaluate();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::CrackField && ImGui::BeginTable("CrackPropertyRows", 2, ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthFixed, 112.0f);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);

        if (DrawPropertyFloatRow("Width", "CrackWidth", &settings.crack.width, 0.0f, 0.2f, "Crack width changed"))
        {
            g_graph.Evaluate();
        }
        if (DrawPropertyFloatRow("Depth", "CrackDepth", &settings.crack.depth, 0.0f, 1.0f, "Crack depth changed"))
        {
            g_graph.Evaluate();
        }
        if (DrawPropertyFloatRow("Roughness", "CrackRoughness", &settings.crack.roughness, 0.0f, 1.0f, "Crack roughness changed"))
        {
            g_graph.Evaluate();
        }

        ImGui::EndTable();
        return;
    }

    if (selectedNode->kind == rock::NodeKind::OutputMesh)
    {
        ImGui::TextWrapped("このノードは最終出力を表します。");
        ImGui::Spacing();
        if (ImGui::Button("Build Mesh"))
        {
            g_graph.Evaluate();
        }
        ImGui::SameLine();
        if (ImGui::Button("Export OBJ"))
        {
            if (g_graph.Evaluation().dirty)
            {
                g_graph.Evaluate();
            }

            std::string error;
            const std::filesystem::path exportPath = std::filesystem::path("exports") / "rock_debug.obj";
            if (rock::ExportDebugTrianglesObj(g_graph.Evaluation().finalSdf, exportPath, &error))
            {
                g_exportStatus = "Exported " + exportPath.string();
            }
            else
            {
                g_exportStatus = "Export failed: " + error;
            }
        }
        ImGui::TextWrapped("%s", g_exportStatus.c_str());
    }
}

void DrawStatsPanel()
{
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    ImGui::Text("Graph Version: %llu", static_cast<unsigned long long>(evaluation.version));
    ImGui::TextColored(evaluation.dirty ? ImVec4(0.90f, 0.64f, 0.30f, 1.0f) : ImVec4(0.54f, 0.78f, 0.58f, 1.0f), "%s", evaluation.dirty ? "Dirty" : "Evaluated");
    ImGui::TextWrapped("%s", evaluation.status.c_str());

    const rock::SdfPreviewStats& previewSdf = evaluation.previewSdf;
    if (previewSdf.totalVoxels <= 0)
    {
        return;
    }

    ImGui::SeparatorText("Preview");
    ImGui::Text("Stage: %s", rock::ToString(evaluation.previewStage).data());
    ImGui::Text("Dense SDF: %d^3", previewSdf.resolution);
    ImGui::Text("Slice: %d x %d", previewSdf.sliceResolution, previewSdf.sliceResolution);
    ImGui::Text("SDF Range: %.3f / %.3f", previewSdf.minSdf, previewSdf.maxSdf);
    ImGui::Text("Fill: %.1f%%", previewSdf.fillRatio * 100.0f);
    ImGui::Text("Volume: %.3f", previewSdf.estimatedVolume);
    ImGui::Text("Surface Points: %zu", previewSdf.surfacePoints.size());
    ImGui::Text("Surface Lines: %zu", previewSdf.surfaceSegments.size());
    ImGui::Text("Surface Triangles: %zu", previewSdf.surfaceTriangles.size());
}

void DrawAssetExportPanel()
{
    ImGui::Columns(4, nullptr, false);
    ImGui::TextUnformatted("High mesh");
    ImGui::Text("%s", g_graph.Evaluation().dirty ? "needs evaluate" : "debug triangles");
    ImGui::NextColumn();
    ImGui::TextUnformatted("LOD");
    ImGui::TextUnformatted("0 generated");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Textures");
    ImGui::TextUnformatted("normal / AO later");
    ImGui::NextColumn();
    ImGui::TextUnformatted("Export");
    if (ImGui::Button("Build Mesh"))
    {
        g_graph.Evaluate();
    }
    ImGui::SameLine();
    if (ImGui::Button("Export OBJ"))
    {
        if (g_graph.Evaluation().dirty)
        {
            g_graph.Evaluate();
        }

        std::string error;
        const std::filesystem::path exportPath = std::filesystem::path("exports") / "rock_debug.obj";
        if (rock::ExportDebugTrianglesObj(g_graph.Evaluation().finalSdf, exportPath, &error))
        {
            g_exportStatus = "Exported " + exportPath.string();
        }
        else
        {
            g_exportStatus = "Export failed: " + error;
        }
    }
    ImGui::TextWrapped("%s", g_exportStatus.c_str());
    ImGui::Columns(1);
}

void DrawUi()
{
    static const auto start = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    const float timeSeconds = std::chrono::duration<float>(now - start).count();
    constexpr ImGuiWindowFlags shellFlags =
        ImGuiWindowFlags_NoDecoration |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_MenuBar;
    constexpr ImGuiWindowFlags fixedPaneFlags =
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("Rock Generator Shell", nullptr, shellFlags);

    if (ImGui::BeginMenuBar())
    {
        if (ImGui::BeginMenu("ファイル"))
        {
            ImGui::MenuItem("新規", "Ctrl+N", false, false);
            ImGui::MenuItem("開く", "Ctrl+O", false, false);
            ImGui::MenuItem("保存", "Ctrl+S", false, false);
            ImGui::MenuItem("名前を付けて保存", nullptr, false, false);
            ImGui::Separator();
            if (ImGui::MenuItem("終了"))
            {
                PostQuitMessage(0);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("編集"))
        {
            ImGui::MenuItem("元に戻す", "Ctrl+Z", false, false);
            ImGui::MenuItem("やり直し", "Ctrl+Y", false, false);
            ImGui::Separator();
            ImGui::MenuItem("コピー", "Ctrl+C", false, false);
            ImGui::MenuItem("貼り付け", "Ctrl+V", false, false);
            ImGui::MenuItem("削除", "Delete", false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("表示"))
        {
            ImGui::MenuItem("Mesh Preview", nullptr, &g_ui.meshPreview);
            ImGui::MenuItem("SDF Raymarch Preview", nullptr, &g_ui.sdfPreview, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("設定"))
        {
            if (ImGui::BeginMenu("UIテーマ"))
            {
                for (const rock::UiThemeInfo& themeInfo : g_themeManager.ThemeInfos())
                {
                    const bool selected = themeInfo.id == g_themeManager.CurrentThemeId();
                    if (ImGui::MenuItem(themeInfo.name.c_str(), nullptr, selected))
                    {
                        g_themeManager.ApplyTheme(themeInfo.id);
                    }
                }
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::MenuItem("環境設定", nullptr, false, false);
            ImGui::MenuItem("ショートカット設定", nullptr, false, false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("ビルド"))
        {
            if (ImGui::MenuItem("グラフを評価"))
            {
                g_graph.Evaluate();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("エクスポート"))
        {
            if (ImGui::MenuItem("OBJ"))
            {
                if (g_graph.Evaluation().dirty)
                {
                    g_graph.Evaluate();
                }

                std::string error;
                const std::filesystem::path exportPath = std::filesystem::path("exports") / "rock_debug.obj";
                if (rock::ExportDebugTrianglesObj(g_graph.Evaluation().finalSdf, exportPath, &error))
                {
                    g_exportStatus = "Exported " + exportPath.string();
                }
                else
                {
                    g_exportStatus = "Export failed: " + error;
                }
            }
            ImGui::MenuItem("glTF", nullptr, false, false);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    const ImVec2 content = ImGui::GetContentRegionAvail();
    const float statusBarHeight = ImGui::GetTextLineHeight() + 16.0f;
    const float leftWidth = std::clamp(content.x * 0.24f, 260.0f, 420.0f);
    const float rightWidth = std::clamp(content.x * 0.24f, 300.0f, 460.0f);
    const float workHeight = std::max(260.0f, content.y - statusBarHeight);
    const float viewportWidth = std::max(360.0f, content.x - leftWidth - rightWidth);

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild("Left Sidebar", ImVec2(leftWidth, workHeight), true, fixedPaneFlags);
    if (ImGui::BeginTabBar("LeftSidebarTabs"))
    {
        if (ImGui::BeginTabItem("ノード"))
        {
            DrawNodeGraph();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("エクスポート"))
        {
            DrawAssetExportPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::BeginChild("Viewport", ImVec2(viewportWidth, workHeight), true, fixedPaneFlags);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + ImGui::GetContentRegionAvail().x, min.y + ImGui::GetContentRegionAvail().y);
    DrawViewportCube(min, max, timeSeconds);
    ImGui::Dummy(ImGui::GetContentRegionAvail());
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::SameLine();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 6.0f));
    ImGui::BeginChild("Right Sidebar", ImVec2(rightWidth, workHeight), true, fixedPaneFlags);
    if (ImGui::BeginTabBar("RightSidebarTabs"))
    {
        if (ImGui::BeginTabItem("プロパティ"))
        {
            DrawPropertiesPanel();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("統計"))
        {
            DrawStatsPanel();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 4.0f));
    ImGui::BeginChild("Status Bar", ImVec2(0.0f, statusBarHeight), true, fixedPaneFlags);
    const rock::EvaluationSummary& evaluation = g_graph.Evaluation();
    ImGui::Text("%s | %s | %s", evaluation.dirty ? "Dirty" : "Evaluated", rock::ToString(evaluation.previewStage).data(), g_exportStatus.c_str());
    ImGui::EndChild();
    ImGui::PopStyleVar();

    ImGui::PopStyleVar(3);

    ImGui::End();
    ImGui::PopStyleVar();
}

void RenderFrame()
{
    FrameContext& frameContext = WaitForNextFrameResources();
    ThrowIfFailed(frameContext.commandAllocator->Reset(), "CommandAllocator reset failed");

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_renderTargets[g_frameIndex].Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;

    ThrowIfFailed(g_commandList->Reset(frameContext.commandAllocator.Get(), nullptr), "CommandList reset failed");
    g_commandList->ResourceBarrier(1, &barrier);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtvHandle.ptr += static_cast<SIZE_T>(g_frameIndex) * g_rtvDescriptorSize;

    const float clearColor[4] = {0.10f, 0.11f, 0.12f, 1.0f};
    g_commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    g_commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    ID3D12DescriptorHeap* heaps[] = {g_srvHeap.Get()};
    g_commandList->SetDescriptorHeaps(1, heaps);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_commandList.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_commandList->ResourceBarrier(1, &barrier);
    ThrowIfFailed(g_commandList->Close(), "CommandList close failed");

    ID3D12CommandList* commandLists[] = {g_commandList.Get()};
    g_commandQueue->ExecuteCommandLists(1, commandLists);
    ThrowIfFailed(g_swapChain->Present(1, 0), "Present failed");

    const UINT64 fenceValue = ++g_fenceLastSignaledValue;
    ThrowIfFailed(g_commandQueue->Signal(g_fence.Get(), fenceValue), "Signal failed");
    frameContext.fenceValue = fenceValue;
}

LRESULT WINAPI WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
    {
        return true;
    }

    switch (msg)
    {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
        {
            ResizeSwapChain(static_cast<UINT>(LOWORD(lParam)), static_cast<UINT>(HIWORD(lParam)));
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
        {
            return 0;
        }
        break;
    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    try
    {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = instance;
        wc.lpszClassName = L"RockGeneratorWindow";
        RegisterClassExW(&wc);

        RECT rect{0, 0, static_cast<LONG>(g_width), static_cast<LONG>(g_height)};
        AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
        const std::wstring windowTitle = MakeWindowTitle();
        g_hwnd = CreateWindowW(wc.lpszClassName, windowTitle.c_str(), WS_OVERLAPPEDWINDOW, 100, 100, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, wc.hInstance, nullptr);
        if (!g_hwnd)
        {
            throw std::runtime_error("CreateWindow failed");
        }

        InitD3D(g_hwnd);

        ShowWindow(g_hwnd, showCommand);
        UpdateWindow(g_hwnd);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        LoadJapaneseFont(io);
        g_themeManager.LoadThemes(std::filesystem::path("data") / "ui_themes");
        g_themeManager.ApplyTheme("road_editor_dark");

        ImGui_ImplWin32_Init(g_hwnd);
        ImGui_ImplDX12_InitInfo dx12InitInfo{};
        dx12InitInfo.Device = g_device.Get();
        dx12InitInfo.CommandQueue = g_commandQueue.Get();
        dx12InitInfo.NumFramesInFlight = kFrameCount;
        dx12InitInfo.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        dx12InitInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        dx12InitInfo.SrvDescriptorHeap = g_srvHeap.Get();
        dx12InitInfo.SrvDescriptorAllocFn = AllocateSrvDescriptor;
        dx12InitInfo.SrvDescriptorFreeFn = FreeSrvDescriptor;
        ImGui_ImplDX12_Init(&dx12InitInfo);

        ed::Config nodeEditorConfig{};
        nodeEditorConfig.SettingsFile = "RockGeneratorNodeEditor.json";
        g_nodeEditor = ed::CreateEditor(&nodeEditorConfig);

        MSG msg{};
        while (g_running)
        {
            while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
                if (msg.message == WM_QUIT)
                {
                    g_running = false;
                }
            }

            if (!g_running)
            {
                break;
            }

            ImGui_ImplDX12_NewFrame();
            ImGui_ImplWin32_NewFrame();
            ImGui::NewFrame();
            DrawUi();
            ImGui::Render();
            RenderFrame();
        }

        WaitForLastSubmittedFrame();
        ed::DestroyEditor(g_nodeEditor);
        g_nodeEditor = nullptr;
        ImGui_ImplDX12_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
        CleanupD3D();
        DestroyWindow(g_hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
    }
    catch (const std::exception& ex)
    {
        MessageBoxA(nullptr, ex.what(), "Rock Generator Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    return 0;
}
