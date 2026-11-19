#include "qmx.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace rigface {

namespace {

int md_code(Mode m)                        // 0: the QMX has no such mode
{
    switch (m) {
    case Mode::LSB: return 1;
    case Mode::USB: return 2;
    case Mode::CW: return 3;
    case Mode::AM: return 5;
    default: return 0;
    }
}

std::optional<Mode> md_mode(long long code)
{
    switch (code) {
    case 1: return Mode::LSB;
    case 2: return Mode::USB;
    case 3: return Mode::CW;
    case 5: return Mode::AM;
    case 7: return Mode::CW;
    }
    return std::nullopt;
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

/// A whole decimal number, as CAT replies carry them.
std::optional<long long> to_int(const std::optional<std::string> &text)
{
    if (!text)
        return std::nullopt;
    std::string s = strip(*text);
    size_t i = (!s.empty() && (s[0] == '+' || s[0] == '-')) ? 1 : 0;
    if (i == s.size() || s.size() > 18)
        return std::nullopt;
    for (size_t k = i; k < s.size(); ++k)
        if (!std::isdigit(static_cast<unsigned char>(s[k])))
            return std::nullopt;
    return std::stoll(s);
}

std::string read_file(const fs::path &path)
{
    std::ifstream f(path);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return strip(text);
}

/// sysfs directory of the USB device behind /dev/ttyACMn, or empty.
fs::path usb_device(const std::string &tty)
{
    std::error_code ec;
    fs::path real = fs::weakly_canonical(tty, ec);
    if (ec)
        real = tty;
    fs::path node = fs::path("/sys/class/tty") / real.filename() / "device";
    if (!fs::exists(node, ec))
        return {};
    fs::path dev = fs::canonical(node, ec);
    return ec ? fs::path() : dev.parent_path();
}

/// ALSA card number of the sound card on the same USB device, or -1.
int card_of(const fs::path &usb)
{
    std::vector<fs::path> cards;
    std::error_code ec;
    for (const auto &iface : fs::directory_iterator(usb, ec)) {
        std::error_code ec2;
        for (const auto &e : fs::directory_iterator(iface.path() / "sound", ec2))
            if (e.path().filename().string().rfind("card", 0) == 0)
                cards.push_back(e.path());
    }
    std::sort(cards.begin(), cards.end());
    for (const fs::path &p : cards)
        if (auto n = to_int(p.filename().string().substr(4)))
            return int(*n);
    return -1;
}

void sleep_s(double s)
{
    std::this_thread::sleep_for(std::chrono::duration<double>(s));
}

}  // namespace

namespace qmx {

std::vector<Port> find()
{
    std::vector<Port> out;
    std::vector<std::string> ttys;
    std::error_code ec;
    for (const auto &e : fs::directory_iterator("/sys/class/tty", ec)) {
        std::string name = e.path().filename().string();
        if (name.rfind("ttyACM", 0) == 0)
            ttys.push_back(name);
    }
    std::sort(ttys.begin(), ttys.end(), [](const std::string &a, const std::string &b) {
        return a.size() != b.size() ? a.size() < b.size() : a < b;
    });
    std::vector<fs::path> seen;
    for (const std::string &name : ttys) {
        std::string tty = "/dev/" + name;
        fs::path usb = usb_device(tty);
        // a QMX can show up to three serial ports
        if (usb.empty() || std::find(seen.begin(), seen.end(), usb) != seen.end())
            continue;
        std::string product = read_file(usb / "product");
        std::string text = read_file(usb / "manufacturer") + " " + product;
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (text.find("qrp labs") != std::string::npos || text.find("qmx") != std::string::npos) {
            seen.push_back(usb);
            out.push_back({tty, card_of(usb), product.empty() ? "QMX" : product});
        }
    }
    return out;
}

Caps caps()
{
    Caps c;
    c.kind = "qmx";
    c.rates = {RATE};
    c.modes = {Mode::LSB, Mode::USB, Mode::CW, Mode::AM};
    c.freq_lo = 10'000;                    // the radio refuses what its band filters don't cover
    c.freq_hi = 60'000'000;
    c.has_rf_gain = false;
    c.rf_gain_lo = c.rf_gain_hi = 0;
    c.has_drive = false;
    c.max_power = 6.0;
    c.smeter_cal = -20.0;                  // dBm at ADC full scale: nominal, not measured
    return c;
}

}  // namespace qmx

QmxRadio::QmxRadio()
{
    caps = qmx::caps();
    rate = qmx::RATE;
    rf_gain = 0;
    if_offset = qmx::IF_DEFAULT;
}

QmxRadio::~QmxRadio()
{
    close();
}

// ---- control -----------------------------------------------------------------

void QmxRadio::open(const std::string &tty_arg, const std::string &audio_in,
                    const std::string &audio_out)
{
    close();
    std::string tty = tty_arg;
    int card = -1;
    if (tty.empty()) {
        std::vector<qmx::Port> found = qmx::find();
        if (found.empty())
            throw IoError("no QMX on USB");
        tty = found[0].tty;
        card = found[0].card;
    } else {
        fs::path usb = usb_device(tty);
        card = usb.empty() ? -1 : card_of(usb);
    }
    label_ = tty;
    const std::string hw = card < 0 ? "" : "hw:" + std::to_string(card) + ",0";
    audio_in_ = !audio_in.empty() ? audio_in
                : hw.empty() ? "" : "alsasrc device=" + hw + " latency-time=20000";
    audio_out_ = !audio_out.empty() ? audio_out : hw.empty() ? "" : "alsasink device=" + hw;
    open_port();
    alive_ = true;
    thread_ = std::thread(&QmxRadio::run, this);
}

void QmxRadio::close()
{
    if (!alive_.exchange(false))
        return;
    mox = false;
    if (thread_.joinable())
        thread_.join();
    streaming = false;
}

void QmxRadio::send_mic(const float *samples, size_t n)
{
    std::shared_ptr<AudioOut> out;
    double gain;
    {
        std::lock_guard<std::mutex> g(out_lock_);
        out = out_;
        if (!out || !n)
            return;
        // same ALC as the HPSDR transmitter: never exceed 0.9 of full scale
        double peak = 0.0;
        for (size_t i = 0; i < n; ++i)
            peak = std::max(peak, double(std::fabs(samples[i])));
        env_ = std::max({peak, env_ * 0.97, 1e-6});
        gain = std::min(mic_gain_.load(), 0.9 / env_);
    }
    std::vector<float> scaled(samples, samples + n);
    for (float &v : scaled)
        v = static_cast<float>(v * gain);
    out->write(scaled.data(), n);
}

bool QmxRadio::has_tx_audio()
{
    std::lock_guard<std::mutex> g(out_lock_);
    return bool(out_);
}

// ---- serial port -------------------------------------------------------------

void QmxRadio::open_port()
{
    int fd = ::open(label_.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        throw IoError(std::strerror(errno));
    termios a;
    bool ok = tcgetattr(fd, &a) == 0;
    if (ok) {
        a.c_iflag = a.c_oflag = a.c_lflag = 0;                 // raw, no echo
        a.c_cflag = (a.c_cflag & ~(CSIZE | PARENB | HUPCL)) | CS8 | CLOCAL | CREAD;
        a.c_cc[VMIN] = a.c_cc[VTIME] = 0;
        ok = tcsetattr(fd, TCSANOW, &a) == 0;
    }
    if (!ok) {
        ::close(fd);
        throw IoError(label_ + " is not a serial port");
    }
    // The QMX can be set to key or PTT from DTR; never hold it asserted.
    int lines = TIOCM_DTR | TIOCM_RTS;
    ioctl(fd, TIOCMBIC, &lines);
    fd_ = fd;
    buf_.clear();
    dirty_ = false;
}

void QmxRadio::write(const std::string &text)
{
    // CAT is terminated by ';' only: a carriage return would drop the
    // port into the QMX's terminal mode.
    if (::write(fd_, text.data(), text.size()) < 0)
        throw IoError(std::strerror(errno));
}

/// The next ';'-terminated reply, waiting until `deadline` for it.
bool QmxRadio::token(double deadline, std::string &tok)
{
    for (;;) {
        size_t i = buf_.find(';');
        if (i != std::string::npos) {
            tok = strip(buf_.substr(0, i));
            buf_.erase(0, i + 1);
            return true;
        }
        const double left = deadline - monotonic();
        if (left <= 0)
            return false;
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd_, &rd);
        timeval tv;
        tv.tv_sec = long(left);
        tv.tv_usec = long((left - double(tv.tv_sec)) * 1e6);
        int r = select(fd_ + 1, &rd, nullptr, nullptr, &tv);
        if (r < 0 && errno != EINTR)
            throw IoError(std::strerror(errno));
        if (r > 0) {
            char d[512];
            ssize_t n = ::read(fd_, d, sizeof d);
            if (n == 0)
                throw IoError("serial port closed");
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR)
                    continue;
                throw IoError(std::strerror(errno));
            }
            buf_.append(d, size_t(n));
        }
    }
}

/// Send commands that answer nothing (or '?;', collected later).
void QmxRadio::set(const std::string &text)
{
    write(text);
    dirty_ = true;
}

/// Send a get command; its reply without the two-letter code, or nothing.
std::optional<std::string> QmxRadio::query(const std::string &cmd)
{
    if (dirty_ && cmd != "ID") {           // swallow errors from earlier sets
        dirty_ = false;
        query("ID");
    }
    write(cmd + ";");
    const double end = monotonic() + 0.4;
    for (;;) {
        std::string tok;
        if (!token(end, tok))
            return std::nullopt;
        if (tok.rfind(cmd, 0) == 0) {
            last_ok_ = monotonic();
            return tok.substr(2);
        }
        if (tok == "?" && cmd != "ID") {
            last_ok_ = monotonic();
            return std::nullopt;
        }
    }
}

// ---- audio -------------------------------------------------------------------

void QmxRadio::on_audio(std::vector<cfloat> &&iq)          // GStreamer thread
{
    last_iq_ = monotonic();
    pend_.insert(pend_.end(), iq.begin(), iq.end());
    if (pend_.size() < size_t(qmx::BLOCK))
        return;
    const size_t n = pend_.size() / qmx::BLOCK * qmx::BLOCK;
    float peak = 0.0f;
    for (const cfloat &v : pend_)
        peak = std::max({peak, std::fabs(v.real()), std::fabs(v.imag())});
    adc_overflow = peak >= 0.999f;
    if (on_iq)
        for (size_t i = 0; i < n; i += qmx::BLOCK)
            on_iq(qmx::RATE, std::vector<cfloat>(pend_.begin() + i,
                                                 pend_.begin() + i + qmx::BLOCK));
    pend_.erase(pend_.begin(), pend_.begin() + n);
}

std::string QmxRadio::start_iq()
{
    if (audio_in_.empty())
        return "no sound card found for the QMX on " + label_;
    try {
        pend_.clear();
        iq_ = std::make_unique<IQIn>(audio_in_,
                                     [this](std::vector<cfloat> &&iq) { on_audio(std::move(iq)); });
    } catch (const AudioError &e) {
        return std::string("QMX audio: ") + e.what();
    }
    return "";
}

void QmxRadio::stop_iq()
{
    iq_.reset();
}

// ---- transmit ----------------------------------------------------------------

void QmxRadio::begin_tx(bool carrier)
{
    if (carrier) {
        if (sent_mode_ != Mode::CW) {      // TUNE: borrow CW for a plain carrier
            set("MD3;");
            restore_mode_ = true;
            need_lo_ = true;               // CW moves the LO
            t_set_ = monotonic();
        }
        set("KD1;");
        tx_state = TX_KEY;
    } else {
        if (!audio_out_.empty()) {
            try {
                auto out = std::make_shared<AudioOut>(audio_out_);
                std::lock_guard<std::mutex> g(out_lock_);
                env_ = 0.1;
                out_ = std::move(out);
            } catch (const AudioError &e) {
                set_error(std::string("QMX audio: ") + e.what());
            }
        }
        set("TX;");
        tx_state = TX_VOICE;
    }
}

void QmxRadio::end_tx()
{
    set(tx_state == TX_KEY ? "KD0;RX;" : "RX;");
    if (restore_mode_) {
        restore_mode_ = false;
        const int code = sent_mode_ ? md_code(*sent_mode_) : 0;
        set("MD" + std::to_string(code ? code : 2) + ";");
        need_lo_ = true;
    }
    std::shared_ptr<AudioOut> out;
    {
        std::lock_guard<std::mutex> g(out_lock_);
        out.swap(out_);
    }
    if (out)
        out->close();
    tx_state = TX_NONE;
    t_set_ = monotonic();
    fwd_power = NAN;
    swr = NAN;
}

// ---- thread ------------------------------------------------------------------

void QmxRadio::run()
{
    while (alive_) {
        try {
            if (fd_ < 0)
                open_port();
            session();
        } catch (const IoError &e) {
            set_error("QMX on " + label_ + ": " + e.what());
        }
        streaming = false;
        stop_iq();
        if (fd_ >= 0) {
            try {
                if (tx_state != TX_NONE)
                    end_tx();
                if (!alive_)
                    write("Q90;");         // hand the sound card back
            } catch (const IoError &) {
            }
            ::close(fd_);
        }
        fd_ = -1;
        tx_state = TX_NONE;
        {
            std::lock_guard<std::mutex> g(out_lock_);
            out_.reset();
        }
        for (int i = 0; i < 10; ++i)       // unplugged? look again in a second
            if (alive_)
                sleep_s(0.1);
    }
}

void QmxRadio::session()
{
    std::optional<std::string> ver = query("VN");
    if (!ver && !query("FA"))
        throw IoError("no answer to CAT commands");
    std::string fw = ver.value_or("");
    for (size_t i; (i = fw.find("QMX")) != std::string::npos;)
        fw.erase(i, 3);
    std::replace(fw.begin(), fw.end(), '_', '.');
    set_info(RadioInfo{"QMX", 0, fw});
    // VFO A simplex, raw I/Q on the sound card, SSB audio from USB
    set(std::string("FR0;Q91;") + (usb_tx_audio ? "SS0;" : ""));
    sent_freq = -1;
    sent_mode_.reset();
    need_lo_ = has_pl_ = true;
    double now = monotonic();
    last_iq_ = now;
    std::string audio_err = start_iq();
    double t_audio = now, t_poll = now;

    while (alive_) {
        now = monotonic();
        const bool want_tx = mox;
        if (want_tx && tx_state == TX_NONE)
            begin_tx(tx_carrier);
        else if (tx_state != TX_NONE && !want_tx)
            end_tx();

        const int64_t f = rx_freq;
        if (f != sent_freq) {
            set("FA" + std::to_string(f) + ";");
            sent_freq = f;
            t_set_ = now;
            need_lo_ = true;
        }
        const Mode m = mode_;
        if (m != sent_mode_ && md_code(m) && tx_state == TX_NONE) {
            set("MD" + std::to_string(md_code(m)) + ";");
            sent_mode_ = m;
            t_set_ = now;
            need_lo_ = true;
        }

        if (need_lo_ && now - t_set_ > 0.15) {
            read_lo();
        } else if (tx_state != TX_NONE) {
            if (now - t_poll > 0.25) {     // also keeps the QMX's CAT timeout at bay
                t_poll = now;
                read_power();
            }
        } else if (now - t_poll > 0.3) {
            t_poll = now;
            follow_panel(now);
        }

        // ---- health
        if (iq_) {
            std::string err = iq_->error();
            if (!err.empty()) {
                audio_err = "QMX audio: " + err;
                stop_iq();
                t_audio = now;
            }
        } else if (now - t_audio > 2.0) {
            t_audio = now;
            audio_err = start_iq();
            last_iq_ = now;
        }
        if (now - last_ok_ > 3.0)
            throw IoError("no answer to CAT commands");
        streaming = iq_ && now - last_iq_ < 1.0;
        if (streaming)
            set_error("");
        else if (now - last_iq_ > 1.5 || !iq_)
            set_error(!audio_err.empty() ? audio_err
                                         : "no I/Q audio from the QMX on " + label_);
        sleep_s(0.02);
    }
}

/// Where is the tuned frequency within the I/Q? Ask for the real LO.
void QmxRadio::read_lo()
{
    need_lo_ = false;
    if (!has_pl_)
        return;
    std::optional<std::string> r = query("PL");
    std::optional<long long> lo;
    if (r)
        lo = to_int(r->substr(0, r->find('|')));
    if (!lo || sent_freq < 0) {
        has_pl_ = r.has_value();           // '?;' = firmware before 1_03_000
        return;
    }
    const long long off = sent_freq - *lo;
    if (std::llabs(off) < qmx::RATE / 2)
        if_offset = int(off);
}

/// Pick up tuning and mode changes made with the radio's own knobs.
void QmxRadio::follow_panel(double now)
{
    std::optional<long long> f = to_int(query("FA"));
    std::optional<long long> md = to_int(query("MD"));
    if (now - t_set_ < 0.6 || dirty_)
        return;
    std::optional<Mode> name = md ? md_mode(*md) : std::nullopt;
    if (!(f && *f && *f != sent_freq && rx_freq == sent_freq))
        f.reset();
    if (!(name && name != sent_mode_ && sent_mode_ == mode_.load()))
        name.reset();
    if (f) {
        rx_freq = *f;
        tx_freq = *f;
        sent_freq = *f;
    }
    if (name) {
        mode_ = *name;
        sent_mode_ = name;
    }
    if (f || name) {
        need_lo_ = true;
        if (on_panel)
            on_panel(f ? std::optional<int64_t>(*f) : std::nullopt, name);
    }
}

void QmxRadio::read_power()
{
    if (auto v = to_int(query("PC")))
        fwd_power = double(*v) / 10.0;
    if (auto v = to_int(query("SW")))
        swr = double(*v) / 100.0;
}

}  // namespace rigface
