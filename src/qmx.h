// QRP Labs QMX / QMX+ over USB.
//
// The QMX is not a network SDR: it shows up as a serial port speaking Kenwood
// style CAT plus a 48 kHz stereo sound card. In "IQ mode" (CAT `Q91;`, lasts
// until the radio is switched off) that sound card carries the raw I and Q
// channels of its ADC, which is what the receiver and scope here need. The
// radio is a 12 kHz-IF superhet, so the tuned frequency is not at the centre of
// the I/Q; the real LO frequency is read back with `PL;` and published as
// `if_offset`.
//
// Transmit is the other way round from an HPSDR radio: the QMX makes the signal
// itself. Voice is microphone audio played into its sound card with `TX;`
// asserted; a carrier is a CW key-down (`KD1;`).
//
// Command reference: "QMX CAT programming manual", firmware 1_04_003.
#pragma once

#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio.h"
#include "radio.h"

namespace rigface {

namespace qmx {

constexpr int RATE = 48000;
constexpr int BLOCK = 1024;                // samples handed to the DSP at a time (21 ms)
constexpr int IF_DEFAULT = 12000;          // used until / unless the radio answers PL;

struct Port {
    std::string tty;
    int card;                              // ALSA card number, -1 if none was found
    std::string name;
};

/// QMX radios plugged into this machine.
std::vector<Port> find();

Caps caps();

}  // namespace qmx

/// CAT and I/Q audio for one QMX.
class QmxRadio : public Radio {
public:
    enum Tx { TX_NONE, TX_KEY, TX_VOICE };

    QmxRadio();
    ~QmxRadio() override;

    /// Open the CAT port now (throws IoError); everything else is threaded.
    ///
    /// tty is found automatically when empty. audio_in / audio_out are
    /// GStreamer element descriptions replacing the QMX's own sound card.
    void open(const std::string &tty = "", const std::string &audio_in = "",
              const std::string &audio_out = "");
    void close() override;
    void set_rate(int) override {}
    void tx_clear() override {}
    std::string label() const override { return label_; }

    Mode mode() const override { return mode_; }
    void set_mode(Mode m) override { mode_ = m; }
    double mic_gain() const override { return mic_gain_; }
    void set_mic_gain(double gain) override { mic_gain_ = gain; }

    void send_mic(const float *samples, size_t n) override;

    bool usb_tx_audio = true;              // tell the radio to transmit the audio we send it

    // For the tests: what the radio's thread is doing.
    std::atomic<int64_t> sent_freq{-1};    // last frequency sent to the radio, -1 = none yet
    std::atomic<int> tx_state{TX_NONE};
    bool has_tx_audio();

private:
    void run();
    void session();
    void open_port();
    void write(const std::string &text);
    bool token(double deadline, std::string &tok);
    void set(const std::string &text);
    std::optional<std::string> query(const std::string &cmd);
    void on_audio(std::vector<cfloat> &&iq);
    std::string start_iq();
    void stop_iq();
    void begin_tx(bool carrier);
    void end_tx();
    void read_lo();
    void follow_panel(double now);
    void read_power();

    std::atomic<Mode> mode_{Mode::USB};
    std::atomic<double> mic_gain_{4.0};
    std::string label_;
    std::string audio_in_, audio_out_;

    std::atomic<bool> alive_{false};
    std::thread thread_;
    int fd_ = -1;
    std::string buf_;
    bool dirty_ = false;                   // set commands sent since the last reply we matched
    bool restore_mode_ = false;
    std::optional<Mode> sent_mode_;
    double t_set_ = 0.0;
    bool need_lo_ = true;
    bool has_pl_ = true;
    double last_ok_ = 0.0;
    std::atomic<double> last_iq_{0.0};
    std::unique_ptr<IQIn> iq_;
    std::vector<cfloat> pend_;             // I/Q not yet handed on (GStreamer thread)

    std::mutex out_lock_;                  // the two below: radio thread and microphone thread
    std::shared_ptr<AudioOut> out_;
    double env_ = 0.1;
};

}  // namespace rigface
