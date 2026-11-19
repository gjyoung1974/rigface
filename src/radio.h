// What the two radio backends (HpsdrRadio, QmxRadio) have in common.
#pragma once

#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rigface {

using cfloat = std::complex<float>;

enum class Mode { LSB, USB, CW, AM, FM };
inline constexpr Mode MODES[] = {Mode::LSB, Mode::USB, Mode::CW, Mode::AM, Mode::FM};

const char *mode_name(Mode m);
std::optional<Mode> mode_from(const std::string &name);

/// Seconds on a clock that never goes backwards.
double monotonic();

/// Python-style modulo: the result has the sign of `b`.
inline double pymod(double a, double b)
{
    double r = std::fmod(a, b);
    return (r != 0 && (r < 0) != (b < 0)) ? r + b : r;
}

/// A radio could not be opened or stopped answering; what() is for the status line.
struct IoError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct RadioInfo {
    std::string name;
    int version = 0;               // HPSDR firmware version byte, 0 if unknown
    std::string fw;                // firmware as text, when the radio says so itself
};

/// What a radio can do; the panel adapts to it.
struct Caps {
    const char *kind;
    std::vector<int> rates;
    std::vector<Mode> modes;
    int64_t freq_lo, freq_hi;
    bool has_rf_gain;              // false: not adjustable from here
    int rf_gain_lo, rf_gain_hi;
    bool has_drive;                // TX power is set from here
    double max_power;              // watts at full scale if power is measured, else 0
    double smeter_cal;             // dBm at ADC full scale with 0 dB RF gain

    bool has_mode(Mode m) const;
};

/// 'qmx', 'qmx:/dev/ttyACM0' or a serial device path -> {qmx = true, tty or ""};
/// anything else is the network address of an HPSDR radio.
struct RadioAddr {
    bool qmx;
    std::string target;
    bool operator==(const RadioAddr &o) const { return qmx == o.qmx && target == o.target; }
};
RadioAddr parse_radio(const std::string &addr);

/// One radio connection running in its own thread.
///
/// The settings (rx_freq .. tx_carrier, mode, mic gain) may be changed from
/// any thread at any time; the status fields are written by the radio's
/// thread. The callbacks must be set before the radio is opened.
class Radio {
public:
    virtual ~Radio() = default;

    Caps caps;

    // ---- settings
    std::atomic<int> rate{48000};
    std::atomic<int64_t> rx_freq{7100000};
    std::atomic<int64_t> tx_freq{7100000};
    std::atomic<int> rf_gain{20};          // dB
    std::atomic<int> drive{0};             // 0..255
    std::atomic<bool> mox{false};
    std::atomic<bool> tx_carrier{false};   // send a steady carrier instead of tx audio

    virtual Mode mode() const = 0;
    virtual void set_mode(Mode m) = 0;
    virtual double mic_gain() const = 0;
    virtual void set_mic_gain(double gain) = 0;
    virtual void set_rate(int rate) = 0;

    // ---- status
    std::atomic<bool> streaming{false};
    std::atomic<bool> adc_overflow{false};
    std::atomic<int> if_offset{0};         // Hz from the centre of the I/Q to rx_freq
    std::atomic<double> fwd_power{NAN};    // measured watts / SWR while transmitting,
    std::atomic<double> swr{NAN};          // NaN when not known

    /// Text for the status line when not streaming; empty if there is none.
    std::string error() const
    {
        std::lock_guard<std::mutex> g(text_lock_);
        return error_;
    }
    std::optional<RadioInfo> info() const
    {
        std::lock_guard<std::mutex> g(text_lock_);
        return info_;
    }
    virtual std::string label() const = 0;

    // ---- callbacks, from the radio's threads
    std::function<void(int rate, std::vector<cfloat> &&iq)> on_iq;
    /// Tuned at the radio itself: the new frequency and/or mode.
    std::function<void(std::optional<int64_t> freq, std::optional<Mode> mode)> on_panel;

    /// Microphone audio (48 kHz float) to be transmitted.
    virtual void send_mic(const float *samples, size_t n) = 0;
    virtual void tx_clear() = 0;
    virtual void close() = 0;

protected:
    void set_error(const std::string &text)
    {
        std::lock_guard<std::mutex> g(text_lock_);
        error_ = text;
    }
    void set_info(const std::optional<RadioInfo> &info)
    {
        std::lock_guard<std::mutex> g(text_lock_);
        info_ = info;
    }

private:
    mutable std::mutex text_lock_;
    std::string error_;
    std::optional<RadioInfo> info_;
};

}  // namespace rigface
