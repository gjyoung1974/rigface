#include "sim.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <termios.h>
#include <unistd.h>

namespace rigface::sim {

namespace {

constexpr int RATES[] = {48000, 96000, 192000, 384000};
constexpr double FS_DBM = -12.0;           // antenna level that hits ADC full scale at 0 dB gain
constexpr double TWO_PI = 2.0 * M_PI;

struct Ham {
    double start, phone, end;              // band start, phone segment start, band end
};
const Ham HAM[] = {{1.8e6, 1.84e6, 2.0e6}, {3.5e6, 3.6e6, 4.0e6}, {7.0e6, 7.05e6, 7.3e6},
                   {10.1e6, 10.15e6, 10.15e6}, {14.0e6, 14.1e6, 14.35e6},
                   {18.068e6, 18.11e6, 18.168e6}, {21.0e6, 21.15e6, 21.45e6},
                   {24.89e6, 24.93e6, 24.99e6}, {28.0e6, 28.3e6, 29.7e6}};
struct Broadcast {
    double start, end, step;               // step: channel spacing
};
const Broadcast BROADCAST[] = {{0.53e6, 1.7e6, 10e3}, {5.9e6, 6.2e6, 5e3}, {7.3e6, 7.45e6, 5e3},
                               {9.4e6, 9.9e6, 5e3}, {11.6e6, 12.1e6, 5e3},
                               {13.57e6, 13.87e6, 5e3}, {15.1e6, 15.8e6, 5e3},
                               {17.48e6, 17.9e6, 5e3}};
const double NOISE_F[] = {1e6, 4e6, 7e6, 14e6, 21e6, 30e6};
const double NOISE_DBM_HZ[] = {-135, -140, -145, -152, -158, -162};
constexpr double ADC_DBFS_HZ = -152.0;
const double NOTES[] = {262, 294, 330, 392, 440, 523};

const char *morse(char c)
{
    static const char *const LETTERS[] = {
        ".-", "-...", "-.-.", "-..", ".", "..-.", "--.", "....", "..", ".---", "-.-", ".-..",
        "--", "-.", "---", ".--.", "--.-", ".-.", "...", "-", "..-", "...-", ".--", "-..-",
        "-.--", "--.."};
    static const char *const DIGITS[] = {"-----", ".----", "..---", "...--", "....-",
                                         ".....", "-....", "--...", "---..", "----."};
    if (c >= 'A' && c <= 'Z')
        return LETTERS[c - 'A'];
    if (c >= '0' && c <= '9')
        return DIGITS[c - '0'];
    return "";
}

/// CRC-32 (the zlib one) of the arguments as little-endian 64-bit integers.
uint32_t hash(std::initializer_list<int64_t> values)
{
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (int64_t v : values)
        for (int b = 0; b < 8; ++b)
            crc = table[(crc ^ uint8_t(uint64_t(v) >> (8 * b))) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

std::vector<float> morse_bits(const std::string &text)
{
    std::vector<float> bits;
    for (char ch : text) {
        if (ch == ' ') {
            bits.insert(bits.end(), 4, 0.0f);
            continue;
        }
        for (const char *sym = morse(ch); *sym; ++sym) {
            bits.insert(bits.end(), *sym == '-' ? 3 : 1, 1.0f);
            bits.push_back(0.0f);
        }
        bits.insert(bits.end(), 2, 0.0f);
    }
    bits.insert(bits.end(), 14, 0.0f);
    return bits;
}

std::string callsign(uint32_t h)
{
    static const char *const PREFIX[] = {"K", "W", "N", "DL", "G", "JA", "VK", "EA"};
    std::string s = PREFIX[h % 8];
    s += char('0' + (h >> 3) % 10);
    s += char('A' + (h >> 7) % 26);
    s += char('A' + (h >> 12) % 26);
    s += char('A' + (h >> 17) % 26);
    return s;
}

/// Channel numbers k with max(lo, a) <= k * step <= min(hi, b).
void slots(double lo, double hi, double a, double b, double step, int64_t &first, int64_t &last)
{
    first = int64_t(std::ceil(std::max(lo, a) / step));
    last = int64_t(std::floor(std::min(hi, b) / step));
}

double interp(double x, const double *xs, const double *ys, int n)
{
    if (x <= xs[0])
        return ys[0];
    for (int i = 1; i < n; ++i)
        if (x <= xs[i])
            return ys[i - 1] + (ys[i] - ys[i - 1]) * (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[n - 1];
}

inline double frac(double x)
{
    return x - std::floor(x);
}

/// exp(2*pi*j*cycles)
inline cfloat phasor(double cycles)
{
    const float ph = static_cast<float>(TWO_PI * frac(cycles));
    return cfloat(std::cos(ph), std::sin(ph));
}

inline cfloat mul(cfloat a, cfloat b)
{
    return cfloat(a.real() * b.real() - a.imag() * b.imag(),
                  a.real() * b.imag() + a.imag() * b.real());
}

/// exp(2*pi*j*f*t) for the samples n0 .. n0+n-1 at `rate`, one rotation per sample.
class Carrier {
public:
    Carrier(double f, int64_t n0, int rate)
    {
        const double ph = TWO_PI * frac(f * (double(n0) / rate)), w = TWO_PI * f / rate;
        re_ = std::cos(ph);
        im_ = std::sin(ph);
        wr_ = std::cos(w);
        wi_ = std::sin(w);
    }
    cfloat next()
    {
        const cfloat v(static_cast<float>(re_), static_cast<float>(im_));
        const double re = re_ * wr_ - im_ * wi_;
        im_ = re_ * wi_ + im_ * wr_;
        re_ = re;
        return v;
    }

private:
    double re_, im_, wr_, wi_;
};

double now_s()
{
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

std::vector<Station> band_signals(double lo, double hi)
{
    std::vector<Station> out;
    int64_t first, last;
    for (const Ham &b : HAM) {
        slots(lo, hi, b.start + 500, b.phone - 500, 500, first, last);
        for (int64_t k = first; k <= last; ++k) {
            const uint32_t h = hash({1, k});
            if (h % 100 < 12)
                out.push_back({Kind::CW, double(k * 500 + int64_t((h >> 8) % 200) - 100), h});
        }
        slots(lo, hi, b.phone + 3000, b.end - 3000, 3000, first, last);
        for (int64_t k = first; k <= last; ++k) {
            const uint32_t h = hash({2, k});
            if (h % 100 < 22)
                out.push_back({Kind::SSB, double(k * 3000), h});
        }
    }
    for (const Broadcast &b : BROADCAST) {
        slots(lo, hi, b.start, b.end, b.step, first, last);
        for (int64_t k = first; k <= last; ++k) {
            const uint32_t h = hash({3, k});
            if (h % 100 < 45)
                out.push_back({Kind::AM, k * b.step, h});
        }
    }
    return out;
}

const std::vector<Station> &Band::stations()
{
    if (rate != key_rate_ || rx_freq != key_freq_) {
        key_rate_ = rate;
        key_freq_ = rx_freq;
        const double half = rate / 2.0;
        sigs_ = band_signals(rx_freq - half, rx_freq + half);
    }
    return sigs_;
}

std::vector<cfloat> Band::noise(size_t n, bool antenna)
{
    double dens = std::pow(10.0, ADC_DBFS_HZ / 10);
    if (antenna) {
        const double ant = interp(double(rx_freq), NOISE_F, NOISE_DBM_HZ, 6);
        dens += std::pow(10.0, (ant - FS_DBM + gain) / 10);
    }
    const float sigma = static_cast<float>(std::sqrt(dens * rate / 2));
    std::vector<cfloat> z(n);
    for (cfloat &v : z) {
        const float re = normal_(rng_) * sigma;
        v = cfloat(re, normal_(rng_) * sigma);
    }
    return z;
}

std::vector<cfloat> Band::generate(size_t n)
{
    const double t0 = double(n0) / rate;
    std::vector<cfloat> out = noise(n);
    for (const Station &s : stations()) {
        const uint32_t h = s.seed;
        if (s.kind != Kind::AM) {
            const int period = 8 + h % 20;
            if (hash({int64_t(h), int64_t(t0 / period)}) % 100 >= 65)
                continue;                  // station is listening
        }
        const double dbm = -100.0 + (h >> 12) % 50;
        const double qsb = 0.55 + 0.45 * std::sin(TWO_PI * t0 / (5 + h % 13) + h % 7);
        const float amp = static_cast<float>(qsb * std::pow(10.0, (dbm - FS_DBM + gain) / 20));
        Carrier carrier(s.freq - double(rx_freq), n0, rate);

        if (s.kind == Kind::CW) {
            const std::vector<float> env = cw_env(h, n);
            for (size_t i = 0; i < n; ++i)
                out[i] += carrier.next() * (env[i] * amp);
        } else if (s.kind == Kind::SSB) {
            // Five wandering harmonics with a syllable envelope; single sideband.
            const double sign = s.freq < 10e6 ? -1.0 : 1.0;
            const double f0 = 380 + 15 * (h % 5);
            const double dev = 40 / (TWO_PI * 0.5);
            float a[5];
            const double weight[5] = {1.0, 0.8, 0.6, 0.4, 0.3};
            for (int k = 0; k < 5; ++k)
                a[k] = static_cast<float>(
                    weight[k] * (0.5 + 0.5 * std::sin(TWO_PI * (1.1 + 0.4 * k) * t0 + k + h % 5)));
            for (size_t i = 0; i < n; ++i) {
                const double t = double(n0 + int64_t(i)) / rate;
                const double cyc = f0 * t + dev * std::sin(TWO_PI * 0.5 * t + h % 9);
                const cfloat w = phasor(sign * cyc);
                cfloat v(a[4], 0.0f);
                for (int k = 3; k >= 0; --k)
                    v = mul(v, w) + a[k];
                v = mul(v, w);
                const double e = 0.5 + 0.5 * std::sin(TWO_PI * 3.1 * t + h % 11);
                out[i] += mul(carrier.next(), v) * (static_cast<float>(0.4 * e * e) * amp);
            }
        } else {
            for (size_t i = 0; i < n; ++i) {
                const double t = double(n0 + int64_t(i)) / rate;
                const int64_t idx = int64_t(t / 0.25);
                const double note = NOTES[((idx * 2654435761LL + h) >> 7) % 6];
                const float ph = static_cast<float>(TWO_PI * frac(note * t));
                const float m = 0.5f + 0.2f * std::sin(ph) + 0.1f * std::sin(2 * ph);
                out[i] += carrier.next() * (m * amp);
            }
        }
    }
    n0 += int64_t(n);
    return out;
}

std::vector<float> Band::cw_env(uint32_t h, size_t n)
{
    auto it = cw_.find(h);
    if (it == cw_.end()) {
        const std::string call = callsign(h);
        it = cw_.emplace(h, morse_bits("CQ CQ DE " + call + " " + call + " K")).first;
    }
    const std::vector<float> &bits = it->second;
    const int64_t len = int64_t(bits.size());
    const double dot = 1.2 / (14 + h % 16);
    const int64_t pad = int64_t(0.003 * rate);          // 6 ms raised edges
    const size_t total = n + 2 * size_t(pad);
    std::vector<double> c(total + 1, 0.0);              // running sum of the keying
    for (size_t i = 0; i < total; ++i) {
        const double tt = double(n0 - pad + int64_t(i)) / rate;
        int64_t k = int64_t(tt / dot) % len;
        if (k < 0)
            k += len;
        c[i + 1] = c[i] + bits[size_t(k)];
    }
    std::vector<float> env(n);
    for (size_t i = 0; i < n; ++i)
        env[i] = static_cast<float>((c[i + 2 * pad + 1] - c[i]) / double(2 * pad + 1));
    return env;
}

std::vector<uint8_t> pack24(const std::vector<cfloat> &x)
{
    std::vector<uint8_t> out(x.size() * 8, 0);
    for (size_t i = 0; i < x.size(); ++i) {
        const float part[2] = {x[i].real(), x[i].imag()};
        for (int c = 0; c < 2; ++c) {
            const double s = std::max(-8388608.0, std::min(8388607.0, part[c] * 8388607.0));
            const int32_t v = static_cast<int32_t>(s);
            uint8_t *p = &out[i * 8 + 3 * c];
            p[0] = (v >> 16) & 0xFF;
            p[1] = (v >> 8) & 0xFF;
            p[2] = v & 0xFF;
        }
    }
    return out;
}

// ---- HPSDR Protocol 1 ----------------------------------------------------------

int serve(int port, const std::string &host, bool verbose, int parent)
{
    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1
        || bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof addr) < 0) {
        std::fprintf(stderr, "rigface-sim: cannot listen on %s:%d: %s\n", host.c_str(), port,
                     errno ? std::strerror(errno) : "bad address");
        return 1;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);

    Band band;
    bool running = false;
    sockaddr_in client = {};
    uint32_t seq = 0;
    bool mox = false;
    std::vector<cfloat> tx;                // looped-back transmit samples, 48 kHz
    double next_t = 0.0;
    double last_ep2 = 0.0;
    int cc_addr = 0;
    const uint8_t mac[6] = {0x00, 0x1c, 0xc0, 0xa2, 0x13, 0xdd};
    if (verbose) {
        std::printf("simulated Radioberry listening on %s:%d\n", host.c_str(), port);
        std::fflush(stdout);
    }

    for (;;) {
        double now = now_s();
        const double wait = running ? std::max(0.0, next_t - now) : 1.0;
        pollfd pfd = {sock, POLLIN, 0};
        if (poll(&pfd, 1, int(std::ceil(wait * 1000))) > 0) {
            for (;;) {
                uint8_t d[2048];
                sockaddr_in from = {};
                socklen_t flen = sizeof from;
                const ssize_t len = recvfrom(sock, d, sizeof d, 0,
                                             reinterpret_cast<sockaddr *>(&from), &flen);
                if (len < 0)
                    break;
                if (len < 4 || d[0] != 0xef || d[1] != 0xfe)
                    continue;
                if (d[2] == 2) {
                    uint8_t reply[60] = {0xef, 0xfe, uint8_t(running ? 3 : 2)};
                    std::memcpy(reply + 3, mac, 6);
                    reply[9] = 73;
                    reply[10] = 6;
                    sendto(sock, reply, sizeof reply, 0, reinterpret_cast<sockaddr *>(&from),
                           flen);
                } else if (d[2] == 4) {
                    running = d[3] & 1;
                    client = from;
                    seq = 0;
                    next_t = last_ep2 = now_s();
                    tx.clear();
                    if (verbose) {
                        char ip[INET_ADDRSTRLEN] = "";
                        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
                        std::printf("stream %s %s:%d\n", running ? "start" : "stop", ip,
                                    ntohs(from.sin_port));
                        std::fflush(stdout);
                    }
                } else if (d[2] == 1 && d[3] == 2 && len == 1032) {
                    last_ep2 = now_s();
                    for (size_t off : {size_t(8), size_t(520)}) {
                        const uint8_t c0 = d[off + 3];
                        mox = c0 & 1;
                        const int reg = c0 >> 1;
                        if (reg == 0) {
                            band.rate = RATES[d[off + 4] & 3];
                        } else if (reg == 1) {
                            // TX frequency: loopback is co-channel
                        } else if (reg == 2) {
                            band.rx_freq = (int64_t(d[off + 4]) << 24) | (d[off + 5] << 16)
                                           | (d[off + 6] << 8) | d[off + 7];
                        } else if (reg == 10 && (d[off + 7] & 0x40)) {
                            band.gain = (d[off + 7] & 0x3F) - 12;
                        }
                        if (mox) {
                            for (size_t i = 0; i < 63; ++i) {
                                const uint8_t *s = d + off + 8 + 8 * i;
                                const int16_t re = int16_t((s[4] << 8) | s[5]);
                                const int16_t im = int16_t((s[6] << 8) | s[7]);
                                tx.emplace_back(re / 32768.0f, im / 32768.0f);
                            }
                        }
                    }
                }
            }
        }
        now = now_s();
        if (parent && getppid() != parent)
            return 0;                      // the GUI that started us is gone
        if (!running)
            continue;
        if (now - last_ep2 > 2.0) {        // client went away
            running = false;
            continue;
        }
        if (now < next_t)
            continue;
        const int m = band.rate / 48000;
        const int npk = 4 * m;             // ~10.5 ms per burst
        const size_t n = size_t(npk) * 126;
        std::vector<cfloat> x;
        if (mox) {
            x = band.noise(n, false);
            band.n0 += int64_t(n);
            const size_t need = n / m;
            if (tx.size() >= need) {
                for (size_t i = 0; i < n; ++i)
                    x[i] += tx[i / m] * 0.1f;
                size_t drop = need;
                if (tx.size() - drop > 4800)        // keep loopback delay bounded
                    drop = tx.size() - 4800;
                tx.erase(tx.begin(), tx.begin() + drop);
            }
        } else {
            tx.clear();
            x = band.generate(n);
        }
        bool overflow = false;
        for (const cfloat &v : x)
            if (std::fabs(v.real()) >= 1.0f || std::fabs(v.imag()) >= 1.0f)
                overflow = true;
        const std::vector<uint8_t> raw = pack24(x);
        for (int p = 0; p < npk; ++p) {
            uint8_t pkt[1032] = {0xef, 0xfe, 0x01, 0x06, uint8_t(seq >> 24), uint8_t(seq >> 16),
                                 uint8_t(seq >> 8), uint8_t(seq)};
            ++seq;
            for (int f = 0; f < 2; ++f) {
                uint8_t *frame = pkt + 8 + 512 * f;
                frame[0] = frame[1] = frame[2] = 0x7f;
                frame[3] = uint8_t((cc_addr << 3) | int(mox));
                frame[4] = cc_addr == 0 ? uint8_t(overflow) : 0;
                frame[7] = 73;
                cc_addr = (cc_addr + 1) % 4;
                std::memcpy(frame + 8, raw.data() + size_t(p * 2 + f) * 504, 504);
            }
            sendto(sock, pkt, sizeof pkt, 0, reinterpret_cast<sockaddr *>(&client),
                   sizeof client);
        }
        next_t += double(n) / band.rate;
        if (now - next_t > 0.25)
            next_t = now;
    }
}

// ---- QMX -------------------------------------------------------------------------

namespace {

constexpr int QMX_IF = 12000;              // the QMX's LO sits this far below the dial ...
constexpr int QMX_CW = 700;                // ... and this much further in CW

bool all_digits(const std::string &s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) {
        return std::isdigit(c);
    });
}

std::string strip(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

}  // namespace

int serve_qmx(int iq_port, int parent)
{
    const int master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0 || grantpt(master) < 0 || unlockpt(master) < 0) {
        std::perror("rigface-sim: pseudo-terminal");
        return 1;
    }
    const std::string name = ptsname(master);
    // the slave stays open so the port survives reconnects
    const int slave = open(name.c_str(), O_RDWR | O_NOCTTY);
    termios raw;
    if (slave < 0 || tcgetattr(slave, &raw) < 0) {
        std::perror("rigface-sim: pseudo-terminal");
        return 1;
    }
    cfmakeraw(&raw);
    tcsetattr(slave, TCSAFLUSH, &raw);
    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    std::printf("%s\n", name.c_str());
    std::fflush(stdout);

    const int sock = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in dest = {};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(uint16_t(iq_port));
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    Band band;
    band.rate = 48000;
    std::map<std::string, int64_t> st = {{"FA", 7100000}, {"FB", 7100000}, {"MD", 2}, {"TQ", 0},
                                         {"KD", 0}, {"Q9", 0}, {"SS", 2}, {"FR", 0}};
    std::string buf;
    const size_t n = 480;                  // 10 ms per datagram
    double next_t = now_s();

    auto lo = [&st]() -> int64_t {
        const int64_t md = st["MD"];
        return st["FA"] - QMX_IF - (md == 3 ? QMX_CW : md == 7 ? -QMX_CW : 0);
    };

    // One CAT command -> its reply, or "" if it has none.
    auto cat = [&](const std::string &cmd) -> std::string {
        const std::string code = cmd.substr(0, 2), arg = cmd.size() > 2 ? cmd.substr(2) : "";
        const bool sending = st["KD"] || st["TQ"];
        if ((code == "TX" || code == "RX") && arg.empty()) {
            st["TQ"] = code == "TX";
        } else if (st.count(code)) {
            if (arg.empty()) {
                char out[32];
                const int width = (code == "FA" || code == "FB") ? 11 : 1;
                std::snprintf(out, sizeof out, "%s%0*lld", code.c_str(), width,
                              static_cast<long long>(st[code]));
                return out;
            }
            if (!all_digits(arg) || arg.size() > 15)
                return "?";
            st[code] = std::stoll(arg);
        } else if (cmd == "ID") {
            return "ID020";
        } else if (cmd == "VN") {
            return "VN1_04_003QMX";
        } else if (cmd == "PL") {
            return "PL" + std::to_string(lo()) + "|20|22|274|625";
        } else if (cmd == "PC") {
            return sending ? "PC45" : "PC0";
        } else if (cmd == "SW") {
            return sending ? "SW121" : "SW";
        } else {
            return "?";
        }
        return "";
    };

    for (;;) {
        const double wait = std::max(0.0, next_t - now_s());
        pollfd pfd = {master, POLLIN, 0};
        if (poll(&pfd, 1, int(std::ceil(wait * 1000))) > 0) {
            char d[1024];
            const ssize_t got = read(master, d, sizeof d);
            if (got > 0)
                buf.append(d, size_t(got));
            size_t end;
            while ((end = buf.find(';')) != std::string::npos) {
                std::string reply = cat(strip(buf.substr(0, end)));
                buf.erase(0, end + 1);
                if (!reply.empty()) {
                    reply += ';';
                    if (write(master, reply.data(), reply.size()) < 0) {
                        // nobody is reading; the reply is lost, as on a real port
                    }
                }
            }
        }
        if (parent && getppid() != parent)
            return 0;
        const double now = now_s();
        if (now < next_t)
            continue;
        band.rx_freq = lo();
        std::vector<cfloat> x;
        if (st["KD"] || st["TQ"]) {        // receiver sees only our own carrier
            x = band.noise(n, false);
            if (st["KD"]) {
                Carrier carrier(double(st["FA"] - band.rx_freq), band.n0, band.rate);
                for (cfloat &v : x)
                    v += carrier.next() * 0.1f;
            }
            band.n0 += int64_t(n);
        } else {
            x = band.generate(n);
        }
        if (st["Q9"])                      // no I/Q until IQ mode is switched on
            sendto(sock, x.data(), x.size() * sizeof(cfloat), 0,
                   reinterpret_cast<sockaddr *>(&dest), sizeof dest);
        next_t += double(n) / band.rate;
        if (now - next_t > 0.25)
            next_t = now;
    }
}

}  // namespace rigface::sim
