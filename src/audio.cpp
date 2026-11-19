#include "audio.h"

#include <algorithm>
#include <mutex>

#include <gst/gst.h>

namespace rigface {

namespace {

void init()
{
    static std::once_flag once;
    std::call_once(once, [] { gst_init(nullptr, nullptr); });
}

std::string default_sink()
{
    if (GstElementFactory *f = gst_element_factory_find("pulsesink")) {
        gst_object_unref(f);
        return "pulsesink buffer-time=80000 latency-time=20000";
    }
    return "autoaudiosink";
}

GstElement *launch(const std::string &desc)
{
    init();
    GError *err = nullptr;
    GstElement *pipe = gst_parse_launch(desc.c_str(), &err);
    if (err) {
        std::string msg = err->message;
        g_error_free(err);
        if (pipe)
            gst_object_unref(pipe);
        throw AudioError(msg);
    }
    if (!pipe)
        throw AudioError("could not build the audio pipeline");
    return pipe;
}

void shut(GstElement *&pipe)
{
    if (pipe) {
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(pipe);
        pipe = nullptr;
    }
}

/// Run `fn(data, bytes)` on the next sample of an appsink.
template <typename Fn>
void pull(GstElement *sink, Fn fn)
{
    GstSample *sample = nullptr;
    g_signal_emit_by_name(sink, "pull-sample", &sample);
    if (!sample)
        return;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (buf && gst_buffer_map(buf, &map, GST_MAP_READ)) {
        fn(map.data, map.size);
        gst_buffer_unmap(buf, &map);
    }
    gst_sample_unref(sample);
}

void on_new_sample(GstElement *pipe, GCallback handler, gpointer self)
{
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipe), "sink");
    g_signal_connect(sink, "new-sample", handler, self);
    gst_object_unref(sink);
}

}  // namespace

// ---- AudioOut ----------------------------------------------------------------

AudioOut::AudioOut(const std::string &sink)
{
    init();
    pipe_ = launch("appsrc name=src format=time is-live=true "
                   "caps=audio/x-raw,format=S16LE,rate=" + std::to_string(AUDIO_RATE)
                   + ",channels=1,layout=interleaved ! audioconvert ! audioresample ! "
                   + (sink.empty() ? default_sink() : sink));
    src_ = gst_bin_get_by_name(GST_BIN(pipe_), "src");
    gst_element_set_state(pipe_, GST_STATE_PLAYING);
}

AudioOut::~AudioOut()
{
    if (src_)
        gst_object_unref(src_);
    shut(pipe_);
}

void AudioOut::write(const float *samples, size_t n)
{
    // If the sound card runs slower than the radio, drop a block rather
    // than let latency build up.
    guint64 queued = 0;
    g_object_get(src_, "current-level-bytes", &queued, nullptr);
    if (queued > AUDIO_RATE * 2 / 8 || !n)
        return;
    GstBuffer *buf = gst_buffer_new_allocate(nullptr, n * sizeof(gint16), nullptr);
    GstMapInfo map;
    if (gst_buffer_map(buf, &map, GST_MAP_WRITE)) {
        gint16 *out = reinterpret_cast<gint16 *>(map.data);
        for (size_t i = 0; i < n; ++i) {
            float v = std::max(-1.0f, std::min(1.0f, samples[i]));
            out[i] = GINT16_TO_LE(static_cast<gint16>(v * 32767.0f));
        }
        gst_buffer_unmap(buf, &map);
        GstFlowReturn ret;
        g_signal_emit_by_name(src_, "push-buffer", buf, &ret);
    }
    gst_buffer_unref(buf);
}

void AudioOut::close()
{
    gst_element_set_state(pipe_, GST_STATE_NULL);
}

// ---- MicIn -------------------------------------------------------------------

MicIn::MicIn(Callback callback, const std::string &source)
    : callback_(std::move(callback)), source_(source.empty() ? "autoaudiosrc" : source)
{
}

MicIn::~MicIn()
{
    stop();
}

void MicIn::start()
{
    if (pipe_)
        return;
    pipe_ = launch(source_ + " ! audioconvert ! audioresample "
                   "! audio/x-raw,format=F32LE,rate=" + std::to_string(AUDIO_RATE)
                   + ",channels=1 ! appsink name=sink emit-signals=true sync=false "
                   "max-buffers=50 drop=true");
    auto handler = +[](GstElement *sink, gpointer self) -> GstFlowReturn {
        pull(sink, [self](const guint8 *data, gsize bytes) {
            static_cast<MicIn *>(self)->callback_(reinterpret_cast<const float *>(data),
                                                  bytes / sizeof(float));
        });
        return GST_FLOW_OK;
    };
    on_new_sample(pipe_, G_CALLBACK(handler), this);
    gst_element_set_state(pipe_, GST_STATE_PLAYING);
}

void MicIn::stop()
{
    shut(pipe_);
}

// ---- IQIn --------------------------------------------------------------------

IQIn::IQIn(const std::string &source, Callback callback) : callback_(std::move(callback))
{
    pipe_ = launch(source + " ! audioconvert ! audioresample "
                   "! audio/x-raw,format=F32LE,rate=" + std::to_string(AUDIO_RATE)
                   + ",channels=2,layout=interleaved "
                   "! appsink name=sink emit-signals=true sync=false "
                   "max-buffers=50 drop=true");
    auto handler = +[](GstElement *sink, gpointer self) -> GstFlowReturn {
        pull(sink, [self](const guint8 *data, gsize bytes) {
            const float *a = reinterpret_cast<const float *>(data);
            const size_t n = bytes / (2 * sizeof(float));
            std::vector<cfloat> iq(n);
            for (size_t i = 0; i < n; ++i)
                iq[i] = cfloat(a[2 * i], a[2 * i + 1]);
            static_cast<IQIn *>(self)->callback_(std::move(iq));
        });
        return GST_FLOW_OK;
    };
    on_new_sample(pipe_, G_CALLBACK(handler), this);
    gst_element_set_state(pipe_, GST_STATE_PLAYING);
}

IQIn::~IQIn()
{
    shut(pipe_);
}

std::string IQIn::error()
{
    if (!pipe_)
        return "";
    GstBus *bus = gst_element_get_bus(pipe_);
    GstMessage *msg = gst_bus_pop_filtered(
        bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    gst_object_unref(bus);
    if (!msg)
        return "";
    std::string text = "stream ended";
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = nullptr;
        gst_message_parse_error(msg, &err, nullptr);
        text = err ? err->message : "error";
        if (err)
            g_error_free(err);
    }
    gst_message_unref(msg);
    return text;
}

void IQIn::close()
{
    shut(pipe_);
}

}  // namespace rigface
