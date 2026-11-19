// HPSDR Protocol-1 (Metis / Hermes-Lite) client.
//
// The Radioberry daemon emulates a Hermes-Lite on UDP port 1024, so this is all
// that is needed to drive it over the network or on the Pi itself.
#pragma once

#include <string>
#include <thread>
#include <vector>

#include "dsp.h"
#include "radio.h"

namespace rigface {

namespace hpsdr {

constexpr int PORT = 1024;
inline constexpr int RATES[] = {48000, 96000, 192000, 384000};

struct Found {
    std::string addr;
    int port;
    std::string mac;
    int version;
    int board;
    bool busy;
    std::string name;
};

/// Radios answering a discovery request; the default targets are the
/// broadcast address and this machine.
std::vector<Found> discover(std::vector<std::string> targets = {}, int port = PORT,
                            double timeout = 0.7);

/// What an HPSDR radio can do (also the panel's limits while there is no radio).
Caps caps();

}  // namespace hpsdr

/// One-receiver Protocol-1 connection running in its own thread.
class HpsdrRadio : public Radio {
public:
    HpsdrRadio();
    ~HpsdrRadio() override;

    /// Throws IoError if the address cannot be used.
    void open(const std::string &addr, int port = hpsdr::PORT);
    void close() override;
    void set_rate(int rate) override;
    std::string label() const override { return addr_; }

    Mode mode() const override { return txdsp_.mode(); }
    void set_mode(Mode m) override;
    double mic_gain() const override { return txdsp_.mic_gain; }
    void set_mic_gain(double gain) override { txdsp_.mic_gain = gain; }

    void send_mic(const float *samples, size_t n) override;
    /// Queue packed TX samples (8 bytes each: L, R, I, Q big-endian).
    void tx_push(const std::vector<uint8_t> &data);
    void tx_clear() override;

    std::atomic<int> lost{0};              // receive packets missed

private:
    void run();
    void cc(uint8_t *out);
    bool tx_payload(uint8_t *out);
    void send_ep2();
    void send_stop();
    void start_stream();

    dsp::Transmitter txdsp_;
    std::string addr_;
    int sock_ = -1;
    std::thread thread_;
    std::atomic<bool> alive_{false};
    std::atomic<int> new_rate_{0};
    uint32_t seq_ = 0;
    int cci_ = 0;
    std::vector<uint8_t> tx_;
    std::mutex txlock_;
    std::vector<uint8_t> carrier_;
};

}  // namespace rigface
