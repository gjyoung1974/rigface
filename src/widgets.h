// Custom-drawn front panel elements: frequency readout, meter, scope.
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtk/gtk.h>

namespace rigface {

constexpr int BINS = 1024;                 // scope resolution (columns)
constexpr int WF_ROWS = 240;               // waterfall history

/// Remembers the pointer position over a widget and accumulates smooth scrolling.
class Pointer {
public:
    void attach(GtkWidget *widget, std::function<void(int)> on_scroll);
    std::optional<double> px, py;

private:
    GtkWidget *widget_ = nullptr;
    std::function<void(int)> on_scroll_;
    double acc_ = 0.0;
};

/// Big frequency readout. Scroll or click a digit to tune by that digit.
class FreqDisplay {
public:
    /// on_tune(delta_hz, steps): delta_hz is empty to tune by `steps` tuning steps.
    using OnTune = std::function<void(std::optional<int64_t> delta, int steps)>;
    explicit FreqDisplay(OnTune on_tune);
    FreqDisplay(const FreqDisplay &) = delete;

    GtkWidget *widget;
    int64_t freq = 0;
    std::optional<std::string> entry;      // text while typing a frequency
    bool tx = false;
    void queue_draw() { gtk_widget_queue_draw(widget); }

private:
    struct Cell {
        double x0, x1;
        int64_t place;
    };
    int64_t place_at(const std::optional<double> &x) const;    // 0: not over a digit
    void draw(cairo_t *cr, int w, int h);

    OnTune on_tune_;
    Pointer pointer_;
    std::vector<Cell> cells_;
};

/// Segmented S-meter; shows power output while transmitting.
class Meter {
public:
    Meter();
    Meter(const Meter &) = delete;

    GtkWidget *widget;
    double dbm = -140.0;
    bool tx = false;
    double po = 0.0;                       // 0..1 while transmitting
    double po_max = 0.0;                   // watts at full scale, or 0 for a % scale
    std::optional<std::string> po_text;    // readout, when the radio measures its power
    void queue_draw() { gtk_widget_queue_draw(widget); }

    static std::string s_text(double dbm);

private:
    void draw(cairo_t *cr, int w, int h);
};

/// Spectrum scope with waterfall. Click to tune, drag to pan, scroll to step.
class Scope {
public:
    using OnTune = std::function<void(double frequency_hz, bool snap)>;
    using OnStep = std::function<void(int n_steps)>;
    Scope(OnTune on_tune, OnStep on_step);
    Scope(const Scope &) = delete;

    GtkWidget *widget;
    int rate = 192000;
    int64_t center = 7100000;              // frequency in the middle of the display
    int mark = 0;                          // tuned frequency, Hz from the centre
    double pass_lo = 100, pass_hi = 3000;  // receiver passband
    bool live = false;
    std::string message = "NOT CONNECTED";
    std::optional<std::vector<double>> db; // the trace, BINS values in dBFS
    double floor = -110.0;
    void queue_draw() { gtk_widget_queue_draw(widget); }

    void clear();
    /// Feed one linear power spectrum (a multiple of BINS long).
    void push(const std::vector<float> &power);
    /// Slide the history sideways so signals stay put when the VFO moves.
    void retune(int64_t new_center);

private:
    double hz_per_px() const;
    void draw(cairo_t *cr, int w, int h);

    OnTune on_tune_;
    Pointer pointer_;
    std::optional<std::vector<double>> avg_;
    double frac_ = 0.0;
    long tick_ = 0;
    std::vector<uint32_t> lut_;
    std::vector<uint32_t> wf_;             // WF_ROWS x BINS, newest row first
    struct Drag {
        double x;
        int64_t center;
        bool moved;
    };
    std::optional<Drag> drag_;
};

}  // namespace rigface
