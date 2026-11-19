// A pretend Radioberry: HPSDR Protocol-1 server with a synthetic HF band.
//
// Used by `rigface --demo`, and runnable on its own (`rigface-sim`) as a target
// for any Protocol-1 client. With `--qmx` it pretends to be a QRP Labs QMX
// instead: CAT on a pseudo-terminal and the I/Q "sound card" as float32 stereo
// over UDP.
//
// CW stations live in the CW segments of the ham bands, SSB "voices" in the
// phone segments, AM broadcasters in the shortwave broadcast bands. Everything
// is a pure function of absolute time and frequency, so tuning around behaves
// like a real band. While the client transmits, its TX I/Q is looped back into
// the receiver so the signal shows on the scope.
#pragma once

#include <complex>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace rigface::sim {

using cfloat = std::complex<float>;

enum class Kind { CW, SSB, AM };

struct Station {
    Kind kind;
    double freq;
    uint32_t seed;
};

/// Stations with a carrier between lo and hi Hz.
std::vector<Station> band_signals(double lo, double hi);

class Band {
public:
    int rate = 48000;
    int64_t rx_freq = 7100000;
    int gain = 20;
    int64_t n0 = 0;                        // samples generated so far

    std::vector<cfloat> noise(size_t n, bool antenna = true);
    std::vector<cfloat> generate(size_t n);

private:
    const std::vector<Station> &stations();
    std::vector<float> cw_env(uint32_t h, size_t n);

    std::mt19937 rng_{1};
    std::normal_distribution<float> normal_{0.0f, 1.0f};
    int key_rate_ = 0;
    int64_t key_freq_ = -1;
    std::vector<Station> sigs_;
    std::map<uint32_t, std::vector<float>> cw_;
};

/// Samples -> Protocol-1 receive payload (I24 Q24 mic16, big-endian).
std::vector<uint8_t> pack24(const std::vector<cfloat> &x);

/// Serve Protocol 1 until the process is killed, or until the process with
/// id `parent` (if not 0) is gone. Returns 1 if the port cannot be used.
int serve(int port = 1024, const std::string &host = "0.0.0.0", bool verbose = false,
          int parent = 0);

/// Simulated QMX. Prints the CAT pseudo-terminal's path, then runs.
int serve_qmx(int iq_port, int parent = 0);

}  // namespace rigface::sim
