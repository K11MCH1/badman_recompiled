#include "badman_window.hpp"
#include "badman_presenter.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <chrono>
#include <thread>
#include <vector>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
using namespace psprecomp;
constexpr std::uint32_t base = GuestMemory::kVramPhysicalBase;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void guest_frame_rate_ignores_repaints() {
    badman::FrameRateCounter counter;
    for (std::uint64_t i=0;i<60;++i) counter.update(i*1000000000ull/60,i%2==0);
    require(counter.rate()==0,"FPS counter reported an incomplete initial interval");
    counter.update(1000000000ull,false);
    require(std::abs(counter.rate()-30.0)<0.001,"FPS counted host repaints instead of 30 guest submissions");
    for (std::uint64_t i=1;i<60;++i) counter.update(1000000000ull+i*1000000000ull/60,false);
    counter.update(2000000000ull,false);
    require(counter.rate()==0,"A static scanout/repaint interval invented guest frames");
    for (std::uint64_t i=0;i<15;++i) counter.update(2000000000ull+i*50000000ull,true);
    counter.update(3500000000ull,false);
    require(std::abs(counter.rate()-10.0)<0.001,"FPS calculation ignored elapsed wall time");
    counter.update(1,false);
    require(counter.rate()==0,"FPS clock reset produced a bogus rate");
}
template <class Operation> void rejects(Operation operation, const char *message) {
    bool rejected = false;
    try { operation(); } catch (const Error &) { rejected = true; }
    require(rejected, message);
}
void color_components_and_alpha() {
    GuestMemory memory;
    const auto check16 = [&](std::uint32_t format, std::initializer_list<std::uint16_t> raw,
                             std::initializer_list<std::uint32_t> expected) {
        std::uint32_t address = base;
        for (const auto pixel : raw) { memory.store16(address, pixel); address += 2; }
        const auto converted = badman::decode_framebuffer_bgra(memory, base,
            static_cast<std::uint32_t>(raw.size()), format, static_cast<std::uint32_t>(raw.size()), 1);
        require(converted == std::vector<std::uint32_t>(expected), "16-bit framebuffer channel/alpha conversion is wrong");
    };
    // These are externally visible primary colors and known bit-depth midpoints,
    // rather than assertions repeating the conversion expression.
    check16(0, {0x001F, 0x07E0, 0xF800, 0xFFFF, 0x0400},
               {0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF, 0xFF008200});
    check16(1, {0x801F, 0x03E0, 0xFC00, 0x8010, 0x0000},
               {0xFFFF0000, 0x0000FF00, 0xFF0000FF, 0xFF840000, 0x00000000});
    check16(2, {0xA321, 0xF00F, 0x0F00, 0xFFFF},
               {0xAA112233, 0xFFFF0000, 0x000000FF, 0xFFFFFFFF});
    memory.store32(base, 0x7F332211); memory.store32(base + 4, 0x00332211);
    require(badman::decode_framebuffer_bgra(memory, base, 2, 3, 2, 1) ==
            std::vector<std::uint32_t>({0x7F112233, 0x00112233}), "8888 conversion swapped alpha or the red/blue channels");
}
void visible_rows_and_vram_aliases() {
    GuestMemory memory;
    memory.store32(base, 0xFF0000FF); memory.store32(base + 4, 0xFF00FF00);
    memory.store32(base + 8, 0xDEADBEEF); // Stride padding must not become a visible pixel.
    memory.store32(base + 12, 0xFFFF0000); memory.store32(base + 16, 0xFFFFFFFF);
    const std::vector<std::uint32_t> expected{0xFFFF0000, 0xFF00FF00, 0xFF0000FF, 0xFFFFFFFF};
    for (const auto address : {base, 0x44000000u, 0x04200000u, 0x44600000u, 0xA4000000u})
        require(badman::decode_framebuffer_bgra(memory, address, 3, 3, 2, 2) == expected,
                "Framebuffer rows, stride cropping, or guest VRAM aliases are wrong");
    require(memory.load32(base + 8) == 0xDEADBEEF, "Framebuffer conversion changed guest memory");
    const auto last = base + GuestMemory::kVramSize - 4;
    memory.store32(last, 0xAA332211);
    require(badman::decode_framebuffer_bgra(memory, last, 2048, 3, 1, 1) ==
            std::vector<std::uint32_t>({0xAA112233}), "Visible final pixel incorrectly required unobserved row padding");
}
void invalid_frames_reject_before_reading() {
    GuestMemory memory;
    memory.store32(base, 0x12345678);
    const auto decode = [&](std::uint32_t address, std::uint32_t stride, std::uint32_t format,
                            std::uint32_t width, std::uint32_t height) {
        (void)badman::decode_framebuffer_bgra(memory, address, stride, format, width, height);
    };
    rejects([&] { decode(0, 1, 3, 1, 1); }, "Disabled/null framebuffer was accepted");
    rejects([&] { decode(0x08804000, 1, 3, 1, 1); }, "RAM framebuffer bypassed the VRAM policy");
    rejects([&] { decode(0x04800000, 1, 3, 1, 1); }, "Framebuffer beyond the VRAM mirrors was accepted");
    rejects([&] { decode(base + 2, 1, 3, 1, 1); }, "Unaligned 8888 framebuffer was accepted");
    rejects([&] { decode(base + 1, 1, 0, 1, 1); }, "Unaligned 16-bit framebuffer was accepted");
    rejects([&] { decode(base, 1, 4, 1, 1); }, "Unknown display pixel format was accepted");
    rejects([&] { decode(base, 1, 3, 2, 1); }, "Framebuffer stride shorter than visible width was accepted");
    rejects([&] { decode(base, 1, 3, 0, 1); }, "Empty framebuffer width was accepted");
    rejects([&] { decode(base, 1, 3, 1, 0); }, "Empty framebuffer height was accepted");
    rejects([&] { decode(base, std::numeric_limits<std::uint32_t>::max(), 3,
        std::numeric_limits<std::uint32_t>::max(), 2); }, "Overflowing framebuffer dimensions were accepted");
    rejects([&] { decode(base, 2048, 3, 2048, 2048); }, "Framebuffer larger than physical VRAM was accepted");
    rejects([&] { decode(base + GuestMemory::kVramSize - 4, 2, 3, 2, 1); },
            "Framebuffer wrapping across physical VRAM backing was accepted");
    require(memory.load32(base) == 0x12345678, "Rejected framebuffer changed guest memory");
}
#if defined(_WIN32)
BOOL CALLBACK find_owned_presenter(HWND handle,LPARAM parameter){
    wchar_t name[64]{};GetClassNameW(handle,name,64);DWORD process{};GetWindowThreadProcessId(handle,&process);
    if(process==GetCurrentProcessId()&&std::wstring_view(name)==L"BadmanGuestFramebuffer"){
        *reinterpret_cast<HWND*>(parameter)=handle;return FALSE;
    }
    return TRUE;
}
void hidden_composed_presentation(bool check_process_resources=true) {
    const auto resources_before=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    {
        badman::Window window("Hidden composed presentation test",{},false);
        HWND parent{};EnumThreadWindows(GetCurrentThreadId(),find_owned_presenter,reinterpret_cast<LPARAM>(&parent));
        require(parent&&!IsWindowVisible(parent),"Presentation test activated a window");
        GuestMemory memory;
        // Independent visible colors: PSP low byte is red, not GDI's blue.
        memory.store32(base,0xFF0000FF);memory.store32(base+4,0xFF00FF00);
        window.present(memory,base,2,3,2,1);
        const auto dc=CreateCompatibleDC(nullptr);require(dc!=nullptr,"Cannot create hidden capture DC");
        BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth=800;info.bmiHeader.biHeight=-600;
        info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
        void *bits{};const auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
        require(bitmap&&bits,"Cannot create hidden capture bitmap");
        const auto previous=SelectObject(dc,bitmap);
        const auto render=[&]{
            SendMessageW(parent,WM_PRINTCLIENT,reinterpret_cast<WPARAM>(dc),PRF_CLIENT);window.pump();GdiFlush();
        };
        const auto pixel=[&](int x,int y){return static_cast<const std::uint32_t*>(bits)[y*800+x]&0xFFFFFFu;};
        const auto resize=[&](int width,int height){
            RECT rect{0,0,width,height};const auto dpi=GetDpiForWindow(parent);
            require(AdjustWindowRectExForDpi(&rect,WS_OVERLAPPEDWINDOW,FALSE,0,dpi)!=0,"Cannot size hidden test window");
            require(SetWindowPos(parent,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,
                SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE)!=0,"Cannot resize hidden test window");
            RECT actual{};GetClientRect(parent,&actual);
            require(actual.right==width&&actual.bottom==height,"Unexpected hidden test client dimensions");
        };
        resize(640,480);render();
        require(pixel(0,0)==0&&pixel(0,479)==0&&pixel(100,240)==0xFF0000&&pixel(500,240)==0x00FF00,
            "Composed image lost letterbox bars, scale or independent framebuffer colors");
        unsigned yellow=0;
        for(int y=0;y<64;++y)for(int x=0;x<220;++x){
            const auto rgb=pixel(x,y);
            if(((rgb>>16)&255)>200&&((rgb>>8)&255)>200&&(rgb&255)<40)++yellow;
        }
        require(yellow>10,"Yellow FPS text is missing from the top-left corner");
        // A clipped destination models the real paint DC's child-control clip.
        const auto saved=SaveDC(dc);ExcludeClipRect(dc,10,10,20,20);
        static_cast<std::uint32_t*>(bits)[15*800+15]=0x123456;
        render();RestoreDC(dc,saved);
        require(pixel(15,15)==0x123456,"Composed publication ignored the target's child/update clipping");
        window.set_utility_message("Real utility composition");render();
        require(pixel(320,240)!=0x00FF00,"Utility panel was omitted from the composed image");
        window.set_utility_message("");render();
        require(pixel(100,240)==0xFF0000,"Closing utility panel left stale offscreen pixels");
        badman::Osk::Dialog dialog;dialog.generation=700;dialog.description=u"Name";
        dialog.initial=u"Smith";dialog.max_units=6;window.set_osk_dialog(dialog);render();
        require(FindWindowExW(parent,nullptr,L"EDIT",nullptr)!=nullptr&&pixel(320,240)!=0x00FF00,
            "OSK parent panel or native child was lost during composition");
        window.set_osk_dialog(std::nullopt);
        const auto stable_resources=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
        for(int i=0;i<80;++i){resize(i%2?640:600,i%2?480:400);render();}
        require(GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS)<=stable_resources,
            "Resizing the reusable composition surface leaked GDI resources");
        require(!IsWindowVisible(parent)&&GetForegroundWindow()!=parent,"Hidden presentation activated the desktop");
        SelectObject(dc,previous);DeleteObject(bitmap);DeleteDC(dc);
    }
    GdiFlush();
    const auto resources_after=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    std::cout << "Composition GDI resources: " << resources_before << " -> " << resources_after << '\n';
    require(!check_process_resources||resources_after<=resources_before,
        "Destroying composed window leaked its retained DC or bitmap");
}
void hidden_native_text_controls() {
    unsigned closed=0;
    badman::Window window("Hidden OSK control test",[&]{++closed;},false);
    HWND parent{};EnumThreadWindows(GetCurrentThreadId(),find_owned_presenter,reinterpret_cast<LPARAM>(&parent));
    require(parent&&!IsWindowVisible(parent)&&GetForegroundWindow()!=parent,
            "Native-control test must remain hidden and must not activate the desktop");
    badman::Osk::Dialog dialog;dialog.generation=100;dialog.description=u"Name";dialog.initial=u"Smith";dialog.max_units=6;
    window.set_osk_dialog(dialog);
    const auto edit=[&]{return FindWindowExW(parent,nullptr,L"EDIT",nullptr);};
    const auto accept=[&]{return FindWindowExW(parent,nullptr,L"BUTTON",L"OK");};
    const auto submit=[&]{const auto button=accept();require(button!=nullptr,"Missing native OK control");
        SendMessageW(parent,WM_COMMAND,MAKEWPARAM(GetDlgCtrlID(button),BN_CLICKED),reinterpret_cast<LPARAM>(button));window.pump();};
    const auto value=[&]{wchar_t text[64]{};const auto length=GetWindowTextW(edit(),text,64);return std::u16string(text,text+length);};
    require(edit()!=nullptr&&SendMessageW(edit(),EM_GETLIMITTEXT,0,0)==6,"Native editor did not enforce the game's text length");
    require(SetWindowTextW(edit(),L"Rook")!=0,"Cannot stage native text input");
    window.set_osk_dialog(dialog);
    require(value()==u"Rook","Repeated utility snapshots reset the user's in-progress typing");
    submit();auto response=window.take_osk_response();
    require(response&&response->generation==100&&response->action==badman::Osk::Response::Action::confirm&&
            response->text==u"Rook"&&!window.take_osk_response(),"Native confirmation did not return text exactly once");
    window.set_osk_dialog(std::nullopt);require(!edit(),"Closed OSK left native controls alive");

    ++dialog.generation;dialog.initial.clear();window.set_osk_dialog(dialog);
    SetWindowTextW(edit(),L"\x03A9\xD83D\xDE42");
    PostMessageW(edit(),WM_KEYDOWN,VK_RETURN,0);window.pump();response=window.take_osk_response();
    require(response&&response->text==u"\u03A9\U0001F642","Native Enter path corrupted UTF16 or a surrogate pair");
    require(window.input_state().buttons==0,"Typing in OSK leaked into PSP controller buttons");
    window.set_osk_dialog(std::nullopt);

    ++dialog.generation;dialog.initial=u"Smith";window.set_osk_dialog(dialog);
    SetWindowTextW(edit(),L"ABCDE\xD83D");submit();
    require(!window.take_osk_response()&&IsWindowEnabled(edit())&&IsWindowEnabled(accept()),
            "An incomplete Unicode character accepted or stranded native text input");
    SetWindowTextW(edit(),L"Fixed");submit();response=window.take_osk_response();
    require(response&&response->text==u"Fixed","Native text input could not recover after rejected text");
    window.set_osk_dialog(std::nullopt);

    ++dialog.generation;dialog.initial=u"Comment";dialog.line_count=3;dialog.max_units=20;window.set_osk_dialog(dialog);
    PostMessageW(edit(),WM_KEYDOWN,VK_RETURN,0);window.pump();
    require(!window.take_osk_response(),"Multiline Enter confirmed instead of editing");
    PostMessageW(edit(),WM_KEYDOWN,VK_ESCAPE,0);window.pump();response=window.take_osk_response();
    require(response&&response->action==badman::Osk::Response::Action::cancel&&closed==0,
            "Native Escape failed to cancel only the utility dialog");
    window.set_osk_dialog(std::nullopt);

    ++dialog.generation;window.set_osk_dialog(dialog);
    const auto cancel=FindWindowExW(parent,nullptr,L"BUTTON",L"Cancel");
    PostMessageW(cancel,WM_KEYDOWN,VK_RETURN,0);window.pump();response=window.take_osk_response();
    require(response&&response->action==badman::Osk::Response::Action::cancel,
            "Enter on the native Cancel button incorrectly accepted text");
    window.set_osk_dialog(std::nullopt);

    ++dialog.generation;dialog.initial.clear();dialog.line_count=1;dialog.max_units=0;window.set_osk_dialog(dialog);
    require((GetWindowLongPtrW(edit(),GWL_STYLE)&ES_READONLY)!=0,"Zero text limit accidentally enabled unlimited Win32 input");
    submit();response=window.take_osk_response();
    require(response&&response->text.empty(),"Empty-only text dialog did not confirm the actual empty value");
    window.set_osk_dialog(std::nullopt);
    require(!IsWindowVisible(parent)&&GetForegroundWindow()!=parent,"Hidden OSK controls activated a visible window");
    SendMessageW(parent,WM_CLOSE,0,0);window.set_osk_dialog(dialog);
    require(closed==1&&!window.input_state().ready,"Closing native game left an OSK window/provider alive");
}
#endif
}
#if defined(_WIN32)
// GPU scanout lifecycle on a hidden parent: frames reach the presentation
// thread, resize and visibility changes are accepted, teardown joins cleanly.
// Machines without a hardware Direct3D 11 device report the fallback instead.
void gpu_scanout_lifecycle() {
    WNDCLASSW type{}; type.lpfnWndProc = DefWindowProcW; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"BadmanScanoutTestParent";
    RegisterClassW(&type);
    const auto parent = CreateWindowExW(0, type.lpszClassName, L"scanout test", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        0, 0, 640, 400, nullptr, nullptr, type.hInstance, nullptr);
    require(parent != nullptr, "Cannot create the scanout test parent");
    try {
        std::unique_ptr<badman::Presenter> presenter;
        try { presenter = std::make_unique<badman::Presenter>(parent, badman::Presenter::Scaling::integer, false); }
        catch (const psprecomp::Error &error) {
            std::cout << "GPU scanout unavailable here, GDI fallback applies: " << error.what() << std::endl;
            DestroyWindow(parent); return;
        }
        require(!presenter->description().empty() && presenter->visible() && !presenter->failed(), "Scanout did not start");
        std::vector<std::uint32_t> frame(480 * 272, 0xFF204080u);
        for (int i = 0; i < 3; ++i) { frame[i] = 0xFFFFFFFFu; presenter->submit(frame.data(), 480, 272, L"FPS: 30.0"); }
        const auto wait = [&](std::uint64_t count) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (presenter->statistics().presented < count && !presenter->failed() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        };
        wait(1);
        require(!presenter->failed() && presenter->statistics().presented >= 1, "Submitted frames were never presented");
        require(presenter->statistics().submitted == 3, "Scanout lost or invented submissions");
        SetWindowPos(parent, nullptr, 0, 0, 900, 600, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        const auto before = presenter->statistics().presented;
        presenter->resize();
        wait(before + 1);
        require(!presenter->failed() && presenter->statistics().presented > before, "Resize did not present again");
        presenter->set_visible(false); require(!presenter->visible(), "Scanout did not hide for GDI dialogs");
        presenter->set_visible(true); require(presenter->visible(), "Scanout did not return after GDI dialogs");
        presenter->submit(frame.data(), 480, 272, L"");
        presenter.reset();
    } catch (...) { DestroyWindow(parent); throw; }
    DestroyWindow(parent);
}
#endif
int main() {
    try {
        guest_frame_rate_ignores_repaints();
        color_components_and_alpha(); visible_rows_and_vram_aliases(); invalid_frames_reject_before_reading();
#if defined(_WIN32)
        hidden_native_text_controls();
        hidden_composed_presentation(false); // Warm up Win32's lazy font/GDI process caches.
        hidden_composed_presentation();
        hidden_composed_presentation();
        gpu_scanout_lifecycle();
#endif
        std::cout << "Badman offscreen framebuffer color, stride, alias and bounds tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
