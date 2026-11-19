// Rigface: the window and the operating logic.
#include "app.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <glib-unix.h>

#include "protocol.h"
#include "qmx.h"
#include "style_css.h"

namespace rigface {

namespace {

constexpr double TX_TIMEOUT = 180.0;       // seconds
constexpr size_t QUEUE_MAX = 40;           // I/Q blocks waiting for the DSP thread

struct Band {
    const char *name;
    double lo, hi;
    int64_t freq;                          // default frequency and mode
    Mode mode;
};
const Band BANDS[] = {
    {"160", 1.8e6, 2.0e6, 1'840'000, Mode::LSB},     {"80", 3.5e6, 4.0e6, 3'700'000, Mode::LSB},
    {"60", 5.25e6, 5.45e6, 5'357'000, Mode::USB},    {"40", 7.0e6, 7.3e6, 7'100'000, Mode::LSB},
    {"30", 10.1e6, 10.15e6, 10'120'000, Mode::CW},   {"20", 14.0e6, 14.35e6, 14'200'000, Mode::USB},
    {"17", 18.068e6, 18.168e6, 18'130'000, Mode::USB}, {"15", 21.0e6, 21.45e6, 21'250'000, Mode::USB},
    {"12", 24.89e6, 24.99e6, 24'950'000, Mode::USB}, {"10", 28.0e6, 29.7e6, 28'500'000, Mode::USB},
    {"6", 50.0e6, 54.0e6, 50'150'000, Mode::USB},
};

struct Passband {
    double lo, hi;
};
const Passband *filters(Mode m)
{
    static const Passband usb[] = {{100, 3000}, {200, 2600}, {300, 2100}};
    static const Passband lsb[] = {{-3000, -100}, {-2600, -200}, {-2100, -300}};
    static const Passband cw[] = {{-500, 500}, {-250, 250}, {-125, 125}};
    static const Passband am[] = {{-4500, 4500}, {-3000, 3000}, {-1500, 1500}};
    static const Passband fm[] = {{-7500, 7500}, {-5000, 5000}, {-3500, 3500}};
    switch (m) {
    case Mode::LSB: return lsb;
    case Mode::CW: return cw;
    case Mode::AM: return am;
    case Mode::FM: return fm;
    case Mode::USB: break;
    }
    return usb;
}

const int STEPS[] = {1, 10, 100, 1000, 5000, 10000};

const char *const DEFAULTS = R"({
 "vfo": [{"freq": 7100000, "mode": "LSB"}, {"freq": 14200000, "mode": "USB"}],
 "cur": 0, "fil": {}, "bands": {}, "step": 100, "rate": 192000, "agc": "MID",
 "af": 35, "rf_gain": 20, "power": 25, "mic_gain": 4.0,
 "radio": "", "iq_swap": false, "audio_sink": "", "audio_source": "",
 "qmx_iq_swap": false, "qmx_usb_tx_audio": true, "qmx_audio_in": "", "qmx_audio_out": "",
 "qmx_port": ""
})";

std::string fmt(const char *format, ...) G_GNUC_PRINTF(1, 2);
std::string fmt(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    char *s = g_strdup_vprintf(format, ap);
    va_end(ap);
    std::string out = s;
    g_free(s);
    return out;
}

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

const char *band_of(double freq)
{
    for (const Band &b : BANDS)
        if (b.lo <= freq && freq <= b.hi)
            return b.name;
    return nullptr;
}

void set_class(GtkWidget *w, const char *name, bool on)
{
    if (on)
        gtk_widget_add_css_class(w, name);
    else
        gtk_widget_remove_css_class(w, name);
}

void set_label(GtkWidget *label, const std::string &text)
{
    gtk_label_set_label(GTK_LABEL(label), text.c_str());
}

GtkWidget *new_label(const char *text, const char *css, float xalign = 0.5f)
{
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), xalign);
    gtk_widget_add_css_class(l, css);
    return l;
}

GtkWidget *badge(const char *text, const char *extra = nullptr)
{
    GtkWidget *l = new_label(text, "badge");
    if (extra)
        gtk_widget_add_css_class(l, extra);
    return l;
}

GtkWidget *row(GtkWidget *parent, const char *caption)
{
    GtkWidget *r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_box_append(GTK_BOX(r), new_label(caption, "rowcap", 0.0f));
    gtk_box_append(GTK_BOX(parent), r);
    return r;
}

// ---- signal handlers carrying a std::function -----------------------------------

using Fn = std::function<void()>;

void call_fn(GtkWidget *, gpointer data)
{
    (*static_cast<Fn *>(data))();
}

void free_fn(gpointer data, GClosure *)
{
    delete static_cast<Fn *>(data);
}

/// Connect a (widget, user_data) signal such as "clicked" to fn.
void on_signal(gpointer object, const char *signal, Fn fn)
{
    g_signal_connect_data(object, signal, G_CALLBACK(call_fn), new Fn(std::move(fn)), free_fn,
                          GConnectFlags(0));
}

struct Idle {
    std::shared_ptr<bool> alive;
    Fn fn;
};

gboolean run_idle(gpointer data)
{
    auto *i = static_cast<Idle *>(data);
    if (*i->alive)
        i->fn();
    return G_SOURCE_REMOVE;
}

void free_idle(gpointer data)
{
    delete static_cast<Idle *>(data);
}

/// Run fn on the GTK thread, unless the panel is gone by then. Any thread.
void idle(const std::shared_ptr<bool> &alive, Fn fn)
{
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, run_idle, new Idle{alive, std::move(fn)}, free_idle);
}

gboolean key_pressed(GtkEventControllerKey *, guint keyval, guint, GdkModifierType state,
                     gpointer data)
{
    return static_cast<Panel *>(data)->on_key(keyval, state);
}

// ---- the simulator process ---------------------------------------------------------

int free_udp_port()
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof a;
    int port = 0;
    if (s >= 0 && bind(s, reinterpret_cast<sockaddr *>(&a), sizeof a) == 0
        && getsockname(s, reinterpret_cast<sockaddr *>(&a), &len) == 0)
        port = ntohs(a.sin_port);
    if (s >= 0)
        close(s);
    return port;
}

/// `rigface-sim`, which is installed next to this program.
std::string sim_path()
{
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n > 0) {
        std::string dir(buf, size_t(n));
        dir.erase(dir.rfind('/') + 1);
        if (access((dir + "rigface-sim").c_str(), X_OK) == 0)
            return dir + "rigface-sim";
    }
    return "rigface-sim";                  // look on PATH
}

/// Start the simulator. With out_fd its standard output comes back on a pipe,
/// otherwise it is discarded. Returns 0 if it could not be started.
pid_t spawn_sim(std::vector<std::string> args, int *out_fd)
{
    const std::string path = sim_path();
    args.insert(args.begin(), path);
    args.push_back("--parent");
    args.push_back(std::to_string(getpid()));
    std::vector<char *> argv;
    for (std::string &a : args)
        argv.push_back(a.data());
    argv.push_back(nullptr);

    int fds[2] = {-1, -1};
    if (out_fd && pipe(fds) != 0)
        return 0;
    pid_t pid = fork();
    if (pid == 0) {
        int out = out_fd ? fds[1] : open("/dev/null", O_WRONLY);
        if (out >= 0)
            dup2(out, STDOUT_FILENO);
        if (out_fd)
            close(fds[0]);
        execvp(path.c_str(), argv.data());
        _exit(127);
    }
    if (out_fd) {
        close(fds[1]);
        if (pid > 0)
            *out_fd = fds[0];
        else
            close(fds[0]);
    }
    return pid > 0 ? pid : 0;
}

/// The first line a process prints, or "" if none arrives in time.
std::string read_line(int fd, double timeout)
{
    std::string line;
    const double end = monotonic() + timeout;
    for (;;) {
        pollfd p{fd, POLLIN, 0};
        int ms = int((end - monotonic()) * 1000);
        if (ms <= 0 || poll(&p, 1, ms) <= 0)
            return "";
        char c;
        if (read(fd, &c, 1) != 1)
            return "";
        if (c == '\n')
            return trim(line);
        line += c;
    }
}

}  // namespace

// ---- settings ----------------------------------------------------------------------

static std::string config_file(const char *program)
{
    char *p = g_build_filename(g_get_user_config_dir(), program, "config.json", nullptr);
    std::string out = p;
    g_free(p);
    return out;
}

std::string config_path()
{
    return config_file("rigface");
}

Json load_config()
{
    Json cfg = *Json::parse(DEFAULTS);
    cfg["qmx_smeter_cal"] = qmx::caps().smeter_cal;
    const Json defaults = cfg;
    // settings from before the program was renamed are read once if there are no new ones
    for (const std::string &path : {config_path(), config_file("radioberry-gui")}) {
        char *text = nullptr;
        if (!g_file_get_contents(path.c_str(), &text, nullptr, nullptr))
            continue;
        std::optional<Json> saved = Json::parse(text);
        g_free(text);
        if (!saved || !saved->is_object())
            continue;
        for (const auto &[key, value] : saved->object())
            cfg[key] = value;
        break;
    }
    // A hand-edited file must not be able to crash the panel.
    const Json &v = cfg.get("vfo");
    bool ok = v.is_array() && v.size() == 2;
    for (size_t i = 0; ok && i < 2; ++i)
        ok = v.at(i).is_object() && v.at(i).get("freq").is_number();
    if (!ok)
        cfg["vfo"] = defaults.get("vfo");
    cfg["cur"] = int(cfg["cur"].integer() == 1);
    for (const char *key : {"fil", "bands"})
        if (!cfg[key].is_object())
            cfg[key] = Json(Json::Object());
    if (cfg["step"].integer() < 1)
        cfg["step"] = 100;
    return cfg;
}

std::string fmt_freq(int64_t hz)
{
    std::string s = fmt("%08lld", static_cast<long long>(hz));
    s = s.substr(0, 2) + "." + s.substr(2, 3) + "." + s.substr(5);
    return s.substr(std::min(s.find_first_not_of('0'), s.size()));
}

std::string fmt_step(int hz)
{
    return hz >= 1000 ? fmt("%g kHz", hz / 1000.0) : fmt("%d Hz", hz);
}

std::string fmt_width(double lo, double hi)
{
    const double w = hi - lo;
    return w >= 1000 ? fmt("%gk", w / 1000.0) : fmt("%d", int(w));
}

// ---- construction -------------------------------------------------------------------

Panel::Panel(GtkApplication *app_, const Args &args_) : app(app_), args(args_), cfg(load_config())
{
    if (args.rate)
        cfg["rate"] = args.rate;
    try {
        audio = std::make_unique<AudioOut>(!args.audio_sink.empty() ? args.audio_sink
                                                                    : cfg["audio_sink"].str());
    } catch (const AudioError &e) {
        std::fprintf(stderr, "audio output unavailable: %s\n", e.what());
    }
    mic = std::make_unique<MicIn>([this](const float *s, size_t n) { on_mic(s, n); },
                                  cfg["audio_source"].str());
    dsp_thread_ = std::thread(&Panel::dsp_loop, this);

    build();
    apply_caps();
    tick_id_ = g_timeout_add(
        33, +[](gpointer p) -> gboolean { return static_cast<Panel *>(p)->tick(); }, this);

    const std::string saved = cfg["radio"].str();
    if (!args.demo.empty())
        start_demo(args.demo);
    else if (!args.radio.empty() || !saved.empty())
        connect(!args.radio.empty() ? args.radio : saved);
    else
        search();
}

Panel::~Panel()
{
    on_close();
    {
        std::lock_guard<std::mutex> g(q_lock_);
        q_stop_ = true;
    }
    q_cond_.notify_all();
    if (dsp_thread_.joinable())
        dsp_thread_.join();
}

Key &Panel::band_key(const std::string &name)
{
    for (auto &[n, k] : band_keys)
        if (n == name)
            return k;
    return band_keys.front().second;
}

Key &Panel::mode_key(Mode m)
{
    for (auto &[mode, k] : mode_keys)
        if (mode == m)
            return k;
    return mode_keys.front().second;
}

Key Panel::key(const std::string &label, std::function<void()> cb, const char *cap,
               std::initializer_list<const char *> classes)
{
    Key k;
    k.button = gtk_button_new();
    gtk_widget_set_focusable(k.button, FALSE);
    gtk_widget_add_css_class(k.button, "key");
    for (const char *c : classes)
        gtk_widget_add_css_class(k.button, c);
    if (cap) {
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
        gtk_widget_set_valign(box, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(box), new_label(cap, "cap"));
        k.value = new_label(label.c_str(), "val");
        gtk_box_append(GTK_BOX(box), k.value);
        gtk_button_set_child(GTK_BUTTON(k.button), box);
    } else {
        gtk_button_set_label(GTK_BUTTON(k.button), label.c_str());
    }
    on_signal(k.button, "clicked", std::move(cb));
    gtk_widget_set_hexpand(k.button, TRUE);
    return k;
}

void Panel::build()
{
    GtkCssProvider *prov = gtk_css_provider_new();
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS       // the replacement needs GTK 4.12
    gtk_css_provider_load_from_data(prov, STYLE_CSS, -1);
    G_GNUC_END_IGNORE_DEPRECATIONS
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(prov),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(prov);

    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, nullptr);

    win = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(win), "Rigface");
    gtk_widget_add_css_class(win, "rb");
    gtk_window_set_default_size(GTK_WINDOW(win), 1120, 720);
    g_signal_connect(win, "close-request", G_CALLBACK(+[](GtkWindow *, gpointer p) -> gboolean {
                         static_cast<Panel *>(p)->on_close();
                         return FALSE;
                     }),
                     this);
    GtkEventController *keys = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(keys, GTK_PHASE_CAPTURE);
    g_signal_connect(keys, "key-pressed", G_CALLBACK(key_pressed), this);
    gtk_widget_add_controller(win, keys);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_top(root, 10);
    gtk_widget_set_margin_bottom(root, 10);
    gtk_widget_set_margin_start(root, 10);
    gtk_widget_set_margin_end(root, 10);
    gtk_window_set_child(GTK_WINDOW(win), root);

    // --- bezel strip: brand, connection ---
    GtkWidget *bezel = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_add_css_class(bezel, "bezel");
    gtk_box_append(GTK_BOX(bezel), new_label("RIGFACE", "brand"));
    status_label = new_label("", "status", 0.0f);
    gtk_widget_set_hexpand(status_label, TRUE);
    gtk_box_append(GTK_BOX(bezel), status_label);
    gtk_box_append(GTK_BOX(bezel), build_conn_menu());
    gtk_box_append(GTK_BOX(root), bezel);

    // --- the "LCD" ---
    GtkWidget *screen = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_add_css_class(screen, "screen");
    gtk_widget_set_vexpand(screen, TRUE);
    gtk_box_append(GTK_BOX(root), screen);

    GtkWidget *top = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    b_trx = badge("RX", "rx");
    b_mode = badge("USB", "mode");
    b_fil = badge("");
    b_agc = badge("");
    b_gain = badge("");
    b_ovf = badge("OVF", "off");
    for (GtkWidget *b : {b_trx, b_mode, b_fil, b_agc, b_gain, b_ovf})
        gtk_box_append(GTK_BOX(top), b);
    clock = new_label("", "clock", 1.0f);
    gtk_widget_set_hexpand(clock, TRUE);
    gtk_box_append(GTK_BOX(top), clock);
    gtk_box_append(GTK_BOX(screen), top);

    GtkWidget *mid = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_valign(vbox, GTK_ALIGN_CENTER);
    vfo_label = new_label("VFO A", "vfo", 0.0f);
    sub_label = new_label("", "sub", 0.0f);
    gtk_box_append(GTK_BOX(vbox), vfo_label);
    gtk_box_append(GTK_BOX(vbox), sub_label);
    gtk_box_append(GTK_BOX(mid), vbox);
    freq = std::make_unique<FreqDisplay>(
        [this](std::optional<int64_t> delta, int steps) { on_digit_tune(delta, steps); });
    gtk_box_append(GTK_BOX(mid), freq->widget);
    meter = std::make_unique<Meter>();
    gtk_widget_set_valign(meter->widget, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(mid), meter->widget);
    gtk_box_append(GTK_BOX(screen), mid);

    scope = std::make_unique<Scope>([this](double f, bool snap) { on_scope_tune(f, snap); },
                                    [this](int n) { tune(double(n) * step()); });
    gtk_box_append(GTK_BOX(screen), scope->widget);

    // --- keys and knobs ---
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
    gtk_box_append(GTK_BOX(root), controls);
    GtkWidget *left = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(left, TRUE);
    gtk_box_append(GTK_BOX(controls), left);

    GtkWidget *r = row(left, "BAND");
    for (const Band &b : BANDS) {
        const std::string name = b.name;
        band_keys.emplace_back(name, key(name, [this, name] { select_band(name); }));
        gtk_box_append(GTK_BOX(r), band_keys.back().second.button);
    }

    r = row(left, "MODE");
    for (Mode m : MODES) {
        mode_keys.emplace_back(m, key(mode_name(m), [this, m] { set_mode(m); }));
        gtk_box_append(GTK_BOX(r), mode_keys.back().second.button);
    }

    r = row(left, "FUNC");
    k_fil = key("", [this] { cycle_filter(); }, "FILTER");
    k_agc = key("", [this] { cycle_agc(); }, "AGC");
    k_step = key("", [this] { cycle_step(); }, "STEP");
    k_span = key("", [this] { cycle_span(); }, "SPAN");
    k_ab = key("A/B", [this] { swap_vfo(); });
    k_aeqb = key("A=B", [this] { copy_vfo(); });
    for (const Key *k : {&k_fil, &k_agc, &k_step, &k_span, &k_ab, &k_aeqb})
        gtk_box_append(GTK_BOX(r), k->button);

    GtkWidget *right = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_box_append(GTK_BOX(controls), right);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    gtk_box_append(GTK_BOX(right), grid);
    struct Knob {
        const char *name, *key;
        int lo, hi;
        std::string (*text)(int);
    };
    const Knob knobs[] = {
        {"AF", "af", 0, 100, +[](int v) { return fmt("%d", v); }},
        {"RF", "rf_gain", -12, 48, +[](int v) { return fmt("%+d dB", v); }},
        {"PWR", "power", 0, 100, +[](int v) { return fmt("%d %%", v); }},
    };
    int line = 0;
    for (const Knob &k : knobs) {
        GtkWidget *sc = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, k.lo, k.hi, 1);
        gtk_scale_set_draw_value(GTK_SCALE(sc), FALSE);
        gtk_widget_set_focusable(sc, FALSE);
        gtk_widget_set_hexpand(sc, TRUE);
        gtk_widget_set_size_request(sc, 190, -1);
        const int start = int(std::clamp<int64_t>(cfg[k.key].integer(), k.lo, k.hi));
        cfg[k.key] = start;
        gtk_range_set_value(GTK_RANGE(sc), start);
        GtkWidget *val = new_label(k.text(start).c_str(), "knobval", 1.0f);
        sliders[k.key] = sc;
        const std::string name = k.key;
        auto text = k.text;
        on_signal(sc, "value-changed", [this, sc, val, name, text] {
            const int v = int(gtk_range_get_value(GTK_RANGE(sc)));
            cfg[name] = v;
            set_label(val, text(v));
            apply_all();
        });
        gtk_grid_attach(GTK_GRID(grid), new_label(k.name, "rowcap", 0.0f), 0, line, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), sc, 1, line, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), val, 2, line, 1, 1);
        ++line;
    }
    GtkWidget *txrow = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_set_homogeneous(GTK_BOX(txrow), TRUE);
    k_tune = key("TUNE", [this] { set_tx(!tx_on, true); }, nullptr, {"tx", "tune"});
    k_tx = key("TRANSMIT", [this] { set_tx(!tx_on); }, nullptr, {"tx"});
    gtk_box_append(GTK_BOX(txrow), k_tune.button);
    gtk_box_append(GTK_BOX(txrow), k_tx.button);
    gtk_box_append(GTK_BOX(right), txrow);
}

GtkWidget *Panel::build_conn_menu()
{
    GtkWidget *mb = gtk_menu_button_new();
    gtk_menu_button_set_label(GTK_MENU_BUTTON(mb), "RADIO");
    gtk_widget_add_css_class(mb, "conn");
    gtk_widget_set_focusable(mb, FALSE);
    GtkWidget *pop = gtk_popover_new();
    gtk_widget_add_css_class(pop, "connpop");
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget *cap = gtk_label_new("Radio address");
    gtk_label_set_xalign(GTK_LABEL(cap), 0.0f);
    gtk_box_append(GTK_BOX(box), cap);
    addr_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(addr_entry), "192.168.1.50, or qmx");
    gtk_editable_set_text(GTK_EDITABLE(addr_entry), cfg["radio"].str().c_str());
    gtk_box_append(GTK_BOX(box), addr_entry);

    Fn typed = [this] { connect(trim(gtk_editable_get_text(GTK_EDITABLE(addr_entry)))); };
    auto go = [pop](Fn fn) {
        return [pop, fn] {
            gtk_popover_popdown(GTK_POPOVER(pop));
            fn();
        };
    };
    on_signal(addr_entry, "activate", go(typed));
    const std::pair<const char *, Fn> items[] = {
        {"Connect", typed},
        {"Search network and USB", [this] { search(); }},
        {"Demo (simulated band)", [this] { start_demo(); }},
        {"Demo (simulated QMX)", [this] { start_demo("qmx"); }},
        {"Disconnect", [this] { disconnect(); }},
        {"QMX settings…", [this] { show_qmx_settings(); }},
    };
    for (const auto &[label, fn] : items) {
        GtkWidget *b = gtk_button_new_with_label(label);
        on_signal(b, "clicked", go(fn));
        gtk_box_append(GTK_BOX(box), b);
    }
    gtk_popover_set_child(GTK_POPOVER(pop), box);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(mb), pop);
    return mb;
}

// ---- state -> radio / display -------------------------------------------------------

Caps Panel::caps() const
{
    return radio ? radio->caps : hpsdr::caps();
}

int Panel::rate() const
{
    const std::vector<int> rates = caps().rates;
    const int want = int(cfg.get("rate").integer());
    return std::find(rates.begin(), rates.end(), want) != rates.end() ? want : rates.back();
}

Mode Panel::vfo_mode()
{
    return mode_from(vfo()["mode"].str()).value_or(Mode::USB);
}

/// Fit the panel to the connected radio, then refresh everything.
void Panel::apply_caps()
{
    const Caps c = caps();
    for (auto &[m, k] : mode_keys)
        gtk_widget_set_sensitive(k.button, c.has_mode(m));
    for (const Band &b : BANDS)
        gtk_widget_set_sensitive(band_key(b.name).button,
                                 double(c.freq_lo) <= b.lo && b.hi <= double(c.freq_hi));
    gtk_widget_set_sensitive(sliders["rf_gain"], c.has_rf_gain);
    gtk_widget_set_sensitive(sliders["power"], c.has_drive);
    gtk_widget_set_sensitive(k_span.button, c.rates.size() > 1);
    gtk_widget_set_visible(b_gain, c.has_rf_gain);
    meter->po_max = c.max_power;
    const bool is_qmx = std::strcmp(c.kind, "qmx") == 0;
    rx.iq_swap = cfg[is_qmx ? "qmx_iq_swap" : "iq_swap"].boolean();
    for (size_t i = 0; i < 2; ++i) {
        Json &v = cfg["vfo"][i];
        const std::optional<Mode> m = mode_from(v["mode"].str());
        if (!m || !c.has_mode(*m))
            v["mode"] = "USB";
        v["freq"] = std::clamp<int64_t>(v["freq"].integer(), c.freq_lo, c.freq_hi);
    }
    apply_all();
}

int Panel::filter_index(Mode mode)
{
    const Json &saved = cfg["fil"].get(mode_name(mode));
    const int64_t fallback = (mode == Mode::AM || mode == Mode::FM) ? 0 : 1;
    return int(((saved.integer(fallback) % 3) + 3) % 3);
}

std::pair<double, double> Panel::passband()
{
    const Mode mode = vfo_mode();
    const Passband &p = filters(mode)[filter_index(mode)];
    return {p.lo, p.hi};
}

void Panel::apply_radio()
{
    if (!radio)
        return;
    const int64_t f = vfo()["freq"].integer();
    radio->rx_freq = f;
    radio->tx_freq = f;
    radio->set_mode(vfo_mode());
    radio->rf_gain = int(cfg["rf_gain"].integer());
    radio->drive = int(std::lround(cfg["power"].num() * 2.55));
    radio->set_mic_gain(cfg["mic_gain"].num(4.0));
    radio->set_rate(rate());
}

/// Push the whole state to DSP, radio and every readout.
void Panel::apply_all()
{
    const Mode mode = vfo_mode();
    const auto [lo, hi] = passband();
    const std::tuple<Mode, double, double> dsp_key{mode, lo, hi};
    if (dsp_key_ != dsp_key) {
        dsp_key_ = dsp_key;
        rx.set_mode(mode, lo, hi);
    }
    const std::string agc = cfg["agc"].str("MID");
    rx.agc = dsp::agc_from(agc).value_or(dsp::Agc::MID);
    const double af = cfg["af"].num() / 100.0;
    rx.volume = float(af * af * 3.0);
    apply_radio();

    // A radio with an IF (the QMX) is not tuned to the centre of its I/Q.
    const int off = offset_ = radio ? radio->if_offset.load() : 0;
    const int64_t f = vfo()["freq"].integer();
    rx.offset = off;
    scope->pass_lo = lo;
    scope->pass_hi = hi;
    scope->mark = off;
    if (scope->rate != rate()) {
        scope->rate = rate();
        scope->clear();
    }
    scope->retune(f - off);
    scope->queue_draw();
    freq->freq = f;
    freq->queue_draw();

    const size_t cur = size_t(cfg["cur"].integer());
    Json &other = cfg["vfo"][1 - cur];
    set_label(vfo_label, cur ? "VFO B" : "VFO A");
    set_label(sub_label, fmt("%s  %s  %s", cur ? "A" : "B", fmt_freq(other["freq"].integer()).c_str(),
                             other["mode"].str().c_str()));
    set_label(b_mode, mode_name(mode));
    const int fi = filter_index(mode);
    set_label(b_fil, fmt("FIL%d %s", fi + 1, fmt_width(lo, hi).c_str()));
    set_label(b_agc, agc == "OFF" ? "AGC OFF" : "AGC-" + agc.substr(0, 1));
    set_label(b_gain, fmt("RF %+d dB", int(cfg["rf_gain"].integer())));
    set_label(k_fil.value, fmt("FIL%d  %s", fi + 1, fmt_width(lo, hi).c_str()));
    set_label(k_agc.value, agc);
    set_label(k_step.value, fmt_step(step()));
    set_label(k_span.value, fmt("%d kHz", rate() / 1000));
    for (auto &[m, k] : mode_keys)
        set_class(k.button, "on", m == mode);
    const char *band = band_of(double(f));
    for (auto &[name, k] : band_keys)
        set_class(k.button, "on", band && name == band);
}

// ---- operating controls ----------------------------------------------------------------

void Panel::set_freq(double hz)
{
    const Caps c = caps();
    vfo()["freq"] = std::clamp<int64_t>(std::llround(hz), c.freq_lo, c.freq_hi);
    apply_all();
}

void Panel::tune(double delta)
{
    const double s = step();
    set_freq(std::round((double(vfo()["freq"].integer()) + delta) / s) * s);
}

void Panel::on_digit_tune(std::optional<int64_t> delta, int steps)
{
    if (!delta)
        tune(double(steps) * step());
    else
        set_freq(double(vfo()["freq"].integer() + *delta));
}

void Panel::on_scope_tune(double f, bool snap)
{
    const double grid = snap ? std::max(step(), 100) : 10;
    set_freq(std::round(f / grid) * grid);
}

void Panel::select_band(const std::string &name)
{
    const char *cur = band_of(double(vfo()["freq"].integer()));
    if (cur) {
        const Json now = vfo();
        cfg["bands"][cur] = now;
    }
    const Json saved = cfg["bands"].get(name);
    if (saved.is_object() && saved.get("freq").is_number() && !(cur && name == cur)) {
        vfo()["freq"] = saved.get("freq").integer();
        vfo()["mode"] = saved.get("mode").str("USB");
    } else {
        for (const Band &b : BANDS)
            if (name == b.name) {
                vfo()["freq"] = b.freq;
                vfo()["mode"] = mode_name(b.mode);
            }
    }
    apply_caps();                          // a saved entry may not suit this radio
}

void Panel::set_mode(Mode mode)
{
    vfo()["mode"] = mode_name(mode);
    apply_all();
}

void Panel::cycle_filter()
{
    const Mode mode = vfo_mode();
    cfg["fil"][mode_name(mode)] = (filter_index(mode) + 1) % 3;
    apply_all();
}

void Panel::cycle_agc()
{
    const dsp::Agc now = dsp::agc_from(cfg["agc"].str()).value_or(dsp::Agc::MID);
    const size_t n = std::size(dsp::AGCS);
    const size_t i = size_t(std::find(std::begin(dsp::AGCS), std::end(dsp::AGCS), now)
                            - std::begin(dsp::AGCS));
    cfg["agc"] = dsp::agc_name(dsp::AGCS[(i + 1) % n]);
    apply_all();
}

void Panel::cycle_step()
{
    const int *at = std::find(std::begin(STEPS), std::end(STEPS), step());
    const size_t i = at == std::end(STEPS) ? 1 : size_t(at - std::begin(STEPS));
    cfg["step"] = STEPS[(i + 1) % std::size(STEPS)];
    apply_all();
}

void Panel::cycle_span()
{
    const std::vector<int> rates = caps().rates;
    const size_t i = size_t(std::find(rates.begin(), rates.end(), rate()) - rates.begin());
    cfg["rate"] = rates[(i + 1) % rates.size()];
    apply_all();
}

void Panel::swap_vfo()
{
    cfg["cur"] = int(1 - cfg["cur"].integer());
    apply_all();
}

void Panel::copy_vfo()
{
    const Json now = vfo();
    cfg["vfo"][size_t(1 - cfg["cur"].integer())] = now;
    apply_all();
}

/// The radio was tuned with its own knob (called on the GTK thread).
void Panel::on_panel(const std::shared_ptr<Radio> &r, std::optional<int64_t> f,
                     std::optional<Mode> mode)
{
    if (r != radio || tx_on)
        return;
    if (f && r->rx_freq == *f)
        vfo()["freq"] = *f;
    if (mode && r->caps.has_mode(*mode) && r->mode() == *mode)
        vfo()["mode"] = mode_name(*mode);
    apply_all();
}

void Panel::set_tx(bool on, bool tune)
{
    const std::shared_ptr<Radio> r = radio;
    if (on && !(r && r->streaming))
        return;
    if (on == tx_on)
        return;
    tx_on = on;
    if (on) {
        tx_since = monotonic();
        muted = true;
        const bool carrier = tune || vfo_mode() == Mode::CW;
        r->tx_clear();
        r->tx_carrier = carrier;
        r->mox = true;
        if (!carrier) {
            try {
                mic->start();
            } catch (const AudioError &e) {
                std::fprintf(stderr, "microphone unavailable: %s\n", e.what());
            }
        }
        gtk_widget_add_css_class((tune ? k_tune : k_tx).button, "on");
    } else {
        mic->stop();
        if (r) {
            r->mox = false;
            r->tx_carrier = false;
            r->tx_clear();
        }
        muted = false;
        gtk_widget_remove_css_class(k_tx.button, "on");
        gtk_widget_remove_css_class(k_tune.button, "on");
    }
    set_label(b_trx, on ? "TX" : "RX");
    set_class(b_trx, "rx", !on);
    set_class(b_trx, "tx", on);
    freq->tx = meter->tx = on;
    freq->queue_draw();
    meter->queue_draw();
}

// ---- keyboard --------------------------------------------------------------------------

bool Panel::on_key(guint keyval, GdkModifierType state)
{
    GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(win));
    if (focus && GTK_IS_EDITABLE(focus))
        return false;
    if (state & (GDK_CONTROL_MASK | GDK_ALT_MASK))
        return false;
    const char *raw = gdk_keyval_name(keyval);
    const std::string name = raw ? raw : "";
    std::string ch = name.rfind("KP_", 0) == 0 ? name.substr(3) : name;
    if (ch == "period" || ch == "Decimal" || ch == "comma")
        ch = ".";
    if (ch.size() == 1 && (g_ascii_isdigit(ch[0]) || ch[0] == '.')) {
        entry = entry.value_or("") + ch;
    } else if (name == "BackSpace" && entry) {
        entry->pop_back();
        if (entry->empty())
            entry.reset();
    } else if (name == "Escape") {
        if (!entry)
            set_tx(false);
        entry.reset();
    } else if ((name == "Return" || name == "KP_Enter") && entry) {
        char *end = nullptr;
        const double v = g_ascii_strtod(entry->c_str(), &end);
        if (end != entry->c_str() && *end == '\0') {
            const bool mhz = entry->find('.') != std::string::npos || v < 100;
            set_freq(v * (mhz ? 1e6 : 1e3));
        }
        entry.reset();
    } else if (name == "Right" || name == "Left") {
        tune(step() * (name == "Right" ? 1.0 : -1.0));
    } else if (name == "Up" || name == "Down") {
        tune(step() * (name == "Up" ? 10.0 : -10.0));
    } else {
        return false;
    }
    freq->entry = entry;
    freq->queue_draw();
    return true;
}

// ---- connection ------------------------------------------------------------------------

void Panel::stop_sim()
{
    if (sim > 0) {
        kill(sim, SIGTERM);
        waitpid(sim, nullptr, 0);
    }
    sim = 0;
    if (sim_fd_ >= 0)
        close(sim_fd_);
    sim_fd_ = -1;
}

void Panel::disconnect()
{
    set_tx(false);
    std::shared_ptr<Radio> old;
    {
        std::lock_guard<std::mutex> g(radio_lock_);
        old.swap(radio);
    }
    if (old)
        old->close();
    stop_sim();
    info.reset();
    status = "Not connected";
    scope->message = "NOT CONNECTED";
    apply_caps();
}

void Panel::connect(const std::string &addr, int port, const std::optional<RadioInfo> &info_,
                    bool keep_sim, const std::string &audio_in, const std::string &audio_out)
{
    if (addr.empty())
        return;
    const std::optional<RadioInfo> known = info_;      // may refer to this->info
    const pid_t keep = keep_sim ? sim : 0;
    const int keep_fd = keep_sim ? sim_fd_ : -1;
    if (keep_sim) {
        sim = 0;
        sim_fd_ = -1;
    }
    disconnect();
    sim = keep;
    sim_fd_ = keep_fd;
    if (!sim) {
        cfg["radio"] = addr;
        gtk_editable_set_text(GTK_EDITABLE(addr_entry), addr.c_str());
    }
    info = known;
    conn_t_ = monotonic();
    const RadioAddr where = parse_radio(addr);
    const std::string what = where.qmx ? "QMX" : addr;
    status = "Connecting to " + what + "…";
    scope->message = "WAITING FOR " + what;

    std::shared_ptr<Radio> r;
    Fn open;
    if (where.qmx) {
        auto q = std::make_shared<QmxRadio>();
        q->usb_tx_audio = cfg["qmx_usb_tx_audio"].boolean(true);
        q->caps.smeter_cal = cfg["qmx_smeter_cal"].num(q->caps.smeter_cal);
        std::weak_ptr<Radio> weak = q;
        q->on_panel = [this, alive = alive_, weak](std::optional<int64_t> f, std::optional<Mode> m) {
            idle(alive, [this, weak, f, m] {
                if (auto same = weak.lock())
                    on_panel(same, f, m);
            });
        };
        const std::string in = !audio_in.empty() ? audio_in : cfg["qmx_audio_in"].str();
        const std::string out = !audio_out.empty() ? audio_out : cfg["qmx_audio_out"].str();
        // plain "qmx" means the port chosen in the QMX settings (or the first one found)
        const std::string target = !where.target.empty() || sim ? where.target : cfg["qmx_port"].str();
        open = [q, target, in, out] { q->open(target, in, out); };
        r = q;
    } else {
        auto h = std::make_shared<HpsdrRadio>();
        h->rate = rate();
        open = [h, addr, port] { h->open(addr, port); };
        r = h;
    }
    r->on_iq = [this](int rate_, std::vector<cfloat> &&iq) { on_iq(rate_, std::move(iq)); };
    {
        std::lock_guard<std::mutex> g(radio_lock_);
        radio = r;
    }
    apply_radio();
    try {
        open();
    } catch (const IoError &e) {
        {
            std::lock_guard<std::mutex> g(radio_lock_);
            radio.reset();
        }
        status = "Cannot reach " + what + ": " + e.what();
        scope->message = "NOT CONNECTED";
        apply_caps();
        return;
    }
    apply_caps();
    if (!known && !where.qmx) {            // ask the radio what it is, off the GTK thread
        std::thread([this, alive = alive_, addr, port, r] {
            std::vector<hpsdr::Found> found = hpsdr::discover({addr}, port, 0.5);
            if (found.empty())
                return;
            RadioInfo i{found[0].name, found[0].version, ""};
            idle(alive, [this, r, i] {
                if (radio == r)
                    info = i;
            });
        }).detach();
    }
}

void Panel::search()
{
    disconnect();
    status = "Searching for a radio…";
    scope->message = "SEARCHING";
    std::thread([this, alive = alive_] {
        std::vector<hpsdr::Found> found = hpsdr::discover();
        std::vector<qmx::Port> usb = qmx::find();
        idle(alive, [this, found, usb] {
            if (radio)
                return;
            // with both kinds present, go back to the kind used last time
            if (!usb.empty() && (found.empty() || parse_radio(cfg["radio"].str()).qmx)) {
                connect("qmx:" + usb[0].tty);
            } else if (!found.empty()) {
                connect(found[0].addr, found[0].port,
                        RadioInfo{found[0].name, found[0].version, ""});
            } else {
                status = "No radio found — use RADIO to enter an address or start the demo";
                scope->message = "NO RADIO FOUND";
            }
        });
    }).detach();
}

void Panel::start_demo(const std::string &kind)
{
    disconnect();
    const int port = free_udp_port();
    const char *failed = "Could not start the simulator (rigface-sim)";
    if (kind == "qmx") {                   // CAT on a pty, the "sound card" over UDP
        sim = spawn_sim({"--qmx", "--iq-port", std::to_string(port)}, &sim_fd_);
        const std::string tty = sim ? read_line(sim_fd_, 3.0) : "";
        if (tty.empty()) {
            stop_sim();
            status = failed;
            return;
        }
        connect("qmx:" + tty, hpsdr::PORT, std::nullopt, true,
                "udpsrc address=127.0.0.1 port=" + std::to_string(port)
                    + " caps=audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved",
                "fakesink");
        return;
    }
    sim = spawn_sim({"--host", "127.0.0.1", "--port", std::to_string(port)}, nullptr);
    if (!sim) {
        status = failed;
        return;
    }
    connect("127.0.0.1", port, RadioInfo{"Demo radio", 0, ""}, true);
}

// ---- QMX settings ----------------------------------------------------------------------

QmxSettings Panel::qmx_settings()
{
    QmxSettings s;
    s.port = cfg["qmx_port"].str();
    s.usb_tx_audio = cfg["qmx_usb_tx_audio"].boolean(true);
    s.iq_swap = cfg["qmx_iq_swap"].boolean();
    s.smeter_cal = cfg["qmx_smeter_cal"].num(qmx::caps().smeter_cal);
    s.mic_gain = cfg["mic_gain"].num(4.0);
    s.audio_in = cfg["qmx_audio_in"].str();
    s.audio_out = cfg["qmx_audio_out"].str();
    return s;
}

void Panel::apply_qmx_settings(const QmxSettings &s, bool connect_now)
{
    const QmxSettings old = qmx_settings();
    const bool rewire = s.port != old.port || s.usb_tx_audio != old.usb_tx_audio
                        || s.audio_in != old.audio_in || s.audio_out != old.audio_out;
    cfg["qmx_port"] = s.port;
    cfg["qmx_usb_tx_audio"] = s.usb_tx_audio;
    cfg["qmx_iq_swap"] = s.iq_swap;
    cfg["qmx_smeter_cal"] = s.smeter_cal;
    cfg["mic_gain"] = s.mic_gain;
    cfg["qmx_audio_in"] = s.audio_in;
    cfg["qmx_audio_out"] = s.audio_out;
    save_config();

    const bool on_qmx = radio && std::strcmp(radio->caps.kind, "qmx") == 0;
    if (connect_now || (on_qmx && rewire && !sim)) {
        connect("qmx");
    } else if (on_qmx) {                   // the rest takes effect at once
        radio->caps.smeter_cal = s.smeter_cal;
        apply_caps();
    } else {
        apply_all();                       // the microphone gain is shared with other radios
    }
}

namespace {

struct SettingsForm {
    Panel *panel;
    GtkWidget *win, *port, *usb_tx, *iq_swap, *cal, *mic, *audio_in, *audio_out;
    std::vector<std::string> ports;        // what each line of the port list stands for

    QmxSettings read() const
    {
        QmxSettings s;
        const guint at = gtk_drop_down_get_selected(GTK_DROP_DOWN(port));
        s.port = at < ports.size() ? ports[at] : "";
        s.usb_tx_audio = gtk_switch_get_active(GTK_SWITCH(usb_tx));
        s.iq_swap = gtk_switch_get_active(GTK_SWITCH(iq_swap));
        s.smeter_cal = gtk_spin_button_get_value(GTK_SPIN_BUTTON(cal));
        s.mic_gain = gtk_spin_button_get_value(GTK_SPIN_BUTTON(mic));
        s.audio_in = trim(gtk_editable_get_text(GTK_EDITABLE(audio_in)));
        s.audio_out = trim(gtk_editable_get_text(GTK_EDITABLE(audio_out)));
        return s;
    }
};

gboolean settings_key(GtkEventControllerKey *, guint keyval, guint, GdkModifierType, gpointer win)
{
    if (keyval != GDK_KEY_Escape)
        return FALSE;
    gtk_window_destroy(GTK_WINDOW(win));
    return TRUE;
}

void settings_closed(GtkWidget *, gpointer panel)
{
    static_cast<Panel *>(panel)->settings_win = nullptr;
}

}  // namespace

void Panel::show_qmx_settings()
{
    if (settings_win) {
        gtk_window_present(GTK_WINDOW(settings_win));
        return;
    }
    const QmxSettings now = qmx_settings();
    auto form = std::make_shared<SettingsForm>();
    form->panel = this;

    GtkWidget *dlg = form->win = settings_win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dlg), "QMX settings");
    gtk_window_set_transient_for(GTK_WINDOW(dlg), GTK_WINDOW(win));
    gtk_window_set_modal(GTK_WINDOW(dlg), TRUE);
    gtk_window_set_resizable(GTK_WINDOW(dlg), FALSE);
    gtk_widget_add_css_class(dlg, "rb");
    gtk_widget_add_css_class(dlg, "settings");
    g_signal_connect(dlg, "destroy", G_CALLBACK(settings_closed), this);
    GtkEventController *keys = gtk_event_controller_key_new();
    g_signal_connect(keys, "key-pressed", G_CALLBACK(settings_key), dlg);
    gtk_widget_add_controller(dlg, keys);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_widget_set_margin_top(box, 18);
    gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_margin_start(box, 20);
    gtk_widget_set_margin_end(box, 20);
    gtk_window_set_child(GTK_WINDOW(dlg), box);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 14);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_box_append(GTK_BOX(box), grid);
    int line = 0;
    auto add = [&](const char *caption, GtkWidget *control, const char *hint) {
        gtk_grid_attach(GTK_GRID(grid), new_label(caption, "setcap", 0.0f), 0, line, 1, 1);
        gtk_widget_set_halign(control, GTK_IS_SWITCH(control) ? GTK_ALIGN_START : GTK_ALIGN_FILL);
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_widget_set_hexpand(control, TRUE);
        gtk_grid_attach(GTK_GRID(grid), control, 1, line++, 1, 1);
        if (hint) {
            GtkWidget *h = new_label(hint, "hint", 0.0f);
            gtk_label_set_wrap(GTK_LABEL(h), TRUE);
            gtk_label_set_width_chars(GTK_LABEL(h), 46);      // a fixed width, so the
            gtk_label_set_max_width_chars(GTK_LABEL(h), 46);  // window's height is known
            gtk_widget_set_margin_bottom(h, 6);
            gtk_grid_attach(GTK_GRID(grid), h, 1, line++, 1, 1);
        }
    };

    // Serial port: what is plugged in now, plus the saved choice if it is not.
    std::vector<std::string> names = {"Automatic"};
    form->ports = {""};
    for (const qmx::Port &p : qmx::find()) {
        form->ports.push_back(p.tty);
        names.push_back(p.tty + "   " + p.name);
    }
    guint selected = 0;
    for (size_t i = 1; i < form->ports.size(); ++i)
        if (form->ports[i] == now.port)
            selected = guint(i);
    if (!now.port.empty() && !selected) {
        form->ports.push_back(now.port);
        names.push_back(now.port + "   (not connected)");
        selected = guint(form->ports.size() - 1);
    }
    std::vector<const char *> strings;
    for (const std::string &n : names)
        strings.push_back(n.c_str());
    strings.push_back(nullptr);
    form->port = gtk_drop_down_new_from_strings(strings.data());
    gtk_drop_down_set_selected(GTK_DROP_DOWN(form->port), selected);
    add("Serial port", form->port,
        form->ports.size() > 1 ? nullptr : "No QMX is plugged in at the moment.");

    form->usb_tx = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(form->usb_tx), now.usb_tx_audio);
    add("Transmit computer audio", form->usb_tx,
        "On: voice comes from this computer's microphone over USB. "
        "Off: the radio uses its own microphone setting.");

    form->mic = gtk_spin_button_new_with_range(0.5, 20.0, 0.5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(form->mic), now.mic_gain);
    add("Microphone gain", form->mic, nullptr);

    form->iq_swap = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(form->iq_swap), now.iq_swap);
    add("Swap I and Q", form->iq_swap, "Turn on if the scope is mirrored.");

    form->cal = gtk_spin_button_new_with_range(-80.0, 20.0, 1.0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(form->cal), now.smeter_cal);
    add("S-meter calibration", form->cal, "Signal level in dBm that reads full scale.");

    form->audio_in = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(form->audio_in), "QMX sound card");
    gtk_editable_set_text(GTK_EDITABLE(form->audio_in), now.audio_in.c_str());
    add("I/Q audio source", form->audio_in, nullptr);
    form->audio_out = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(form->audio_out), "QMX sound card");
    gtk_editable_set_text(GTK_EDITABLE(form->audio_out), now.audio_out.c_str());
    add("Transmit audio sink", form->audio_out,
        "GStreamer elements, for when the desktop sound server holds the QMX card, "
        "e.g. pulsesrc device=…  Leave empty to use the card directly.");

    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(buttons, GTK_ALIGN_END);
    gtk_box_append(GTK_BOX(box), buttons);
    auto button = [&](const char *label, bool primary, Fn fn) {
        GtkWidget *b = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(b, "key");
        if (primary)
            gtk_widget_add_css_class(b, "on");
        on_signal(b, "clicked", std::move(fn));
        gtk_box_append(GTK_BOX(buttons), b);
    };
    // Read the form before the window goes, apply after: applying may reconnect.
    auto finish = [form](bool connect_now) {
        const QmxSettings s = form->read();
        Panel *panel = form->panel;
        gtk_window_destroy(GTK_WINDOW(form->win));
        panel->apply_qmx_settings(s, connect_now);
    };
    button("Cancel", false, [form] { gtk_window_destroy(GTK_WINDOW(form->win)); });
    button("Save", false, [finish] { finish(false); });
    button("Save and connect", true, [finish] { finish(true); });
    gtk_window_present(GTK_WINDOW(dlg));
}

// ---- streaming -------------------------------------------------------------------------

void Panel::on_iq(int rate_, std::vector<cfloat> &&iq)      // radio's thread
{
    {
        std::lock_guard<std::mutex> g(q_lock_);
        if (q_.size() >= QUEUE_MAX)
            return;                        // the DSP thread is behind: drop the block
        q_.emplace_back(rate_, std::move(iq));
    }
    q_cond_.notify_one();
}

void Panel::dsp_loop()
{
    for (;;) {
        std::pair<int, std::vector<cfloat>> item;
        {
            std::unique_lock<std::mutex> g(q_lock_);
            q_cond_.wait(g, [this] { return q_stop_ || !q_.empty(); });
            if (q_stop_)
                return;
            item = std::move(q_.front());
            q_.pop_front();
        }
        const std::vector<float> out = rx.process(item.first, std::move(item.second));
        if (audio && !muted && !out.empty())
            audio->write(out.data(), out.size());
    }
}

void Panel::on_mic(const float *samples, size_t n)          // GStreamer thread
{
    std::shared_ptr<Radio> r;
    {
        std::lock_guard<std::mutex> g(radio_lock_);
        r = radio;
    }
    if (r && tx_on)
        r->send_mic(samples, n);
}

bool Panel::tick()
{
    ++ticks_;
    const std::shared_ptr<Radio> r = radio;
    const bool live = r && r->streaming;
    if (live != scope->live) {
        scope->live = live;
        scope->queue_draw();
    }
    if (r && r->if_offset != offset_)      // the radio moved its LO (QMX in CW)
        apply_all();
    if (live) {
        if (rx.rate == scope->rate)
            scope->push(rx.spectrum());
        const double gain = r->caps.has_rf_gain ? cfg["rf_gain"].num() : 0.0;
        meter->dbm = rx.level_dbfs - gain + r->caps.smeter_cal;
        if (r->caps.max_power > 0) {       // the radio measures its own output
            const double fwd = r->fwd_power, swr = r->swr;
            const double w = std::isnan(fwd) ? 0.0 : fwd;
            meter->po = w / r->caps.max_power;
            meter->po_text = fmt("%.1f W", w)
                             + (std::isnan(swr) || swr == 0 ? "" : fmt("   SWR %.2f", swr));
        } else {
            meter->po = cfg["power"].num() / 100.0;
            meter->po_text.reset();
        }
        meter->queue_draw();
    } else if (tx_on) {
        set_tx(false);
    }
    if (tx_on && monotonic() - tx_since > TX_TIMEOUT)
        set_tx(false);
    if (ticks_ % 8 == 0) {
        GDateTime *now = g_date_time_new_now_utc();
        char *text = g_date_time_format(now, "%H:%M:%S UTC");
        gtk_label_set_label(GTK_LABEL(clock), text);
        g_free(text);
        g_date_time_unref(now);
        const bool ovf = live && r->adc_overflow;
        set_class(b_ovf, "warn", ovf);
        set_class(b_ovf, "off", !ovf);
        if (live) {
            const RadioInfo i = info ? *info : r->info().value_or(RadioInfo{"Radio", 0, ""});
            const std::string fw =
                !i.fw.empty() ? i.fw : i.version ? fmt("%d.%d", i.version / 10, i.version % 10) : "";
            status = (i.name.empty() ? "Radio" : i.name) + "  ·  "
                     + (sim ? "simulated band" : r->label()) + (fw.empty() ? "" : "  ·  fw " + fw);
        } else if (r && monotonic() - conn_t_ > 2.0) {
            const std::string err = r->error();
            status = !err.empty() ? err : "No response from " + r->label() + " — retrying";
        }
        set_label(status_label, status);
    }
    return true;
}

void Panel::on_close()
{
    if (closed_)
        return;
    closed_ = true;
    if (settings_win)
        gtk_window_destroy(GTK_WINDOW(settings_win));
    disconnect();
    *alive_ = false;
    if (tick_id_)
        g_source_remove(tick_id_);
    tick_id_ = 0;
    mic->stop();
    if (audio)
        audio->close();
    save_config();
}

void Panel::save_config()
{
    const std::string path = config_path();
    char *dir = g_path_get_dirname(path.c_str());
    GError *err = nullptr;
    if (g_mkdir_with_parents(dir, 0755) != 0)
        std::fprintf(stderr, "could not save settings: %s\n", std::strerror(errno));
    else if (!g_file_set_contents(path.c_str(), cfg.dump().c_str(), -1, &err)) {
        std::fprintf(stderr, "could not save settings: %s\n", err->message);
        g_error_free(err);
    }
    g_free(dir);
}

// ---- command line ----------------------------------------------------------------------

static void usage(FILE *to)
{
    std::fputs(
        "usage: rigface [-h] [--radio ADDR] [--demo [{hpsdr,qmx}]]\n"
        "               [--rate {48000,96000,192000,384000}] [--audio-sink GST]\n"
        "\n"
        "GTK front panel for the Radioberry and other HPSDR Protocol-1 radios, and the\n"
        "QRP Labs QMX\n"
        "\n"
        "options:\n"
        "  -h, --help        show this help message and exit\n"
        "  --radio ADDR      network address of an HPSDR radio, or 'qmx' /\n"
        "                    'qmx:/dev/ttyACM0' for a QMX on USB (default: discover)\n"
        "  --demo [{hpsdr,qmx}]\n"
        "                    run against a built-in simulated band (optionally as a QMX)\n"
        "  --rate RATE       sample rate / scope span\n"
        "  --audio-sink GST  GStreamer sink, e.g. 'alsasink device=hw:1'\n",
        to);
}

int run_app(int argc, char **argv)
{
    Args args;
    auto fail = [](const std::string &why) {
        std::fprintf(stderr, "rigface: %s\n", why.c_str());
        usage(stderr);
        return 2;
    };
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        std::optional<std::string> value;
        if (a.rfind("--", 0) == 0 && a.find('=') != std::string::npos) {
            value = a.substr(a.find('=') + 1);
            a.erase(a.find('='));
        }
        auto need = [&]() -> bool {
            if (!value && i + 1 < argc)
                value = argv[++i];
            return value.has_value();
        };
        if (a == "-h" || a == "--help") {
            usage(stdout);
            return 0;
        } else if (a == "--demo") {
            if (!value && i + 1 < argc && argv[i + 1][0] != '-')
                value = argv[++i];
            args.demo = value.value_or("hpsdr");
            if (args.demo != "hpsdr" && args.demo != "qmx")
                return fail("--demo takes 'hpsdr' or 'qmx'");
        } else if (a == "--radio") {
            if (!need())
                return fail("--radio needs an address");
            args.radio = *value;
        } else if (a == "--rate") {
            if (!need())
                return fail("--rate needs a value");
            args.rate = std::atoi(value->c_str());
            if (std::find(std::begin(hpsdr::RATES), std::end(hpsdr::RATES), args.rate)
                == std::end(hpsdr::RATES))
                return fail("--rate must be 48000, 96000, 192000 or 384000");
        } else if (a == "--audio-sink") {
            if (!need())
                return fail("--audio-sink needs a GStreamer element");
            args.audio_sink = *value;
        } else {
            return fail("unknown argument " + a);
        }
    }

    struct Session {
        Args args;
        std::unique_ptr<Panel> panel;
    } session{args, nullptr};
    GtkApplication *app = gtk_application_new("io.github.rigface", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(+[](GtkApplication *a, gpointer data) {
                         auto *s = static_cast<Session *>(data);
                         if (!s->panel)
                             s->panel = std::make_unique<Panel>(a, s->args);
                         gtk_window_present(GTK_WINDOW(s->panel->win));
                     }),
                     &session);
    // Ctrl-C and `kill` close the window properly: transmit off, settings saved.
    for (int sig : {SIGINT, SIGTERM})
        g_unix_signal_add(sig, +[](gpointer data) -> gboolean {
            auto *s = static_cast<Session *>(data);
            if (s->panel)
                gtk_window_close(GTK_WINDOW(s->panel->win));
            return G_SOURCE_CONTINUE;
        }, &session);
    const int status = g_application_run(G_APPLICATION(app), 0, nullptr);
    session.panel.reset();
    g_object_unref(app);
    return status;
}

}  // namespace rigface
