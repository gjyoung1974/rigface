// Receive and transmit DSP, 48 kHz audio.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "radio.h"

namespace rigface::dsp {

constexpr int FS = 48000;
constexpr int NFFT = 4096;
constexpr double CW_PITCH = 600.0;

enum class Agc { FAST, MID, SLOW, OFF };
inline constexpr Agc AGCS[] = {Agc::FAST, Agc::MID, Agc::SLOW, Agc::OFF};
const char *agc_name(Agc a);
std::optional<Agc> agc_from(const std::string &name);

/// In-place complex FFT of a fixed power-of-two length.
class Fft {
public:
    explicit Fft(size_t n);
    size_t size() const { return n_; }
    void forward(cfloat *x) const { run(x, false); }
    void inverse(cfloat *x) const { run(x, true); }   // scaled by 1/n

private:
    void run(cfloat *x, bool inverse) const;
    size_t n_;
    std::vector<uint32_t> rev_;
    std::vector<cfloat> tw_;
};

/// A complex FIR passing lo..hi Hz (relative to the carrier), ready to apply.
struct Taps;
std::shared_ptr<const Taps> bandpass(double lo, double hi);

/// Protocol-1 receive payload (I24 Q24 mic16, big-endian) -> complex samples.
std::vector<cfloat> unpack_iq(const uint8_t *raw, size_t nbytes);

class Receiver {
public:
    Receiver();

    // Settings; safe to change from another thread while process() runs.
    std::atomic<Agc> agc{Agc::MID};
    std::atomic<float> volume{0.3f};
    std::atomic<bool> iq_swap{false};
    std::atomic<double> offset{0.0};        // Hz from the centre of the I/Q to the tuned frequency
    void set_mode(Mode mode, double lo, double hi);
    Mode mode() const;

    // Status.
    std::atomic<int> rate{0};
    std::atomic<double> level_dbfs{-140.0};  // passband power, for the S-meter

    /// Power spectrum, linear, NFFT bins, DC centred; 1.0 = full scale.
    /// Called from the GUI thread.
    std::vector<float> spectrum() const;

    /// One block of I/Q at `rate` -> audio at 48 kHz (DSP thread).
    std::vector<float> process(int rate, std::vector<cfloat> iq);

private:
    void set_rate(int rate);
    std::vector<float> run_agc(std::vector<float> a);

    mutable std::mutex ring_lock_;
    std::vector<cfloat> ring_;
    std::vector<float> win_;
    double wsum2_;

    mutable std::mutex cfg_lock_;
    Mode mode_ = Mode::USB;
    std::shared_ptr<const Taps> taps_;

    std::vector<cfloat> ch_tail_;
    std::vector<float> dec_;
    std::vector<cfloat> dec_tail_;
    long n_ = 0;                            // BFO sample counter
    double mix_ = 0.0;                      // phase of the offset mixer
    cfloat fm_last_{1.0f, 0.0f};
    double fm_lp_ = 0.0;
    double am_dc_ = 0.0;
    double env_ = 1e-6;
    double gain_ = 1.0;
    std::optional<std::vector<float>> prev_;
};

/// Microphone audio (48 kHz float) -> packed Protocol-1 TX samples.
class Transmitter {
public:
    Transmitter();
    void set_mode(Mode mode);
    Mode mode() const;
    std::atomic<double> mic_gain{4.0};

    /// 8 bytes per sample: L, R, I, Q as big-endian int16.
    std::vector<uint8_t> process(const float *mic, size_t n);

private:
    mutable std::mutex lock_;
    Mode mode_ = Mode::USB;
    std::shared_ptr<const Taps> taps_;
    std::vector<cfloat> tail_;
    double env_ = 0.1;
    double phase_ = 0.0;
};

}  // namespace rigface::dsp
