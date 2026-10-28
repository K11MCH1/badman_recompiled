#include "badman_presenter.hpp"
#include "badman_scene_render.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#endif

namespace badman {
#if defined(_WIN32)
namespace {
using Microsoft::WRL::ComPtr;
using psprecomp::Error;

void checked(HRESULT result, const char *what) {
    if (SUCCEEDED(result)) return;
    char code[16]{};
    std::snprintf(code, sizeof(code), "0x%08lX", static_cast<unsigned long>(result));
    throw Error(std::string("GPU scanout: cannot ") + what + " (" + code + ")");
}

// One full-viewport triangle; the viewport places and scales the picture.
constexpr char shader_source[] = R"(
Texture2D image : register(t0);
SamplerState image_sampler : register(s0);
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vertex_main(uint id : SV_VertexID) {
    Vertex result;
    result.uv = float2((id << 1) & 2, id & 2);
    result.position = float4(result.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return result;
}
float4 picture_main(Vertex input) : SV_Target { return float4(image.Sample(image_sampler, input.uv).rgb, 1); }
float4 label_main(Vertex input) : SV_Target { return image.Sample(image_sampler, input.uv); }
)";

// Scene replay: the GE's sprites, triangles and clears at the window's
// resolution. Texel addressing, 1/16-texel bilinear weights and the modulate
// rounding follow the exact rasterizer; positions arrive in native pixels.
constexpr char scene_source[] = R"(
cbuffer Frame : register(b0) { float4 native; }; // Width, height, left and top of the native area shown.
cbuffer Draw : register(b1) { int2 texture_size; uint flags; uint unused; };
Texture2D<float4> scene_texture : register(t0);
struct SceneIn { float2 position : POSITION; float2 uv : TEXCOORD0; float4 color : COLOR0; };
struct SceneOut { float4 position : SV_Position; noperspective float2 uv : TEXCOORD0; noperspective float4 color : COLOR0; };
SceneOut scene_vertex(SceneIn input) {
    SceneOut output;
    output.position = float4((input.position - native.zw) / native.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    output.uv = input.uv; output.color = input.color;
    return output;
}
float4 texel(int2 at) {
    if (flags & 8u) at = at & (texture_size - 1); else at = clamp(at, int2(0, 0), texture_size - 1);
    return round(scene_texture.Load(int3(at, 0)) * 255);
}
float4 scene_pixel(SceneOut input) : SV_Target {
    float4 color = round(input.color * 255);
    if ((flags & 1u) == 0) return color / 255;
    float4 sampled;
    if (flags & 2u) {
        float2 position = input.uv - 0.5;
        float2 weight = floor(frac(position) * 16);
        int2 at = int2(floor(position));
        float4 top = floor((texel(at) * (16 - weight.x) + texel(at + int2(1, 0)) * weight.x) / 16);
        float4 bottom = floor((texel(at + int2(0, 1)) * (16 - weight.x) + texel(at + int2(1, 1)) * weight.x) / 16);
        sampled = floor((top * (16 - weight.y) + bottom * weight.y) / 16);
    } else sampled = texel(int2(floor(input.uv)));
    float3 rgb = floor(sampled.rgb * (color.rgb + 1) / 256);
    float alpha = (flags & 4u) ? floor(sampled.a * (color.a + 1) / 256) : color.a;
    return float4(rgb, alpha) / 255;
}
)";

ComPtr<ID3DBlob> compile(const char *entry, const char *profile, const char *source = shader_source) {
    ComPtr<ID3DBlob> code, errors;
    const auto result = D3DCompile(source, std::strlen(source), "BadmanScanout", nullptr, nullptr,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(result)) {
        std::string diagnostic = errors ? std::string(static_cast<const char *>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        throw Error(std::string("GPU scanout: shader ") + entry + " failed to compile: " + diagnostic);
    }
    return code;
}

LRESULT CALLBACK scanout_procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    switch (message) {
    case WM_NCHITTEST: return HTTRANSPARENT; // Input belongs to the game window.
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ValidateRect(handle, nullptr); return 0;
    }
    return DefWindowProcW(handle, message, wparam, lparam);
}
}

struct Presenter::Impl {
    HWND parent{}, child{};
    Scaling scaling{};
    bool smooth{}, interpolate{}, statistics_log{};
    // Widest picture shape for dungeon scenes (width / height); 0 keeps 480x272.
    double wide_aspect{};
    bool uses_scenes() const noexcept { return interpolate || wide_aspect > 0; }
    std::string name;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain1> swap_chain;
    ComPtr<ID3D11RenderTargetView> target;
    UINT buffer_width{}, buffer_height{};
    ComPtr<ID3D11VertexShader> vertex_shader;
    ComPtr<ID3D11PixelShader> picture_shader, label_shader;
    ComPtr<ID3D11SamplerState> picture_sampler, label_sampler;
    ComPtr<ID3D11BlendState> label_blend;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11Texture2D> picture;
    ComPtr<ID3D11ShaderResourceView> picture_view;
    std::uint32_t picture_width{}, picture_height{};
    ComPtr<ID3D11Texture2D> label_texture;
    ComPtr<ID3D11ShaderResourceView> label_view;
    int label_width{}, label_height{}, label_margin{};
    std::wstring label_text, label_base;
    UINT label_dpi{};
    // Presents per second, shown after the game's FPS while in-between frames run.
    std::uint64_t rate_presents{};
    std::chrono::steady_clock::time_point rate_start{};
    int present_rate{};

    // Scene replay resources and the two most recent scenes.
    ComPtr<ID3D11VertexShader> scene_vertex_shader;
    ComPtr<ID3D11PixelShader> scene_pixel_shader;
    ComPtr<ID3D11InputLayout> scene_layout;
    ComPtr<ID3D11Buffer> scene_vertices, frame_constants, draw_constants;
    UINT scene_vertex_capacity{};
    ComPtr<ID3D11RasterizerState> scissored;
    std::unordered_map<std::uint32_t, ComPtr<ID3D11BlendState>> blends;
    struct CachedTexture { ComPtr<ID3D11ShaderResourceView> view; std::uint32_t width{}, height{}; std::uint64_t used{}; };
    std::unordered_map<std::uint64_t, CachedTexture> textures;
    std::uint64_t texture_clock{};
    ComPtr<ID3D11Texture2D> native_target, native_readback;
    ComPtr<ID3D11RenderTargetView> native_view;
    // Matching scenes and the display time they are drawn at.
    ScenePacer pacer{false};
    std::vector<SceneGpuVertex> gpu_vertices;
    std::vector<SceneBatch> batches;
    // Statistics for BADMAN_SCENE_STATS.
    std::uint64_t mismatch_pixels{};
    // Developer capture (BADMAN_PRESENT_CAPTURE=<folder>): request.txt there,
    // holding a file name, saves the next presented picture as <name>.bmp.
    // Checks then see exactly what the window shows, whatever covers it.
    std::filesystem::path capture_folder;
    std::chrono::steady_clock::time_point next_statistics{};

    // Handed over by the game thread under the mutex.
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::uint32_t> pending, working;
    std::uint32_t pending_width{}, pending_height{};
    std::wstring pending_label;
    bool frame_ready{}, resize_requested{}, stopping{}, label_only{};
    std::string failure;
    std::atomic<bool> failed{false}, shown{true};
    std::atomic<std::uint64_t> submitted{}, presented{}, scenes{}, matching_scenes{}, interpolated{};
    std::shared_ptr<const Scene> pending_scene;
    std::thread thread;

    Impl(HWND owner, Scaling picture_scaling, bool smooth_filter, bool interpolation, double wide)
        : parent(owner), scaling(picture_scaling), smooth(smooth_filter), interpolate(interpolation),
          statistics_log(std::getenv("BADMAN_SCENE_STATS") != nullptr),
          wide_aspect(picture_scaling == Scaling::stretch ? 0.0 : std::max(0.0, wide)) {
        if (const auto folder = std::getenv("BADMAN_PRESENT_CAPTURE")) capture_folder = std::filesystem::u8path(folder);
        pacer = ScenePacer(interpolate);
        const auto instance = GetModuleHandleW(nullptr);
        constexpr auto class_name = L"BadmanScanout";
        WNDCLASSW existing{};
        if (!GetClassInfoW(instance, class_name, &existing)) {
            WNDCLASSW type{}; type.lpfnWndProc = scanout_procedure; type.hInstance = instance;
            type.hbrBackground = nullptr; type.lpszClassName = class_name;
            if (!RegisterClassW(&type)) throw Error("GPU scanout: cannot register its window class");
        }
        RECT client{}; GetClientRect(parent, &client);
        child = CreateWindowExW(0, class_name, L"", WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS,
            0, 0, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom), parent, nullptr, instance, nullptr);
        if (!child) throw Error("GPU scanout: cannot create its child window");
        try { create_device(); }
        catch (...) { DestroyWindow(child); child = nullptr; throw; }
        thread = std::thread([this] { run(); });
    }
    ~Impl() {
        { std::lock_guard lock(mutex); stopping = true; }
        wake.notify_all();
        if (thread.joinable()) thread.join();
        if (context) context->ClearState();
        target.Reset(); swap_chain.Reset();
        if (child && IsWindow(child)) DestroyWindow(child);
    }

    void create_device() {
        constexpr D3D_FEATURE_LEVEL levels[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
        auto result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            levels, 4, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
        if (result == E_INVALIDARG)
            result = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                levels + 1, 3, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, context.GetAddressOf());
        checked(result, "create a hardware Direct3D 11 device");
        ComPtr<IDXGIDevice1> dxgi_device; checked(device.As(&dxgi_device), "query the DXGI device");
        dxgi_device->SetMaximumFrameLatency(1); // Show each new frame at the next refresh, not later.
        ComPtr<IDXGIAdapter> adapter; checked(dxgi_device->GetAdapter(adapter.GetAddressOf()), "query the adapter");
        DXGI_ADAPTER_DESC adapter_desc{}; adapter->GetDesc(&adapter_desc);
        char adapter_name[256]{};
        WideCharToMultiByte(CP_UTF8, 0, adapter_desc.Description, -1, adapter_name, sizeof(adapter_name) - 1, nullptr, nullptr);
        ComPtr<IDXGIFactory2> factory; checked(adapter->GetParent(IID_PPV_ARGS(factory.GetAddressOf())), "query the DXGI factory");
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_NONE; desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        result = factory->CreateSwapChainForHwnd(device.Get(), child, &desc, nullptr, nullptr, swap_chain.GetAddressOf());
        if (FAILED(result)) { // Windows 8.1 has only the sequential flip model.
            desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
            result = factory->CreateSwapChainForHwnd(device.Get(), child, &desc, nullptr, nullptr, swap_chain.ReleaseAndGetAddressOf());
        }
        checked(result, "create the flip-model swap chain");
        factory->MakeWindowAssociation(child, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
        const auto vertex_code = compile("vertex_main", "vs_4_0");
        checked(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr,
            vertex_shader.GetAddressOf()), "create the vertex shader");
        const auto picture_code = compile("picture_main", "ps_4_0");
        checked(device->CreatePixelShader(picture_code->GetBufferPointer(), picture_code->GetBufferSize(), nullptr,
            picture_shader.GetAddressOf()), "create the picture shader");
        const auto label_code = compile("label_main", "ps_4_0");
        checked(device->CreatePixelShader(label_code->GetBufferPointer(), label_code->GetBufferSize(), nullptr,
            label_shader.GetAddressOf()), "create the label shader");
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = smooth ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        checked(device->CreateSamplerState(&sampler, picture_sampler.GetAddressOf()), "create the picture sampler");
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        checked(device->CreateSamplerState(&sampler, label_sampler.GetAddressOf()), "create the label sampler");
        D3D11_BLEND_DESC blend{}; // The label texture is premultiplied.
        blend.RenderTarget[0].BlendEnable = TRUE;
        blend.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        checked(device->CreateBlendState(&blend, label_blend.GetAddressOf()), "create the label blend state");
        D3D11_RASTERIZER_DESC rasterizer{}; rasterizer.FillMode = D3D11_FILL_SOLID; rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE;
        checked(device->CreateRasterizerState(&rasterizer, raster.GetAddressOf()), "create the rasterizer state");
        name = std::string("Direct3D 11 scanout: ") + adapter_name + ", vsync at the display refresh rate" +
            (interpolate ? ", in-between frames on" : "") + (wide_aspect > 0 ? ", wider view in the dungeon" : "");
        if (uses_scenes()) create_scene_resources();
        create_target();
    }

    void create_scene_resources() {
        const auto vertex_code = compile("scene_vertex", "vs_4_0", scene_source);
        checked(device->CreateVertexShader(vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(), nullptr,
            scene_vertex_shader.GetAddressOf()), "create the scene vertex shader");
        const auto pixel_code = compile("scene_pixel", "ps_4_0", scene_source);
        checked(device->CreatePixelShader(pixel_code->GetBufferPointer(), pixel_code->GetBufferSize(), nullptr,
            scene_pixel_shader.GetAddressOf()), "create the scene pixel shader");
        const D3D11_INPUT_ELEMENT_DESC layout[]{
            {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0}};
        checked(device->CreateInputLayout(layout, 3, vertex_code->GetBufferPointer(), vertex_code->GetBufferSize(),
            scene_layout.GetAddressOf()), "create the scene vertex layout");
        D3D11_BUFFER_DESC constants{}; constants.ByteWidth = 16; constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        checked(device->CreateBuffer(&constants, nullptr, frame_constants.GetAddressOf()), "create the scene frame constants");
        checked(device->CreateBuffer(&constants, nullptr, draw_constants.GetAddressOf()), "create the scene draw constants");
        D3D11_RASTERIZER_DESC rasterizer{}; rasterizer.FillMode = D3D11_FILL_SOLID; rasterizer.CullMode = D3D11_CULL_NONE;
        rasterizer.DepthClipEnable = TRUE; rasterizer.ScissorEnable = TRUE;
        checked(device->CreateRasterizerState(&rasterizer, scissored.GetAddressOf()), "create the scene rasterizer state");
        D3D11_TEXTURE2D_DESC native{}; native.Width = 480; native.Height = 272; native.MipLevels = 1; native.ArraySize = 1;
        native.Format = DXGI_FORMAT_R8G8B8A8_UNORM; native.SampleDesc.Count = 1; native.Usage = D3D11_USAGE_DEFAULT;
        native.BindFlags = D3D11_BIND_RENDER_TARGET;
        checked(device->CreateTexture2D(&native, nullptr, native_target.GetAddressOf()), "create the scene check target");
        checked(device->CreateRenderTargetView(native_target.Get(), nullptr, native_view.GetAddressOf()), "create the scene check view");
        native.Usage = D3D11_USAGE_STAGING; native.BindFlags = 0; native.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        checked(device->CreateTexture2D(&native, nullptr, native_readback.GetAddressOf()), "create the scene check readback");
    }
    static D3D11_BLEND source_factor(unsigned code) {
        switch (code) {
        case 0: return D3D11_BLEND_DEST_COLOR; case 1: return D3D11_BLEND_INV_DEST_COLOR;
        case 2: return D3D11_BLEND_SRC_ALPHA; case 3: return D3D11_BLEND_INV_SRC_ALPHA;
        case 4: return D3D11_BLEND_DEST_ALPHA; case 5: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return D3D11_BLEND_BLEND_FACTOR;
        }
    }
    static D3D11_BLEND destination_factor(unsigned code) {
        switch (code) {
        case 0: return D3D11_BLEND_SRC_COLOR; case 1: return D3D11_BLEND_INV_SRC_COLOR;
        case 2: return D3D11_BLEND_SRC_ALPHA; case 3: return D3D11_BLEND_INV_SRC_ALPHA;
        case 4: return D3D11_BLEND_DEST_ALPHA; case 5: return D3D11_BLEND_INV_DEST_ALPHA;
        default: return D3D11_BLEND_BLEND_FACTOR;
        }
    }
    // Key: bit 0 blend on, bits 1..11 GE blend mode, bits 12..15 write mask.
    ID3D11BlendState *blend_state(std::uint32_t key) {
        auto &state = blends[key];
        if (state) return state.Get();
        D3D11_BLEND_DESC desc{};
        auto &target = desc.RenderTarget[0];
        target.RenderTargetWriteMask = static_cast<UINT8>((key >> 12) & 15u);
        if (key & 1u) {
            const auto mode = (key >> 1) & 0x7FFu;
            static constexpr D3D11_BLEND_OP operations[]{D3D11_BLEND_OP_ADD, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_OP_REV_SUBTRACT,
                                                         D3D11_BLEND_OP_MIN, D3D11_BLEND_OP_MAX, D3D11_BLEND_OP_ADD};
            target.BlendEnable = TRUE;
            target.SrcBlend = source_factor(mode & 15u); target.DestBlend = destination_factor((mode >> 4) & 15u);
            target.BlendOp = operations[std::min((mode >> 8) & 7u, 5u)];
            target.SrcBlendAlpha = D3D11_BLEND_ONE; target.DestBlendAlpha = D3D11_BLEND_ZERO; target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        }
        checked(device->CreateBlendState(&desc, state.GetAddressOf()), "create a scene blend state");
        return state.Get();
    }
    ID3D11ShaderResourceView *scene_texture(const SceneTexture &texture) {
        auto found = textures.find(texture.key);
        if (found == textures.end()) {
            if (textures.size() >= 384) { // Drop the least recently used texture.
                auto oldest = textures.begin();
                for (auto it = textures.begin(); it != textures.end(); ++it) if (it->second.used < oldest->second.used) oldest = it;
                textures.erase(oldest);
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = texture.width; desc.Height = texture.height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE;
            desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            const D3D11_SUBRESOURCE_DATA data{texture.texels.data(), texture.width * 4u, 0};
            ComPtr<ID3D11Texture2D> resource;
            checked(device->CreateTexture2D(&desc, &data, resource.GetAddressOf()), "upload a scene texture");
            CachedTexture cached; cached.width = texture.width; cached.height = texture.height;
            checked(device->CreateShaderResourceView(resource.Get(), nullptr, cached.view.GetAddressOf()), "view a scene texture");
            found = textures.emplace(texture.key, std::move(cached)).first;
        }
        found->second.used = ++texture_clock;
        return found->second.view.Get();
    }
    // Builds the scene's triangles at interpolation position t (1 = this frame).
    // The check against the exact frame samples exactly.
    bool build_scene(const Scene &scene, const SceneMotion *moving, float t, bool exact = false,
                     const WideView *view = nullptr, const std::vector<ScenePlacement> *placement = nullptr) {
        return build_scene_geometry(gpu_vertices, batches, scene, moving, t, smooth && !exact, view, placement);
    }
    // Draws the batches built by build_scene into the bound target and viewport,
    // which shows native x from left over width pixels.
    void render_batches(const Scene &scene, const D3D11_VIEWPORT &viewport, double left = 0, double width = 480) {
        if (gpu_vertices.empty()) return;
        const auto bytes = static_cast<UINT>(gpu_vertices.size() * sizeof(SceneGpuVertex));
        if (bytes > scene_vertex_capacity) {
            D3D11_BUFFER_DESC desc{};
            desc.ByteWidth = std::max<UINT>(bytes * 2, 1u << 20); desc.Usage = D3D11_USAGE_DYNAMIC;
            desc.BindFlags = D3D11_BIND_VERTEX_BUFFER; desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            checked(device->CreateBuffer(&desc, nullptr, scene_vertices.ReleaseAndGetAddressOf()), "create the scene vertex buffer");
            scene_vertex_capacity = desc.ByteWidth;
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(scene_vertices.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "map the scene vertices");
        std::memcpy(mapped.pData, gpu_vertices.data(), bytes);
        context->Unmap(scene_vertices.Get(), 0);
        const float native[4]{static_cast<float>(width), 272, static_cast<float>(left), 0};
        context->UpdateSubresource(frame_constants.Get(), 0, nullptr, native, 0, 0);
        const UINT stride = sizeof(SceneGpuVertex), offset = 0;
        context->IASetInputLayout(scene_layout.Get());
        context->IASetVertexBuffers(0, 1, scene_vertices.GetAddressOf(), &stride, &offset);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(scene_vertex_shader.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, frame_constants.GetAddressOf());
        context->PSSetShader(scene_pixel_shader.Get(), nullptr, 0);
        context->PSSetConstantBuffers(1, 1, draw_constants.GetAddressOf());
        context->RSSetState(scissored.Get());
        context->RSSetViewports(1, &viewport);
        const auto sx = viewport.Width / static_cast<float>(width), sy = viewport.Height / 272.0f;
        const auto origin = static_cast<float>(left);
        for (const auto &batch : batches) {
            ID3D11ShaderResourceView *view = nullptr;
            std::int32_t size[4]{1, 1, 0, 0};
            if (batch.texture >= 0) {
                const auto &texture = *scene.textures[batch.texture];
                view = scene_texture(texture);
                size[0] = static_cast<std::int32_t>(texture.width); size[1] = static_cast<std::int32_t>(texture.height);
            }
            std::uint32_t constants[4]{static_cast<std::uint32_t>(size[0]), static_cast<std::uint32_t>(size[1]), batch.flags, 0};
            context->UpdateSubresource(draw_constants.Get(), 0, nullptr, constants, 0, 0);
            context->PSSetShaderResources(0, 1, &view);
            const auto factor = batch.blend_factor;
            const float blend_factor[4]{(factor & 255u) / 255.0f, ((factor >> 8) & 255u) / 255.0f, ((factor >> 16) & 255u) / 255.0f, 1.0f};
            context->OMSetBlendState(blend_state(batch.blend_key), blend_factor, 0xffffffffu);
            const D3D11_RECT scissor{static_cast<LONG>(std::lround(viewport.TopLeftX + (batch.scissor[0] - origin) * sx)),
                static_cast<LONG>(std::lround(viewport.TopLeftY + batch.scissor[1] * sy)),
                static_cast<LONG>(std::lround(viewport.TopLeftX + (batch.scissor[2] - origin) * sx)),
                static_cast<LONG>(std::lround(viewport.TopLeftY + batch.scissor[3] * sy))};
            context->RSSetScissorRects(1, &scissor);
            context->Draw(batch.count, batch.first);
        }
        ID3D11ShaderResourceView *none = nullptr;
        context->PSSetShaderResources(0, 1, &none);
    }
    // A scene is used only if it re-renders to the exact frame at native size,
    // apart from dither and rounding differences.
    bool scene_matches(const Scene &scene, const std::vector<std::uint32_t> &exact, std::uint32_t width, std::uint32_t height) {
        if (!scene.complete || width != 480 || height != 272 || exact.size() != 480u * 272u) return false;
        if (!build_scene(scene, nullptr, 1, true)) return false;
        context->OMSetRenderTargets(1, native_view.GetAddressOf(), nullptr);
        constexpr float black[4]{0, 0, 0, 0};
        context->ClearRenderTargetView(native_view.Get(), black);
        render_batches(scene, D3D11_VIEWPORT{0, 0, 480, 272, 0, 1});
        context->CopyResource(native_readback.Get(), native_target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(native_readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "read the scene check");
        const auto different = scene_mismatch(static_cast<const std::uint8_t *>(mapped.pData), mapped.RowPitch, false, exact.data());
        context->Unmap(native_readback.Get(), 0);
        mismatch_pixels += different;
        return different <= scene_mismatch_limit;
    }
    void accept_scene(std::shared_ptr<const Scene> scene, const std::vector<std::uint32_t> &exact, std::uint32_t width, std::uint32_t height) {
        const auto now = std::chrono::steady_clock::now();
        const bool matches = scene && scene_matches(*scene, exact, width, height);
        scenes.fetch_add(1, std::memory_order_relaxed);
        if (matches) matching_scenes.fetch_add(1, std::memory_order_relaxed);
        pacer.accept(matches ? std::move(scene) : nullptr, now);
        if (statistics_log && now >= next_statistics) {
            if (next_statistics != std::chrono::steady_clock::time_point{}) {
                const auto total = std::max<std::uint64_t>(1, scenes.load());
                std::cout << "[scene] scenes=" << scenes.load() << " matching=" << matching_scenes.load()
                          << " mismatch_pixels_per_scene=" << mismatch_pixels / total
                          << " paired_draws=" << pacer.paired() / total << " moving_draws=" << pacer.moving() / total
                          << " interpolated=" << interpolated.load() << " presented=" << presented.load()
                          << " interval_ms=" << pacer.interval() * 1000 << " lateness_ms=" << pacer.lateness() * 1000
                          << " holds=" << pacer.holds() << std::endl;
            }
            next_statistics = now + std::chrono::seconds(5);
        }
    }
    void create_target() {
        ComPtr<ID3D11Texture2D> buffer;
        checked(swap_chain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())), "get the swap chain buffer");
        checked(device->CreateRenderTargetView(buffer.Get(), nullptr, target.ReleaseAndGetAddressOf()), "create the render target");
        D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
        buffer_width = desc.Width; buffer_height = desc.Height;
    }
    bool resize_buffers() {
        RECT client{}; GetClientRect(child, &client);
        if (client.right <= 0 || client.bottom <= 0) return false; // Minimized: keep the old buffers.
        if (static_cast<UINT>(client.right) == buffer_width && static_cast<UINT>(client.bottom) == buffer_height) return true;
        context->OMSetRenderTargets(0, nullptr, nullptr);
        target.Reset();
        checked(swap_chain->ResizeBuffers(0, static_cast<UINT>(client.right), static_cast<UINT>(client.bottom),
            DXGI_FORMAT_UNKNOWN, 0), "resize the swap chain");
        create_target();
        return true;
    }
    void upload(const std::vector<std::uint32_t> &pixels, std::uint32_t width, std::uint32_t height) {
        if (!picture || width != picture_width || height != picture_height) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            checked(device->CreateTexture2D(&desc, nullptr, picture.ReleaseAndGetAddressOf()), "create the picture texture");
            checked(device->CreateShaderResourceView(picture.Get(), nullptr, picture_view.ReleaseAndGetAddressOf()),
                "create the picture view");
            picture_width = width; picture_height = height;
        }
        context->UpdateSubresource(picture.Get(), 0, nullptr, pixels.data(), width * 4u, 0);
    }
    // GDI renders the label's coverage once per change; the texture holds
    // premultiplied yellow text over a one-pixel black shadow.
    void update_label(const std::wstring &base, bool force) {
        label_base = base;
        auto text = base;
        // While in-between frames run, lead with the frames actually shown;
        // the game's own rate follows: "FPS: 165 (game 30.0)".
        if (!text.empty() && interpolate && pacer.scene_mode() && present_rate > 0 && text.rfind(L"FPS: ", 0) == 0)
            text = L"FPS: " + std::to_wstring(present_rate) + L" (game " + text.substr(5) + L")";
        const auto dpi = std::max<UINT>(96, GetDpiForWindow(child));
        if (!force && text == label_text && dpi == label_dpi) return;
        label_text = text; label_dpi = dpi;
        if (text.empty()) { label_view.Reset(); label_texture.Reset(); return; }
        label_margin = MulDiv(8, static_cast<int>(dpi), 96);
        const int shadow = std::max(1, MulDiv(1, static_cast<int>(dpi), 96));
        const auto font = CreateFontW(-MulDiv(13, static_cast<int>(dpi), 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (!font) throw Error("GPU scanout: cannot create the FPS label font");
        const auto dc = CreateCompatibleDC(nullptr);
        const auto previous_font = SelectObject(dc, font);
        RECT measured{};
        DrawTextW(dc, text.c_str(), -1, &measured, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        const int width = std::max(1, static_cast<int>(measured.right) + shadow);
        const int height = std::max(1, static_cast<int>(measured.bottom) + shadow);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height; info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32; info.bmiHeader.biCompression = BI_RGB;
        void *bits{};
        const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        std::vector<std::uint8_t> shadow_coverage(static_cast<std::size_t>(width) * height);
        std::vector<std::uint32_t> premultiplied(shadow_coverage.size());
        if (bitmap) {
            const auto previous_bitmap = SelectObject(dc, bitmap);
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            const auto *pixels = static_cast<const std::uint32_t *>(bits);
            const auto coverage = [&](int x, int y, auto &&store) {
                std::fill_n(static_cast<std::uint32_t *>(bits), shadow_coverage.size(), 0u);
                RECT place{x, y, width, height};
                DrawTextW(dc, text.c_str(), -1, &place, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                GdiFlush();
                for (std::size_t i = 0; i < shadow_coverage.size(); ++i) {
                    const auto value = pixels[i];
                    store(i, static_cast<std::uint8_t>(std::max({value & 255u, (value >> 8) & 255u, (value >> 16) & 255u})));
                }
            };
            coverage(shadow, shadow, [&](std::size_t i, std::uint8_t value) { shadow_coverage[i] = value; });
            coverage(0, 0, [&](std::size_t i, std::uint8_t text_value) {
                const std::uint32_t alpha = std::max(text_value, shadow_coverage[i]);
                premultiplied[i] = (alpha << 24) | (static_cast<std::uint32_t>(text_value) << 16) |
                                   (static_cast<std::uint32_t>(text_value) << 8);
            });
            SelectObject(dc, previous_bitmap);
            DeleteObject(bitmap);
        }
        SelectObject(dc, previous_font);
        DeleteDC(dc); DeleteObject(font);
        if (!bitmap) throw Error("GPU scanout: cannot render the FPS label");
        if (!label_texture || width != label_width || height != label_height) {
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = static_cast<UINT>(width); desc.Height = static_cast<UINT>(height); desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            checked(device->CreateTexture2D(&desc, nullptr, label_texture.ReleaseAndGetAddressOf()), "create the label texture");
            checked(device->CreateShaderResourceView(label_texture.Get(), nullptr, label_view.ReleaseAndGetAddressOf()),
                "create the label view");
            label_width = width; label_height = height;
        }
        context->UpdateSubresource(label_texture.Get(), 0, nullptr, premultiplied.data(), static_cast<UINT>(width) * 4u, 0);
    }
    // Draws the newest frame, or its scene at in-between position t.
    // The same placement as the GDI composition.
    D3D11_VIEWPORT picture_viewport(std::int64_t client_width, std::int64_t client_height) const {
        const std::int64_t width = picture_width, height = picture_height;
        std::int64_t target_width{}, target_height{};
        const auto factor = scaling == Scaling::integer ? std::min(client_width / width, client_height / height) : 0;
        if (scaling == Scaling::stretch) { target_width = client_width; target_height = client_height; }
        else if (factor > 0) { target_width = width * factor; target_height = height * factor; }
        else if (client_width * height <= client_height * width) {
            target_width = client_width; target_height = std::max<std::int64_t>(1, client_width * height / width);
        } else {
            target_height = client_height; target_width = std::max<std::int64_t>(1, client_height * width / height);
        }
        return D3D11_VIEWPORT{static_cast<float>((client_width - target_width) / 2), static_cast<float>((client_height - target_height) / 2),
            static_cast<float>(target_width), static_cast<float>(target_height), 0, 1};
    }
    // Native width of the wider view for this client area: the window's shape
    // up to wide_aspect. Integer scaling keeps whole multiples and an even
    // width, so the 2D layer moves by whole pixels.
    double wide_width(std::int64_t client_width, std::int64_t client_height) const {
        const auto most = 272.0 * wide_aspect;
        if (scaling == Scaling::integer) {
            const auto factor = std::min(client_width / 480, client_height / 272);
            if (factor > 0) return std::clamp(std::floor(static_cast<double>(client_width) / factor / 2) * 2, 480.0, std::max(480.0, std::floor(most)));
        }
        return std::clamp(272.0 * client_width / client_height, 480.0, std::max(480.0, most));
    }
    // As picture_viewport, for a picture width native pixels wide and 272 tall.
    D3D11_VIEWPORT wide_viewport(std::int64_t client_width, std::int64_t client_height, double width) const {
        double target_width{}, target_height{};
        const auto factor = scaling == Scaling::integer ? std::floor(std::min(client_width / width, client_height / 272.0)) : 0.0;
        if (factor > 0) { target_width = width * factor; target_height = 272 * factor; }
        else if (std::abs(client_width * 272.0 - client_height * width) < 1.0) { target_width = static_cast<double>(client_width); target_height = static_cast<double>(client_height); }
        else if (client_width * 272.0 <= client_height * width) {
            target_width = static_cast<double>(client_width); target_height = std::max(1.0, std::floor(client_width * 272.0 / width));
        } else {
            target_height = static_cast<double>(client_height); target_width = std::max(1.0, std::floor(client_height * width / 272.0));
        }
        return D3D11_VIEWPORT{static_cast<float>(std::floor((client_width - target_width) / 2)), static_cast<float>(std::floor((client_height - target_height) / 2)),
            static_cast<float>(target_width), static_cast<float>(target_height), 0, 1};
    }
    // before: the scene interpolated from, whose view the wide view moves from.
    void draw(const ScenePacer::Entry *entry, float t, const ScenePacer::Entry *before = nullptr) {
        context->OMSetRenderTargets(1, target.GetAddressOf(), nullptr);
        constexpr float black[4]{0, 0, 0, 1};
        context->ClearRenderTargetView(target.Get(), black);
        const auto client_width = static_cast<std::int64_t>(buffer_width), client_height = static_cast<std::int64_t>(buffer_height);
        bool use_scene = entry != nullptr;
        if (use_scene && picture_width && picture_height) {
            std::optional<WideView> view;
            if (wide_aspect > 0) view = wide_view(*entry->scene, wide_width(client_width, client_height));
            if (view && before && t < 1)
                if (const auto from = wide_view(*before->scene, view->width)) view->left = from->left + (view->left - from->left) * t;
            const auto motion = entry->motion.valid ? &entry->motion : nullptr;
            if (view) {
                const auto placement = wide_placement(*entry->scene, *view);
                if (build_scene(*entry->scene, motion, t, false, &*view, &placement))
                    render_batches(*entry->scene, wide_viewport(client_width, client_height, view->width), view->left, view->width);
                else use_scene = false;
            } else {
                const auto viewport = picture_viewport(client_width, client_height);
                if (build_scene(*entry->scene, motion, t)) render_batches(*entry->scene, viewport);
                else use_scene = false;
            }
        }
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(nullptr);
        context->RSSetState(raster.Get());
        context->VSSetShader(vertex_shader.Get(), nullptr, 0);
        if (!use_scene && picture && picture_width && picture_height) {
            const auto viewport = picture_viewport(client_width, client_height);
            context->RSSetViewports(1, &viewport);
            context->PSSetShader(picture_shader.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, picture_view.GetAddressOf());
            context->PSSetSamplers(0, 1, picture_sampler.GetAddressOf());
            context->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
            context->Draw(3, 0);
        }
        if (label_view) {
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->IASetInputLayout(nullptr);
            context->RSSetState(raster.Get());
            context->VSSetShader(vertex_shader.Get(), nullptr, 0);
            const D3D11_VIEWPORT viewport{static_cast<float>(label_margin), static_cast<float>(label_margin),
                static_cast<float>(label_width), static_cast<float>(label_height), 0, 1};
            context->RSSetViewports(1, &viewport);
            context->PSSetShader(label_shader.Get(), nullptr, 0);
            context->PSSetShaderResources(0, 1, label_view.GetAddressOf());
            context->PSSetSamplers(0, 1, label_sampler.GetAddressOf());
            context->OMSetBlendState(label_blend.Get(), nullptr, 0xffffffffu);
            context->Draw(3, 0);
        }
    }
    void capture_if_requested() {
        if (capture_folder.empty()) return;
        std::error_code error;
        const auto request = capture_folder / "request.txt";
        if (!std::filesystem::exists(request, error)) return;
        std::string name;
        { std::ifstream in(request); std::getline(in, name); }
        std::filesystem::remove(request, error);
        if (name.empty() || name.find_first_of("/\\:.") != std::string::npos) return;
        ComPtr<ID3D11Texture2D> buffer;
        checked(swap_chain->GetBuffer(0, IID_PPV_ARGS(buffer.GetAddressOf())), "get the buffer to capture");
        D3D11_TEXTURE2D_DESC desc{}; buffer->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ; desc.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        checked(device->CreateTexture2D(&desc, nullptr, staging.GetAddressOf()), "create the capture copy");
        context->CopyResource(staging.Get(), buffer.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        checked(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "read the capture");
        // A top-down 32-bit BMP of the BGRA back buffer.
        const auto row = desc.Width * 4u, size = 54u + row * desc.Height;
        std::vector<std::uint8_t> file(size);
        const auto put = [&](std::size_t at, std::uint32_t value) { std::memcpy(file.data() + at, &value, 4); };
        file[0] = 'B'; file[1] = 'M'; put(2, size); put(10, 54); put(14, 40); put(18, desc.Width);
        put(22, static_cast<std::uint32_t>(-static_cast<std::int32_t>(desc.Height)));
        put(26, 1u | (32u << 16)); put(34, row * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
            std::memcpy(file.data() + 54 + std::size_t(y) * row, static_cast<const std::uint8_t *>(mapped.pData) + std::size_t(y) * mapped.RowPitch, row);
        context->Unmap(staging.Get(), 0);
        const auto temporary = capture_folder / (name + ".tmp");
        { std::ofstream out(temporary, std::ios::binary); out.write(reinterpret_cast<const char *>(file.data()), static_cast<std::streamsize>(file.size())); }
        std::filesystem::rename(temporary, capture_folder / (name + ".bmp"), error);
    }
    void run() noexcept {
        std::unique_lock lock(mutex);
        for (;;) {
            // While in-between frames are due, render on every display refresh;
            // Present below waits for it. Otherwise sleep until something changes.
            if (!pacer.animating()) wake.wait(lock, [&] { return stopping || frame_ready || resize_requested || label_only; });
            // Taken right after the previous Present returned (a display refresh)
            // and before any scene work, so in-between positions stay evenly spaced.
            const auto refresh_time = std::chrono::steady_clock::now();
            if (stopping) return;
            const bool resize = std::exchange(resize_requested, false);
            const bool frame = std::exchange(frame_ready, false);
            const auto width = pending_width, height = pending_height;
            std::wstring label;
            std::shared_ptr<const Scene> scene;
            if (frame) { working.swap(pending); label = pending_label; scene = std::move(pending_scene); }
            const bool relabel = std::exchange(label_only, false) && !frame;
            if (relabel) label = pending_label;
            lock.unlock();
            try {
                const bool drawable = resize_buffers() && buffer_width && buffer_height;
                if (frame) {
                    upload(working, width, height); update_label(label, false);
                    if (uses_scenes()) accept_scene(std::move(scene), working, width, height);
                }
                else if (resize) update_label(label_base, true); // The DPI may have changed with the size.
                if (relabel) update_label(label, false);
                const auto pick = uses_scenes() ? pacer.pick(refresh_time) : ScenePacer::Pick{};
                if (pick.in_between) interpolated.fetch_add(1, std::memory_order_relaxed);
                const auto *entry = pick.entry, *before = pick.before;
                const auto t = pick.t;
                if (drawable) {
                    draw(entry, t, before);
                    capture_if_requested();
                    const auto result = swap_chain->Present(1, 0);
                    if (result != DXGI_STATUS_OCCLUDED) checked(result, "present");
                    presented.fetch_add(1, std::memory_order_relaxed);
                    ++rate_presents;
                    const auto elapsed = std::chrono::duration<double>(refresh_time - rate_start).count();
                    if (elapsed >= 1.0) {
                        const auto rate = static_cast<int>(std::lround(rate_presents / elapsed));
                        rate_presents = 0; rate_start = refresh_time;
                        if (rate != present_rate) { present_rate = rate; update_label(label_base, false); }
                    }
                }
            } catch (const std::exception &error) {
                lock.lock(); failure = error.what(); failed = true; return;
            } catch (...) {
                lock.lock(); failure = "GPU scanout: unknown failure"; failed = true; return;
            }
            lock.lock();
        }
    }
};

Presenter::Presenter(void *parent, Scaling scaling, bool smooth, bool interpolate, double wide_aspect)
    : impl_(std::make_unique<Impl>(static_cast<HWND>(parent), scaling, smooth, interpolate, wide_aspect)) {}
Presenter::~Presenter() = default;
void Presenter::submit(const std::uint32_t *pixels, std::uint32_t width, std::uint32_t height, std::wstring_view label,
                       std::shared_ptr<const Scene> scene) {
    if (impl_->failed || !pixels || !width || !height) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->pending_scene = std::move(scene);
        impl_->pending.assign(pixels, pixels + static_cast<std::size_t>(width) * height);
        impl_->pending_width = width; impl_->pending_height = height;
        impl_->pending_label.assign(label);
        impl_->frame_ready = true;
    }
    impl_->submitted.fetch_add(1, std::memory_order_relaxed);
    impl_->wake.notify_one();
}
void Presenter::set_label(std::wstring_view label) {
    if (impl_->failed) return;
    { std::lock_guard lock(impl_->mutex); impl_->pending_label.assign(label); impl_->label_only = true; }
    impl_->wake.notify_one();
}
void Presenter::resize() {
    RECT client{}; GetClientRect(impl_->parent, &client);
    MoveWindow(impl_->child, 0, 0, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom), FALSE);
    { std::lock_guard lock(impl_->mutex); impl_->resize_requested = true; }
    impl_->wake.notify_one();
}
void Presenter::set_visible(bool visible) {
    if (impl_->shown.exchange(visible) == visible) return;
    ShowWindow(impl_->child, visible ? SW_SHOWNA : SW_HIDE);
}
bool Presenter::visible() const noexcept { return impl_->shown; }
bool Presenter::interpolating() const noexcept { return impl_->interpolate; }
double Presenter::wide_width() const noexcept { return impl_->wide_aspect > 0 ? std::floor(272.0 * impl_->wide_aspect) : 0.0; }
bool Presenter::failed() const noexcept { return impl_->failed; }
std::string Presenter::error() const { std::lock_guard lock(impl_->mutex); return impl_->failure; }
std::string Presenter::description() const { return impl_->name; }
Presenter::Statistics Presenter::statistics() const {
    return {impl_->submitted.load(std::memory_order_relaxed), impl_->presented.load(std::memory_order_relaxed),
            impl_->scenes.load(std::memory_order_relaxed), impl_->matching_scenes.load(std::memory_order_relaxed),
            impl_->interpolated.load(std::memory_order_relaxed)};
}
#else
struct Presenter::Impl {};
Presenter::Presenter(void *, Scaling, bool, bool, double) { throw psprecomp::Error("GPU scanout requires Windows"); }
Presenter::~Presenter() = default;
void Presenter::submit(const std::uint32_t *, std::uint32_t, std::uint32_t, std::wstring_view, std::shared_ptr<const Scene>) {}
void Presenter::set_label(std::wstring_view) {}
void Presenter::resize() {}
void Presenter::set_visible(bool) {}
bool Presenter::visible() const noexcept { return false; }
bool Presenter::interpolating() const noexcept { return false; }
double Presenter::wide_width() const noexcept { return 0; }
bool Presenter::failed() const noexcept { return true; }
std::string Presenter::error() const { return "GPU scanout requires Windows"; }
std::string Presenter::description() const { return {}; }
Presenter::Statistics Presenter::statistics() const { return {}; }
#endif
}
