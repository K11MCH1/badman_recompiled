#include "badman_window.hpp"
#include "badman_presenter.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cwchar>
#include <exception>
#include <iostream>
#include <limits>
#include <utility>

#if defined(_WIN32)
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <xinput.h>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#elif defined(__ANDROID__)
#include "badman_android_bridge.hpp"
#endif

namespace badman {
void FrameRateCounter::update(std::uint64_t now, bool submitted) {
    if (!start_ || now < *start_) { start_ = now; frames_ = 0; rate_ = 0; }
    const auto elapsed = now - *start_;
    if (elapsed >= 1000000000ull) {
        rate_ = static_cast<double>(frames_) * 1000000000.0 / static_cast<double>(elapsed);
        frames_ = 0; start_ = now;
    }
    if (submitted) ++frames_;
}
namespace {
using psprecomp::Error;
std::uint32_t expand5(std::uint32_t value) { return (value << 3u) | (value >> 2u); }
std::uint32_t expand6(std::uint32_t value) { return (value << 2u) | (value >> 4u); }
std::uint32_t convert(std::uint32_t pixel, std::uint32_t format) {
    std::uint32_t red{}, green{}, blue{}, alpha{};
    switch (format) {
    case 0: // PSP 5650: R is the least significant component.
        red = expand5(pixel & 31u); green = expand6((pixel >> 5u) & 63u);
        blue = expand5((pixel >> 11u) & 31u); alpha = 255; break;
    case 1:
        red = expand5(pixel & 31u); green = expand5((pixel >> 5u) & 31u);
        blue = expand5((pixel >> 10u) & 31u); alpha = (pixel & 0x8000u) ? 255 : 0; break;
    case 2:
        red = (pixel & 15u) * 17u; green = ((pixel >> 4u) & 15u) * 17u;
        blue = ((pixel >> 8u) & 15u) * 17u; alpha = ((pixel >> 12u) & 15u) * 17u; break;
    case 3:
        red = pixel & 255u; green = (pixel >> 8u) & 255u;
        blue = (pixel >> 16u) & 255u; alpha = pixel >> 24u; break;
    default: throw Error("Unsupported PSP framebuffer pixel format");
    }
    return (alpha << 24u) | (red << 16u) | (green << 8u) | blue;
}
}
std::span<const std::uint8_t> framebuffer_bytes(const psprecomp::GuestMemory &memory,
    std::uint32_t address, std::uint32_t stride, std::uint32_t format,
    std::uint32_t width, std::uint32_t height) {
    using Memory = psprecomp::GuestMemory;
    if (format > 3 || !width || !height || width > 2048 || height > 2048 || stride < width || stride > 2048)
        throw Error("Invalid framebuffer dimensions, stride or format");
    const auto bytes_per_pixel = format == 3 ? 4u : 2u;
    const auto canonical = Memory::canonical(address);
    const auto footprint = (static_cast<std::uint64_t>(height - 1) * stride + width) * bytes_per_pixel;
    if ((canonical & (bytes_per_pixel - 1u)) || canonical < Memory::kVramPhysicalBase ||
        canonical >= Memory::kVramPhysicalBase + Memory::kVramAddressSpan)
        throw Error("Framebuffer must start at an aligned guest VRAM address");
    const auto physical_offset = (canonical - Memory::kVramPhysicalBase) & (Memory::kVramSize - 1u);
    if (footprint > Memory::kVramSize - physical_offset ||
        !memory.contains(address, static_cast<std::size_t>(footprint)))
        throw Error("Framebuffer exceeds its physical VRAM backing");
    // The complete footprint was checked above, including physical mirror
    // boundaries. Read that immutable contiguous backing directly rather than
    // repeating address translation and bounds checks for every LCD pixel.
    return {memory.vram_bytes().data() + physical_offset, static_cast<std::size_t>(footprint)};
}
std::vector<std::uint32_t> decode_framebuffer(std::span<const std::uint8_t> bytes, std::uint32_t stride,
    std::uint32_t format, std::uint32_t width, std::uint32_t height) {
    std::vector<std::uint32_t> result(static_cast<std::size_t>(width) * height);
    const auto *source = bytes.data();
    const auto decode = [&]<unsigned Format>() {
        constexpr auto pixel_bytes = Format == 3 ? 4u : 2u;
        for (std::uint32_t y = 0; y < height; ++y) {
            const auto *row = source + static_cast<std::size_t>(y) * stride * pixel_bytes;
            auto *target = result.data() + static_cast<std::size_t>(y) * width;
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto *pixel = row + x * pixel_bytes;
                std::uint32_t value = pixel[0] | (static_cast<std::uint32_t>(pixel[1]) << 8);
                if constexpr (Format == 3)
                    value |= (static_cast<std::uint32_t>(pixel[2]) << 16) |
                             (static_cast<std::uint32_t>(pixel[3]) << 24);
                target[x] = convert(value, Format);
            }
        }
    };
    switch (format) {
    case 0: decode.operator()<0>(); break;
    case 1: decode.operator()<1>(); break;
    case 2: decode.operator()<2>(); break;
    case 3: decode.operator()<3>(); break;
    }
    return result;
}
std::vector<std::uint32_t> decode_framebuffer_bgra(const psprecomp::GuestMemory &memory,
    std::uint32_t address, std::uint32_t stride, std::uint32_t format,
    std::uint32_t width, std::uint32_t height) {
    return decode_framebuffer(framebuffer_bytes(memory, address, stride, format, width, height), stride, format, width, height);
}

#if defined(_WIN32)
struct Window::Impl {
    HWND window{};
    // Compose every parent-window pixel offscreen; the visible DC receives only
    // the finished image. Retain the surface until the client size changes.
    struct PaintSurface {
        HDC dc{};
        HBITMAP bitmap{};
        HGDIOBJ previous{};
        int width{}, height{};
        ~PaintSurface() { reset(); }
        void reset() noexcept {
            if (dc && previous) SelectObject(dc, previous);
            if (bitmap) DeleteObject(bitmap);
            if (dc) DeleteDC(dc);
            dc=nullptr; bitmap=nullptr; previous=nullptr; width=height=0;
        }
        void ensure(HDC target, int next_width, int next_height) {
            if (dc && width==next_width && height==next_height) return;
            PaintSurface next;
            next.dc=CreateCompatibleDC(target);
            if (!next.dc) throw Error("Cannot create the framebuffer composition DC");
            next.bitmap=CreateCompatibleBitmap(target,next_width,next_height);
            if (!next.bitmap) throw Error("Cannot allocate the framebuffer composition bitmap");
            next.previous=SelectObject(next.dc,next.bitmap);
            if (!next.previous || next.previous==HGDI_ERROR) {
                next.previous=nullptr;
                throw Error("Cannot select the framebuffer composition bitmap");
            }
            next.width=next_width; next.height=next_height;
            std::swap(dc,next.dc); std::swap(bitmap,next.bitmap); std::swap(previous,next.previous);
            std::swap(width,next.width); std::swap(height,next.height);
        }
    } surface;
    DWORD owner_thread{GetCurrentThreadId()};
    std::function<void()> on_close;
    std::exception_ptr callback_error;
    std::vector<std::uint32_t> pixels;
    FrameRateCounter frame_rate;
    std::array<wchar_t,48> last_present_fps{};
    bool has_presented{};
    std::wstring utility_message;
    std::optional<Osk::Dialog> osk_dialog;
    std::optional<Osk::Response> osk_response;
    HWND osk_label{}, osk_edit{}, osk_hint{}, osk_accept{}, osk_cancel{};
    HFONT osk_font{};
    static constexpr int osk_accept_id=2001, osk_cancel_id=2002;
    std::uint32_t width{}, height{};
    bool closed{}, visible{};
    Keyboard keyboard;
    // Launcher options, read once: picture shape, filter, FPS overlay,
    // fullscreen and XInput controllers (BADMAN_* environment variables).
    struct Options {
        enum class Scaling { integer, fit, stretch } scaling{Scaling::integer};
        bool smooth{}, fps_overlay{true}, fullscreen{}, controller{true}, interpolation{}, widescreen{};
        int deadzone_percent{20};
    } options = read_options();
    static Options read_options() {
        Options result;
        const auto text = [](const char *name) { const auto v = std::getenv(name); return std::string_view(v ? v : ""); };
        if (text("BADMAN_SCALING") == "fit") result.scaling = Options::Scaling::fit;
        else if (text("BADMAN_SCALING") == "stretch") result.scaling = Options::Scaling::stretch;
        result.smooth = text("BADMAN_FILTER") == "smooth";
        result.fps_overlay = text("BADMAN_FPS_OVERLAY") != "0";
        result.fullscreen = text("BADMAN_FULLSCREEN") == "1";
        result.controller = text("BADMAN_CONTROLLER") != "0";
        result.interpolation = text("BADMAN_INTERPOLATION") == "1";
        result.widescreen = text("BADMAN_WIDESCREEN") == "1";
        if (const auto zone = text("BADMAN_STICK_DEADZONE"); !zone.empty()) {
            const auto value = std::atoi(std::string(zone).c_str());
            if (value >= 0 && value <= 90) result.deadzone_percent = value;
        }
        return result;
    }
    // GPU scanout on its own thread; GDI composition stays the fallback and
    // draws the native text-input and utility dialogs.
    std::unique_ptr<Presenter> presenter;
    bool scanout_wanted() const noexcept { return !osk_dialog && utility_message.empty(); }
    bool scanout_active() const noexcept { return presenter && presenter->visible(); }
    void sync_scanout() {
        if (presenter && presenter->failed()) {
            std::cout << presenter->error() << "; using GDI composition" << std::endl;
            presenter.reset();
            if (window) InvalidateRect(window, nullptr, FALSE);
        }
        if (!presenter) return;
        const bool show = scanout_wanted();
        if (show == presenter->visible()) return;
        presenter->set_visible(show);
        if (show && !pixels.empty()) presenter->submit(pixels.data(), width, height, scanout_label());
        if (!show && window) InvalidateRect(window, nullptr, FALSE);
    }
    std::wstring_view scanout_label() const noexcept {
        return options.fps_overlay ? std::wstring_view(last_present_fps.data()) : std::wstring_view();
    }
    void stop_scanout() noexcept {
        if (!presenter) return;
        const auto statistics = presenter->statistics();
        std::cout << "GPU scanout: " << statistics.submitted << " frames submitted, " << statistics.presented << " presented" << std::endl;
        presenter.reset();
    }
    WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};
    bool fullscreen{};
    void toggle_fullscreen() {
        const auto style = GetWindowLongPtrW(window, GWL_STYLE);
        if (!fullscreen) {
            MONITORINFO monitor{sizeof(MONITORINFO)};
            if (!GetWindowPlacement(window, &windowed_placement) ||
                !GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return;
            SetWindowLongPtrW(window, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
            SetWindowPos(window, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
            SetWindowPlacement(window, &windowed_placement);
            SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        }
        fullscreen = !fullscreen;
        InvalidateRect(window, nullptr, FALSE);
    }
    // XInput pads: Xbox controllers and the Steam Deck; other pads through
    // Steam Input or DS4Windows. Empty slots are rescanned at most every two
    // seconds because querying a disconnected slot is slow.
    DWORD pad_slot{XUSER_MAX_COUNT};
    std::chrono::steady_clock::time_point pad_scan{};
    std::optional<XINPUT_GAMEPAD> read_pad() {
        XINPUT_STATE state{};
        if (pad_slot < XUSER_MAX_COUNT && XInputGetState(pad_slot, &state) == ERROR_SUCCESS) return state.Gamepad;
        pad_slot = XUSER_MAX_COUNT;
        const auto now = std::chrono::steady_clock::now();
        if (now < pad_scan) return std::nullopt;
        pad_scan = now + std::chrono::seconds(2);
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot)
            if (XInputGetState(slot, &state) == ERROR_SUCCESS) { pad_slot = slot; return state.Gamepad; }
        return std::nullopt;
    }
    void merge_pad(InputState &input) {
        if (!options.controller) return;
        const auto pad = read_pad();
        if (!pad) return;
        static constexpr std::pair<WORD, Key> map[]{
            {XINPUT_GAMEPAD_A, Key::cross}, {XINPUT_GAMEPAD_B, Key::circle}, {XINPUT_GAMEPAD_X, Key::square},
            {XINPUT_GAMEPAD_Y, Key::triangle}, {XINPUT_GAMEPAD_START, Key::start}, {XINPUT_GAMEPAD_BACK, Key::select},
            {XINPUT_GAMEPAD_LEFT_SHOULDER, Key::left_trigger}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, Key::right_trigger},
            {XINPUT_GAMEPAD_DPAD_UP, Key::up}, {XINPUT_GAMEPAD_DPAD_DOWN, Key::down},
            {XINPUT_GAMEPAD_DPAD_LEFT, Key::left}, {XINPUT_GAMEPAD_DPAD_RIGHT, Key::right}};
        for (const auto &[bit, key] : map) if (pad->wButtons & bit) input.buttons |= static_cast<std::uint32_t>(key);
        const auto axis = [&](SHORT raw, bool invert) -> std::uint8_t {
            const int value = invert ? -static_cast<int>(raw) - 1 : raw;
            const int zone = 32767 * options.deadzone_percent / 100;
            if (std::abs(value) <= zone) return 128;
            const int sign = value < 0 ? -1 : 1;
            const int scaled = (std::abs(value) - zone) * 127 / std::max(1, 32767 - zone);
            return static_cast<std::uint8_t>(std::clamp(128 + sign * scaled, 0, 255));
        };
        input.analog_x = axis(pad->sThumbLX, false);
        input.analog_y = axis(pad->sThumbLY, true); // PSP analog grows downward.
    }
    explicit Impl(std::string_view title, std::function<void()> callback, bool show) : on_close(std::move(callback)),visible(show) {
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.data(), static_cast<int>(title.size()), nullptr, 0);
        if (length <= 0) throw Error("Framebuffer window title must be nonempty UTF-8");
        std::wstring wide_title(length, L'\0');
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, title.data(), static_cast<int>(title.size()), wide_title.data(), length);
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto instance = GetModuleHandleW(nullptr);
        constexpr auto class_name = L"BadmanGuestFramebuffer";
        WNDCLASSEXW existing{sizeof(existing)};
        if (!GetClassInfoExW(instance, class_name, &existing)) {
            // The executable's own icon (resource 1, badman.rc) when it has one.
            WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = procedure; type.hInstance = instance;
            type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
            type.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
            if (!type.hIcon) type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
            type.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)); type.lpszClassName = class_name;
            if (!RegisterClassExW(&type)) throw Error("Cannot register the guest framebuffer window");
        }
        // The largest whole multiple of 480x272 that fits about 85% of the monitor the
        // pointer is on (1x at least), centred there: the picture starts sharp and
        // large at any resolution. Fullscreen and resizing still work as before.
        const auto dpi = GetDpiForSystem();
        POINT pointer{}; GetCursorPos(&pointer);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const auto &area = monitor.rcWork;
        RECT frame{0, 0, 0, 0};
        if (!AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi))
            throw Error("Cannot size the guest framebuffer window");
        const int frame_width = frame.right - frame.left, frame_height = frame.bottom - frame.top;
        const long room_width = (area.right - area.left) * 85 / 100 - frame_width;
        const long room_height = (area.bottom - area.top) * 85 / 100 - frame_height;
        const int scale = static_cast<int>(std::max(1L, std::min(room_width / 480L, room_height / 272L)));
        const int width = 480 * scale + frame_width, height = 272 * scale + frame_height;
        window = CreateWindowExW(WS_EX_CONTROLPARENT, class_name, wide_title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            area.left + (area.right - area.left - width) / 2, area.top + (area.bottom - area.top - height) / 2, width, height,
            nullptr, nullptr, instance, this);
        if (!window) throw Error("Cannot create the guest framebuffer window");
        const auto gdi_only = std::getenv("BADMAN_PRESENTER");
        if (visible && !(gdi_only && std::string_view(gdi_only) == "gdi")) {
            try {
                presenter = std::make_unique<Presenter>(window, options.scaling == Options::Scaling::fit ? Presenter::Scaling::fit :
                    options.scaling == Options::Scaling::stretch ? Presenter::Scaling::stretch : Presenter::Scaling::integer, options.smooth,
                    options.interpolation, options.widescreen ? 64.0 / 27.0 : 0.0); // Up to 21:9.
                std::cout << presenter->description() << std::endl;
            } catch (const std::exception &error) {
                std::cout << error.what() << "; using GDI composition" << std::endl;
            }
        }
        // The launcher hides the setup console. Explicitly show this separate
        // game window even if a child inherited a hidden startup show state.
        if(visible){
            SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
            if (options.fullscreen) toggle_fullscreen();
            UpdateWindow(window);
        }
    }
    ~Impl() { stop_scanout(); if (window) DestroyWindow(window); if(osk_font)DeleteObject(osk_font); }
    void check_thread() const {
        if (GetCurrentThreadId() != owner_thread) throw Error("Framebuffer window must run on its owning thread");
    }
    void request_close() noexcept {
        if (closed) return;
        closed = true;
        keyboard.set_focused(false);
        try { if (on_close) on_close(); } catch (...) { callback_error = std::current_exception(); }
        stop_scanout();
        if (window) DestroyWindow(window);
    }
    RECT osk_panel() const noexcept {
        RECT client{};GetClientRect(window,&client);const auto dpi=GetDpiForWindow(window);
        const auto margin=MulDiv(12,dpi,96);
        const auto panel_width=std::max(1,std::min(static_cast<int>(client.right)-2*margin,MulDiv(600,dpi,96)));
        const auto lines=osk_dialog?std::clamp(osk_dialog->line_count,1u,8u):1u;
        const auto panel_height=std::min(std::max(1,static_cast<int>(client.bottom)-2*margin),
            MulDiv(156+static_cast<int>(lines)*20,dpi,96));
        return {(client.right-panel_width)/2,(client.bottom-panel_height)/2,
                (client.right+panel_width)/2,(client.bottom+panel_height)/2};
    }
    void layout_osk() noexcept {
        if(!osk_dialog||!window||!osk_edit)return;
        const auto panel=osk_panel();const auto dpi=GetDpiForWindow(window);
        const auto pad=MulDiv(16,dpi,96), gap=MulDiv(8,dpi,96), button_width=MulDiv(88,dpi,96);
        const auto button_height=MulDiv(28,dpi,96), hint_height=MulDiv(20,dpi,96);
        const auto left=static_cast<int>(panel.left)+pad,right=static_cast<int>(panel.right)-pad,top=static_cast<int>(panel.top)+pad;
        const auto buttons_y=static_cast<int>(panel.bottom)-pad-button_height;
        const auto hint_y=buttons_y-gap-hint_height;
        const auto edit_height=MulDiv(osk_dialog->line_count>1?static_cast<int>(osk_dialog->line_count)*20+8:28,dpi,96);
        const auto edit_y=hint_y-gap-edit_height;
        MoveWindow(osk_label,left,top,std::max(1,right-left),std::max(1,edit_y-top-gap),TRUE);
        MoveWindow(osk_edit,left,edit_y,std::max(1,right-left),edit_height,TRUE);
        MoveWindow(osk_hint,left,hint_y,std::max(1,right-left),hint_height,TRUE);
        MoveWindow(osk_accept,right-2*button_width-gap,buttons_y,button_width,button_height,TRUE);
        MoveWindow(osk_cancel,right-button_width,buttons_y,button_width,button_height,TRUE);
    }
    void update_osk_font() noexcept {
        if(!osk_dialog||!window)return;
        NONCLIENTMETRICSW metrics{};metrics.cbSize=sizeof(metrics);
        if(!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS,sizeof(metrics),&metrics,0,GetDpiForWindow(window)))return;
        const auto next=CreateFontIndirectW(&metrics.lfMessageFont);if(!next)return;
        const auto previous=osk_font;osk_font=next;
        for(const auto child:{osk_label,osk_edit,osk_hint,osk_accept,osk_cancel})
            if(child)SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(osk_font),TRUE);
        if(previous)DeleteObject(previous);
    }
    void close_osk() noexcept {
        const bool restore=visible&&osk_edit&&GetFocus()&&IsChild(window,GetFocus())&&GetForegroundWindow()==window;
        for(auto *child:{&osk_label,&osk_edit,&osk_hint,&osk_accept,&osk_cancel}){
            if(*child)DestroyWindow(*child);*child=nullptr;
        }
        osk_dialog.reset();keyboard.set_focused(false);
        try{sync_scanout();}catch(...){callback_error=std::current_exception();}
        if(osk_font){DeleteObject(osk_font);osk_font=nullptr;}
        if(restore&&window)SetFocus(window);
        if(window)InvalidateRect(window,nullptr,FALSE);
    }
    void submit_osk(Osk::Response::Action action) noexcept {
        if(!osk_dialog||osk_response||!osk_edit)return;
        try{
            std::u16string text;
            if(action==Osk::Response::Action::confirm){
                const auto length=GetWindowTextLengthW(osk_edit);
                if(length<0)throw Error("Cannot measure text input");
                if(static_cast<std::uint32_t>(length)>osk_dialog->max_units){
                    SetWindowTextW(osk_hint,L"The text exceeds the length limit.");return;
                }
                std::wstring wide(static_cast<std::size_t>(length)+1,L'\0');
                const auto copied=GetWindowTextW(osk_edit,wide.data(),static_cast<int>(wide.size()));
                if(copied!=length)throw Error("Cannot read the text input field");
                text.assign(wide.begin(),wide.begin()+length);
                if(!Osk::accepts_text(*osk_dialog,text)){
                    SetWindowTextW(osk_hint,L"Check the length and complete characters before continuing.");
                    if(visible&&GetForegroundWindow()==window)SetFocus(osk_edit);
                    return;
                }
            }
            osk_response=Osk::Response{osk_dialog->generation,action,std::move(text)};
            EnableWindow(osk_edit,FALSE);EnableWindow(osk_accept,FALSE);EnableWindow(osk_cancel,FALSE);
            keyboard.set_focused(false);
        }catch(...){callback_error=std::current_exception();}
    }
    void set_osk(const std::optional<Osk::Dialog> &dialog) {
        check_thread();
        if(closed||!window)return;
        if(!dialog){close_osk();return;}
        if(osk_dialog&&osk_dialog->generation==dialog->generation)return; // Preserve in-progress text/selection.
        if(dialog->line_count<1||dialog->line_count>8||dialog->max_units>4095||dialog->initial.size()>dialog->max_units)
            throw Error("Unsupported text-input dialog bounds");
        close_osk();osk_dialog=dialog;sync_scanout();
        const auto instance=GetModuleHandleW(nullptr);
        const auto child=[&](DWORD extended,const wchar_t *kind,const wchar_t *text,DWORD style,int id){
            const auto handle=CreateWindowExW(extended,kind,text,WS_CHILD|WS_VISIBLE|style,0,0,1,1,
                window,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
            if(!handle)throw Error("Cannot create a text-input control");return handle;
        };
        const std::wstring description(dialog->description.begin(),dialog->description.end());
        const std::wstring initial(dialog->initial.begin(),dialog->initial.end());
        try{
            osk_label=child(0,L"STATIC",description.c_str(),SS_LEFT|SS_NOPREFIX,0);
            const auto style=WS_TABSTOP|(dialog->line_count>1?ES_MULTILINE|ES_AUTOVSCROLL|ES_WANTRETURN|WS_VSCROLL:ES_AUTOHSCROLL);
            osk_edit=child(WS_EX_CLIENTEDGE,L"EDIT",L"",style,0);
            SendMessageW(osk_edit,EM_SETLIMITTEXT,dialog->max_units,0);
            if(!dialog->max_units)SendMessageW(osk_edit,EM_SETREADONLY,TRUE,0);
            if(!SetWindowTextW(osk_edit,initial.c_str()))throw Error("Cannot initialize text input");
            const auto hint=L"Limit: "+std::to_wstring(dialog->max_units)+(dialog->line_count>1?L"   Ctrl+Enter: OK   Esc: Cancel":L"   Enter: OK   Esc: Cancel");
            osk_hint=child(0,L"STATIC",hint.c_str(),SS_LEFT|SS_NOPREFIX,0);
            osk_accept=child(0,L"BUTTON",L"OK",WS_TABSTOP|BS_DEFPUSHBUTTON,osk_accept_id);
            osk_cancel=child(0,L"BUTTON",L"Cancel",WS_TABSTOP|BS_PUSHBUTTON,osk_cancel_id);
            update_osk_font();
            const auto dpi=GetDpiForWindow(window);RECT minimum{0,0,MulDiv(480,dpi,96),MulDiv(360,dpi,96)},current{};
            AdjustWindowRectExForDpi(&minimum,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);GetWindowRect(window,&current);
            SetWindowPos(window,nullptr,0,0,std::max(current.right-current.left,minimum.right-minimum.left),
                std::max(current.bottom-current.top,minimum.bottom-minimum.top),SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
            layout_osk();keyboard.set_focused(false);
            if(visible&&GetForegroundWindow()==window){SetFocus(osk_edit);SendMessageW(osk_edit,EM_SETSEL,0,-1);}
            InvalidateRect(window,nullptr,FALSE);UpdateWindow(window);
        }catch(...){close_osk();throw;}
    }
    void compose(HDC target) {
        RECT client{}; GetClientRect(window, &client);
        const auto client_width = static_cast<int>(client.right - client.left);
        const auto client_height = static_cast<int>(client.bottom - client.top);
        if (client_width<=0 || client_height<=0) return;
        surface.ensure(target,client_width,client_height);
        const auto dc=surface.dc;
        if (!FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))))
            throw Error("Cannot clear the framebuffer composition surface");
        if (!pixels.empty() && client_width > 0 && client_height > 0) {
            const auto factor = options.scaling == Options::Scaling::integer ?
                std::min(client_width / static_cast<int>(width), client_height / static_cast<int>(height)) : 0;
            int target_width{}, target_height{};
            if (options.scaling == Options::Scaling::stretch) { target_width = client_width; target_height = client_height; }
            else if (factor > 0) { target_width = static_cast<int>(width) * factor; target_height = static_cast<int>(height) * factor; }
            else if (static_cast<std::int64_t>(client_width) * height <= static_cast<std::int64_t>(client_height) * width) {
                target_width = client_width; target_height = std::max(1, static_cast<int>(static_cast<std::int64_t>(client_width) * height / width));
            } else {
                target_height = client_height; target_width = std::max(1, static_cast<int>(static_cast<std::int64_t>(client_height) * width / height));
            }
            BITMAPINFO bitmap{}; bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = static_cast<LONG>(width); bitmap.bmiHeader.biHeight = -static_cast<LONG>(height);
            bitmap.bmiHeader.biPlanes = 1; bitmap.bmiHeader.biBitCount = 32; bitmap.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, options.smooth ? HALFTONE : COLORONCOLOR);
            if (options.smooth) SetBrushOrgEx(dc, 0, 0, nullptr);
            // BI_RGB ignores framebuffer alpha, as LCD scanout does; the
            // offscreen conversion still retains the original alpha component.
            if (StretchDIBits(dc, (client_width - target_width) / 2, (client_height - target_height) / 2,
                    target_width, target_height, 0, 0, width, height, pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY) == static_cast<int>(GDI_ERROR)) {
                throw Error("Cannot compose the guest framebuffer");
            }
        }
        // Draw the rate into the same completed image as the game. A repaint
        // never increments it; only an immediate or newly latched guest frame does.
        if (options.fps_overlay) {
        const auto fps_dpi = GetDpiForWindow(window);
        const auto fps_margin = MulDiv(8,fps_dpi,96), shadow = std::max(1,MulDiv(1,fps_dpi,96));
        wchar_t fps[48]{};std::swprintf(fps,48,L"FPS: %.1f",frame_rate.rate());
        const auto old_font = SelectObject(dc,GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc,TRANSPARENT);
        RECT label{fps_margin+shadow,fps_margin+shadow,fps_margin+MulDiv(160,fps_dpi,96),fps_margin+MulDiv(24,fps_dpi,96)};
        SetTextColor(dc,RGB(0,0,0));DrawTextW(dc,fps,-1,&label,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        OffsetRect(&label,-shadow,-shadow);
        SetTextColor(dc,RGB(255,255,0));DrawTextW(dc,fps,-1,&label,DT_LEFT|DT_TOP|DT_SINGLELINE|DT_NOPREFIX);
        SelectObject(dc,old_font);
        }
        if(osk_dialog&&client_width>0&&client_height>0){
            const auto panel=osk_panel();FillRect(dc,&panel,static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
            FrameRect(dc,&panel,static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
        }
        if (!osk_dialog && !utility_message.empty() && client_width > 0 && client_height > 0) {
            const auto dpi = GetDpiForWindow(window);
            const auto margin = MulDiv(8, dpi, 96), padding = MulDiv(16, dpi, 96);
            const auto panel_width = std::max(1, std::min(client_width - 2 * margin, MulDiv(600, dpi, 96)));
            const auto previous_font = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
            RECT measured{0, 0, std::max(1, panel_width - 2 * padding), 0};
            DrawTextW(dc, utility_message.data(), static_cast<int>(utility_message.size()), &measured,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_CALCRECT);
            const auto panel_height = std::max(1, std::min(client_height - 2 * margin,
                static_cast<int>(measured.bottom - measured.top) + 2 * padding));
            RECT panel{(client_width - panel_width) / 2, (client_height - panel_height) / 2,
                (client_width + panel_width) / 2, (client_height + panel_height) / 2};
            FillRect(dc, &panel, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
            FrameRect(dc, &panel, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            panel.left += padding; panel.right -= padding; panel.top += padding; panel.bottom -= padding;
            SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, utility_message.data(), static_cast<int>(utility_message.size()), &panel,
                DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
            SelectObject(dc, previous_font);
        }
        // The target DC retains its update region and WS_CLIPCHILDREN clip, so
        // this parent image never paints over the native OSK child controls.
        if (!BitBlt(target,0,0,client_width,client_height,dc,0,0,SRCCOPY))
            throw Error("Cannot publish the composed guest framebuffer");
    }
    void paint() noexcept {
        PAINTSTRUCT painting{};
        const auto dc=BeginPaint(window,&painting);
        if (!dc) return;
        // The scanout child covers the client area; composing under it would
        // only spend game-thread time.
        if (!scanout_active()) try { compose(dc); } catch (...) { callback_error=std::current_exception(); }
        EndPaint(window,&painting);
    }
    static LRESULT CALLBACK procedure(HWND handle, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        auto *self = reinterpret_cast<Impl *>(GetWindowLongPtrW(handle, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Impl *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
            self->window = handle; SetWindowLongPtrW(handle, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(handle, message, wparam, lparam);
        switch (message) {
        case WM_CLOSE: self->request_close(); return 0;
        case WM_SYSKEYDOWN:
            if (wparam == VK_RETURN && (lparam & (1 << 29))) { self->toggle_fullscreen(); return 0; }
            break;
        case WM_COMMAND:
            if(self->osk_dialog&&HIWORD(wparam)==BN_CLICKED&&reinterpret_cast<HWND>(lparam)==self->osk_accept){self->submit_osk(Osk::Response::Action::confirm);return 0;}
            if(self->osk_dialog&&HIWORD(wparam)==BN_CLICKED&&reinterpret_cast<HWND>(lparam)==self->osk_cancel){self->submit_osk(Osk::Response::Action::cancel);return 0;}
            break;
        case WM_CTLCOLORSTATIC:
            if(self->osk_dialog){SetTextColor(reinterpret_cast<HDC>(wparam),RGB(255,255,255));
                SetBkColor(reinterpret_cast<HDC>(wparam),RGB(64,64,64));return reinterpret_cast<LRESULT>(GetStockObject(DKGRAY_BRUSH));}
            break;
        case WM_SETFOCUS: self->keyboard.set_focused(true); return 0;
        case WM_KILLFOCUS: self->keyboard.set_focused(false); return 0;
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) self->keyboard.set_focused(false);
            break;
        case WM_KEYDOWN: case WM_KEYUP: {
            if(self->osk_dialog){
                if(message==WM_KEYDOWN&&wparam==VK_ESCAPE)self->submit_osk(Osk::Response::Action::cancel);
                else if(message==WM_KEYDOWN&&wparam==VK_RETURN)self->submit_osk(GetFocus()==self->osk_cancel?
                    Osk::Response::Action::cancel:Osk::Response::Action::confirm);
                return 0;
            }
            if (message == WM_KEYDOWN && wparam == VK_F11) { self->toggle_fullscreen(); return 0; }
            Key key{};
            switch (wparam) {
            case VK_UP: key = Key::up; break;
            case VK_RIGHT: key = Key::right; break;
            case VK_DOWN: key = Key::down; break;
            case VK_LEFT: key = Key::left; break;
            case 'Z': key = Key::cross; break;
            case 'X': key = Key::circle; break;
            case 'A': key = Key::square; break;
            case 'S': key = Key::triangle; break;
            case VK_RETURN: key = Key::start; break;
            case VK_BACK: key = Key::select; break;
            case 'Q': key = Key::left_trigger; break;
            case 'E': key = Key::right_trigger; break;
            default: return DefWindowProcW(handle, message, wparam, lparam);
            }
            if (GetForegroundWindow() == handle) self->keyboard.set_key(key, message == WM_KEYDOWN);
            return 0;
        }
        case WM_PAINT: self->paint(); return 0;
        case WM_PRINTCLIENT:
            try { if(wparam)self->compose(reinterpret_cast<HDC>(wparam)); }
            catch (...) { self->callback_error=std::current_exception(); }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_SIZE: self->layout_osk();if(self->presenter)self->presenter->resize();InvalidateRect(handle, nullptr, FALSE); return 0;
        case WM_DESTROY: self->stop_scanout(); break;
        case WM_DPICHANGED: {
            const auto *rect = reinterpret_cast<RECT *>(lparam);
            SetWindowPos(handle, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                SWP_NOACTIVATE | SWP_NOZORDER); self->update_osk_font();self->layout_osk();return 0;
        }
        case WM_GETMINMAXINFO: {
            auto *info = reinterpret_cast<MINMAXINFO *>(lparam); const auto dpi = GetDpiForWindow(handle);
            RECT rect{0, 0, MulDiv(self->osk_dialog||!self->utility_message.empty()?480:240, dpi, 96),
                MulDiv(self->osk_dialog?360:self->utility_message.empty()?136:272, dpi, 96)};
            AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
            info->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top}; return 0;
        }
        case WM_NCDESTROY:
            self->window = nullptr; SetWindowLongPtrW(handle, GWLP_USERDATA, 0); break;
        }
        return DefWindowProcW(handle, message, wparam, lparam);
    }
};
Window::Window(std::string_view title, std::function<void()> on_close, bool visible) : impl_(std::make_unique<Impl>(title, std::move(on_close),visible)) {}
Window::~Window() = default;
void Window::set_osk_dialog(const std::optional<Osk::Dialog> &dialog){impl_->set_osk(dialog);}
std::optional<Osk::Response> Window::take_osk_response(){impl_->check_thread();return std::exchange(impl_->osk_response,std::nullopt);}
void Window::set_utility_message(std::string_view text) {
    impl_->check_thread();
    if (text.size() > 8192) throw Error("Utility message exceeds its bounded size");
    std::wstring converted;
    if (!text.empty()) {
        const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
        if (length <= 0) throw Error("Utility message is not valid UTF-8");
        converted.resize(length);
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), converted.data(), length);
    }
    if (converted != impl_->utility_message) {
        impl_->utility_message = std::move(converted);
        if (impl_->window) {
            if (!impl_->utility_message.empty()) {
                const auto dpi = GetDpiForWindow(impl_->window);
                RECT minimum{0, 0, MulDiv(480, dpi, 96), MulDiv(272, dpi, 96)}, current{};
                AdjustWindowRectExForDpi(&minimum, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
                GetWindowRect(impl_->window, &current);
                if (current.right - current.left < minimum.right - minimum.left || current.bottom - current.top < minimum.bottom - minimum.top)
                    SetWindowPos(impl_->window, nullptr, 0, 0,
                        std::max(current.right - current.left, minimum.right - minimum.left),
                        std::max(current.bottom - current.top, minimum.bottom - minimum.top),
                        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
            impl_->sync_scanout();
            InvalidateRect(impl_->window, nullptr, FALSE); UpdateWindow(impl_->window);
        }
    }
}
InputState Window::input_state() {
    pump();
    const bool alive = !impl_->closed && impl_->window;
    if(impl_->osk_dialog){impl_->keyboard.set_focused(false);return impl_->keyboard.state(alive);}
    impl_->keyboard.set_focused(alive && GetForegroundWindow() == impl_->window);
    // An unfocused live keyboard releases its keys without disconnecting the
    // provider. Alt-tab must not abort a running game.
    auto state = impl_->keyboard.state(alive);
    if (alive && GetForegroundWindow() == impl_->window) impl_->merge_pad(state);
    return state;
}
void Window::pump() {
    impl_->check_thread();
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) { impl_->request_close(); break; }
        if(impl_->osk_dialog&&(message.hwnd==impl_->window||IsChild(impl_->window,message.hwnd))){
            if(message.message==WM_KEYDOWN&&message.wParam==VK_ESCAPE){impl_->submit_osk(Osk::Response::Action::cancel);continue;}
            if(message.message==WM_KEYDOWN&&message.wParam==VK_RETURN&&
                (message.hwnd!=impl_->osk_edit||impl_->osk_dialog->line_count==1||(GetKeyState(VK_CONTROL)&0x8000))){
                impl_->submit_osk(message.hwnd==impl_->osk_cancel?Osk::Response::Action::cancel:Osk::Response::Action::confirm);continue;
            }
            if(IsDialogMessageW(impl_->window,&message))continue;
        }
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (impl_->callback_error) std::rethrow_exception(impl_->callback_error);
}
bool Window::wants_scenes() const noexcept {
    return impl_->presenter && (impl_->presenter->interpolating() || impl_->presenter->wide_width() > 0);
}
// At a level edge the view is pushed to one side, so all its extra width can
// lie past one screen edge; one more tile covers the camera's step between frames.
double Window::scene_margin() const noexcept {
    const auto width = impl_->presenter ? impl_->presenter->wide_width() : 0.0;
    return width > 480 ? width - 480 + 18 : 0.0;
}
void Window::present(const psprecomp::GuestMemory &memory, std::uint32_t address,
    std::uint32_t stride, std::uint32_t format, std::uint32_t width, std::uint32_t height, bool submitted,
    std::shared_ptr<const Scene> scene) {
    pump(); if (impl_->closed) return;
    // A stopped presenter falls back to GDI at once, even if this frame repeats.
    if (impl_->presenter && impl_->presenter->failed()) impl_->sync_scanout();
    auto pixels = decode_framebuffer_bgra(memory, address, stride, format, width, height);
    impl_->frame_rate.update(static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count()),submitted);
    std::array<wchar_t,48> fps{};
    std::swprintf(fps.data(),fps.size(),L"FPS: %.1f",impl_->frame_rate.rate());
    const bool same_pixels=impl_->has_presented && impl_->width==width && impl_->height==height && impl_->pixels==pixels;
    // A new frame's scene still matters when its pixels repeat: in-between
    // frames need an unbroken sequence of scenes.
    const bool identical=same_pixels && impl_->last_present_fps==fps && !(submitted && scene && impl_->scanout_active());
    if(identical){
        if(impl_->callback_error)std::rethrow_exception(impl_->callback_error);
        return;
    }
    impl_->pixels = std::move(pixels); impl_->width = width; impl_->height = height;
    impl_->last_present_fps=fps;impl_->has_presented=true;
    impl_->sync_scanout();
    if (impl_->scanout_active()) {
        if (same_pixels && !submitted) impl_->presenter->set_label(impl_->scanout_label()); // Only the FPS label changed.
        else impl_->presenter->submit(impl_->pixels.data(), width, height, impl_->scanout_label(), submitted ? std::move(scene) : nullptr);
    } else { InvalidateRect(impl_->window, nullptr, FALSE); UpdateWindow(impl_->window); }
    if (impl_->callback_error) std::rethrow_exception(impl_->callback_error);
}
#elif defined(__ANDROID__)
// The Android app's SurfaceView, shared through AndroidBridge. Input, utility
// text and the keyboard dialog are exchanged with the app's UI thread there.
struct Window::Impl {
    std::function<void()> on_close;
    bool closed{};
    // Game frames per second, written to the session log every five seconds.
    std::chrono::steady_clock::time_point since{std::chrono::steady_clock::now()};
    std::uint32_t frames{};
};
Window::Window(std::string_view, std::function<void()> on_close, bool) {
    if (!AndroidBridge::instance().attached()) throw Error("The game window requires the Android app's surface");
    impl_ = std::make_unique<Impl>();
    impl_->on_close = std::move(on_close);
}
Window::~Window() = default;
void Window::pump() {
    // The app in the background pauses the game here rather than losing frames.
    if (AndroidBridge::instance().wait_while_paused() && !impl_->closed) { impl_->closed = true; impl_->on_close(); }
}
InputState Window::input_state() { return AndroidBridge::instance().input(); }
void Window::set_utility_message(std::string_view text) { AndroidBridge::instance().set_utility_message(std::string(text)); }
void Window::set_osk_dialog(const std::optional<Osk::Dialog> &dialog) { AndroidBridge::instance().set_osk_dialog(dialog); }
std::optional<Osk::Response> Window::take_osk_response() { return AndroidBridge::instance().take_osk_response(); }
bool Window::wants_scenes() const noexcept { return AndroidBridge::instance().wants_scenes(); }
double Window::scene_margin() const noexcept { return 0; }
void Window::present(const psprecomp::GuestMemory &memory, std::uint32_t address, std::uint32_t stride, std::uint32_t format,
    std::uint32_t width, std::uint32_t height, bool submitted, std::shared_ptr<const Scene> scene) {
    // Only the raw frame is copied here; the scanout thread or presenter converts it.
    AndroidBridge::instance().post(framebuffer_bytes(memory, address, stride, format, width, height), stride, format, width, height,
        std::move(scene));
    if (submitted) AndroidBridge::instance().frame_finished();
    impl_->frames += submitted;
    const auto now = std::chrono::steady_clock::now();
    if (const auto elapsed = std::chrono::duration<double>(now - impl_->since).count(); elapsed >= 5.0) {
        std::cout << "[android-fps] game " << impl_->frames / elapsed << std::endl;
        impl_->frames = 0; impl_->since = now;
    }
}
#else
struct Window::Impl {};
Window::Window(std::string_view, std::function<void()>, bool) { throw Error("The guest framebuffer window requires Windows"); }
Window::~Window() = default;
InputState Window::input_state() { throw Error("The guest keyboard input window requires Windows"); }
void Window::set_utility_message(std::string_view) { throw Error("The utility window requires Windows"); }
void Window::set_osk_dialog(const std::optional<Osk::Dialog>&) { throw Error("The text-input window requires Windows"); }
std::optional<Osk::Response> Window::take_osk_response() { throw Error("The text-input window requires Windows"); }
void Window::pump() { throw Error("The guest framebuffer window requires Windows"); }
bool Window::wants_scenes() const noexcept { return false; }
double Window::scene_margin() const noexcept { return 0; }
void Window::present(const psprecomp::GuestMemory &, std::uint32_t, std::uint32_t, std::uint32_t,
    std::uint32_t, std::uint32_t, bool, std::shared_ptr<const Scene>) { throw Error("The guest framebuffer window requires Windows"); }
#endif
}
