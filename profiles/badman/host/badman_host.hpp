#pragma once
#include "badman_ge.hpp"
#include "badman_disc.hpp"
#include "badman_registry.hpp"
#include "badman_window.hpp"
#include "badman_savedata.hpp"
#include "badman_startup.hpp"
#include "badman_osk.hpp"
#include "badman_screenshot.hpp"
#include "badman_mpeg.hpp"
#include "badman_audio.hpp"
#include "badman_atrac.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/runtime.hpp"
#include <cstdint>
#include <array>
#include <chrono>
#include <functional>
#include <iosfwd>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace badman {
// An intentionally bounded bring-up host. Unimplemented imports stop execution.
// It never invents success for graphics, audio, file loading or synchronization.
class Host {
public:
    static constexpr std::uint32_t thread_return_pc = 0x08000000u;
    Host(psprecomp::Runtime &runtime, std::uint32_t arena_start,
         std::uint32_t image_end, std::uint32_t gp, std::ostream &trace);
    ~Host();
    Host(const Host &) = delete;
    Host &operator=(const Host &) = delete;
    void install(const std::vector<psprecomp::PspImport> &imports);
    void bootstrap(std::uint32_t entry, const std::string &argument);
    std::size_t import_calls() const { return import_calls_; }
    std::size_t started_threads() const { return started_threads_; }
    const std::string &last_import() const { return last_import_; }
    std::uint32_t free_bytes() const;
    std::uint32_t allocate(std::uint32_t size, bool high = false, std::uint32_t alignment = 256);
    void release(std::uint32_t address);
    void reserve(std::uint32_t address, std::uint32_t size);
    using CodeRegistrar = void (*)(psprecomp::Runtime &);
    void configure_font_module(const std::filesystem::path &prepared_elf,
        const std::string &prepared_hash, const std::string &source_hash, CodeRegistrar registrar);
    bool font_loaded() const { return font_uid_ != 0; }
    bool font_started() const { return font_started_; }
    void configure_font_directory(const std::filesystem::path &path);
    std::array<std::uint64_t, 2> power_tick_counts() const { return power_ticks_; }
    std::size_t configured_font_count() const { return registry_.font_count(); }
    void enable_window();
    void set_input_provider(Input::Provider provider);
    void set_osk_provider(Osk::Provider provider) { osk_provider_ = std::move(provider); }
    void configure_memory_stick(const std::filesystem::path &path);
    // Automatic save profile: no slot chooser/overwrite prompt; see Savedata.
    void set_automatic_profile(bool enabled);
    // Explicit boot options. Configure before bootstrap.
    void configure_startup_policy(StartupPolicy::Options options);
    const StartupPolicy &startup_policy() const { return startup_; }
    void configure_video_decoder(const std::filesystem::path &directory);
    void configure_audio_transport(std::unique_ptr<AudioTransport> transport);
    struct SasVoiceConfiguration {
        std::uint32_t pitch{0x1000}, envelope1{}, envelope2{};
        std::array<std::int32_t,4> volumes{};
    };
    struct SasConfiguration {
        std::uint32_t grain{}, max_voices{}, output_mode{}, sample_rate{};
        std::array<SasVoiceConfiguration,32> voices;
    };
    std::optional<SasConfiguration> sas_configuration(std::uint32_t core) const;
    void set_video_decoder_factory(Mpeg::DecoderFactory factory) { mpeg_.set_decoder_factory(std::move(factory)); }
    void configure_atrac_decoder(const std::filesystem::path &directory);
    void set_atrac_decoder_factory(Atrac::DecoderFactory factory);
    void wait_for_window_close();
    using SchedulerClock = std::function<std::uint64_t()>;
    using SchedulerWait = std::function<void(std::uint64_t)>;
    // Optional deterministic monotonic-nanosecond timing; configure before
    // bootstrap. Supplying neither restores the native monotonic clock/waiter.
    void set_scheduler_timing(SchedulerClock clock, SchedulerWait wait);
    const Ge &ge_state() const { return ge_; }
    struct Framebuffer { std::uint32_t address{}, stride{}, format{}; };
    struct Display { std::uint32_t width{480}, height{272}; Framebuffer current; std::uint32_t vblanks{};
        std::uint64_t interrupt_callbacks{}; };
    const Display &display_state() const { return display_; }
    using FrameObserver = std::function<void(const psprecomp::GuestMemory &, const Display &)>;
    void set_frame_observer(FrameObserver observer) { frame_observer_ = std::move(observer); }
    const Disc &disc_state() const { return disc_; }
    std::uint64_t dma_bytes() const { return dma_bytes_; }
    void dump_scheduler(std::ostream &output) const;
    Audio::Statistics audio_statistics() const noexcept { return audio_ ? audio_->statistics() : Audio::Statistics{}; }

private:
    using Context = psprecomp::AllegrexContext;
    enum class State { dormant, ready, running, waiting, ended };
    struct EventWait {
        std::int32_t event{};
        std::uint32_t wanted{}, mode{}, output{}, timeout_output{};
        std::optional<std::uint64_t> deadline;
    };
    struct AudioWait { std::int32_t channel{}; std::uint64_t ticket{}; std::uint32_t result{}; };
    struct InputWait { std::uint32_t address{}, count{}; std::uint64_t order{}; std::uint32_t return_pc{}; };
    struct Thread {
        std::int32_t uid{};
        std::string name;
        std::uint32_t entry{}, stack{}, stack_size{}, tls{}, priority{};
        std::uint32_t initial_priority{}, initial_gp{};
        std::uint32_t attributes{};
        Context context;
        State state{State::dormant};
        std::int32_t wait_thread{};
        std::int32_t wait_semaphore{}, wait_semaphore_count{};
        std::optional<std::uint64_t> delay_deadline;
        std::optional<EventWait> wait_event;
        bool wait_vblank{};
        std::optional<AudioWait> wait_audio;
        std::optional<InputWait> wait_input;
    };
    using Handler = std::function<void(Context &)>;
    void bind(const char *library, std::uint32_t nid, const char *name, Handler handler);
    void observe(const std::string &name, const Context &context);
    std::string string_at(std::uint32_t address) const;
    Thread &current();
    void finish_thread(Context &context);
    bool schedule(Context &context);
    void install_thread_services();
    std::uint64_t scheduler_now();
    void wake_expired_delays();
    bool wait_for_next_delay(Context &context);
    void poll_thread_timers(Context &context, std::uint32_t continuation);
    void poll_input_waits();
    std::optional<std::uint64_t> next_input_deadline() const;
    static void return_from_thread(psprecomp::Runtime &, Context &context);
    static Host *active_;
    void install_ge();
    void install_exit_services();
    void install_self_exit();
    void install_display();
    void install_display_timing();
    void service_display_timing(Context &context);
    std::optional<std::uint64_t> next_display_deadline() const;
    void invoke_display_interrupt(Context &context, std::uint32_t index);
    bool in_interrupt_context() const { return in_ge_callback_ || in_display_callback_; }
    void present_display(bool submitted = false);
    // Marks the dungeon level's extent on a scene for the wider view.
    void note_level(Scene &scene) const;
    void install_io();
    void install_modules();
    void install_semaphores();
    void install_events();
    std::size_t count_event_waiters(std::int32_t event) const;
    std::optional<std::uint64_t> next_event_deadline() const;
    void wake_expired_event_waits(std::uint64_t now);
    void complete_event_wait(Thread &, std::uint32_t result, std::uint64_t now,
                             std::optional<std::uint32_t> pattern = {});
    void install_savedata();
    void install_osk();
    void install_screenshot();
    void refresh_osk_dialog();
    void install_audio();
    void poll_audio_waits();
    std::optional<std::uint64_t> next_audio_poll_deadline() const;
    bool channel_audio_waiter(std::int32_t channel) const;
    void output_audio(Context &, bool panned);
    void refresh_savedata_dialog();
    void install_mpeg();
    void install_atrac();
    void install_dma();
    void enter_mpeg_callback(Context &, const Mpeg::RingCallback &);
    static void return_from_mpeg_callback(psprecomp::Runtime &, Context &);
    void select_module_gp(Context &ctx) const;
    void observe_entry(Context &ctx);
    void install_entry_hooks();
    Input::PacketFilter controller_filter(std::int32_t uid, std::uint32_t return_pc);
    static void module_dispatch(psprecomp::Runtime &, Context &, std::uint32_t, std::int32_t);
    static void module_chain(psprecomp::Runtime &, Context &, std::uint32_t, std::uint32_t);
    void load_font(Context &ctx);
    void invoke_ge_callback(const GeInterrupt &interrupt);
    psprecomp::Runtime &runtime_;
    std::ostream &trace_;
    std::uint32_t image_end_, gp_;
    std::map<std::uint32_t, std::uint32_t> free_, allocations_;
    std::map<std::int32_t, Thread> threads_;
    SchedulerClock scheduler_clock_;
    SchedulerWait scheduler_wait_;
    std::uint64_t scheduler_last_now_{};
    std::uint64_t input_wait_order_{};
    std::map<std::int32_t, std::uint32_t> partitions_;
    struct FixedPool {
        std::uint32_t base{}, stride{};
        std::vector<std::uint8_t> used;
    };
    std::map<std::int32_t, FixedPool> pools_;
    struct Mutex { std::uint32_t uid{}, owner{}, count{}, attributes{}; };
    std::map<std::uint32_t, Mutex> mutexes_;
    struct Event {
        std::uint32_t initial{}, bits{}, attributes{};
        std::string name;
        std::vector<std::int32_t> waiters;
    };
    std::map<std::int32_t, Event> events_;
    struct Semaphore {
        std::string name;
        std::uint32_t attributes{};
        std::int32_t initial{}, count{}, maximum{};
        std::vector<std::int32_t> waiters;
    };
    std::map<std::int32_t, Semaphore> semaphores_;
    void sync_mutex(std::uint32_t address, const Mutex &mutex);
    std::int32_t next_uid_{16};
    std::size_t import_calls_{}, started_threads_{};
    std::string last_import_;
    bool interrupts_enabled_{true};
    std::array<std::uint32_t, 3> clock_frequencies_{222, 222, 111};
    std::array<std::uint64_t, 2> power_ticks_{};
    std::array<std::chrono::steady_clock::time_point, 2> power_activity_{};
    const std::chrono::steady_clock::time_point boot_time_{std::chrono::steady_clock::now()};
    struct Callback { std::string name; std::uint32_t entry{}, argument{}; std::int32_t owner{}; std::uint32_t pending{}; };
    std::map<std::int32_t, Callback> callbacks_;
    std::int32_t exit_callback_{};
    std::array<bool, 8> utility_modules_{};
    bool in_ge_callback_{};
    bool in_display_callback_{};
    std::uint32_t ge_callback_stack_{};
    Ge ge_;
    Display display_;
    FrameObserver frame_observer_;
    struct DisplayInterrupt { std::uint32_t function{}, argument{}, gp{}; bool enabled{}, pending{}; };
    std::array<DisplayInterrupt, 16> display_interrupts_{};
    std::uint64_t display_origin_ns_{}, display_tick_{};
    std::uint32_t display_callback_stack_{};
    static constexpr std::uint64_t display_period_ns = 16683333;
    std::uint64_t latch_lateness_ns_{}; // Scheduler time between the latching vblank and its service.
    Framebuffer pending_framebuffer_;
    bool framebuffer_pending_{};
    std::unique_ptr<Window> window_;
    bool window_closed_{};
    Disc disc_;
    Registry registry_;
    Input input_;
    std::unique_ptr<Audio> audio_;
    struct AudioChannel { bool reserved{}; std::uint32_t frames{}, left{0x8000}, right{0x8000}; std::uint64_t last_ticket{}; };
    std::array<AudioChannel,Audio::channel_count> audio_channels_{};
    std::optional<std::uint64_t> audio_poll_deadline_;
    std::map<std::uint32_t,SasConfiguration> sas_cores_;
    Savedata savedata_;
    StartupPolicy startup_;
    Osk osk_;
    Screenshot screenshot_;
    std::filesystem::path memory_stick_root_;
    Osk::Provider osk_provider_;
    std::string utility_message_;
    Mpeg mpeg_;
    Atrac atrac_;
    std::uint64_t dma_bytes_{};
    struct MpegCall { Context caller; std::uint32_t ring{}; };
    std::map<std::int32_t, MpegCall> mpeg_calls_;
    static constexpr std::uint32_t mpeg_return_pc = 0x08000008u;
    std::optional<psprecomp::Elf32Image> font_image_;
    std::string font_source_hash_;
    CodeRegistrar font_registrar_{};
    std::uint32_t font_base_{}, font_end_{}, font_gp_{};
    std::int32_t font_uid_{};
    bool font_started_{};
    std::uint32_t font_reservation_size_{};
    bool font_reserved_{}, font_stopped_{};
};
}
