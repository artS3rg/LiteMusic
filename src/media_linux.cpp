#include "portable_media.hpp"
#include <gio/gio.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>

namespace {
constexpr char path[] = "/org/mpris/MediaPlayer2";
constexpr char service[] = "org.mpris.MediaPlayer2.LiteMusic";
constexpr char root[] = "org.mpris.MediaPlayer2";
constexpr char player[] = "org.mpris.MediaPlayer2.Player";
constexpr char interfaces[] = R"xml(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek"><arg name="Offset" type="x" direction="in"/></method>
    <method name="SetPosition">
      <arg name="TrackId" type="o" direction="in"/><arg name="Position" type="x" direction="in"/>
    </method>
    <method name="OpenUri"><arg name="Uri" type="s" direction="in"/></method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Position" type="x" access="read">
      <annotation name="org.freedesktop.DBus.Property.EmitsChangedSignal" value="false"/>
    </property>
    <property name="Rate" type="d" access="readwrite"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>
)xml";

gint64 microseconds(double seconds) { return static_cast<gint64>(seconds * 1'000'000); }

std::string track_path(std::string id) {
    if (id.empty()) return "/org/mpris/MediaPlayer2/TrackList/NoTrack";
    for (char& c : id) {
        if (!g_ascii_isalnum(c) && c != '_') c = '_';
    }
    return "/org/mpris/MediaPlayer2/Track/" + id;
}

GVariant* property(std::string_view name, const MediaState& state) {
    using Status = MediaState::Status;
    if (name == "PlaybackStatus") return g_variant_new_string(
        state.status == Status::playing ? "Playing" : state.status == Status::paused ? "Paused" : "Stopped");
    if (name == "Position") return g_variant_new_int64(microseconds(state.position));
    if (name == "Volume") return g_variant_new_double(state.volume);
    if (name == "Rate" || name == "MinimumRate" || name == "MaximumRate") return g_variant_new_double(1);
    if (name == "CanPlay") return g_variant_new_boolean(state.can_play);
    if (name == "CanPause") return g_variant_new_boolean(state.can_pause);
    if (name == "CanGoNext") return g_variant_new_boolean(state.can_next);
    if (name == "CanGoPrevious") return g_variant_new_boolean(state.can_previous);
    if (name == "CanSeek") return g_variant_new_boolean(state.can_seek);
    if (name == "CanControl") return g_variant_new_boolean(TRUE);
    if (name != "Metadata") return nullptr;

    GVariantBuilder map;
    g_variant_builder_init(&map, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&map, "{sv}", "mpris:trackid", g_variant_new_object_path(track_path(state.track_id).c_str()));
    if (!state.title.empty()) g_variant_builder_add(&map, "{sv}", "xesam:title", g_variant_new_string(state.title.c_str()));
    if (!state.artist.empty()) {
        const char* artists[] = {state.artist.c_str()};
        g_variant_builder_add(&map, "{sv}", "xesam:artist", g_variant_new_strv(artists, 1));
    }
    if (state.duration > 0) g_variant_builder_add(&map, "{sv}", "mpris:length", g_variant_new_int64(microseconds(state.duration)));
    return g_variant_builder_end(&map);
}
}

struct MediaKeys::Impl {
    Handler handler;
    std::mutex mutex;
    MediaState state;
    GMainContext* context = g_main_context_new();
    GMainLoop* loop = g_main_loop_new(context, FALSE);
    GCancellable* connecting = g_cancellable_new();
    GDBusConnection* bus = nullptr;
    std::thread worker;

    explicit Impl(Handler callback) : handler(std::move(callback)), worker([this] { run(); }) {}

    ~Impl() {
        g_cancellable_cancel(connecting);
        // Attach a source so an early shutdown also works before the loop has started.
        auto* stop = g_idle_source_new();
        g_source_set_callback(stop, [](gpointer data) -> gboolean {
            g_main_loop_quit(static_cast<GMainLoop*>(data));
            return G_SOURCE_REMOVE;
        }, loop, nullptr);
        g_source_attach(stop, context);
        g_source_unref(stop);
        worker.join();
        g_object_unref(connecting);
        g_main_loop_unref(loop);
        g_main_context_unref(context);
    }

    void run() {
        g_main_context_push_thread_default(context);
        auto* info = g_dbus_node_info_new_for_xml(interfaces, nullptr);
        auto* connection = g_bus_get_sync(G_BUS_TYPE_SESSION, connecting, nullptr);
        guint registered_root = 0, registered_player = 0, owner = 0;
        if (connection && info) {
            const GDBusInterfaceVTable vtable{call, read, write, {}};
            registered_root = g_dbus_connection_register_object(connection, path, info->interfaces[0], &vtable, this, nullptr, nullptr);
            registered_player = g_dbus_connection_register_object(connection, path, info->interfaces[1], &vtable, this, nullptr, nullptr);
            if (registered_root && registered_player) {
                std::lock_guard lock(mutex);
                bus = connection;
                owner = g_bus_own_name_on_connection(connection, service, G_BUS_NAME_OWNER_FLAGS_NONE,
                                                     nullptr, nullptr, nullptr, nullptr);
            }
        }
        g_main_loop_run(loop);
        {
            std::lock_guard lock(mutex);
            bus = nullptr;
        }
        if (owner) g_bus_unown_name(owner);
        if (registered_root) g_dbus_connection_unregister_object(connection, registered_root);
        if (registered_player) g_dbus_connection_unregister_object(connection, registered_player);
        if (connection) g_object_unref(connection);
        if (info) g_dbus_node_info_unref(info);
        g_main_context_pop_thread_default(context);
    }

    static GVariant* read(GDBusConnection*, const gchar*, const gchar*, const gchar* iface,
                          const gchar* key, GError**, gpointer data) {
        auto& self = *static_cast<Impl*>(data);
        std::string_view name(key);
        if (std::string_view(iface) == root) {
            if (name == "Identity") return g_variant_new_string("LiteMusic");
            if (name == "DesktopEntry") return g_variant_new_string("litemusic");
            if (name == "SupportedUriSchemes" || name == "SupportedMimeTypes") return g_variant_new_strv(nullptr, 0);
            return g_variant_new_boolean(name == "CanRaise");
        }
        std::lock_guard lock(self.mutex);
        return property(name, self.state);
    }

    static gboolean write(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* key,
                          GVariant* value, GError** error, gpointer data) {
        auto& self = *static_cast<Impl*>(data);
        double level = g_variant_get_double(value);
        if (std::isfinite(level)) {
            if (std::string_view(key) == "Volume") {
                self.handler(MediaAction::volume, std::clamp(level, 0.0, 1.0));
                return TRUE;
            }
            if (level == 1) return TRUE;
        }
        g_set_error_literal(error, G_DBUS_ERROR, G_DBUS_ERROR_INVALID_ARGS, "Unsupported value");
        return FALSE;
    }

    static void call(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method,
                     GVariant* args, GDBusMethodInvocation* reply, gpointer data) {
        auto& self = *static_cast<Impl*>(data);
        std::string_view name(method);
        if (name == "Quit" || name == "OpenUri") {
            g_dbus_method_invocation_return_error_literal(reply, G_DBUS_ERROR, G_DBUS_ERROR_NOT_SUPPORTED, "Unsupported command");
            return;
        }
        MediaState snapshot;
        {
            std::lock_guard lock(self.mutex);
            snapshot = self.state;
        }
        if (name == "Play" && snapshot.can_play) self.handler(MediaAction::play, 0);
        else if (name == "Pause" && snapshot.can_pause) self.handler(MediaAction::pause, 0);
        else if (name == "PlayPause" && (snapshot.can_play || snapshot.can_pause)) self.handler(MediaAction::toggle, 0);
        else if (name == "Next" && snapshot.can_next) self.handler(MediaAction::next, 0);
        else if (name == "Previous" && snapshot.can_previous) self.handler(MediaAction::previous, 0);
        else if (name == "Stop" && snapshot.can_pause) self.handler(MediaAction::stop, 0);
        else if (name == "Raise") self.handler(MediaAction::raise, 0);
        else if (name == "Seek" && snapshot.can_seek) {
            gint64 offset;
            g_variant_get(args, "(x)", &offset);
            auto target = snapshot.position + double(offset) / 1'000'000;
            if (target > snapshot.duration && snapshot.can_next) self.handler(MediaAction::next, 0);
            else self.handler(MediaAction::seek, std::clamp(target, 0.0, snapshot.duration));
        } else if (name == "SetPosition" && snapshot.can_seek) {
            const char* track;
            gint64 requested;
            g_variant_get(args, "(&ox)", &track, &requested);
            auto position = double(requested) / 1'000'000;
            if (track_path(snapshot.track_id) == track && position >= 0 && position <= snapshot.duration) {
                self.handler(MediaAction::seek, position);
            }
        }
        g_dbus_method_invocation_return_value(reply, nullptr);
    }
};

MediaKeys::MediaKeys(Handler handler) : impl_(std::make_unique<Impl>(std::move(handler))) {}
MediaKeys::~MediaKeys() = default;

void MediaKeys::update(MediaState state) {
    std::lock_guard lock(impl_->mutex);
    impl_->state.position = state.position;
    if (impl_->state == state) return;
    impl_->state = std::move(state);
    if (!impl_->bus) return;
    GVariantBuilder changed;
    g_variant_builder_init(&changed, G_VARIANT_TYPE_VARDICT);
    for (const char* key : {"PlaybackStatus", "Metadata", "CanPlay", "CanPause", "CanGoNext",
                            "CanGoPrevious", "CanSeek", "Volume"}) {
        g_variant_builder_add(&changed, "{sv}", key, property(key, impl_->state));
    }
    g_dbus_connection_emit_signal(impl_->bus, nullptr, path, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                                  g_variant_new("(sa{sv}as)", player, &changed, nullptr), nullptr);
}

void MediaKeys::seeked(double seconds) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->bus) {
        g_dbus_connection_emit_signal(impl_->bus, nullptr, path, player, "Seeked",
                                      g_variant_new("(x)", microseconds(seconds)), nullptr);
    }
}
