// Speaker and microphone through GStreamer (PipeWire / PulseAudio / ALSA).
#pragma once

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "radio.h"

typedef struct _GstElement GstElement;

namespace rigface {

constexpr int AUDIO_RATE = 48000;

/// A pipeline could not be built; what() is GStreamer's message.
struct AudioError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class AudioOut {
public:
    /// `sink`: a GStreamer element description; empty for the default output.
    explicit AudioOut(const std::string &sink = "");
    ~AudioOut();
    AudioOut(const AudioOut &) = delete;
    AudioOut &operator=(const AudioOut &) = delete;

    void write(const float *samples, size_t n);
    void close();

private:
    GstElement *pipe_ = nullptr;
    GstElement *src_ = nullptr;
};

/// Captures only while started, so the mic is closed during receive.
class MicIn {
public:
    using Callback = std::function<void(const float *samples, size_t n)>;
    explicit MicIn(Callback callback, const std::string &source = "");
    ~MicIn();
    MicIn(const MicIn &) = delete;
    MicIn &operator=(const MicIn &) = delete;

    void start();
    void stop();

private:
    Callback callback_;
    std::string source_;
    GstElement *pipe_ = nullptr;
};

/// Stereo capture from a radio whose sound card carries I (left) and Q (right).
class IQIn {
public:
    using Callback = std::function<void(std::vector<cfloat> &&iq)>;
    IQIn(const std::string &source, Callback callback);
    ~IQIn();
    IQIn(const IQIn &) = delete;
    IQIn &operator=(const IQIn &) = delete;

    /// Message of a pending pipeline error, or empty.
    std::string error();
    void close();

private:
    Callback callback_;
    GstElement *pipe_ = nullptr;
};

}  // namespace rigface
