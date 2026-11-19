#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rigface::dsp {

namespace {

constexpr int CH_TAPS = 1025;
constexpr size_t FIR_FFT = 4096;            // block length of the FFT convolution
constexpr double AGC_TARGET = 0.25;
constexpr double AGC_MAXGAIN = 1e5;
constexpr double TWO_PI = 2.0 * M_PI;

double agc_tau(Agc a)
{
    switch (a) {
    case Agc::FAST: return 0.15;
    case Agc::SLOW: return 2.5;
    default: return 0.6;
    }
}

/// Modified Bessel function of the first kind, order zero.
double bessel_i0(double x)
{
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 200; ++k) {
        term *= (x / 2.0) / k;
        sum += term * term;
        if (term * term < sum * 1e-17)
            break;
    }
    return sum;
}

/// Windowed-sinc low-pass with unity gain at DC. kaiser_beta < 0: Hamming window.
std::vector<double> firwin(int ntaps, double cutoff, double fs, double kaiser_beta = -1.0)
{
    std::vector<double> h(ntaps);
    const double c = cutoff / (fs / 2.0);
    const double alpha = (ntaps - 1) / 2.0;
    const double i0b = kaiser_beta >= 0 ? bessel_i0(kaiser_beta) : 1.0;
    double sum = 0.0;
    for (int n = 0; n < ntaps; ++n) {
        double m = n - alpha;
        double x = M_PI * c * m;
        double v = c * (x == 0.0 ? 1.0 : std::sin(x) / x);
        double w;
        if (kaiser_beta >= 0) {
            double r = alpha > 0 ? m / alpha : 0.0;
            w = bessel_i0(kaiser_beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0b;
        } else {
            w = 0.54 - 0.46 * std::cos(TWO_PI * n / (ntaps - 1));
        }
        h[n] = v * w;
        sum += h[n];
    }
    for (double &v : h)
        v /= sum;
    return h;
}

const Fft &fir_plan()
{
    static const Fft plan(FIR_FFT);
    return plan;
}

}  // namespace

struct Taps {
    std::vector<cfloat> H;                  // spectrum of the taps, FIR_FFT long
};

namespace {

/// "valid" convolution of x (CH_TAPS - 1 + n samples) with the taps: n samples.
std::vector<cfloat> fir_valid(const Taps &taps, const std::vector<cfloat> &x)
{
    const size_t overlap = CH_TAPS - 1;
    const size_t hop = FIR_FFT - overlap;
    const size_t n = x.size() > overlap ? x.size() - overlap : 0;
    std::vector<cfloat> y(n);
    std::vector<cfloat> buf(FIR_FFT);
    const Fft &plan = fir_plan();
    for (size_t start = 0; start < n; start += hop) {
        size_t have = std::min(FIR_FFT, x.size() - start);
        std::copy(x.begin() + start, x.begin() + start + have, buf.begin());
        std::fill(buf.begin() + have, buf.end(), cfloat());
        plan.forward(buf.data());
        for (size_t i = 0; i < FIR_FFT; ++i) {
            float ar = buf[i].real(), ai = buf[i].imag();
            float br = taps.H[i].real(), bi = taps.H[i].imag();
            buf[i] = cfloat(ar * br - ai * bi, ar * bi + ai * br);
        }
        plan.inverse(buf.data());
        size_t count = std::min(hop, n - start);
        std::copy(buf.begin() + overlap, buf.begin() + overlap + count, y.begin() + start);
    }
    return y;
}

inline float clip1(double v)
{
    return static_cast<float>(std::max(-1.0, std::min(1.0, v)));
}

}  // namespace

const char *agc_name(Agc a)
{
    switch (a) {
    case Agc::FAST: return "FAST";
    case Agc::MID: return "MID";
    case Agc::SLOW: return "SLOW";
    case Agc::OFF: return "OFF";
    }
    return "MID";
}

std::optional<Agc> agc_from(const std::string &name)
{
    for (Agc a : AGCS)
        if (name == agc_name(a))
            return a;
    return std::nullopt;
}

// ---- FFT -----------------------------------------------------------------

Fft::Fft(size_t n) : n_(n), rev_(n), tw_(n / 2)
{
    int bits = 0;
    while ((size_t(1) << bits) < n)
        ++bits;
    for (size_t i = 0; i < n; ++i) {
        uint32_t r = 0;
        for (int b = 0; b < bits; ++b)
            if (i & (size_t(1) << b))
                r |= 1u << (bits - 1 - b);
        rev_[i] = r;
    }
    for (size_t k = 0; k < n / 2; ++k) {
        double a = -TWO_PI * k / n;
        tw_[k] = cfloat(static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a)));
    }
}

void Fft::run(cfloat *x, bool inverse) const
{
    for (size_t i = 0; i < n_; ++i)
        if (i < rev_[i])
            std::swap(x[i], x[rev_[i]]);
    for (size_t len = 2; len <= n_; len <<= 1) {
        const size_t half = len / 2, step = n_ / len;
        for (size_t i = 0; i < n_; i += len) {
            for (size_t k = 0; k < half; ++k) {
                const float wr = tw_[k * step].real();
                const float wi = inverse ? -tw_[k * step].imag() : tw_[k * step].imag();
                cfloat &a = x[i + k], &b = x[i + k + half];
                const float br = b.real() * wr - b.imag() * wi;
                const float bi = b.real() * wi + b.imag() * wr;
                b = cfloat(a.real() - br, a.imag() - bi);
                a = cfloat(a.real() + br, a.imag() + bi);
            }
        }
    }
    if (inverse) {
        const float s = 1.0f / n_;
        for (size_t i = 0; i < n_; ++i)
            x[i] *= s;
    }
}

// ---- filters and packing ---------------------------------------------------

std::shared_ptr<const Taps> bandpass(double lo, double hi)
{
    std::vector<double> h = firwin(CH_TAPS, (hi - lo) / 2.0, FS, 9.0);
    auto taps = std::make_shared<Taps>();
    taps->H.assign(FIR_FFT, cfloat());
    const double fc = (hi + lo) / 2.0;
    for (int n = 0; n < CH_TAPS; ++n) {
        double ph = TWO_PI * fc * (n - (CH_TAPS - 1) / 2.0) / FS;
        taps->H[n] = cfloat(static_cast<float>(h[n] * std::cos(ph)),
                            static_cast<float>(h[n] * std::sin(ph)));
    }
    fir_plan().forward(taps->H.data());
    return taps;
}

std::vector<cfloat> unpack_iq(const uint8_t *raw, size_t nbytes)
{
    const size_t n = nbytes / 8;
    std::vector<cfloat> iq(n);
    const float scale = 1.0f / 8388608.0f;
    for (size_t k = 0; k < n; ++k) {
        const uint8_t *a = raw + 8 * k;
        int32_t i = (int32_t(int8_t(a[0])) << 16) | (int32_t(a[1]) << 8) | a[2];
        int32_t q = (int32_t(int8_t(a[3])) << 16) | (int32_t(a[4]) << 8) | a[5];
        iq[k] = cfloat(i * scale, q * scale);
    }
    return iq;
}

// ---- receiver --------------------------------------------------------------

Receiver::Receiver()
    : ring_(NFFT), win_(NFFT), taps_(bandpass(100, 3000)), ch_tail_(CH_TAPS - 1)
{
    double sum = 0.0;
    for (int n = 0; n < NFFT; ++n) {
        win_[n] = static_cast<float>(0.5 - 0.5 * std::cos(TWO_PI * n / (NFFT - 1)));
        sum += win_[n];
    }
    wsum2_ = sum * sum;
}

void Receiver::set_mode(Mode mode, double lo, double hi)
{
    auto taps = bandpass(lo, hi);
    std::lock_guard<std::mutex> g(cfg_lock_);
    taps_ = std::move(taps);
    mode_ = mode;
}

Mode Receiver::mode() const
{
    std::lock_guard<std::mutex> g(cfg_lock_);
    return mode_;
}

void Receiver::set_rate(int new_rate)
{
    rate = new_rate;
    const int m = new_rate / FS;
    if (m > 1) {
        std::vector<double> h = firwin(16 * m + 1, 17000, new_rate);
        dec_.assign(h.begin(), h.end());
        dec_tail_.assign(16 * m, cfloat());
    } else {
        dec_.clear();
    }
    std::lock_guard<std::mutex> g(ring_lock_);
    std::fill(ring_.begin(), ring_.end(), cfloat());
}

std::vector<float> Receiver::spectrum() const
{
    static const Fft plan(NFFT);
    std::vector<cfloat> x(NFFT);
    {
        std::lock_guard<std::mutex> g(ring_lock_);
        for (int i = 0; i < NFFT; ++i)
            x[i] = ring_[i] * win_[i];
    }
    plan.forward(x.data());
    std::vector<float> p(NFFT);
    for (int i = 0; i < NFFT; ++i) {
        const cfloat &v = x[(i + NFFT / 2) % NFFT];          // DC to the centre
        p[i] = static_cast<float>((double(v.real()) * v.real() + double(v.imag()) * v.imag())
                                  / wsum2_);
    }
    return p;
}

std::vector<float> Receiver::process(int in_rate, std::vector<cfloat> iq)
{
    if (in_rate != rate)
        set_rate(in_rate);
    if (iq_swap)
        for (cfloat &v : iq)
            v = std::conj(v);
    const size_t n = iq.size();
    {
        std::lock_guard<std::mutex> g(ring_lock_);
        if (n >= size_t(NFFT)) {
            std::copy(iq.end() - NFFT, iq.end(), ring_.begin());
        } else {
            std::copy(ring_.begin() + n, ring_.end(), ring_.begin());
            std::copy(iq.begin(), iq.end(), ring_.end() - n);
        }
    }

    const double off = offset;
    if (off != 0.0 && n) {                  // radio with an IF: shift the signal to 0 Hz
        const double w = TWO_PI * off / in_rate;
        double ph = mix_;
        for (size_t i = 0; i < n; ++i) {
            ph = mix_ - w * double(i + 1);
            const float c = static_cast<float>(std::cos(ph)), s = static_cast<float>(std::sin(ph));
            const float re = iq[i].real(), im = iq[i].imag();
            iq[i] = cfloat(re * c - im * s, re * s + im * c);
        }
        mix_ = pymod(ph, TWO_PI);
    }

    std::vector<cfloat> x;                  // channel filter input: its tail, then baseband
    x.reserve(ch_tail_.size() + n);
    x = ch_tail_;
    if (!dec_.empty()) {
        const size_t m = in_rate / FS;
        std::vector<cfloat> d;
        d.reserve(dec_tail_.size() + n);
        d = dec_tail_;
        d.insert(d.end(), iq.begin(), iq.end());
        dec_tail_.assign(d.end() - 16 * m, d.end());
        const size_t count = n / m, last = 16 * m;
        for (size_t j = 0; j < count; ++j) {
            const cfloat *p = d.data() + j * m;
            float re = 0.0f, im = 0.0f;
            for (size_t k = 0; k <= last; ++k) {
                re += dec_[k] * p[last - k].real();
                im += dec_[k] * p[last - k].imag();
            }
            x.emplace_back(re, im);
        }
    } else {
        x.insert(x.end(), iq.begin(), iq.end());
    }

    Mode mode;
    std::shared_ptr<const Taps> taps;
    {
        std::lock_guard<std::mutex> g(cfg_lock_);
        mode = mode_;
        taps = taps_;
    }
    ch_tail_.assign(x.end() - (CH_TAPS - 1), x.end());
    std::vector<cfloat> y = fir_valid(*taps, x);
    const size_t k = y.size();
    if (!k)
        return {};

    double p = 0.0;
    for (const cfloat &v : y)
        p += double(v.real()) * v.real() + double(v.imag()) * v.imag();
    const double db = 10.0 * std::log10(p / k + 1e-20);
    const double level = level_dbfs;
    level_dbfs = db > level ? db : level + 0.15 * (db - level);

    const double vol = volume;
    std::vector<float> a(k);
    if (mode == Mode::FM) {
        const double scale = FS / (TWO_PI * 5000.0);
        cfloat last = fm_last_;
        for (size_t i = 0; i < k; ++i) {
            // angle(y[i] * conj(y[i-1]))
            const double re = double(y[i].real()) * last.real() + double(y[i].imag()) * last.imag();
            const double im = double(y[i].imag()) * last.real() - double(y[i].real()) * last.imag();
            last = y[i];
            const double out = 0.25 * std::atan2(im, re) * scale + fm_lp_;   // one-pole de-emphasis
            fm_lp_ = 0.75 * out;
            a[i] = clip1(out * 0.5 * vol);
        }
        fm_last_ = last;
        return a;
    }
    if (mode == Mode::AM) {
        double mean = 0.0;
        for (size_t i = 0; i < k; ++i) {
            a[i] = std::hypot(y[i].real(), y[i].imag());
            mean += a[i];
        }
        am_dc_ += 0.05 * (mean / k - am_dc_);
        for (float &v : a)
            v = static_cast<float>(v - am_dc_);
    } else if (mode == Mode::CW) {
        const double w = TWO_PI * CW_PITCH / FS;
        for (size_t i = 0; i < k; ++i) {
            const double ph = w * double(n_ + long(i));
            a[i] = static_cast<float>(y[i].real() * std::cos(ph) - y[i].imag() * std::sin(ph));
        }
        n_ = (n_ + long(k)) % (FS * 10);
    } else {
        for (size_t i = 0; i < k; ++i)
            a[i] = y[i].real();
    }
    return run_agc(std::move(a));
}

/// Peak AGC with one block (21 ms) of look-ahead.
std::vector<float> Receiver::run_agc(std::vector<float> a)
{
    double peak = 0.0;
    for (float v : a)
        peak = std::max(peak, double(std::fabs(v)));
    double g;
    const Agc mode = agc;
    if (mode == Agc::OFF) {
        g = 300.0;
    } else {
        const double decay = std::exp(-(double(a.size()) / FS) / agc_tau(mode));
        env_ = std::max({peak, env_ * decay, 1e-9});
        g = std::min(AGC_TARGET / env_, AGC_MAXGAIN);
    }
    std::optional<std::vector<float>> prev = std::move(prev_);
    const size_t len = a.size();
    prev_ = std::move(a);
    if (!prev || prev->size() != len) {
        gain_ = g;
        return std::vector<float>(len, 0.0f);
    }
    const double vol = volume;
    std::vector<float> out(len);
    for (size_t i = 0; i < len; ++i) {
        const double ramp = gain_ + (g - gain_) * double(i) / double(len);
        out[i] = clip1((*prev)[i] * ramp * vol);
    }
    gain_ = g;
    return out;
}

// ---- transmitter -----------------------------------------------------------

Transmitter::Transmitter() : taps_(bandpass(300, 2700)), tail_(CH_TAPS - 1) {}

void Transmitter::set_mode(Mode mode)
{
    std::shared_ptr<const Taps> taps;
    if (mode == Mode::LSB)
        taps = bandpass(-2700, -300);
    else if (mode == Mode::USB)
        taps = bandpass(300, 2700);
    else
        taps = bandpass(-3000, 3000);
    std::lock_guard<std::mutex> g(lock_);
    taps_ = std::move(taps);
    mode_ = mode;
}

Mode Transmitter::mode() const
{
    std::lock_guard<std::mutex> g(lock_);
    return mode_;
}

std::vector<uint8_t> Transmitter::process(const float *mic, size_t n)
{
    std::lock_guard<std::mutex> g(lock_);
    const Mode mode = mode_;
    std::vector<cfloat> x;
    x.reserve(tail_.size() + n);
    x = tail_;
    for (size_t i = 0; i < n; ++i)
        x.emplace_back(mic[i], 0.0f);
    tail_.assign(x.end() - (CH_TAPS - 1), x.end());
    std::vector<cfloat> y = fir_valid(*taps_, x);
    const bool ssb = mode == Mode::LSB || mode == Mode::USB;

    // ALC: never exceed 0.9 of full scale
    double peak = 0.0;
    for (cfloat &v : y) {
        if (ssb)
            v *= 2.0f;
        peak = std::max(peak, double(std::hypot(v.real(), v.imag())));
    }
    env_ = std::max({peak, env_ * 0.97, 1e-6});
    const double gain = std::min(double(mic_gain), 0.9 / env_);

    std::vector<uint8_t> out(8 * y.size(), 0);
    auto put = [&out](size_t i, int col, double v) {
        const int16_t s = static_cast<int16_t>(
            std::max(-32767.0, std::min(32767.0, std::nearbyint(v * 32767.0))));
        out[8 * i + 2 * col] = static_cast<uint8_t>(uint16_t(s) >> 8);
        out[8 * i + 2 * col + 1] = static_cast<uint8_t>(uint16_t(s) & 0xFF);
    };
    for (size_t i = 0; i < y.size(); ++i) {
        const double re = y[i].real() * gain, im = y[i].imag() * gain;
        if (mode == Mode::AM) {
            put(i, 2, 0.45 * (1.0 + std::max(-1.0, std::min(1.0, re / 0.9))));
            put(i, 3, 0.0);
        } else if (mode == Mode::FM) {
            phase_ += TWO_PI * 2500.0 / FS * re / 0.9;
            put(i, 2, 0.9 * std::cos(phase_));
            put(i, 3, 0.9 * std::sin(phase_));
        } else {
            put(i, 2, re);
            put(i, 3, im);
        }
    }
    if (mode == Mode::FM && !y.empty())
        phase_ = pymod(phase_, TWO_PI);
    return out;
}

}  // namespace rigface::dsp
