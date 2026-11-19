#include "protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace rigface {

namespace {

const uint8_t SYNC[3] = {0x7f, 0x7f, 0x7f};
constexpr size_t FRAME = 504;              // payload bytes per USB frame (63 samples x 8)

const char *board_name(int board)
{
    switch (board) {
    case 0: return "Metis";
    case 1: return "Hermes";
    case 2: return "Griffin";
    case 4: return "Angelia";
    case 5: return "Orion";
    case 6: return "Radioberry / Hermes-Lite";
    case 10: return "Orion II";
    }
    return nullptr;
}

void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = v >> 24;
    p[1] = v >> 16;
    p[2] = v >> 8;
    p[3] = v;
}

void sleep_s(double s)
{
    std::this_thread::sleep_for(std::chrono::duration<double>(s));
}

/// Wait up to `timeout` seconds for a datagram; -1 with errno = EAGAIN if none came.
ssize_t recv_wait(int sock, uint8_t *buf, size_t len, double timeout, sockaddr_in *from = nullptr)
{
    pollfd p = {sock, POLLIN, 0};
    int r = poll(&p, 1, std::max(0, int(timeout * 1000)));
    if (r == 0) {
        errno = EAGAIN;
        return -1;
    }
    if (r < 0)
        return -1;
    socklen_t flen = sizeof *from;
    return recvfrom(sock, buf, len, 0, reinterpret_cast<sockaddr *>(from), from ? &flen : nullptr);
}

bool resolve(const std::string &host, int port, sockaddr_in &out)
{
    addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return false;
    std::memcpy(&out, res->ai_addr, sizeof out);
    freeaddrinfo(res);
    return true;
}

}  // namespace

namespace hpsdr {

std::vector<Found> discover(std::vector<std::string> targets, int port, double timeout)
{
    if (targets.empty())
        targets = {"255.255.255.255", "127.0.0.1"};
    std::vector<Found> found;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return found;
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    sockaddr_in any = {};
    any.sin_family = AF_INET;
    bind(s, reinterpret_cast<sockaddr *>(&any), sizeof any);
    uint8_t msg[63] = {0xef, 0xfe, 0x02};
    for (const std::string &t : targets) {
        sockaddr_in to;
        if (resolve(t, port, to))
            sendto(s, msg, sizeof msg, 0, reinterpret_cast<sockaddr *>(&to), sizeof to);
    }
    const double end = monotonic() + timeout;
    for (;;) {
        const double left = end - monotonic();
        if (left <= 0)
            break;
        uint8_t d[1500];
        sockaddr_in from = {};
        ssize_t n = recv_wait(s, d, sizeof d, left, &from);
        if (n < 0) {
            if (errno == EAGAIN)
                break;
            continue;
        }
        if (n >= 11 && d[0] == 0xef && d[1] == 0xfe && (d[2] == 2 || d[2] == 3)) {
            char ip[INET_ADDRSTRLEN] = "";
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
            char mac[18];
            std::snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x",
                          d[3], d[4], d[5], d[6], d[7], d[8]);
            Found f;
            f.addr = ip;
            f.port = ntohs(from.sin_port);
            f.mac = mac;
            f.version = d[9];
            f.board = d[10];
            f.busy = d[2] == 3;
            const char *name = board_name(d[10]);
            f.name = name ? name : "HPSDR board " + std::to_string(d[10]);
            auto same = std::find_if(found.begin(), found.end(), [&](const Found &o) {
                return o.addr == f.addr && o.port == f.port;
            });
            if (same != found.end())
                *same = f;
            else
                found.push_back(f);
        }
    }
    ::close(s);
    std::stable_sort(found.begin(), found.end(),
                     [](const Found &a, const Found &b) { return a.busy < b.busy; });
    return found;
}

Caps caps()
{
    Caps c;
    c.kind = "hpsdr";
    c.rates.assign(std::begin(RATES), std::end(RATES));
    c.modes.assign(std::begin(MODES), std::end(MODES));
    c.freq_lo = 10'000;
    c.freq_hi = 38'400'000;
    c.has_rf_gain = true;
    c.rf_gain_lo = -12;                    // AD9866 PGA
    c.rf_gain_hi = 48;
    c.has_drive = true;
    c.max_power = 0.0;
    c.smeter_cal = -12.0;
    return c;
}

}  // namespace hpsdr

HpsdrRadio::HpsdrRadio()
{
    caps = hpsdr::caps();
    rate = 192000;
    // a steady carrier at 0.9 of full scale: L, R, I, Q as big-endian int16
    const uint8_t sample[8] = {0, 0, 0, 0, 0x73, 0x33, 0, 0};      // I = 29491
    for (int i = 0; i < 63; ++i)
        carrier_.insert(carrier_.end(), sample, sample + 8);
}

HpsdrRadio::~HpsdrRadio()
{
    close();
}

// ---- control -----------------------------------------------------------------

void HpsdrRadio::open(const std::string &addr, int port)
{
    close();
    sockaddr_in to;
    if (!resolve(addr, port, to))
        throw IoError("unknown address");
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        throw IoError(std::strerror(errno));
    int size = 1 << 21;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &size, sizeof size);
    if (connect(s, reinterpret_cast<sockaddr *>(&to), sizeof to) < 0) {
        std::string why = std::strerror(errno);
        ::close(s);
        throw IoError(why);
    }
    addr_ = addr;
    sock_ = s;
    alive_ = true;
    thread_ = std::thread(&HpsdrRadio::run, this);
}

void HpsdrRadio::close()
{
    if (!alive_.exchange(false))
        return;
    mox = false;
    if (thread_.joinable())
        thread_.join();
    send_stop();
    ::close(sock_);
    sock_ = -1;
    streaming = false;
}

void HpsdrRadio::set_rate(int new_rate)
{
    if (new_rate != rate)
        new_rate_ = new_rate;
}

void HpsdrRadio::set_mode(Mode m)
{
    if (m != txdsp_.mode())
        txdsp_.set_mode(m);
}

void HpsdrRadio::send_mic(const float *samples, size_t n)
{
    tx_push(txdsp_.process(samples, n));
}

void HpsdrRadio::tx_push(const std::vector<uint8_t> &data)
{
    std::lock_guard<std::mutex> g(txlock_);
    tx_.insert(tx_.end(), data.begin(), data.end());
    if (tx_.size() > 8 * 12000)            // > 250 ms queued
        tx_.erase(tx_.begin(), tx_.end() - 8 * 4800);
}

void HpsdrRadio::tx_clear()
{
    std::lock_guard<std::mutex> g(txlock_);
    tx_.clear();
}

// ---- wire format ---------------------------------------------------------------

void HpsdrRadio::cc(uint8_t *out)
{
    const int i = cci_;
    cci_ = (i + 1) % 5;
    const uint8_t m = mox ? 1 : 0;
    std::memset(out, 0, 5);
    if (i == 0) {                          // speed, duplex on, one receiver
        const int *r = std::find(std::begin(hpsdr::RATES), std::end(hpsdr::RATES), rate.load());
        out[0] = m;
        out[1] = r == std::end(hpsdr::RATES) ? 0 : uint8_t(r - hpsdr::RATES);
        out[4] = 0x04;
    } else if (i == 1) {
        out[0] = 0x02 | m;
        put_be32(out + 1, uint32_t(tx_freq.load()));
    } else if (i == 2) {
        out[0] = 0x04 | m;
        put_be32(out + 1, uint32_t(rx_freq.load()));
    } else if (i == 3) {
        out[0] = 0x12 | m;
        out[1] = uint8_t(drive.load() & 0xFF);
    } else {
        const int gain = std::max(0, std::min(60, rf_gain.load() + 12));
        out[0] = 0x14 | m;
        out[4] = uint8_t(0x40 | gain);
    }
}

bool HpsdrRadio::tx_payload(uint8_t *out)
{
    if (!mox)
        return false;
    if (tx_carrier) {
        std::memcpy(out, carrier_.data(), FRAME);
        return true;
    }
    std::lock_guard<std::mutex> g(txlock_);
    if (tx_.size() < FRAME)
        return false;
    std::memcpy(out, tx_.data(), FRAME);
    tx_.erase(tx_.begin(), tx_.begin() + FRAME);
    return true;
}

void HpsdrRadio::send_ep2()
{
    uint8_t pkt[1032] = {0xef, 0xfe, 0x01, 0x02};
    put_be32(pkt + 4, seq_++);
    for (size_t off : {size_t(8), size_t(520)}) {
        std::memcpy(pkt + off, SYNC, 3);
        cc(pkt + off + 3);
        tx_payload(pkt + off + 8);
    }
    send(sock_, pkt, sizeof pkt, 0);
}

void HpsdrRadio::send_stop()
{
    const uint8_t msg[64] = {0xef, 0xfe, 0x04, 0x00};
    send(sock_, msg, sizeof msg, 0);
}

void HpsdrRadio::start_stream()
{
    seq_ = 0;
    for (int i = 0; i < 5; ++i)            // prime every control register
        send_ep2();
    const uint8_t msg[64] = {0xef, 0xfe, 0x04, 0x01};
    send(sock_, msg, sizeof msg, 0);
}

// ---- thread ------------------------------------------------------------------

void HpsdrRadio::run()
{
    start_stream();
    std::vector<uint8_t> raw;              // receive payload collected so far
    size_t chunks = 0;
    long npk = 0;
    bool have_expect = false;
    uint32_t expect = 0;
    double last = monotonic();
    double last_try = last;
    auto reset = [&] {
        raw.clear();
        chunks = 0;
        npk = 0;
        have_expect = false;
    };

    while (alive_) {
        if (int want = new_rate_.exchange(0)) {
            send_stop();
            sleep_s(0.05);
            uint8_t junk[2048];
            while (recv(sock_, junk, sizeof junk, MSG_DONTWAIT) >= 0) {
            }
            rate = want;
            reset();
            start_stream();
        }
        uint8_t d[2048];
        ssize_t n = recv_wait(sock_, d, sizeof d, 0.25);
        if (n < 0 && errno != EAGAIN)
            sleep_s(0.1);
        const double now = monotonic();
        if (n < 0) {
            if (now - last > 1.0) {
                streaming = false;
                if (now - last_try > 1.0) {        // keep knocking
                    last_try = now;
                    reset();
                    start_stream();
                }
            }
            continue;
        }
        if (n != 1032 || d[0] != 0xef || d[1] != 0xfe || d[2] != 0x01 || d[3] != 0x06)
            continue;
        last = now;
        streaming = true;
        const uint32_t seq = (uint32_t(d[4]) << 24) | (uint32_t(d[5]) << 16)
                             | (uint32_t(d[6]) << 8) | d[7];
        if (have_expect && seq != expect)
            ++lost;
        expect = seq + 1;
        have_expect = true;
        for (size_t off : {size_t(8), size_t(520)}) {
            if (std::memcmp(d + off, SYNC, 3) != 0)
                continue;
            if ((d[off + 3] >> 3) == 0)
                adc_overflow = d[off + 4] & 1;
            raw.insert(raw.end(), d + off + 8, d + off + 512);
            ++chunks;
        }
        ++npk;
        const int cur = rate;
        const long ratio = std::max(1, cur / 48000);
        if (npk % ratio == 0)              // TX stream is always 48 kHz
            send_ep2();
        if (chunks >= size_t(16 * ratio)) {        // 1008 audio samples, ~21 ms
            std::vector<cfloat> iq = dsp::unpack_iq(raw.data(), raw.size());
            raw.clear();
            chunks = 0;
            if (on_iq)
                on_iq(cur, std::move(iq));
        }
    }
}

}  // namespace rigface
