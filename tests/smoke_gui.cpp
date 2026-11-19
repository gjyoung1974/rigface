// Drive the real C++ window against the simulator and check it responds.
//
//   make check        (needs a display; plays no sound)
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "app.h"

using namespace rigface;

namespace {

int failures = 0;

void check(const std::string &name, bool ok)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", name.c_str());
    std::fflush(stdout);
    failures += !ok;
}

void key(Panel &p, const char *name)
{
    p.on_key(gdk_keyval_from_name(name), GdkModifierType(0));
}

int64_t vfo_freq(Panel &p) { return p.vfo()["freq"].integer(); }

struct Step {
    int wait_ms;                           // before running this step
    std::function<void(Panel &)> run;
};

const std::vector<Step> STEPS = {
    {2500, [](Panel &p) {
         check("streaming from simulator", p.radio && p.radio->streaming && p.scope->live);
         check("spectrum arriving", p.scope->db.has_value());
         check("audio path running", p.rx.rate == 192000 && p.rx.level_dbfs > -130);

         p.select_band("20");
         check("band 20 m", vfo_freq(p) == 14'200'000 && p.vfo_mode() == Mode::USB);
         p.set_mode(Mode::CW);
         p.cycle_filter();
         p.cycle_agc();
         p.cycle_step();
         check("mode/filter/agc/step",
               p.rx.mode() == Mode::CW && p.cfg["agc"].str() == "SLOW" && p.step() == 1000
                   && p.scope->pass_lo == -125 && p.scope->pass_hi == 125);
         for (const char *k : {"1", "4", "period", "0", "7", "4"})
             key(p, k);
         check("entry shown", p.freq->entry == std::optional<std::string>("14.074"));
         key(p, "Return");
         check("direct entry", vfo_freq(p) == 14'074'000 && !p.freq->entry);
         key(p, "Right");
         key(p, "Up");
         check("arrow tuning", vfo_freq(p) == 14'085'000);
         p.on_digit_tune(-10'000, 0);
         p.on_scope_tune(14'075'000 + 2'340, true);
         check("digit and click tuning", vfo_freq(p) == 14'077'000);
     }},
    {300, [](Panel &p) {
         check("radio follows VFO", p.radio->rx_freq == 14'077'000 && p.radio->tx_freq == 14'077'000);
         p.select_band("40");
         p.select_band("20");
         check("band stack recall", vfo_freq(p) == 14'077'000 && p.vfo_mode() == Mode::CW);
         p.copy_vfo();
         p.swap_vfo();
         p.set_mode(Mode::USB);
         p.set_freq(14'200'000);
         check("VFO B", p.cfg["cur"].integer() == 1 && p.cfg["vfo"][size_t(0)]["freq"].integer() == 14'077'000);
         p.select_band("6");
         check("band outside the radio's range is clamped", vfo_freq(p) == 38'400'000);
         p.set_freq(14'200'000);
         p.cycle_span();
     }},
    {1500, [](Panel &p) {
         check("span change", p.radio->rate == 384000 && p.rx.rate == 384000 && p.scope->live);
         p.set_tx(true, true);
     }},
    {800, [](Panel &p) {
         const std::vector<double> &db = *p.scope->db;
         size_t at = 0;
         for (size_t i = 0; i < db.size(); ++i)
             if (db[i] > db[at])
                 at = i;
         std::printf("     carrier %.1f dBFS at bin %zu\n", db[at], at);
         check("TUNE carrier on scope", p.radio->mox && at >= 511 && at <= 513 && db[at] > -26 && db[at] < -17);
         check("TX indication",
               std::string(gtk_label_get_label(GTK_LABEL(p.b_trx))) == "TX" && p.muted);
         key(p, "Escape");
     }},
    {500, [](Panel &p) {
         check("back to receive", !p.radio->mox && !p.tx_on && !p.muted);
         p.start_demo("qmx");
     }},
    {4000, [](Panel &p) {
         check("simulated QMX streaming", p.radio && p.radio->streaming
                                              && std::string(p.radio->caps.kind) == "qmx");
         check("QMX scope is 48 kHz with the IF offset", p.scope->rate == 48000 && p.scope->mark != 0);
         p.set_freq(7'030'000);
     }},
    {700, [](Panel &p) {
         check("QMX follows VFO", p.radio->rx_freq == 7'030'000);
         const Radio *before = p.radio.get();
         QmxSettings s = p.qmx_settings();
         check("QMX settings defaults", s.port.empty() && s.usb_tx_audio && !s.iq_swap);
         s.iq_swap = true;
         s.smeter_cal = -30;
         s.mic_gain = 6.5;
         p.apply_qmx_settings(s);
         check("QMX settings applied live",
               p.radio.get() == before && p.rx.iq_swap && p.radio->caps.smeter_cal == -30
                   && p.radio->mic_gain() == 6.5);
         check("QMX settings stored", p.cfg["qmx_iq_swap"].boolean() && p.qmx_settings().mic_gain == 6.5);
         s.iq_swap = false;
         p.apply_qmx_settings(s);
         p.show_qmx_settings();
         check("QMX settings window opens", p.settings_win != nullptr);
     }},
    {2500, [](Panel &p) {
         check("still streaming with the window open", p.radio && p.radio->streaming);
         gtk_window_destroy(GTK_WINDOW(p.settings_win));
         check("QMX settings window closes", p.settings_win == nullptr);
         gtk_window_close(GTK_WINDOW(p.win));
     }},
};

struct Run {
    std::unique_ptr<Panel> panel;
    size_t next = 0;
};

gboolean advance(gpointer data)
{
    auto *r = static_cast<Run *>(data);
    STEPS[r->next].run(*r->panel);
    if (++r->next < STEPS.size())
        g_timeout_add(guint(STEPS[r->next].wait_ms), advance, r);
    return G_SOURCE_REMOVE;
}

void activate(GtkApplication *app, gpointer data)
{
    auto *r = static_cast<Run *>(data);
    Args args;
    args.demo = "hpsdr";
    args.rate = 192000;
    args.audio_sink = "fakesink";
    r->panel = std::make_unique<Panel>(app, args);
    gtk_window_present(GTK_WINDOW(r->panel->win));
    g_timeout_add(guint(STEPS[0].wait_ms), advance, r);
}

}  // namespace

int main()
{
    char dir[] = "/tmp/rigface-smoke-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    g_setenv("XDG_CONFIG_HOME", dir, TRUE);          // never touch the real settings

    Run run;
    GtkApplication *app = gtk_application_new("io.github.rigface.smoke", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), &run);
    g_application_run(G_APPLICATION(app), 0, nullptr);
    const bool finished = run.next == STEPS.size();
    char *saved = g_build_filename(dir, "rigface", "config.json", nullptr);
    check("settings saved on close", g_file_test(saved, G_FILE_TEST_EXISTS));
    g_free(saved);
    run.panel.reset();
    g_object_unref(app);
    if (!finished)
        check("ran to the end", false);
    std::printf(failures ? "FAILED: %d check(s)\n" : "gui smoke test passed\n", failures);
    return failures ? 1 : 0;
}
