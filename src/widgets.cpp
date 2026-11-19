#include "widgets.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace rigface {

namespace {

const char FONT[] = "DejaVu Sans";

void font(cairo_t *cr, double size, bool bold = true)
{
    cairo_select_font_face(cr, FONT, CAIRO_FONT_SLANT_NORMAL,
                           bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, size);
}

double advance(cairo_t *cr, const std::string &s)
{
    cairo_text_extents_t ext;
    cairo_text_extents(cr, s.c_str(), &ext);
    return ext.x_advance;
}

/// Draw s with its baseline at y; align 0 = left, 0.5 = centre, 1 = right.
void text(cairo_t *cr, const std::string &s, double x, double y, double align = 0.0)
{
    if (align != 0.0)
        x -= advance(cr, s) * align;
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s.c_str());
}

std::string fmt(const char *format, ...) G_GNUC_PRINTF(1, 2);
std::string fmt(const char *format, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(buf, sizeof buf, format, ap);
    va_end(ap);
    return buf;
}

/// Call `(self->*method)(cr, w, h)` to draw a GtkDrawingArea.
template <typename T>
void set_draw(GtkWidget *area, T *self, void (T::*method)(cairo_t *, int, int))
{
    struct Bound {
        T *self;
        void (T::*method)(cairo_t *, int, int);
    };
    gtk_drawing_area_set_draw_func(
        GTK_DRAWING_AREA(area),
        +[](GtkDrawingArea *, cairo_t *cr, int w, int h, gpointer data) {
            auto *b = static_cast<Bound *>(data);
            (b->self->*b->method)(cr, w, h);
        },
        new Bound{self, method}, +[](gpointer data) { delete static_cast<Bound *>(data); });
}

std::vector<uint32_t> palette()
{
    struct Stop {
        double pos;
        double rgb[3];
    };
    const Stop stops[] = {{0.00, {0, 0, 0}}, {0.18, {0, 10, 70}}, {0.40, {10, 70, 200}},
                          {0.58, {40, 190, 230}}, {0.74, {245, 235, 70}}, {0.90, {245, 60, 30}},
                          {1.00, {255, 255, 255}}};
    const int nstops = sizeof stops / sizeof stops[0];
    std::vector<uint32_t> lut(256);
    for (int i = 0; i < 256; ++i) {
        const double x = i / 255.0;
        int k = 1;
        while (k < nstops - 1 && x > stops[k].pos)
            ++k;
        const double f = (x - stops[k - 1].pos) / (stops[k].pos - stops[k - 1].pos);
        uint32_t c[3];
        for (int ch = 0; ch < 3; ++ch)
            c[ch] = uint32_t(stops[k - 1].rgb[ch] + (stops[k].rgb[ch] - stops[k - 1].rgb[ch]) * f);
        lut[i] = (c[0] << 16) | (c[1] << 8) | c[2];
    }
    return lut;
}

/// The q-th percentile with linear interpolation between neighbours.
double percentile(std::vector<double> v, double q)
{
    std::sort(v.begin(), v.end());
    const double pos = q / 100.0 * double(v.size() - 1);
    const size_t i = size_t(pos);
    if (i + 1 >= v.size())
        return v.back();
    return v[i] + (v[i + 1] - v[i]) * (pos - double(i));
}

}  // namespace

// ---- Pointer -------------------------------------------------------------------

void Pointer::attach(GtkWidget *widget, std::function<void(int)> on_scroll)
{
    widget_ = widget;
    on_scroll_ = std::move(on_scroll);
    GtkEventController *m = gtk_event_controller_motion_new();
    g_signal_connect(m, "motion",
                     G_CALLBACK(+[](GtkEventControllerMotion *, double x, double y, gpointer d) {
                         auto *self = static_cast<Pointer *>(d);
                         self->px = x;
                         self->py = y;
                         gtk_widget_queue_draw(self->widget_);
                     }),
                     this);
    g_signal_connect(m, "leave", G_CALLBACK(+[](GtkEventControllerMotion *, gpointer d) {
                         auto *self = static_cast<Pointer *>(d);
                         self->px.reset();
                         self->py.reset();
                         gtk_widget_queue_draw(self->widget_);
                     }),
                     this);
    gtk_widget_add_controller(widget, m);
    GtkEventController *s = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(
        s, "scroll",
        G_CALLBACK(+[](GtkEventControllerScroll *, double, double dy, gpointer d) -> gboolean {
            auto *self = static_cast<Pointer *>(d);
            self->acc_ -= dy;
            const int n = int(self->acc_);
            if (n) {
                self->acc_ -= n;
                self->on_scroll_(n);
            }
            return TRUE;
        }),
        this);
    gtk_widget_add_controller(widget, s);
}

// ---- FreqDisplay ---------------------------------------------------------------

FreqDisplay::FreqDisplay(OnTune on_tune) : on_tune_(std::move(on_tune))
{
    widget = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widget), 420);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widget), 72);
    gtk_widget_set_hexpand(widget, TRUE);
    set_draw(widget, this, &FreqDisplay::draw);
    pointer_.attach(widget, [this](int n) {
        const int64_t place = place_at(pointer_.px);
        on_tune_(place ? std::optional<int64_t>(n * place) : std::nullopt, n);
    });
    GtkGesture *g = gtk_gesture_click_new();
    g_signal_connect(g, "pressed",
                     G_CALLBACK(+[](GtkGestureClick *, int, double x, double y, gpointer d) {
                         auto *self = static_cast<FreqDisplay *>(d);
                         const int64_t place = self->place_at(x);
                         if (place)
                             self->on_tune_(y < gtk_widget_get_height(self->widget) / 2.0 ? place
                                                                                          : -place,
                                            0);
                     }),
                     this);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(g));
}

int64_t FreqDisplay::place_at(const std::optional<double> &x) const
{
    if (x)
        for (const Cell &c : cells_)
            if (c.x0 <= *x && *x < c.x1)
                return c.place;
    return 0;
}

void FreqDisplay::draw(cairo_t *cr, int w, int h)
{
    const double size = std::min(h * 0.82, (w - 12) / 5.8);
    const double cw = 0.64 * size, dw = 0.30 * size;
    const double base = h / 2.0 + size * 0.36;
    font(cr, size);
    if (entry) {
        cairo_set_source_rgb(cr, 1.0, 0.75, 0.1);
        text(cr, *entry + "_", w - 6, base, 1.0);
        return;
    }
    const std::string s =
        fmt("%08lld", static_cast<long long>(std::max<int64_t>(0, std::min<int64_t>(99999999, freq))));
    const int lead = int(std::min(s.find_first_not_of('0'), s.size()));
    double x = w - 6 - (8 * cw + 2 * dw);
    const int64_t hover = place_at(pointer_.px);
    cells_.clear();
    const double r = tx ? 1.0 : 0.96, g = tx ? 0.45 : 0.98, b = tx ? 0.4 : 1.0;
    int64_t place = 10'000'000;
    for (int i = 0; i < 8; ++i, place /= 10) {
        cells_.push_back({x, x + cw, place});
        if (place == hover) {
            cairo_set_source_rgba(cr, 0.25, 0.55, 1.0, 0.28);
            cairo_rectangle(cr, x + 1, base - size * 0.8, cw - 2, size * 0.92);
            cairo_fill(cr);
        }
        if (i >= lead || i == 7) {
            // the last three digits are drawn a little dimmer, as on a rig
            cairo_set_source_rgba(cr, r, g, b, i < 5 ? 1.0 : 0.72);
            const std::string ch(1, s[i]);
            text(cr, ch, x + (cw - advance(cr, ch)) / 2, base);
        }
        x += cw;
        if (i == 1 || i == 4) {
            if (i >= lead) {
                cairo_set_source_rgba(cr, r, g, b, 0.9);
                text(cr, ".", x + dw * 0.18, base);
            }
            x += dw;
        }
    }
}

// ---- Meter ---------------------------------------------------------------------

Meter::Meter()
{
    widget = gtk_drawing_area_new();
    gtk_drawing_area_set_content_width(GTK_DRAWING_AREA(widget), 330);
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widget), 72);
    set_draw(widget, this, &Meter::draw);
}

std::string Meter::s_text(double dbm)
{
    if (dbm <= -73)
        return fmt("S%d", std::max(0, std::min(9, int(std::nearbyint((dbm + 127) / 6)))));
    return fmt("S9+%d", int((dbm + 73) / 5) * 5);
}

void Meter::draw(cairo_t *cr, int w, int h)
{
    const double x0 = 26.0, x1 = w - 8.0;
    const double span = x1 - x0;
    const double bar_y = h * 0.42, bar_h = h * 0.22;
    double frac, split;
    font(cr, 10);
    if (tx) {
        frac = po;
        cairo_set_source_rgb(cr, 0.95, 0.4, 0.35);
        text(cr, "PO", 2, bar_y + bar_h - 2);
        for (int pct : {0, 25, 50, 75, 100}) {
            std::string lab;
            if (po_max > 0)
                lab = fmt("%g", po_max * pct / 100) + (pct == 100 ? "W" : "");
            else
                lab = pct == 100 ? "100%" : std::to_string(pct);
            text(cr, lab, x0 + span * pct / 100, bar_y - 6, 0.5);
        }
        split = 2.0;
    } else {
        const double d = dbm;
        frac = d <= -73 ? (d + 127) / 54 * 0.6 : 0.6 + (d + 73) / 60 * 0.4;
        cairo_set_source_rgb(cr, 0.85, 0.9, 0.96);
        text(cr, "S", 6, bar_y + bar_h - 2);
        for (int s : {1, 3, 5, 7, 9})
            text(cr, std::to_string(s), x0 + span * 0.6 * s / 9, bar_y - 6, 0.5);
        cairo_set_source_rgb(cr, 1.0, 0.35, 0.3);
        for (int over : {20, 40, 60})
            text(cr, fmt("+%d", over), x0 + span * (0.6 + 0.4 * over / 60), bar_y - 6,
                 over == 60 ? 1.0 : 0.5);
        split = 0.6;
    }
    frac = std::max(0.0, std::min(1.0, frac));
    const int nseg = 48;
    const double sw = span / nseg;
    for (int i = 0; i < nseg; ++i) {
        const double pos = (i + 0.5) / nseg;
        const bool lit = pos <= frac;
        const bool red = pos > split || tx;
        if (lit) {
            if (red)
                cairo_set_source_rgb(cr, 1.0, 0.25, 0.2);
            else
                cairo_set_source_rgb(cr, 0.45, 0.85, 1.0);
        } else {
            if (red)
                cairo_set_source_rgb(cr, 0.22, 0.07, 0.07);
            else
                cairo_set_source_rgb(cr, 0.08, 0.14, 0.2);
        }
        cairo_rectangle(cr, x0 + i * sw, bar_y, sw - 1.5, bar_h);
        cairo_fill(cr);
    }
    // scale line under the bar
    cairo_set_line_width(cr, 1.5);
    cairo_set_source_rgb(cr, 0.85, 0.9, 0.96);
    cairo_move_to(cr, x0, bar_y + bar_h + 3.5);
    cairo_line_to(cr, x0 + span * std::min(split, 1.0), bar_y + bar_h + 3.5);
    cairo_stroke(cr);
    if (split < 1.0) {
        cairo_set_source_rgb(cr, 1.0, 0.3, 0.25);
        cairo_move_to(cr, x0 + span * split, bar_y + bar_h + 3.5);
        cairo_line_to(cr, x1, bar_y + bar_h + 3.5);
        cairo_stroke(cr);
    }
    font(cr, 12);
    cairo_set_source_rgb(cr, 0.8, 0.86, 0.93);
    if (tx) {
        text(cr, po_text ? *po_text : fmt("%d %%", int(std::nearbyint(po * 100))), x1, h - 4, 1.0);
    } else {
        text(cr, s_text(dbm), x0, h - 4);
        cairo_set_source_rgb(cr, 0.5, 0.56, 0.63);
        text(cr, fmt("%d dBm", int(std::nearbyint(dbm))), x1, h - 4, 1.0);
    }
}

// ---- Scope ---------------------------------------------------------------------

Scope::Scope(OnTune on_tune, OnStep on_step)
    : on_tune_(std::move(on_tune)), lut_(palette()), wf_(size_t(WF_ROWS) * BINS, 0)
{
    widget = gtk_drawing_area_new();
    gtk_drawing_area_set_content_height(GTK_DRAWING_AREA(widget), 260);
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_vexpand(widget, TRUE);
    set_draw(widget, this, &Scope::draw);
    pointer_.attach(widget, std::move(on_step));
    GtkGesture *d = gtk_gesture_drag_new();
    g_signal_connect(d, "drag-begin",
                     G_CALLBACK(+[](GtkGestureDrag *, double x, double, gpointer data) {
                         auto *self = static_cast<Scope *>(data);
                         self->drag_ = (Drag{x, self->center, false});  // parens: commas inside a macro argument
                     }),
                     this);
    g_signal_connect(d, "drag-update",
                     G_CALLBACK(+[](GtkGestureDrag *, double dx, double, gpointer data) {
                         auto *self = static_cast<Scope *>(data);
                         if (self->drag_ && (self->drag_->moved || std::fabs(dx) > 5)) {
                             self->drag_->moved = true;
                             self->on_tune_(double(self->drag_->center) - dx * self->hz_per_px()
                                                + self->mark,
                                            false);
                         }
                     }),
                     this);
    g_signal_connect(d, "drag-end",
                     G_CALLBACK(+[](GtkGestureDrag *, double, double, gpointer data) {
                         auto *self = static_cast<Scope *>(data);
                         if (self->drag_ && !self->drag_->moved) {
                             const double w = std::max(1, gtk_widget_get_width(self->widget));
                             self->on_tune_(
                                 double(self->center) + (self->drag_->x / w - 0.5) * self->rate,
                                 true);
                         }
                         self->drag_.reset();
                     }),
                     this);
    gtk_widget_add_controller(widget, GTK_EVENT_CONTROLLER(d));
}

// ---- data

void Scope::clear()
{
    avg_.reset();
    db.reset();
    std::fill(wf_.begin(), wf_.end(), 0);
    queue_draw();
}

void Scope::push(const std::vector<float> &power)
{
    const size_t group = power.size() / BINS;
    if (!group)
        return;
    std::vector<double> p(BINS);
    for (int i = 0; i < BINS; ++i)
        p[i] = *std::max_element(power.begin() + i * group, power.begin() + (i + 1) * group);
    if (!avg_) {
        avg_ = p;
    } else {
        for (int i = 0; i < BINS; ++i)
            (*avg_)[i] += 0.45 * (p[i] - (*avg_)[i]);
    }
    std::vector<double> level(BINS);
    for (int i = 0; i < BINS; ++i)
        level[i] = 10 * std::log10((*avg_)[i] + 1e-20);
    floor += (db ? 0.05 : 0.3) * (percentile(level, 30) - floor);
    db = std::move(level);
    if (++tick_ % 2 == 0) {
        std::memmove(wf_.data() + BINS, wf_.data(), size_t(WF_ROWS - 1) * BINS * sizeof(uint32_t));
        for (int i = 0; i < BINS; ++i) {
            const double v = ((*db)[i] - (floor - 3)) * (255 / 60.0);
            wf_[i] = lut_[size_t(std::max(0.0, std::min(255.0, v)))];
        }
        queue_draw();
    }
}

void Scope::retune(int64_t new_center)
{
    frac_ += double(new_center - center) * BINS / rate;
    center = new_center;
    const long n = std::lrint(frac_);
    if (n == 0)
        return;
    frac_ -= n;
    const size_t k = size_t(std::labs(n));
    if (k >= size_t(BINS)) {
        std::fill(wf_.begin(), wf_.end(), 0);
        avg_.reset();
        return;
    }
    const size_t keep = BINS - k;
    for (int row = 0; row < WF_ROWS; ++row) {
        uint32_t *r = wf_.data() + size_t(row) * BINS;
        if (n > 0) {
            std::memmove(r, r + k, keep * sizeof(uint32_t));
            std::fill(r + keep, r + BINS, 0);
        } else {
            std::memmove(r + k, r, keep * sizeof(uint32_t));
            std::fill(r, r + k, 0);
        }
    }
    if (avg_) {
        double *a = avg_->data();
        if (n > 0)
            std::memmove(a, a + k, keep * sizeof(double));
        else
            std::memmove(a + k, a, keep * sizeof(double));
    }
}

// ---- input

double Scope::hz_per_px() const
{
    return double(rate) / std::max(1, gtk_widget_get_width(widget));
}

// ---- drawing

void Scope::draw(cairo_t *cr, int w, int h)
{
    const int scale_h = 18;
    const int spec_h = std::max(40, int((h - scale_h) * 0.42));
    const int wf_y = spec_h + scale_h;
    cairo_set_source_rgb(cr, 0.012, 0.03, 0.07);
    cairo_rectangle(cr, 0, 0, w, spec_h);
    cairo_fill(cr);

    auto fx = [&](double off) { return (off / rate + 0.5) * w; };

    // frequency grid and labels
    double step = 2e5;
    for (double s : {1e3, 2e3, 5e3, 1e4, 2e4, 5e4, 1e5, 2e5})
        if (rate / s <= std::max(4.0, w / 110.0)) {
            step = s;
            break;
        }
    const double lo = double(center) - rate / 2.0, hi = double(center) + rate / 2.0;
    cairo_set_source_rgb(cr, 0.05, 0.06, 0.08);
    cairo_rectangle(cr, 0, spec_h, w, scale_h);
    cairo_fill(cr);
    font(cr, 10, false);
    cairo_set_line_width(cr, 1);
    for (double f = std::ceil(lo / step) * step; f < hi; f += step) {
        const double x = std::nearbyint(fx(f - double(center))) + 0.5;
        cairo_set_source_rgba(cr, 0.5, 0.65, 0.9, 0.16);
        cairo_move_to(cr, x, 0);
        cairo_line_to(cr, x, spec_h);
        cairo_stroke(cr);
        if (28 < x && x < w - 28) {
            cairo_set_source_rgb(cr, 0.62, 0.69, 0.78);
            text(cr, fmt("%.3f", f / 1e6), x, spec_h + 13, 0.5);
        }
    }

    // dB grid
    const double top = floor + 62, bottom = floor - 8;
    auto fy = [&](double level) { return spec_h * (top - level) / (top - bottom); };
    for (double g = std::ceil(bottom / 10) * 10; g < top; g += 10) {
        const double y = std::nearbyint(fy(g)) + 0.5;
        cairo_set_source_rgba(cr, 0.5, 0.65, 0.9, 0.13);
        cairo_move_to(cr, 0, y);
        cairo_line_to(cr, w, y);
        cairo_stroke(cr);
        cairo_set_source_rgba(cr, 0.6, 0.68, 0.78, 0.7);
        text(cr, fmt("%d", int(g)), 3, y - 2);
    }

    // receiver passband
    const double x0 = fx(mark + pass_lo), x1 = fx(mark + pass_hi);
    cairo_set_source_rgba(cr, 0.35, 0.6, 1.0, 0.22);
    cairo_rectangle(cr, x0, 0, std::max(2.0, x1 - x0), spec_h);
    cairo_fill(cr);

    // spectrum trace
    if (live && db) {
        cairo_move_to(cr, 0, spec_h);
        for (int i = 0; i < BINS; ++i)
            cairo_line_to(cr, (i + 0.5) * (double(w) / BINS),
                          std::max(0.0, std::min(double(spec_h), fy((*db)[i]))));
        cairo_line_to(cr, w, spec_h);
        cairo_close_path(cr);
        cairo_pattern_t *grad = cairo_pattern_create_linear(0, 0, 0, spec_h);
        cairo_pattern_add_color_stop_rgba(grad, 0, 0.45, 0.75, 1.0, 0.85);
        cairo_pattern_add_color_stop_rgba(grad, 1, 0.05, 0.25, 0.7, 0.35);
        cairo_set_source(cr, grad);
        cairo_pattern_destroy(grad);
        cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, 0.85, 0.93, 1.0);
        cairo_set_line_width(cr, 1);
        cairo_stroke(cr);
    }

    // waterfall
    const int wf_h = h - wf_y;
    if (wf_h > 4) {
        cairo_save(cr);
        cairo_rectangle(cr, 0, wf_y, w, wf_h);
        cairo_clip(cr);
        cairo_translate(cr, 0, wf_y);
        cairo_scale(cr, double(w) / BINS, double(wf_h) / WF_ROWS);
        // GTK records the draw, so hand cairo its own copy of the pixels
        cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, BINS, WF_ROWS);
        if (cairo_surface_status(surf) == CAIRO_STATUS_SUCCESS) {
            cairo_surface_flush(surf);
            unsigned char *data = cairo_image_surface_get_data(surf);
            const int stride = cairo_image_surface_get_stride(surf);
            for (int row = 0; row < WF_ROWS; ++row)
                std::memcpy(data + size_t(row) * stride, wf_.data() + size_t(row) * BINS,
                            BINS * sizeof(uint32_t));
            cairo_surface_mark_dirty(surf);
            cairo_set_source_surface(cr, surf, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr);
        }
        cairo_surface_destroy(surf);
        cairo_restore(cr);
    }

    // tuned frequency marker
    const double cx = std::nearbyint(fx(mark)) + 0.5;
    cairo_set_source_rgba(cr, 1.0, 0.3, 0.25, 0.95);
    cairo_set_line_width(cr, 1);
    cairo_move_to(cr, cx, 0);
    cairo_line_to(cr, cx, spec_h);
    cairo_stroke(cr);
    cairo_set_source_rgba(cr, 1.0, 0.3, 0.25, 0.35);
    cairo_move_to(cr, cx, wf_y);
    cairo_line_to(cr, cx, h);
    cairo_stroke(cr);

    // cursor frequency readout
    if (pointer_.px && live) {
        font(cr, 11);
        const double f = double(center) + (*pointer_.px / w - 0.5) * rate;
        const std::string label = fmt("%.3f kHz", f / 1e3);
        const double tw = advance(cr, label);
        const double tx = std::min(std::max(*pointer_.px + 10, 2.0), w - tw - 8);
        cairo_set_source_rgba(cr, 0, 0, 0, 0.7);
        cairo_rectangle(cr, tx - 4, 4, tw + 8, 17);
        cairo_fill(cr);
        cairo_set_source_rgb(cr, 1.0, 0.85, 0.3);
        text(cr, label, tx, 17);
    }

    if (!live) {
        font(cr, 15);
        cairo_set_source_rgba(cr, 0.7, 0.76, 0.84, 0.8);
        text(cr, message, w / 2.0, spec_h / 2.0, 0.5);
    }
}

}  // namespace rigface
