#include "audio.hpp"
#include "desktop.hpp"
#include "music.hpp"
#include "settings.hpp"

#include <FL/Fl.H>
#include <FL/Fl_Box.H>
#include <FL/Fl_Button.H>
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Double_Window.H>
#include <FL/Fl_Value_Slider.H>
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
struct List {
    std::vector<Track> tracks;
    FetchPage fetch;
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
    explicit App(std::string local) : local_(std::move(local)) {
        Fl::scheme("gtk+");
        window_.begin();
        window_.color(base);
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
        account_ = label(12, 48, 466, 29, "Яндекс Музыка", 13, dim);
        favorites_ = button(12, 85, 118, 30, "Любимые", [this] {
            load_list([uid = profile_.uid](const Music& m, size_t page) { return m.favorites(uid, page); });
        });
        playlists_ = button(137, 85, 123, 30, "Плейлисты", [this] { show_playlists(); });
        wave_button_ = button(267, 85, 127, 30, "Моя волна", [this] { start_wave(); });
        search_ = new Fl_Input(12, 123, 454, 29);
        search_->color(panel);
        search_->textcolor(ink);
        search_->cursor_color(ink);
        search_->textsize(14);
        search_->when(FL_WHEN_ENTER_KEY_ALWAYS);
        search_->callback([](Fl_Widget*, void* p) { static_cast<App*>(p)->search(); }, this);
        search_button_ = button(473, 123, 95, 29, "Поиск", [this] { search(); });
        browser_ = new Fl_Hold_Browser(12, 160, 556, 149);
        browser_->format_char(0);
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
            auto seconds = self.seek_->value();
            self.player_.seek(seconds);
            self.save_position(seconds);
            self.last_save_ = seconds;
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
            if (self.local_.empty() && Fl::event() == FL_RELEASE) save_volume(static_cast<int>(self.volume_->value()));
        }, this);
        deck_->end();
        window_.end();
        set_volume();
        refresh_controls();
    }

    ~App() {
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
                return Done([this, token, profile, saved] { signed_in(token, profile, saved); });
            });
        }
        return Fl::run();
    }

private:
    using Done = std::function<void()>;
    Player player_;
    std::string local_;
    Fl_Double_Window window_{width, height, "LiteMusic"};
    Fl_Group *top_{}, *deck_{};
    Fl_Box *brand_{}, *account_{}, *result_{}, *track_{}, *status_{}, *elapsed_{}, *duration_{}, *volume_label_{};
    Fl_Button *small_{}, *pin_{}, *sign_{}, *favorites_{}, *playlists_{}, *wave_button_{}, *search_button_{}, *more_{},
        *previous_{}, *play_{}, *next_{}, *like_{};
    Fl_Input* search_{};
    Fl_Hold_Browser* browser_{};
    std::deque<Done> callbacks_;
    std::deque<Done> clicks_;
    std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> canceled_{};
    bool busy_ = false, signing_ = false, mini_ = false, pinned_ = false;
    Profile profile_;
    std::string token_;
    std::optional<List> visible_, queue_;
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
        enabled(previous_, active_ || (!busy_ && (queue_ || radio_)));
        enabled(next_, !busy_ && ((queue_ && (index_ + 1 < queue_->tracks.size() || queue_->more)) || radio_));
        enabled(like_, connected && !busy_ && !current_.id.empty());
        enabled(seek_, active_ && snap_.duration > 0);
        play_->copy_label(desired_ ? "Пауза" : "Играть");
        like_->copy_label(liked(current_.id) ? "♥ Лайк" : "♡ Лайк");
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
            stop();
            pending_.reset();
            current_ = {};
            queue_.reset();
            visible_.reset();
            radio_ = false;
            likes_.clear();
            browser_->clear();
            update_track();
            message("Выходим…");
            launch([this] {
                forget_token();
                erase_resume_track();
                return Done([this] {
                    token_.clear();
                    profile_ = {};
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
                signed_in(token, profile, saved);
                if (!stored) message("Вход до закрытия: хранилище паролей недоступно");
            });
        });
    }

    void signed_in(const std::string& token, const Profile& profile, const std::optional<ResumeTrack>& saved) {
        token_ = token;
        profile_ = profile;
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

    void load_list(FetchPage fetch) {
        message("Загружаем треки…");
        launch([this, fetch = std::move(fetch), music = Music(token_)] {
            auto page = fetch(music, 0);
            return Done([this, fetch, page = std::move(page)]() mutable {
                visible_ = List{{}, fetch, 0, false, ++list_id_};
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
            if (index < visible_->tracks.size()) play_from(*visible_, index);
        } else if (index < lists_.size()) {
            load_list([list = lists_[index]](const Music& m, size_t page) { return m.playlist(list, page); });
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

    void play_from(List list, size_t index, bool finished = false) {
        message("Открываем аудио…");
        auto event = with_feedback(finished);
        launch([this, music = Music(token_), list = std::move(list), index, event]() mutable {
            while (index >= list.tracks.size() && list.more) {
                auto page = list.fetch(music, list.page);
                extend(list, std::move(page));
            }
            if (index >= list.tracks.size()) return Done([this] { if (queue_) queue_->more = false; update_track(); });
            auto stream = music.stream(list.tracks[index].id);
            if (event) event();
            return Done([this, list = std::move(list), index, stream]() mutable {
                if (visible_ && visible_->id == list.id && visible_->page < list.page) {
                    auto selected = browser_->value();
                    visible_ = list;
                    render_list();
                    browser_->value(selected);
                }
                queue_ = std::move(list);
                radio_ = false;
                index_ = index;
                play_track(queue_->tracks[index_], stream);
            });
        });
    }

    void start_wave() { play_wave({}, 0, true, false); }

    void play_wave(std::vector<Queued> list, size_t index, bool first, bool finished) {
        message(first ? "Запускаем Мою волну…" : "Открываем аудио…");
        auto event = with_feedback(finished);
        launch([this, music = Music(token_), list = std::move(list), index, first, event]() mutable {
            if (event) event();
            if (index >= list.size()) {
                auto batch = music.wave(list.empty() ? std::string{} : list.back().track.id);
                if (first) feedback(music, "radioStarted", Queued{{}, batch.batch});
                for (auto& item : batch.tracks) list.push_back({std::move(item), batch.batch});
            }
            auto stream = music.stream(list.at(index).track.id);
            return Done([this, list = std::move(list), stream, index]() mutable {
                auto trim = index < 32 ? 0 : index - 31;
                list.erase(list.begin(), list.begin() + trim);
                wave_ = std::move(list);
                wave_index_ = index - trim;
                radio_ = true;
                queue_.reset();
                play_track(wave_[wave_index_].track, stream);
            });
        });
    }

    void play_track(const Track& track, const Audio& audio, double at = 0) {
        stop();
        pending_.reset();
        current_ = track;
        update_track();
        player_.open(narrow(audio.url), at);
        began();
        save_position(at);
    }

    void began() {
        desired_ = active_ = true;
        snap_ = {Playback::loading};
        started_sent_ = false;
        last_save_ = 0;
        refresh_controls();
        Fl::add_timeout(0.20, timer, this);
    }

    void stop() {
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

    void previous() {
        if (active_ && (snap_.position > 3 || busy_ || (!queue_ && !radio_) ||
            (radio_ ? wave_index_ == 0 : queue_->tracks.empty() || index_ == 0))) {
            player_.seek(0);
            save_position(0);
            return;
        }
        if (busy_) return;
        if (radio_ && wave_index_ > 0) play_wave(wave_, wave_index_ - 1, false, false);
        else if (queue_ && index_ > 0) play_from(*queue_, index_ - 1);
    }

    void advance(bool finished) {
        if (busy_) {
            if (finished) deferred_end_ = true;
            return;
        }
        if (radio_) play_wave(wave_, wave_index_ + 1, false, finished);
        else if (queue_ && (index_ + 1 < queue_->tracks.size() || queue_->more)) play_from(*queue_, index_ + 1, finished);
        else if (finished && !current_.id.empty()) {
            save_position(0);
            pending_ = ResumeTrack{profile_.uid, current_, 0};
        }
    }

    void set_volume() {
        auto number = volume_->value();
        player_.volume(number / 1600.0);
        volume_label_->copy_label((std::to_string(static_cast<int>(number)) + "%").c_str());
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
        auto local = argc == 3 && std::string_view(argv[1]) == "--audio-test" ? argv[2] : "";
        App app(local);
        return app.run();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
