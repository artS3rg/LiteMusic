#include "audio.hpp"
#include "desktop.hpp"
#include "discord_presence.hpp"
#include "music.hpp"
#include "play_order.hpp"
#include "portable_media.hpp"
#include "settings.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Value_Slider.H>
#include <FL/fl_ask.H>
#include <FL/filename.H>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace {
using namespace std::chrono_literals;
constexpr double ticks_per_second = 10'000'000.0;
constexpr int width = 580, height = 510, mini_height = 158;
const auto base = fl_rgb_color(19, 18, 26);
const auto panel = fl_rgb_color(35, 32, 48);
const auto ink = fl_rgb_color(244, 239, 254);
const auto dim = fl_rgb_color(174, 163, 199);
const auto accent = fl_rgb_color(112, 78, 191);

std::string clock_text(double seconds) {
    int s = static_cast<int>(seconds);
    char text[24];
    std::snprintf(text, sizeof(text), "%d:%02d", s / 60, s % 60);
    return text;
}

std::string row(std::wstring_view value) {
    auto text = narrow(value);
    // FLTK browsers interpret @ at the start and tabs as columns.
    for (auto& c : text) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return text;
}

using FetchPage = std::function<TracksPage(const Music&, size_t)>;
using FetchCollection = std::function<CollectionPages(const Music&, const std::function<bool()>&)>;

class TrackBrowser : public Fl_Hold_Browser {
    using Fl_Hold_Browser::Fl_Hold_Browser;
    int handle(int event) override {
        if (event == FL_KEYDOWN && (Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter)) {
            do_callback();
            return 1;
        }
        return Fl_Hold_Browser::handle(event);
    }
};

struct List {
    std::vector<Track> tracks;
    FetchPage fetch;
    FetchCollection collection;
    size_t page = 0;
    bool more = false;
    unsigned id = 0;
};

void extend(List& list, TracksPage page) {
    ++list.page;
    list.more = page.more;
    for (auto& track : page.tracks) list.tracks.push_back(std::move(track));
}

struct Queued {
    Track track;
    std::string batch;
};

void feedback(const Music& music, const char* type, const Queued& item, double seconds = 0) {
    if (item.batch.empty()) return;
    try { music.wave_event(type, item.batch, item.track.id, seconds); } catch (const std::exception&) {}
}

class App {
public:
    explicit App(std::string local, bool silent_test = false) : player_(silent_test), local_(std::move(local)) {
        if (local_.empty()) {
            discord_application_ = load_discord_id();
            if (!valid_discord_id(discord_application_)) discord_application_.clear();
            discord_.configure(discord_application_);
        }
        Fl::scheme("gtk+");
        window_.begin();
        window_.color(base);
        window_.xclass("LiteMusic");
        window_.callback([](Fl_Widget*, void* p) { static_cast<App*>(p)->window_.hide(); }, this);
        small_ = button(452, 12, 62, 28, "Мини", [this] { toggle_mini(); }, FL_COMMAND | 'm');
        pin_ = button(520, 12, 48, 28, "Пин", [this] {
            pinned_ = !pinned_;
            apply_pin();
        });
        pin_->hide();
        brand_ = label(12, 12, 430, 28, "LiteMusic", 20, ink);
        brand_->labelfont(FL_HELVETICA_BOLD);
        sign_ = button(485, 48, 83, 29, "Войти", [this] { sign(); });

        top_ = new Fl_Group(0, 44, width, height - mini_height - 44);
        account_ = label(12, 48, 381, 29, "Яндекс Музыка", 13, dim);
        discord_button_ = button(401, 48, 77, 29, "Discord", [this] { discord_options(); });
        discord_button_->tooltip("Показать трек в статусе Discord");
        update_discord_button();
        favorites_ = button(12, 85, 118, 30, "Любимые", [this] {
            auto uid = profile_.uid;
            load_list([uid](const Music& m, size_t page) { return m.favorites(uid, page); },
                      [uid](const Music& m, const auto& canceled) { return m.favorite_pages(uid, canceled); });
        });
        playlists_ = button(137, 85, 123, 30, "Плейлисты", [this] { show_playlists(); });
        wave_button_ = button(267, 85, 127, 30, "Моя волна", [this] { start_wave(); });
        shuffle_ = button(401, 85, 76, 30, "Случайно", [this] {
            shuffle_enabled_ = !shuffle_enabled_;
            order_.reset();
            refresh_controls();
        });
        repeat_button_ = button(484, 85, 84, 30, "Повтор", [this] {
            repeat_ = next_repeat(repeat_);
            refresh_controls();
        });
        shuffle_->tooltip("Перемешивать любимые треки и плейлисты");
        repeat_button_->tooltip("Повтор списка, одного трека или выключен");
        search_ = new Fl_Input(12, 123, 454, 29);
        search_->color(panel);
        search_->textcolor(ink);
        search_->cursor_color(ink);
        search_->textsize(14);
        search_->when(FL_WHEN_ENTER_KEY_ALWAYS);
        search_->callback([](Fl_Widget*, void* p) { static_cast<App*>(p)->search(); }, this);
        search_button_ = button(473, 123, 95, 29, "Поиск", [this] { search(); });
        browser_ = new TrackBrowser(12, 160, 556, 149);
        browser_->format_char(0);
        browser_->when(FL_WHEN_RELEASE_ALWAYS);
        browser_->color(panel);
        browser_->textcolor(ink);
        browser_->selection_color(accent);
        browser_->textsize(14);
        browser_->callback([](Fl_Widget*, void* p) {
            auto& self = *static_cast<App*>(p);
            if (Fl::event_clicks() || Fl::event_key() == FL_Enter || Fl::event_key() == FL_KP_Enter) self.open_row();
        }, this);
        more_ = button(470, 316, 98, 27, "Ещё", [this] { more_list(); });
        result_ = label(12, 316, 455, 27, "", 13, dim);
        top_->end();

        deck_ = new Fl_Group(0, height - mini_height, width, mini_height);
        track_ = label(12, 357, 556, 28, "Выберите трек", 18, ink);
        track_->labelfont(FL_HELVETICA_BOLD);
        status_ = label(12, 386, 556, 23, "", 13, dim);
        previous_ = button(12, 460, 87, 33, "Назад", [this] { previous(); });
        play_ = button(106, 460, 89, 33, "Играть", [this] { toggle_play(); });
        play_->color(accent);
        next_ = button(202, 460, 88, 33, "Далее", [this] { advance(false); });
        like_ = button(297, 460, 73, 33, "♡ Лайк", [this] { like(); });
        elapsed_ = label(12, 422, 42, 21, "0:00", 12, dim);
        duration_ = label(522, 422, 46, 21, "0:00", 12, dim);
        seek_ = slider(59, 421, 456, 23);
        seek_->bounds(0, 1);
        seek_->when(FL_WHEN_RELEASE);
        seek_->callback([](Fl_Widget*, void* p) {
            auto& self = *static_cast<App*>(p);
            self.seek_to(self.seek_->value());
        }, this);
        volume_ = slider(442, 465, 126, 22);
        volume_->bounds(0, 100);
        volume_->step(1);
        volume_->value(local_.empty() ? load_volume() : 100);
        volume_label_ = label(379, 461, 60, 30, "Громк.", 12, dim);
        volume_->when(FL_WHEN_CHANGED | FL_WHEN_RELEASE);
        volume_->callback([](Fl_Widget*, void* p) {
            auto& self = *static_cast<App*>(p);
            self.set_volume();
            self.update_media_keys();
            if (self.local_.empty() && Fl::event() == FL_RELEASE) save_volume(static_cast<int>(self.volume_->value()));
        }, this);
        deck_->end();
        window_.end();
        set_volume();
        refresh_controls();
    }

    ~App() {
        media_.reset();
        canceled_ = true;
        Fl::remove_timeout(timer, this);
        if (worker_.joinable()) worker_.join();
        if (active_) save_position(snap_.position);
        if (local_.empty()) save_volume(static_cast<int>(volume_->value()));
    }

    int run() {
        window_.show();
        if (!local_.empty()) {
            current_.name = L"Локальный аудиофайл";
            update_track();
            player_.open(local_);
            began();
        } else {
            launch([this] {
                auto token = saved_token();
                if (token.empty()) return Done([] {});
                auto profile = Music(token).profile();
                auto saved = resume();
                return Done([this, token, profile, saved] { signed_in(token, profile, saved, true); });
            });
        }
        return Fl::run();
    }

private:
    using Done = std::function<void()>;
    Player player_;
    DiscordPresence discord_;
    std::string discord_application_;
    std::unique_ptr<MediaKeys> media_;
    std::string local_;
    Fl_Double_Window window_{width, height, "LiteMusic"};
    Fl_Group *top_{}, *deck_{};
    Fl_Box *brand_{}, *account_{}, *result_{}, *track_{}, *status_{}, *elapsed_{}, *duration_{}, *volume_label_{};
    Fl_Button *small_{}, *pin_{}, *sign_{}, *favorites_{}, *playlists_{}, *wave_button_{}, *search_button_{}, *more_{},
        *previous_{}, *play_{}, *next_{}, *like_{}, *shuffle_{}, *repeat_button_{}, *discord_button_{};
    Fl_Input* search_{};
    Fl_Hold_Browser* browser_{};
    Fl_Value_Slider *seek_{}, *volume_{};
    std::deque<Done> callbacks_;
    std::deque<Done> clicks_;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> canceled_{};
    bool busy_ = false, signing_ = false, mini_ = false, pinned_ = false;
    bool stored_ = false;
    Profile profile_;
    std::string token_;
    std::optional<List> visible_, queue_;
    std::shared_ptr<ShuffleOrder> order_;
    bool shuffle_enabled_ = false;
    Repeat repeat_ = Repeat::off;
    std::vector<Playlist> lists_;
    unsigned list_id_ = 0;
    size_t index_ = 0, wave_index_ = 0;
    std::vector<Queued> wave_;
    Track current_;
    std::vector<std::string> likes_;
    std::optional<ResumeTrack> pending_;
    Playback snap_;
    double last_save_ = 0;
    bool active_ = false, radio_ = false, desired_ = false, deferred_end_ = false;
    bool started_sent_ = false;
    bool media_engaged_ = false;

    Fl_Button* button(int x, int y, int w, int h, const char* title, Done action, int shortcut = 0) {
        auto* b = new Fl_Button(x, y, w, h, title);
        b->box(FL_FLAT_BOX);
        b->color(panel);
        b->selection_color(accent);
        b->labelcolor(ink);
        b->labelsize(13);
        clicks_.push_back(std::move(action));
        b->callback([](Fl_Widget*, void* fn) { (*static_cast<Done*>(fn))(); }, &clicks_.back());
        if (shortcut) b->shortcut(shortcut);
        return b;
    }

    Fl_Box* label(int x, int y, int w, int h, const char* value, int size, Fl_Color color) {
        auto* box = new Fl_Box(x, y, w, h, value);
        box->align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_CLIP);
        box->labelsize(size);
        box->labelcolor(color);
        return box;
    }

    Fl_Value_Slider* slider(int x, int y, int w, int h) {
        auto* range = new Fl_Value_Slider(x, y, w, h);
        range->type(FL_HORIZONTAL);
        range->color(panel);
        range->selection_color(accent);
        range->textcolor(ink);
        range->textsize(12);
        return range;
    }

    static void enabled(Fl_Widget* widget, bool enable) {
        if (enable) widget->activate();
        else widget->deactivate();
    }

    void refresh_controls() {
        bool connected = !token_.empty();
        sign_->copy_label(signing_ ? "Отмена" : connected ? "Выйти" : "Войти");
        enabled(sign_, local_.empty() && (!busy_ || signing_));
        for (auto* b : {favorites_, playlists_, wave_button_, search_button_}) enabled(b, connected && !busy_);
        enabled(search_, connected);
        enabled(browser_, connected && !busy_);
        enabled(more_, !busy_ && visible_ && visible_->more);
        enabled(play_, active_ || (pending_ && !busy_));
        enabled(previous_, active_ || (!busy_ && has_previous()));
        enabled(next_, !busy_ && can_next());
        bool modes = connected && !busy_ && ((queue_ && queue_->collection) || (visible_ && visible_->collection));
        enabled(shuffle_, modes);
        enabled(repeat_button_, modes);
        auto shuffle_color = shuffle_enabled_ ? accent : panel;
        if (shuffle_->color() != shuffle_color) {
            shuffle_->color(shuffle_color);
            shuffle_->redraw();
        }
        const char* repeat_label = repeat_ == Repeat::track ? "Повтор 1" :
                                   repeat_ == Repeat::collection ? "Повтор ∞" : "Повтор";
        if (std::string_view(repeat_button_->label()) != repeat_label) {
            repeat_button_->color(repeat_ == Repeat::off ? panel : accent);
            repeat_button_->copy_label(repeat_label);
        }
        enabled(like_, connected && !busy_ && !current_.id.empty());
        enabled(seek_, active_ && snap_.duration > 0);
        play_->copy_label(desired_ ? "Пауза" : "Играть");
        like_->copy_label(liked(current_.id) ? "♥ Лайк" : "♡ Лайк");
        update_media_keys();
        update_discord();
    }

    void discord_options() {
        Fl_Double_Window dialog(412, 160, "Статус Discord");
        dialog.set_modal();
        Fl_Box tip(12, 8, 388, 45,
                   "Создайте приложение LiteMusic в Discord Developer Portal\nи вставьте его Application ID.");
        tip.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
        Fl_Input id(12, 56, 388, 29);
        id.maximum_size(32);
        id.value(discord_application_.c_str());
        Fl_Box hint(12, 90, 388, 20, "Пустое поле отключает трансляцию статуса.");
        hint.align(FL_ALIGN_INSIDE | FL_ALIGN_LEFT);
        Fl_Button portal(12, 121, 137, 27, "Открыть портал");
        portal.callback([](Fl_Widget*, void*) {
            if (!fl_open_uri("https://discord.com/developers/applications")) {
                fl_alert("%s", "Откройте discord.com/developers/applications в браузере.");
            }
        });
        bool saved = false;
        Fl_Button save(218, 121, 88, 27, "Сохранить");
        save.shortcut(FL_Enter);
        save.callback([](Fl_Widget* widget, void* data) {
            *static_cast<bool*>(data) = true;
            widget->window()->hide();
        }, &saved);
        Fl_Button cancel(312, 121, 88, 27, "Отмена");
        cancel.shortcut(FL_Escape);
        cancel.callback([](Fl_Widget* widget, void*) { widget->window()->hide(); });
        dialog.end();
        dialog.position(window_.x() + (window_.w() - dialog.w()) / 2, window_.y() + 35);
        dialog.show();
        id.take_focus();
        while (dialog.shown() && window_.shown()) Fl::wait();
        if (!saved) return;
        std::string input = id.value();
        auto first = input.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) input.clear();
        else input = input.substr(first, input.find_last_not_of(" \t\r\n") - first + 1);
        if (!input.empty() && !valid_discord_id(input)) {
            fl_alert("%s", "Скопируйте числовой Application ID из Discord Developer Portal.");
            return;
        }
        discord_application_ = std::move(input);
        save_discord_id(discord_application_);
        discord_.configure(discord_application_);
        update_discord_button();
        update_discord();
    }

    void update_discord_button() {
        discord_button_->color(discord_application_.empty() ? panel : accent);
        discord_button_->copy_label(discord_application_.empty() ? "Discord" : "Discord ✓");
    }

    void update_discord(bool seek = false) {
        if (discord_application_.empty()) return;
        if (desired_ && snap_.state == Playback::playing && !current_.id.empty()) {
            discord_.playing(current_, snap_.position, snap_.duration, seek);
        } else {
            discord_.clear();
        }
    }

    void update_media_keys() {
        if (!media_) {
            if (!media_engaged_ && !pending_) return;
            media_ = std::make_unique<MediaKeys>([this](MediaAction action, double value) {
                post([this, action, value] { media_command(action, value); });
            });
        }
        using Status = MediaState::Status;
        MediaState state;
        state.status = active_ ? (desired_ ? Status::playing : Status::paused) : Status::stopped;
        state.can_play = active_ || (pending_ && !busy_);
        state.can_pause = active_;
        state.can_next = !busy_ && can_next();
        state.can_previous = active_ || (!busy_ && has_previous());
        state.can_seek = active_ && snap_.duration > 0;
        if (media_engaged_) {
            state.track_id = current_.id.empty() ? "local" : current_.id;
            state.title = narrow(current_.name);
            state.artist = narrow(current_.artists);
            state.duration = snap_.duration;
            state.position = snap_.position;
        }
        state.volume = volume_->value() / 100;
        media_->update(std::move(state));
    }

    void media_command(MediaAction action, double value) {
        switch (action) {
        case MediaAction::play: if (!desired_) toggle_play(); break;
        case MediaAction::pause: if (desired_) toggle_play(); break;
        case MediaAction::toggle: toggle_play(); break;
        case MediaAction::next: if (!busy_ && can_next()) advance(false, !desired_); break;
        case MediaAction::previous: if (active_ || (!busy_ && has_previous())) previous(!desired_); break;
        case MediaAction::stop:
            if (desired_) toggle_play();
            seek_to(0);
            break;
        case MediaAction::raise: window_.show(); break;
        case MediaAction::volume:
            volume_->value(std::round(value * 100));
            set_volume();
            if (local_.empty()) save_volume(static_cast<int>(volume_->value()));
            update_media_keys();
            break;
        case MediaAction::seek: seek_to(value); break;
        }
    }

    void toggle_mini() {
        mini_ = !mini_;
        if (mini_) {
            top_->hide();
            brand_->hide();
            sign_->hide();
            pin_->show();
        } else {
            top_->show();
            brand_->show();
            sign_->show();
            pin_->hide();
        }
        small_->resize(452, mini_ ? 5 : 12, 62, 28);
        pin_->resize(520, mini_ ? 5 : 12, 48, 28);
        // Keep the same controls and keyboard focus when switching modes.
        deck_->position(0, mini_ ? 31 : height - mini_height);
        window_.size(width, mini_ ? mini_height + 31 : height);
        small_->copy_label(mini_ ? "Окно" : "Мини");
        apply_pin();
        window_.redraw();
    }

    void apply_pin() {
        pin_->copy_label(pinned_ ? "Снять" : "Пин");
        pin_window(window_, mini_ && pinned_);
    }

    void post(Done callback) {
        {
            std::lock_guard guard(mutex_);
            callbacks_.push_back(std::move(callback));
        }
        Fl::awake([](void* data) { static_cast<App*>(data)->drain(); }, this);
    }

    void drain() {
        std::deque<Done> work;
        {
            std::lock_guard guard(mutex_);
            work.swap(callbacks_);
        }
        for (auto& callback : work) callback();
    }

    void message(const std::string& text) {
        status_->copy_label(text.c_str());
    }

    template<class Work>
    void launch(Work work) {
        if (busy_) return;
        busy_ = true;
        canceled_ = false;
        refresh_controls();
        worker_ = std::thread([this, work = std::move(work)]() mutable {
            Done done;
            std::string error;
            try { done = work(); }
            catch (const std::exception& e) { error = e.what(); }
            post([this, done = std::move(done), error = std::move(error)] {
                worker_.join();
                busy_ = false;
                signing_ = false;
                if (canceled_) message("Вход отменён");
                else if (!error.empty()) message(error);
                else if (done) {
                    try { done(); } catch (const std::exception& e) { message(e.what()); }
                }
                refresh_controls();
                if (deferred_end_ && !busy_) {
                    deferred_end_ = false;
                    advance(true);
                }
            });
        });
    }

    static std::optional<ResumeTrack> resume() {
        try { return load_resume_track(); }
        catch (const std::exception&) { return {}; }
    }

    void sign() {
        if (signing_) {
            canceled_ = true;
            return;
        }
        if (!token_.empty()) {
            message("Выходим…");
            launch([this, stored = stored_] {
                if (stored) forget_token();
                return Done([this] {
                    stop();
                    pending_.reset();
                    current_ = {};
                    queue_.reset();
                    order_.reset();
                    visible_.reset();
                    radio_ = stored_ = false;
                    media_engaged_ = false;
                    likes_.clear();
                    browser_->clear();
                    token_.clear();
                    profile_ = {};
                    erase_resume_track();
                    update_track();
                    account_->copy_label("Яндекс Музыка");
                    result_->copy_label("");
                    message("Вы вышли из аккаунта");
                });
            });
            return;
        }
        signing_ = true;
        message("Получаем код входа…");
        launch([this] {
            auto token = Music::sign_in([this] { return canceled_.load(); }, [this](DeviceCode device) {
                post([this, device = std::move(device)] {
                    if (canceled_) return;
                    auto code = narrow(device.code);
                    auto url = narrow(device.url);
                    Fl::copy(code.c_str(), static_cast<int>(code.size()), 1);
                    char error[256]{};
                    if (fl_open_uri(url.c_str(), error, sizeof(error))) message("Код входа: " + code + " (скопирован)");
                    else message("Откройте " + url + " · Код: " + code);
                });
            });
            if (canceled_ || token.empty()) return Done([] {});
            auto profile = Music(token).profile();
            if (canceled_) return Done([] {});
            bool stored = true;
            try { save_token(token); } catch (const std::exception&) { stored = false; }
            return Done([this, token, profile, stored, saved = resume()] {
                signed_in(token, profile, saved, stored);
                if (!stored) message("Вход до закрытия: хранилище паролей недоступно");
            });
        });
    }

    void signed_in(const std::string& token, const Profile& profile, const std::optional<ResumeTrack>& saved, bool stored) {
        token_ = token;
        profile_ = profile;
        stored_ = stored;
        account_->copy_label(narrow(profile.name).c_str());
        if (saved && saved->account == profile.uid) {
            pending_ = saved;
            current_ = pending_->track;
            update_track();
            elapsed_->copy_label(clock_text(saved->position / ticks_per_second).c_str());
        } else {
            message("Вход выполнен");
        }
        launch([this, music = Music(token_), uid = profile_.uid] {
            auto values = music.liked_tracks(uid);
            return Done([this, values = std::move(values)]() mutable { likes_ = std::move(values); });
        });
    }

    void search() {
        if (token_.empty() || busy_) return;
        auto query = widen(search_->value());
        if (!query.empty()) load_list([query](const Music& m, size_t page) { return m.search(query, page); });
    }

    void render_list() {
        browser_->clear();
        for (const auto& item : visible_->tracks) {
            browser_->add((row(item.name) + "  ·  " + row(item.artists)).c_str());
        }
        result_->copy_label(visible_->tracks.empty() ? "Нет треков" : "Двойной щелчок — воспроизвести");
    }

    void load_list(FetchPage fetch, FetchCollection collection = {}) {
        message("Загружаем треки…");
        launch([this, fetch = std::move(fetch), collection = std::move(collection), music = Music(token_)] {
            auto page = fetch(music, 0);
            return Done([this, fetch, collection, page = std::move(page)]() mutable {
                visible_ = List{{}, fetch, collection, 0, false, ++list_id_};
                extend(*visible_, std::move(page));
                render_list();
                update_track();
            });
        });
    }

    void more_list() {
        if (busy_ || !visible_ || !visible_->more) return;
        launch([this, list = *visible_, music = Music(token_)]() mutable {
            auto page = list.fetch(music, list.page);
            extend(list, std::move(page));
            return Done([this, list = std::move(list)]() mutable {
                if (queue_ && queue_->id == list.id && queue_->page < list.page) queue_ = list;
                visible_ = std::move(list);
                render_list();
            });
        });
    }

    void show_playlists() {
        message("Загружаем плейлисты…");
        launch([this, music = Music(token_), uid = profile_.uid] {
            auto items = music.playlists(uid);
            return Done([this, items = std::move(items)]() mutable {
                lists_ = std::move(items);
                visible_.reset();
                browser_->clear();
                for (const auto& item : lists_) browser_->add(row(item.name).c_str());
                result_->copy_label(lists_.empty() ? "Нет плейлистов" : "Двойной щелчок — открыть");
                update_track();
            });
        });
    }

    void open_row() {
        int selected = browser_->value() - 1;
        if (busy_ || selected < 0) return;
        auto index = static_cast<size_t>(selected);
        if (visible_) {
            if (index < visible_->tracks.size()) play_from(*visible_, index, true);
        } else if (index < lists_.size()) {
            auto list = lists_[index];
            load_list([list](const Music& m, size_t page) { return m.playlist(list, page); },
                      [list](const Music& m, const auto& canceled) { return m.playlist_pages(list, canceled); });
        }
    }

    bool liked(const std::string& id) const {
        return std::find(likes_.begin(), likes_.end(), id) != likes_.end();
    }

    void like() {
        if (busy_ || token_.empty() || current_.id.empty()) return;
        auto id = current_.id;
        bool add = !liked(id);
        launch([this, music = Music(token_), uid = profile_.uid, id, add] {
            music.set_track_like(uid, id, add);
            return Done([this, id, add] {
                if (add) likes_.push_back(id);
                else std::erase(likes_, id);
            });
        });
    }

    Queued current_wave() const {
        return radio_ && wave_index_ < wave_.size() ? wave_[wave_index_] : Queued{};
    }

    Done with_feedback(bool finished) {
        auto item = current_wave();
        auto seconds = snap_.position;
        if (!started_sent_ || item.batch.empty()) return {};
        return [music = Music(token_), item, finished, seconds] {
            feedback(music, finished ? "trackFinished" : "skip", item, seconds);
        };
    }

    void play_from(List list, size_t index, bool fresh = false, bool wrap = false, bool paused = false) {
        message("Открываем аудио…");
        auto event = with_feedback(false);
        launch([this, music = Music(token_), list = std::move(list), index, fresh, wrap, paused, event]() mutable {
            while (index >= list.tracks.size() && list.more) {
                if (canceled_) return Done{};
                auto page = list.fetch(music, list.page);
                extend(list, std::move(page));
            }
            if (index >= list.tracks.size() && wrap) index = 0;
            if (index >= list.tracks.size()) return Done([this] { if (queue_) queue_->more = false; update_track(); });
            auto stream = music.stream(list.tracks[index].id);
            if (event) event();
            return Done([this, list = std::move(list), index, stream, fresh, paused]() mutable {
                if (visible_ && visible_->id == list.id && visible_->page < list.page) {
                    auto selected = browser_->value();
                    visible_ = list;
                    render_list();
                    browser_->value(selected);
                }
                queue_ = std::move(list);
                if (fresh) order_.reset();
                radio_ = false;
                index_ = index;
                play_track(queue_->tracks[index_], stream, 0, paused);
            });
        });
    }

    bool collection_mode() const { return queue_ && queue_->collection; }

    bool can_next() const {
        if (radio_) return true;
        if (!queue_) return false;
        bool wrap = collection_mode() && repeat_ == Repeat::collection;
        if (order_) return order_->step(true, wrap).has_value();
        if (collection_mode() && shuffle_enabled_) return wrap || queue_->more || queue_->tracks.size() > 1;
        return wrap || index_ + 1 < queue_->tracks.size() || queue_->more;
    }

    bool has_previous() const {
        if (radio_) return wave_index_ > 0;
        if (!queue_) return false;
        if (collection_mode() && shuffle_enabled_) return order_ && order_->step(false, false).has_value();
        return index_ > 0;
    }

    void step_shuffle(bool forward, bool wrap, bool paused) {
        if (order_) {
            auto cursor = order_->step(forward, wrap);
            if (!cursor) return;
            auto index = order_->tracks[*cursor];
            launch([this, music = Music(token_), cursor = *cursor, index, paused, id = queue_->tracks[index].id] {
                auto audio = music.stream(id);
                return Done([this, cursor, index, audio, paused] {
                    order_->cursor = cursor;
                    index_ = index;
                    play_track(queue_->tracks[index_], audio, 0, paused);
                });
            });
            return;
        }
        auto list = *queue_;
        launch([this, music = Music(token_), list = std::move(list), current = index_, id = current_.id, wrap, paused]() mutable {
            if (list.more) {
                auto pages = list.collection(music, [this] { return canceled_.load(); });
                if (canceled_) return Done{};
                list.tracks.clear();
                list.page = 0;
                for (auto& page : pages) extend(list, std::move(page));
            }
            if (list.tracks.empty()) return Done([this] { message("Треки закончились"); });
            std::optional<size_t> selected;
            if (current < list.tracks.size() && list.tracks[current].id == id) selected = current;
            else {
                auto found = std::find_if(list.tracks.begin(), list.tracks.end(), [&](const Track& track) { return track.id == id; });
                if (found != list.tracks.end()) selected = size_t(found - list.tracks.begin());
            }
            auto order = std::make_shared<ShuffleOrder>(list.tracks.size(), selected);
            auto cursor = selected ? order->step(true, wrap) : std::optional<size_t>(0);
            if (!cursor) return Done([this, order, list = std::move(list), selected]() mutable {
                queue_ = std::move(list);
                order_ = order;
                index_ = *selected;
                message("Треки закончились");
            });
            order->cursor = *cursor;
            auto index = order->tracks[*cursor];
            auto audio = music.stream(list.tracks[index].id);
            return Done([this, list = std::move(list), order, index, audio, paused]() mutable {
                queue_ = std::move(list);
                order_ = order;
                index_ = index;
                play_track(queue_->tracks[index_], audio, 0, paused);
            });
        });
    }

    void start_wave() { play_wave({}, 0, true, false); }

    void play_wave(std::vector<Queued> list, size_t index, bool first, bool finished, bool paused = false) {
        message(first ? "Запускаем Мою волну…" : "Открываем аудио…");
        auto event = with_feedback(finished);
        launch([this, music = Music(token_), list = std::move(list), index, first, event, paused]() mutable {
            if (event) event();
            if (index >= list.size()) {
                auto batch = music.wave(list.empty() ? std::string{} : list.back().track.id);
                if (first) feedback(music, "radioStarted", Queued{{}, batch.batch});
                for (auto& item : batch.tracks) list.push_back({std::move(item), batch.batch});
            }
            auto stream = music.stream(list.at(index).track.id);
            return Done([this, list = std::move(list), stream, index, paused]() mutable {
                auto trim = index < 32 ? 0 : index - 31;
                list.erase(list.begin(), list.begin() + trim);
                wave_ = std::move(list);
                wave_index_ = index - trim;
                radio_ = true;
                queue_.reset();
                order_.reset();
                play_track(wave_[wave_index_].track, stream, 0, paused);
            });
        });
    }

    void play_track(const Track& track, const Audio& audio, double at = 0, bool paused = false) {
        stop();
        pending_.reset();
        current_ = track;
        update_track();
        player_.open(narrow(audio.url), at);
        if (paused) player_.play(false);
        began(paused);
        save_position(at);
    }

    void began(bool paused = false) {
        active_ = true;
        desired_ = !paused;
        media_engaged_ = true;
        snap_ = {Playback::loading};
        started_sent_ = false;
        last_save_ = 0;
        refresh_controls();
        Fl::add_timeout(0.20, timer, this);
    }

    void stop() {
        discord_.clear();
        Fl::remove_timeout(timer, this);
        active_ = desired_ = deferred_end_ = false;
        snap_ = {};
        player_.clear();
        update_times();
    }

    void update_track() {
        track_->copy_label(current_.name.empty() ? "Выберите трек" : narrow(current_.name).c_str());
        message(narrow(current_.artists));
    }

    void toggle_play() {
        if (active_) {
            desired_ = !desired_;
            player_.play(desired_);
            if (!desired_) save_position(snap_.position);
            refresh_controls();
            return;
        }
        if (!pending_ || busy_) return;
        auto saved = *pending_;
        launch([this, music = Music(token_), saved] {
            auto stream = music.stream(saved.track.id);
            return Done([this, stream, saved] { play_track(saved.track, stream, saved.position / ticks_per_second); });
        });
    }

    void previous(bool paused = false) {
        if (active_ && (snap_.position > 3 || busy_ || !has_previous())) {
            seek_to(0);
            return;
        }
        if (busy_) return;
        if (radio_ && wave_index_ > 0) play_wave(wave_, wave_index_ - 1, false, false, paused);
        else if (collection_mode() && shuffle_enabled_ && order_) step_shuffle(false, false, paused);
        else if (queue_ && has_previous()) play_from(*queue_, index_ - 1, false, false, paused);
    }

    void advance(bool finished, bool paused = false) {
        if (busy_) {
            if (finished) deferred_end_ = true;
            return;
        }
        bool wrap = collection_mode() && repeat_ == Repeat::collection;
        if (radio_) play_wave(wave_, wave_index_ + 1, false, finished, paused);
        else if (finished && collection_mode() && repeat_ == Repeat::track) play_from(*queue_, index_);
        else if (can_next()) {
            if (collection_mode() && shuffle_enabled_) step_shuffle(true, wrap, paused);
            else play_from(*queue_, index_ + 1, false, wrap, paused);
        } else if (finished && !current_.id.empty()) {
            save_position(0);
            pending_ = ResumeTrack{profile_.uid, current_, 0};
        }
    }

    void set_volume() {
        auto number = volume_->value();
        player_.volume(number / 1600.0);
        volume_label_->copy_label((std::to_string(static_cast<int>(number)) + "%").c_str());
    }

    void seek_to(double seconds) {
        if (!active_ || snap_.duration <= 0) return;
        seconds = std::clamp(seconds, 0.0, snap_.duration);
        player_.seek(seconds);
        snap_.position = last_save_ = seconds;
        save_position(seconds);
        update_times();
        update_media_keys();
        update_discord(true);
        if (media_) media_->seeked(seconds);
    }

    void save_position(double position) {
        if (token_.empty() || current_.id.empty()) return;
        save_resume_track({profile_.uid, current_, static_cast<std::int64_t>(std::max(0.0, position) * ticks_per_second)});
    }

    void update_times() {
        elapsed_->copy_label(clock_text(snap_.position).c_str());
        duration_->copy_label(clock_text(snap_.duration).c_str());
        if (Fl::pushed() != seek_) {
            seek_->bounds(0, std::max(1.0, snap_.duration));
            seek_->value(snap_.position);
        }
    }

    static void timer(void* data) { static_cast<App*>(data)->tick(); }

    void tick() {
        auto state = player_.poll();
        if (state.state == Playback::failed || state.state == Playback::ended) {
            bool end = state.state == Playback::ended;
            active_ = desired_ = false;
            if (!end) message("Не удалось воспроизвести аудио");
            if (end) advance(true);
            refresh_controls();
            return;
        }
        snap_ = state;
        if (!started_sent_ && state.state == Playback::playing && radio_ && !busy_) {
            started_sent_ = true;
            auto item = current_wave();
            launch([music = Music(token_), item] {
                feedback(music, "trackStarted", item);
                return Done([] {});
            });
        }
        update_times();
        refresh_controls();
        if (std::abs(last_save_ - snap_.position) > 5) {
            save_position(snap_.position);
            last_save_ = snap_.position;
        }
        if (active_) Fl::repeat_timeout(desired_ ? 0.25 : 0.6, timer, this);
    }
};

int verify(const std::string& url) {
    Player player(true);
    player.volume(0);
    player.open(url);
    auto deadline = std::chrono::steady_clock::now() + 12s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto status = player.poll();
        if (status.state == Playback::failed || status.state == Playback::ended) return 1;
        if (status.state == Playback::playing && status.position >= 0.5 && status.duration > 0) return 0;
        Fl::wait(0.03);
    }
    return 1;
}
}

int main(int argc, char** argv) {
    try {
        Fl::lock();
        if (argc == 3 && std::string_view(argv[1]) == "--verify-audio") return verify(argv[2]);
        auto mode = argc == 3 ? std::string_view(argv[1]) : std::string_view{};
        auto local = mode == "--audio-test" || mode == "--verify-media-keys" ? argv[2] : "";
        App app(local, mode == "--verify-media-keys");
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
