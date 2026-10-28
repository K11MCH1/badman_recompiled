// Badman Recomp launcher: a WebView2 host for the HTML launcher in
// launcher-ui/. Native code owns files, settings, dialogs and the setup
// process; the page renders state and sends intents as JSON messages.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wrl.h>
#include <WebView2.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cwctype>
#include <exception>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;
using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;
constexpr UINT post_message = WM_APP + 1;

std::wstring wide(const std::string &value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
    return result;
}
std::string utf8(const std::wstring &value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}
std::wstring win_error(DWORD code) {
    wchar_t *message{};
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<wchar_t *>(&message), 0, nullptr);
    std::wstring result = message ? message : L"Windows error " + std::to_wstring(code);
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}
struct Handle {
    HANDLE value{};
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
std::wstring quote(const std::wstring &argument) {
    // Windows CRT argument quoting; paths are arguments, never shell commands.
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'\"') { result.append(slashes * 2 + 1, L'\\'); result += character; }
        else { result.append(slashes, L'\\'); result += character; }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\'); result += L'\"'; return result;
}
fs::path executable_directory() {
    std::vector<wchar_t> buffer(32768);
    const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!count || count >= buffer.size()) throw std::runtime_error("Cannot locate launcher executable");
    return fs::path(std::wstring(buffer.data(), count)).parent_path();
}
fs::path local_directory() {
    PWSTR value{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &value)))
        throw std::runtime_error("Cannot locate your local application data folder");
    fs::path path = fs::path(value) / L"BadmanRecomp"; CoTaskMemFree(value);
    fs::create_directories(path); return path;
}
bool file_exists(const fs::path &path) { std::error_code error; return !path.empty() && fs::is_regular_file(path, error); }
bool directory_exists(const fs::path &path) { std::error_code error; return !path.empty() && fs::is_directory(path, error); }

// ---------------------------------------------------------------- JSON (flat objects only)
std::string json_string(const std::string &text) {
    std::string out = "\"";
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) { char code[8]; std::snprintf(code, sizeof(code), "\\u%04x", c); out += code; }
            else out += static_cast<char>(c);
        }
    }
    return out + "\"";
}
// Parses {"key": "string" | number | true | false, ...}; nested values are rejected.
std::map<std::string, std::string> parse_message(const std::string &text) {
    std::map<std::string, std::string> result;
    std::size_t at = 0;
    const auto skip = [&] { while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at]))) ++at; };
    const auto expect = [&](char c) { skip(); if (at >= text.size() || text[at] != c) throw std::runtime_error("Malformed launcher message"); ++at; };
    const auto read_string = [&] {
        expect('"'); std::string out;
        while (at < text.size() && text[at] != '"') {
            char c = text[at++];
            if (c == '\\') {
                if (at >= text.size()) break;
                const char e = text[at++];
                if (e == 'n') c = '\n'; else if (e == 't') c = '\t'; else if (e == 'r') c = '\r';
                else if (e == 'u') {
                    if (at + 4 > text.size()) throw std::runtime_error("Malformed launcher message");
                    const auto code = std::stoul(text.substr(at, 4), nullptr, 16); at += 4;
                    if (code < 0x80) c = static_cast<char>(code);
                    else { out += utf8(std::wstring(1, static_cast<wchar_t>(code))); continue; }
                } else c = e;
            }
            out += c;
        }
        expect('"'); return out;
    };
    expect('{'); skip();
    if (at < text.size() && text[at] == '}') return result;
    for (;;) {
        const auto key = read_string(); expect(':'); skip();
        std::string value;
        if (at < text.size() && text[at] == '"') value = read_string();
        else {
            const auto start = at;
            while (at < text.size() && text[at] != ',' && text[at] != '}') ++at;
            value = text.substr(start, at - start);
            while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
            if (value.empty() || value.front() == '{' || value.front() == '[') throw std::runtime_error("Unsupported launcher message value");
        }
        result[key] = value; skip();
        if (at < text.size() && text[at] == ',') { ++at; continue; }
        expect('}'); return result;
    }
}

// ---------------------------------------------------------------- settings
struct Setting { const wchar_t *key; std::vector<std::wstring> allowed; const wchar_t *fallback; };
const std::vector<Setting> &settings_table() {
    static const std::vector<Setting> table{
        {L"Trace", {L"0", L"1"}, L"1"}, {L"Renderer", {L"d3d11", L"vulkan", L"software"}, L"d3d11"},
        {L"SkipIntro", {L"0", L"1"}, L"0"}, {L"Fullscreen", {L"0", L"1"}, L"0"},
        {L"Scaling", {L"integer", L"fit", L"stretch"}, L"integer"}, {L"Filter", {L"nearest", L"smooth"}, L"nearest"}, {L"Interpolation", {L"0", L"1"}, L"1"},
        {L"Widescreen", {L"0", L"1"}, L"0"},
        {L"AutoProfile", {L"0", L"1"}, L"1"}, {L"FpsCounter", {L"0", L"1"}, L"1"}, {L"Controller", {L"0", L"1"}, L"1"},
        {L"StickDeadzone", {L"0", L"5", L"10", L"15", L"20", L"25", L"30", L"35", L"40", L"45", L"50"}, L"20"},
        {L"Jobs", {L"1", L"2", L"3", L"4"}, L"2"}}; // build_game.py accepts 1..4
    return table;
}
const Setting &setting_row(const wchar_t *key) {
    const auto &table = settings_table();
    return *std::find_if(table.begin(), table.end(), [&](const Setting &row) { return std::wstring(key) == row.key; });
}

struct Request {
    fs::path python, backend, working, runtime_source, iso, data, fonts;
    std::vector<std::wstring> extra_arguments;
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    bool play{};
};

struct App {
    HWND window{};
    UINT dpi{96};
    fs::path directory, defaults, settings_path, art;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::thread worker;
    std::mutex job_mutex;
    HANDLE active_job{};
    std::atomic<bool> closing{}, canceled{}, busy{};
    std::exception_ptr callback_error;
    fs::path capture;        // --capture <png>: write the rendered page and exit.
    std::wstring capture_tab;

    App() : directory(executable_directory()), defaults(local_directory()), settings_path(defaults / L"launcher.ini"),
            art(defaults / L"art") {
        fs::create_directories(art);
        // An initial UTF-16 BOM makes the profile APIs keep Unicode paths.
        Handle initial(CreateFileW(settings_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (initial.value != INVALID_HANDLE_VALUE) {
            const unsigned char bom[]{0xFF, 0xFE}; DWORD written{};
            if (!WriteFile(initial.value, bom, sizeof(bom), &written, nullptr) || written != sizeof(bom))
                throw std::runtime_error("Cannot initialize launcher settings");
        } else if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            throw std::runtime_error("Cannot create launcher settings");
    }
    ~App() {
        canceled = true;
        { std::lock_guard lock(job_mutex); if (active_job) TerminateJobObject(active_job, ERROR_CANCELLED); }
        if (worker.joinable()) worker.join();
    }
    std::wstring read_setting(const wchar_t *key, const std::wstring &fallback) const {
        std::vector<wchar_t> buffer(32768);
        GetPrivateProfileStringW(L"Launcher", key, fallback.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()), settings_path.c_str());
        return buffer.data();
    }
    void write_setting(const wchar_t *key, const std::wstring &value) {
        if (!WritePrivateProfileStringW(L"Launcher", key, value.c_str(), settings_path.c_str()))
            throw std::runtime_error("Cannot save launcher options (Windows error " + std::to_string(GetLastError()) + ").");
    }
    std::wstring setting(const wchar_t *key) const {
        const auto &row = setting_row(key);
        auto current = read_setting(row.key, row.fallback);
        return std::find(row.allowed.begin(), row.allowed.end(), current) != row.allowed.end() ? current : row.fallback;
    }
    std::wstring data_directory() const { return read_setting(L"DataDirectory", (defaults / L"game").wstring()); }

    // ---------------------------------------------------------------- page bridge
    void post(const std::string &json) {
        // WebView2 must be called on the UI thread; workers queue messages here.
        auto copy = std::make_unique<std::string>(json);
        if (!closing && PostMessageW(window, post_message, 0, reinterpret_cast<LPARAM>(copy.get()))) copy.release();
    }
    void send_now(const std::string &json) { if (webview) webview->PostWebMessageAsJson(wide(json).c_str()); }
    std::string settings_json() const {
        std::string out = "{";
        const auto add = [&](const std::wstring &key, const std::wstring &value) {
            if (out.size() > 1) out += ",";
            out += json_string(utf8(key)) + ":" + json_string(utf8(value));
        };
        add(L"ISO", read_setting(L"ISO", L"")); add(L"DataDirectory", data_directory());
        for (const auto &row : settings_table()) add(row.key, setting(row.key));
        return out + "}";
    }
    void send_init() {
        const bool ready = file_exists(fs::path(data_directory()) / L"badman-setup.json");
        send_now("{\"type\":\"init\",\"platform\":\"windows\",\"art\":\"https://art.local/\",\"settings\":" + settings_json() +
                 ",\"status\":" + json_string(ready ? "Your game data is prepared. Press Play to launch." :
                                                       "Choose your ISO and a game data folder, then Prepare and build.") +
                 ",\"statusKind\":" + json_string(ready ? "ok" : "") + "}");
        if (!capture_tab.empty()) send_now("{\"type\":\"selectTab\",\"tab\":" + json_string(utf8(capture_tab)) + "}");
    }
    void toast(const std::string &text) { send_now("{\"type\":\"toast\",\"text\":" + json_string(text) + "}"); }
    void refresh_settings() { send_now("{\"type\":\"settings\",\"settings\":" + settings_json() + "}"); }
    void receive(const std::map<std::string, std::string> &message) {
        const auto field = [&](const char *name) { const auto found = message.find(name); return found == message.end() ? std::string() : found->second; };
        const auto type = field("type");
        if (type == "ready") { send_init(); return; }
        if (type == "set") {
            const auto key = wide(field("key")), value = wide(field("value"));
            const auto &table = settings_table();
            const auto row = std::find_if(table.begin(), table.end(), [&](const auto &r) { return key == r.key; });
            if (busy || row == table.end() || std::find(row->allowed.begin(), row->allowed.end(), value) == row->allowed.end()) {
                toast(busy ? "Settings can be changed after the current job finishes." : "That option value is not supported.");
                refresh_settings(); return;
            }
            write_setting(row->key, value);
            return;
        }
        if (type == "browse") { browse(field("target") == "iso"); return; }
        if (type == "prepare" || type == "play" || type == "verify") { start(type == "play", type == "verify"); return; }
        if (type == "openSaves") { open_folder(fs::path(data_directory()) / L"Saves", "Your Saves folder is created the first time you Prepare or Play."); return; }
        if (type == "openData") { open_folder(data_directory(), "The game data folder does not exist yet."); return; }
        if (type == "openLogs") { open_folder(fs::path(data_directory()) / L"launcher-logs", "No logs yet. Logs are written when you Play."); return; }
    }
    void open_folder(const fs::path &path, const char *missing) {
        if (!directory_exists(path)) { toast(missing); return; }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            toast("Windows could not open that folder.");
    }
    void browse(bool iso) {
        if (busy) { toast("Please wait for the current job to finish."); return; }
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) { toast("Windows could not open the file picker."); return; }
        DWORD options{}; dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | (iso ? FOS_FILEMUSTEXIST : FOS_PICKFOLDERS));
        if (iso) { const COMDLG_FILTERSPEC filters[]{{L"PSP game ISO", L"*.iso"}}; dialog->SetFileTypes(1, filters); dialog->SetTitle(L"Choose your own Badman game ISO"); }
        else dialog->SetTitle(L"Choose the game data folder");
        const auto current = iso ? read_setting(L"ISO", L"") : data_directory();
        ComPtr<IShellItem> initial;
        if (!current.empty() && SUCCEEDED(SHCreateItemFromParsingName((iso ? fs::path(current).parent_path() : fs::path(current)).c_str(),
                nullptr, IID_PPV_ARGS(&initial)))) dialog->SetFolder(initial.Get());
        if (FAILED(dialog->Show(window))) return;
        ComPtr<IShellItem> selected; PWSTR path{};
        if (FAILED(dialog->GetResult(&selected)) || FAILED(selected->GetDisplayName(SIGDN_FILESYSPATH, &path))) return;
        write_setting(iso ? L"ISO" : L"DataDirectory", path); CoTaskMemFree(path);
        refresh_settings();
    }

    // ---------------------------------------------------------------- setup backend
    void start(bool play, bool verify) {
        if (busy) { toast("Please wait for the current job to finish."); return; }
        try {
            Request request; request.play = play; request.working = directory;
            request.iso = read_setting(L"ISO", L""); request.data = data_directory();
            if (!file_exists(request.iso)) throw std::runtime_error("Choose your game ISO on the Play page first.");
            auto extension = request.iso.extension().wstring();
            std::transform(extension.begin(), extension.end(), extension.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (extension != L".iso") throw std::runtime_error("The game input must be an .iso file.");
            if (request.data.empty()) throw std::runtime_error("Choose a game data folder.");
            request.iso = fs::absolute(request.iso); request.data = fs::absolute(request.data);
            request.backend = directory / L"tools/setup_game.py"; request.python = directory / L"python/python.exe";
            request.runtime_source = directory / L"runtime-source";
            if (const auto fonts = directory / L"assets/fonts"; directory_exists(fonts)) request.fonts = fonts;  // Picked automatically.
            if (!file_exists(request.runtime_source / L"CMakeLists.txt")) throw std::runtime_error("The runtime source package is missing next to the launcher.");
            if (!file_exists(request.backend)) throw std::runtime_error("The game preparation tool is missing from this package.");
            if (!file_exists(request.python)) throw std::runtime_error("The bundled Python runtime is missing from this package.");
            auto &args = request.extra_arguments;
            args = {L"--renderer", setting(L"Renderer"), setting(L"SkipIntro") == L"1" ? L"--skip-intro" : L"--no-skip-intro",
                    L"--jobs", setting(L"Jobs"), L"--art-directory", art.wstring()};
            if (setting(L"Trace") == L"1") args.push_back(L"--trace");
            if (play) args.push_back(L"--play");
            if (verify) args.push_back(L"--verify-only");
            request.environment = {{L"BADMAN_FULLSCREEN", setting(L"Fullscreen")}, {L"BADMAN_SCALING", setting(L"Scaling")},
                {L"BADMAN_FILTER", setting(L"Filter")}, {L"BADMAN_INTERPOLATION", setting(L"Interpolation")}, {L"BADMAN_WIDESCREEN", setting(L"Widescreen")},
                {L"BADMAN_FPS_OVERLAY", setting(L"FpsCounter")},
                {L"BADMAN_CONTROLLER", setting(L"Controller")}, {L"BADMAN_STICK_DEADZONE", setting(L"StickDeadzone")},
                {L"BADMAN_AUTOMATIC_PROFILE", setting(L"AutoProfile")}};
            if (worker.joinable()) worker.join();
            canceled = false; busy = true;
            send_now(std::string("{\"type\":\"busy\",\"busy\":true,\"clear\":true,\"play\":") + (play ? "true" : "false") + "}");
            worker = std::thread([this, request] { run_backend(request); });
        } catch (const std::exception &error) { toast(error.what()); }
    }
    static std::wstring environment_block(const std::vector<std::pair<std::wstring, std::wstring>> &overrides) {
        // Inherit this environment, overriding the launcher's game options.
        struct Less { bool operator()(const std::wstring &a, const std::wstring &b) const { return _wcsicmp(a.c_str(), b.c_str()) < 0; } };
        std::map<std::wstring, std::wstring, Less> variables;
        if (auto *block = GetEnvironmentStringsW()) {
            for (auto *entry = block; *entry; entry += wcslen(entry) + 1) {
                const std::wstring text(entry); const auto equals = text.find(L'=', 1);
                if (equals != std::wstring::npos) variables[text.substr(0, equals)] = text.substr(equals + 1);
            }
            FreeEnvironmentStringsW(block);
        }
        for (const auto &[name, value] : overrides) variables[name] = value;
        std::wstring environment;
        for (const auto &[name, value] : variables) { environment += name; environment += L'='; environment += value; environment += L'\0'; }
        environment += L'\0';
        return environment;
    }
    void run_backend(Request request) {
        DWORD code = 1; std::string error;
        Handle job;
        try {
            std::vector<std::wstring> args{request.python.wstring(), L"-u", L"-X", L"utf8", request.backend.wstring(),
                L"--iso", request.iso.wstring(), L"--data-root", request.data.wstring(), L"--dispatch-limit", L"18446744073709551615",
                L"--source-root", request.runtime_source.wstring()};
            if (!request.fonts.empty()) { args.push_back(L"--font-directory"); args.push_back(request.fonts.wstring()); }
            args.insert(args.end(), request.extra_arguments.begin(), request.extra_arguments.end());
            std::wstring command;
            for (const auto &argument : args) { if (!command.empty()) command += L' '; command += quote(argument); }
            auto environment = environment_block(request.environment);
            SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
            HANDLE read{}, write{};
            if (!CreatePipe(&read, &write, &security, 0)) throw std::runtime_error("Cannot create setup log pipe");
            Handle output_read(read), output_write(write);
            if (!SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0)) throw std::runtime_error("Cannot secure setup log pipe");
            Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr));
            if (input.value == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open setup input handle");
            job.value = CreateJobObjectW(nullptr, nullptr);
            if (!job.value) throw std::runtime_error("Cannot create launcher process job");
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
                throw std::runtime_error("Cannot configure launcher process job");
            STARTUPINFOW startup{sizeof(STARTUPINFOW)}; startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
            startup.wShowWindow = SW_HIDE; startup.hStdInput = input.value; startup.hStdOutput = write; startup.hStdError = write;
            PROCESS_INFORMATION process{};
            if (canceled) throw std::runtime_error("Operation canceled");
            if (!CreateProcessW(request.python.c_str(), command.data(), nullptr, nullptr, TRUE,
                    CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT, environment.data(), request.working.c_str(), &startup, &process))
                throw std::runtime_error("Could not start setup: " + utf8(win_error(GetLastError())));
            Handle child(process.hProcess), child_thread(process.hThread);
            if (!AssignProcessToJobObject(job.value, child.value)) { TerminateProcess(child.value, ERROR_CANCELLED); throw std::runtime_error("Cannot contain the setup process"); }
            { std::lock_guard lock(job_mutex); active_job = job.value; if (canceled) TerminateJobObject(job.value, ERROR_CANCELLED); }
            if (ResumeThread(child_thread.value) == static_cast<DWORD>(-1)) { TerminateJobObject(job.value, ERROR_CANCELLED); throw std::runtime_error("Cannot resume setup process"); }
            CloseHandle(output_write.value); output_write.value = nullptr;
            char buffer[4096]; DWORD received{}; std::string pending;
            const auto flush = [&](const std::string &text) { if (!text.empty()) post("{\"type\":\"log\",\"text\":" + json_string(text) + "}"); };
            while (ReadFile(read, buffer, sizeof(buffer), &received, nullptr) && received) {
                pending.append(buffer, received);
                const auto end = pending.find_last_of('\n');
                if (end != std::string::npos) { flush(pending.substr(0, end + 1)); pending.erase(0, end + 1); }
                if (pending.size() > 65536) { flush(pending); pending.clear(); }
            }
            flush(pending); WaitForSingleObject(child.value, INFINITE);
            if (!GetExitCodeProcess(child.value, &code)) code = 1;
        } catch (const std::exception &failure) { code = 1; error = failure.what(); }
        { std::lock_guard lock(job_mutex); active_job = nullptr; }
        if (!error.empty()) post("{\"type\":\"log\",\"text\":" + json_string("\n" + error + "\n") + "}");
        post("{\"type\":\"log\",\"text\":" + json_string("\nProcess finished with exit code " + std::to_string(code) + ".\n") + "}");
        post(std::string("{\"type\":\"finished\",\"code\":") + std::to_string(code) + ",\"play\":" + (request.play ? "true" : "false") + "}");
        busy = false;
    }

    // ---------------------------------------------------------------- WebView2
    fs::path ui_directory() const {
        if (const auto packaged = directory / L"launcher-ui"; file_exists(packaged / L"index.html")) return packaged;
#if defined(BADMAN_LAUNCHER_UI_SOURCE)
        if (const fs::path source = wide(BADMAN_LAUNCHER_UI_SOURCE); file_exists(source / L"index.html")) return source;
#endif
        throw std::runtime_error("The launcher interface (launcher-ui) is missing next to the launcher.");
    }
    void resize() {
        if (!controller) return;
        RECT bounds{}; GetClientRect(window, &bounds); controller->put_Bounds(bounds);
    }
    void fail(const wchar_t *message) { MessageBoxW(window, message, L"Badman Recompiled", MB_ICONERROR | MB_OK); DestroyWindow(window); }
    void configure(ICoreWebView2Controller *created, const fs::path &ui) {
        controller = created; controller->get_CoreWebView2(&webview);
        ComPtr<ICoreWebView2Settings> settings; webview->get_Settings(&settings);
        settings->put_AreDefaultContextMenusEnabled(FALSE);
        settings->put_IsZoomControlEnabled(FALSE);
        settings->put_IsStatusBarEnabled(FALSE);
        settings->put_AreDevToolsEnabled(GetEnvironmentVariableW(L"BADMAN_LAUNCHER_DEVTOOLS", nullptr, 0) > 0);
        if (ComPtr<ICoreWebView2Settings3> third; SUCCEEDED(settings.As(&third))) third->put_AreBrowserAcceleratorKeysEnabled(FALSE);
        ComPtr<ICoreWebView2_3> mapped;
        if (FAILED(webview.As(&mapped))) { fail(L"This WebView2 Runtime is too old. Please update it."); return; }
        mapped->SetVirtualHostNameToFolderMapping(L"launcher.local", ui.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS);
        mapped->SetVirtualHostNameToFolderMapping(L"art.local", art.c_str(), COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_ALLOW);
        webview->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [this](ICoreWebView2 *, ICoreWebView2WebMessageReceivedEventArgs *args) -> HRESULT {
                PWSTR json{};
                if (SUCCEEDED(args->get_WebMessageAsJson(&json)) && json) {
                    try { receive(parse_message(utf8(json))); } catch (const std::exception &error) { toast(error.what()); }
                    CoTaskMemFree(json);
                }
                return S_OK;
            }).Get(), nullptr);
        // Only the local launcher pages may load in this window.
        webview->add_NavigationStarting(Callback<ICoreWebView2NavigationStartingEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NavigationStartingEventArgs *args) -> HRESULT {
                PWSTR uri{}; args->get_Uri(&uri);
                if (uri && wcsncmp(uri, L"https://launcher.local/", 23) != 0) args->put_Cancel(TRUE);
                if (uri) CoTaskMemFree(uri);
                return S_OK;
            }).Get(), nullptr);
        webview->add_NewWindowRequested(Callback<ICoreWebView2NewWindowRequestedEventHandler>(
            [](ICoreWebView2 *, ICoreWebView2NewWindowRequestedEventArgs *args) -> HRESULT { args->put_Handled(TRUE); return S_OK; }).Get(), nullptr);
        if (!capture.empty()) {
            webview->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                [this](ICoreWebView2 *, ICoreWebView2NavigationCompletedEventArgs *) -> HRESULT { SetTimer(window, 1, 2600, nullptr); return S_OK; }).Get(), nullptr);
        }
        resize();
        webview->Navigate(L"https://launcher.local/index.html");
    }
    void create_webview() {
        const auto ui = ui_directory();
        const auto user_data = defaults / L"WebView2";
        const auto created = CreateCoreWebView2EnvironmentWithOptions(nullptr, user_data.c_str(), nullptr,
            Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>([this, ui](HRESULT result, ICoreWebView2Environment *environment) -> HRESULT {
                if (FAILED(result) || !environment) { fail(L"The Microsoft Edge WebView2 Runtime is required. Install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703"); return S_OK; }
                environment->CreateCoreWebView2Controller(window, Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, ui](HRESULT result, ICoreWebView2Controller *created) -> HRESULT {
                        if (FAILED(result) || !created) { fail(L"Cannot start the launcher interface (WebView2)."); return S_OK; }
                        configure(created, ui);
                        return S_OK;
                    }).Get());
                return S_OK;
            }).Get());
        if (FAILED(created)) fail(L"The Microsoft Edge WebView2 Runtime is required. Install it from https://go.microsoft.com/fwlink/p/?LinkId=2124703");
    }
    void write_capture() {
        ComPtr<IStream> stream;
        if (FAILED(SHCreateStreamOnFileEx(capture.c_str(), STGM_CREATE | STGM_WRITE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) { DestroyWindow(window); return; }
        webview->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream.Get(),
            Callback<ICoreWebView2CapturePreviewCompletedHandler>([this, stream](HRESULT) -> HRESULT {
                stream->Commit(STGC_DEFAULT); DestroyWindow(window); return S_OK;
            }).Get());
    }
};

LRESULT window_proc_impl(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<App *>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams);
        app->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
    case WM_CREATE: app->dpi = GetDpiForWindow(window); app->create_webview(); return 0;
    case WM_SIZE: app->resize(); return 0;
    case WM_DPICHANGED: {
        app->dpi = HIWORD(wparam); const auto *rect = reinterpret_cast<RECT *>(lparam);
        SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
        info->ptMinTrackSize = {MulDiv(900, app->dpi, 96), MulDiv(560, app->dpi, 96)}; return 0;
    }
    case WM_TIMER:
        if (wparam == 1) { KillTimer(window, 1); app->write_capture(); }
        return 0;
    case post_message: {
        std::unique_ptr<std::string> text(reinterpret_cast<std::string *>(lparam));
        if (text->find("\"type\":\"finished\"") != std::string::npos && app->worker.joinable()) app->worker.join();
        app->send_now(*text); return 0;
    }
    case WM_CLOSE:
        app->closing = true; app->canceled = true;
        { std::lock_guard lock(app->job_mutex); if (app->active_job) TerminateJobObject(app->active_job, ERROR_CANCELLED); }
        if (app->controller) app->controller->Close();
        DestroyWindow(window); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try { return window_proc_impl(window, message, wparam, lparam); }
    catch (...) {
        if (auto *app = reinterpret_cast<App *>(GetWindowLongPtrW(window, GWLP_USERDATA))) { app->callback_error = std::current_exception(); app->closing = true; }
        if (message == WM_CREATE) return -1;
        DestroyWindow(window); return 0;
    }
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    int result = 1;
    try {
        App app;
        int argc = 0;
        if (auto **argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i + 1 < argc; ++i) {
                if (std::wstring(argv[i]) == L"--capture") app.capture = argv[++i];
                else if (std::wstring(argv[i]) == L"--capture-tab") app.capture_tab = argv[++i];
            }
            LocalFree(argv);
        }
        WNDCLASSEXW type{sizeof(type)}; type.lpfnWndProc = window_proc; type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        type.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
            GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
        if (!type.hIcon) type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        type.hbrBackground = CreateSolidBrush(RGB(15, 65, 98)); type.lpszClassName = L"BadmanRecompLauncher";
        if (!RegisterClassExW(&type)) throw std::runtime_error("Cannot register launcher window");
        // Open at three quarters of the monitor the pointer is on (in the page's 1280:760
        // shape), centred; never below the minimum size, whatever the resolution.
        POINT pointer{}; GetCursorPos(&pointer);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT area = monitor.rcWork;
        const UINT dpi = GetDpiForSystem();
        const int area_width = area.right - area.left, area_height = area.bottom - area.top;
        int width = std::min(area_width * 3 / 4, area_height * 4 / 5 * 1280 / 760);
        width = std::max(width, MulDiv(900, dpi, 96));
        const int height = std::max(width * 760 / 1280, MulDiv(560, dpi, 96));
        const auto window = CreateWindowExW(0, type.lpszClassName, L"Badman Recompiled", WS_OVERLAPPEDWINDOW,
            area.left + (area_width - width) / 2, area.top + (area_height - height) / 2, width, height,
            nullptr, nullptr, instance, &app);
        if (!window) { if (app.callback_error) std::rethrow_exception(app.callback_error); throw std::runtime_error("Cannot create launcher window"); }
        ShowWindow(window, show); UpdateWindow(window);
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
        if (app.callback_error) std::rethrow_exception(app.callback_error);
        result = 0;
    } catch (const std::exception &error) { MessageBoxW(nullptr, wide(error.what()).c_str(), L"Badman Recompiled", MB_ICONERROR | MB_OK); }
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
