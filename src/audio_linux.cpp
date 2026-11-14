#include "audio.hpp"

#include <gst/gst.h>
#include <stdexcept>

struct Player::Impl {
    GstElement* element = nullptr;
    GstBus* bus = nullptr;
    Playback::State state = Playback::empty;
    bool desired = true;
    bool buffering = false;
    double start = 0;

    ~Impl() {
        if (element) gst_element_set_state(element, GST_STATE_NULL);
        if (bus) gst_object_unref(bus);
        if (element) gst_object_unref(element);
    }
};

Player::Player(bool silent_test) : impl_(std::make_unique<Impl>()) {
    if (!gst_init_check(nullptr, nullptr, nullptr)) throw std::runtime_error("Не удалось запустить GStreamer");
    impl_->element = gst_element_factory_make("playbin", nullptr);
    if (!impl_->element) throw std::runtime_error("Установите gstreamer1.0-plugins-base");
    impl_->bus = gst_element_get_bus(impl_->element);
    if (silent_test) {
        auto* sink = gst_element_factory_make("fakesink", nullptr);
        if (!sink) throw std::runtime_error("Не удалось запустить проверку аудио");
        gst_object_ref_sink(sink);
        g_object_set(sink, "sync", TRUE, nullptr);
        g_object_set(impl_->element, "audio-sink", sink, nullptr);
        gst_object_unref(sink);
    }
}

Player::~Player() = default;

void Player::open(const std::string& url, double position) {
    clear();
    impl_->start = position;
    impl_->desired = true;
    impl_->state = Playback::loading;
    g_object_set(impl_->element, "uri", url.c_str(), nullptr);
    play(true);
}

void Player::clear() {
    gst_element_set_state(impl_->element, GST_STATE_NULL);
    gst_bus_set_flushing(impl_->bus, TRUE);
    gst_bus_set_flushing(impl_->bus, FALSE);
    impl_->state = Playback::empty;
    impl_->buffering = false;
    impl_->start = 0;
}

void Player::play(bool enabled) {
    impl_->desired = enabled;
    if (gst_element_set_state(impl_->element, enabled && !impl_->buffering ? GST_STATE_PLAYING : GST_STATE_PAUSED)
            == GST_STATE_CHANGE_FAILURE) {
        impl_->state = Playback::failed;
    }
}

void Player::seek(double seconds) {
    gst_element_seek_simple(impl_->element, GST_FORMAT_TIME,
        static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
        static_cast<gint64>(seconds * GST_SECOND));
}

void Player::volume(double level) {
    g_object_set(impl_->element, "volume", level, nullptr);
}

Playback Player::poll() {
    while (auto* message = gst_bus_pop(impl_->bus)) {
        switch (GST_MESSAGE_TYPE(message)) {
        case GST_MESSAGE_ERROR: impl_->state = Playback::failed; break;
        case GST_MESSAGE_EOS: impl_->state = Playback::ended; break;
        case GST_MESSAGE_BUFFERING: {
            gint percentage = 100;
            gst_message_parse_buffering(message, &percentage);
            if (impl_->buffering != (percentage < 100)) {
                impl_->buffering = percentage < 100;
                play(impl_->desired);
            }
            break;
        }
        case GST_MESSAGE_ASYNC_DONE:
            if (GST_MESSAGE_SRC(message) == GST_OBJECT(impl_->element) && impl_->start > 0) {
                seek(impl_->start);
                impl_->start = 0;
            }
            break;
        default: break;
        }
        gst_message_unref(message);
    }
    if (impl_->state == Playback::empty || impl_->state == Playback::failed || impl_->state == Playback::ended) {
        return {impl_->state};
    }
    GstState current = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
    gst_element_get_state(impl_->element, &current, &pending, 0);
    gint64 position = 0, duration = 0;
    gst_element_query_position(impl_->element, GST_FORMAT_TIME, &position);
    gst_element_query_duration(impl_->element, GST_FORMAT_TIME, &duration);
    return {current == GST_STATE_PLAYING ? Playback::playing :
            (current == GST_STATE_PAUSED && !impl_->desired ? Playback::paused : Playback::loading),
            position > 0 ? double(position) / GST_SECOND : 0,
            duration > 0 ? double(duration) / GST_SECOND : 0};
}
