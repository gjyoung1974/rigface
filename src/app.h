// Rigface: GTK4 front panel for HPSDR radios and the QRP Labs QMX.
#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <gtk/gtk.h>
#include <sys/types.h>

#include "audio.h"
#include "dsp.h"
#include "json.h"
#include "radio.h"
#include "widgets.h"

namespace rigface {

struct Args {
    std::string radio;                     // --radio ADDR
    std::string demo;                      // --demo [hpsdr|qmx]; empty: no demo
    int rate = 0;                          // --rate
    std::string audio_sink;                // --audio-sink GST
};

/// What the QMX settings window edits; stored in the settings file as qmx_* keys.
struct QmxSettings {
    std::string port;                      // serial port; empty: find the radio on USB
    bool usb_tx_audio = true;              // transmit this computer's microphone
    bool iq_swap = false;
    double smeter_cal = -20.0;             // dBm at ADC full scale
    double mic_gain = 4.0;
    std::string audio_in, audio_out;       // GStreamer elements; empty: the QMX sound card
};

/// A front panel key; `value` is the lower line of the two-line FUNC keys.
struct Key {
    GtkWidget *button = nullptr;
    GtkWidget *value = nullptr;
};

std::string config_path();
Json load_config();
std::string fmt_freq(int64_t hz);
std::string fmt_step(int hz);
std::string fmt_width(double lo, double hi);

/// The window and the operating logic. Everything here belongs to the GTK
/// main thread except where noted.
class Panel {
public:
    Panel(GtkApplication *app, const Args &args);
    ~Panel();
    Panel(const Panel &) = delete;

    // ---- operating controls
    void set_freq(double hz);
    void tune(double delta);
    void select_band(const std::string &name);
    void set_mode(Mode mode);
    void cycle_filter();
    void cycle_agc();
    void cycle_step();
    void cycle_span();
    void swap_vfo();
    void copy_vfo();
    void set_tx(bool on, bool tune = false);
    void on_digit_tune(std::optional<int64_t> delta, int steps);
    void on_scope_tune(double f, bool snap);
    bool on_key(guint keyval, GdkModifierType state);

    // ---- connection
    void disconnect();
    /// addr: network address of an HPSDR radio, or 'qmx' / 'qmx:/dev/ttyACM0'.
    void connect(const std::string &addr, int port = 1024,
                 const std::optional<RadioInfo> &info = std::nullopt, bool keep_sim = false,
                 const std::string &audio_in = "", const std::string &audio_out = "");
    void search();
    void start_demo(const std::string &kind = "hpsdr");

    // ---- QMX settings
    QmxSettings qmx_settings();
    /// Store the settings and bring a connected QMX in line with them, which
    /// means reconnecting if the port or audio routing changed. With
    /// connect_now, connect to the QMX whatever radio is in use.
    void apply_qmx_settings(const QmxSettings &s, bool connect_now = false);
    void show_qmx_settings();
    GtkWidget *settings_win = nullptr;     // the QMX settings window while it is open

    // ---- state
    Json &vfo() { return cfg["vfo"][size_t(cfg["cur"].integer())]; }
    /// What the radio can do; an HPSDR radio's limits while there is none.
    Caps caps() const;
    int rate() const;
    Mode vfo_mode();
    int step() { return int(cfg["step"].integer(100)); }

    GtkApplication *app;
    Args args;
    Json cfg;
    std::shared_ptr<Radio> radio;
    std::optional<RadioInfo> info;
    pid_t sim = 0;                         // the simulator's process, while it runs
    std::string status = "Searching…";
    std::atomic<bool> tx_on{false};        // also read by the microphone thread
    double tx_since = 0.0;
    std::optional<std::string> entry;      // frequency being typed
    std::atomic<bool> muted{false};

    dsp::Receiver rx;
    std::unique_ptr<AudioOut> audio;
    std::unique_ptr<MicIn> mic;

    GtkWidget *win = nullptr;
    GtkWidget *status_label = nullptr, *addr_entry = nullptr;
    GtkWidget *b_trx = nullptr, *b_mode = nullptr, *b_fil = nullptr, *b_agc = nullptr,
              *b_gain = nullptr, *b_ovf = nullptr;
    GtkWidget *clock = nullptr, *vfo_label = nullptr, *sub_label = nullptr;
    std::unique_ptr<FreqDisplay> freq;
    std::unique_ptr<Meter> meter;
    std::unique_ptr<Scope> scope;
    std::vector<std::pair<std::string, Key>> band_keys;        // in BANDS order
    std::vector<std::pair<Mode, Key>> mode_keys;
    std::map<std::string, GtkWidget *> sliders;
    Key k_fil, k_agc, k_step, k_span, k_ab, k_aeqb, k_tune, k_tx;

    Key &band_key(const std::string &name);
    Key &mode_key(Mode m);

private:
    Key key(const std::string &label, std::function<void()> cb, const char *cap = nullptr,
            std::initializer_list<const char *> classes = {});
    void build();
    GtkWidget *build_conn_menu();
    void apply_caps();
    int filter_index(Mode mode);
    std::pair<double, double> passband();
    void apply_radio();
    void apply_all();
    void on_panel(const std::shared_ptr<Radio> &r, std::optional<int64_t> freq,
                  std::optional<Mode> mode);
    void on_iq(int rate, std::vector<cfloat> &&iq);            // radio's thread
    void dsp_loop();                                           // DSP thread
    void on_mic(const float *samples, size_t n);               // GStreamer thread
    bool tick();
    void on_close();
    void save_config();
    void stop_sim();

    long ticks_ = 0;
    double conn_t_ = 0.0;
    guint tick_id_ = 0;
    bool closed_ = false;
    std::optional<std::tuple<Mode, double, double>> dsp_key_;
    int offset_ = 0;
    int sim_fd_ = -1;                      // the simulator's standard output, if piped

    std::mutex q_lock_;
    std::condition_variable q_cond_;
    std::deque<std::pair<int, std::vector<cfloat>>> q_;
    bool q_stop_ = false;
    std::thread dsp_thread_;

    std::mutex radio_lock_;                // `radio`, for the microphone thread
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);   // for idle callbacks
};

/// Parse the command line and run the application; the process exit status.
int run_app(int argc, char **argv);

}  // namespace rigface
