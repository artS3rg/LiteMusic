#include <windows.h>
#include "discord_presence.hpp"
#include "music.hpp"
#include "play_order.hpp"
#include "media_keys.hpp"
#include "resource.h"
#include "settings.hpp"
#include "ui.hpp"

#include <dwmapi.h>
#include <mfplay.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>

namespace {
constexpr UINT work_done = WM_APP + 1;
constexpr UINT media_notice = WM_APP + 2;
constexpr UINT device_notice = WM_APP + 3;
constexpr UINT seek_notice = WM_APP + 4;
constexpr UINT media_key_notice = WM_APP + 5;
constexpr UINT_PTR test_timer = 1;
constexpr UINT_PTR progress_timer = 2;
constexpr LONGLONG ticks_per_second = 10'000'000;
constexpr LONGLONG restart_threshold = 3 * ticks_per_second;
constexpr int seek_steps = 10'000;
constexpr float player_volume(int percent) { return percent / 1600.0f; }
constexpr size_t wave_history_limit = 32;
constexpr wchar_t window_class[] = L"LiteMusic.Window";
constexpr wchar_t app_title[] = L"LiteMusic";
constexpr DWORD window_style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr int default_width = 558;
constexpr int default_height = 463;
constexpr int min_width = 428;
constexpr int min_height = 356;
constexpr int mini_width = 366;
constexpr int player_height = 137;

enum Control {
    login_id = 101, query_id, search_id, favorites_id, playlists_id, wave_id, links_id,
    open_id, next_page_id, pause_id, next_id, previous_id, account_id, heading_id, now_id, status_id,
    seek_id, elapsed_id, duration_id, volume_id, volume_label_id, like_id, meta_id, mini_id, empty_id, pin_id,
    shuffle_id, repeat_id, discord_id
};
enum class View { none, search, favorites, playlists, playlist, wave };

struct TrackSource {
    View view;
    std::wstring query;
    Playlist playlist;

    bool collection() const { return view == View::favorites || view == View::playlist; }

    TracksPage fetch(const Music& music, const std::string& uid, size_t page) const {
        switch (view) {
        case View::search: return music.search(query, page);
        case View::favorites: return music.favorites(uid, page);
        case View::playlist: return music.playlist(playlist, page);
        default: return {};
        }
    }
};

struct ShuffledPages {
    CollectionPages pages;
    std::vector<std::pair<size_t, size_t>> positions;
    ShuffleOrder order;
};

struct TrackQueue {
    TrackSource source;
    TracksPage list;
    size_t page;
    size_t index;
    std::shared_ptr<ShuffledPages> shuffle;
};

struct WavePosition {
    std::shared_ptr<const Wave> batch;
    size_t index;
};

std::wstring window_text(HWND control) {
    int size = GetWindowTextLengthW(control);
    std::wstring result(size + 1, L'\0');
    result.resize(GetWindowTextW(control, result.data(), size + 1));
    return result;
}

INT_PTR CALLBACK discord_dialog(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INITDIALOG) {
        auto* id = reinterpret_cast<std::string*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, lparam);
        SendDlgItemMessageW(dialog, IDC_DISCORD_ID, EM_SETLIMITTEXT, 32, 0);
        SetDlgItemTextW(dialog, IDC_DISCORD_ID, widen(*id).c_str());
        return TRUE;
    }
    if (message != WM_COMMAND) return FALSE;
    switch (LOWORD(wparam)) {
    case IDC_DISCORD_PORTAL:
        ShellExecuteW(dialog, L"open", L"https://discord.com/developers/applications", nullptr, nullptr, SW_SHOWNORMAL);
        return TRUE;
    case IDCANCEL:
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    case IDOK: {
        auto input = window_text(GetDlgItem(dialog, IDC_DISCORD_ID));
        auto first = input.find_first_not_of(L" \t\r\n");
        std::string id;
        if (first != std::wstring::npos) {
            auto last = input.find_last_not_of(L" \t\r\n");
            auto value = input.substr(first, last - first + 1);
            if (value.find_first_not_of(L"0123456789") == std::wstring::npos) id.assign(value.begin(), value.end());
            if (!valid_discord_id(id)) {
                MessageBoxW(dialog, L"Скопируйте числовой Application ID из Discord Developer Portal.",
                            L"Статус Discord", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
        }
        *reinterpret_cast<std::string*>(GetWindowLongPtrW(dialog, DWLP_USER)) = std::move(id);
        EndDialog(dialog, IDOK);
        return TRUE;
    }
    default: return FALSE;
    }
}

std::wstring media_error(HRESULT hr) {
    wchar_t code[16];
    swprintf(code, 16, L"0x%08lX", static_cast<unsigned long>(hr));
    return std::wstring(L"Ошибка воспроизведения · ") + code;
}

std::wstring time_label(LONGLONG ticks) {
    auto seconds = ticks / ticks_per_second;
    auto remainder = seconds % 60;
    return std::to_wstring(seconds / 60) + L":" + (remainder < 10 ? L"0" : L"") + std::to_wstring(remainder);
}

class MediaCallback final : public IMFPMediaPlayerCallback {
public:
    MediaCallback(HWND window, DWORD session) : window_(window), session_(session) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(IMFPMediaPlayerCallback)) return E_NOINTERFACE;
        *object = static_cast<IMFPMediaPlayerCallback*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }

    ULONG STDMETHODCALLTYPE Release() override {
        auto remaining = InterlockedDecrement(&refs_);
        if (!remaining) delete this;
        return remaining;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* event) override {
        auto type = event->eEventType;
        auto hr = event->hrEvent;
        if (type == MFP_EVENT_TYPE_POSITION_SET) {
            PostMessageW(window_, seek_notice, session_, hr);
            return;
        }
        if (SUCCEEDED(hr) && type == MFP_EVENT_TYPE_MEDIAITEM_CREATED) {
            auto item = MFP_GET_MEDIAITEM_CREATED_EVENT(event)->pMediaItem;
            hr = event->pMediaPlayer->SetMediaItem(item);
        }
        if (FAILED(hr) || type == MFP_EVENT_TYPE_MEDIAITEM_SET || type == MFP_EVENT_TYPE_PLAY || type == MFP_EVENT_TYPE_PAUSE ||
            type == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            auto code = FAILED(hr) ? static_cast<INT_PTR>(hr) : static_cast<INT_PTR>(type);
            PostMessageW(window_, media_notice, session_, code);
        }
    }

private:
    volatile LONG refs_ = 1;
    HWND window_;
    DWORD session_;
};

class App {
public:
    App(std::wstring file, bool verify) : local_file_(std::move(file)), verify_(verify) {}
    ~App() {
        free_fonts();
        for (auto brush : {background_brush_, header_brush_, panel_brush_, field_brush_}) {
            if (brush) DeleteObject(brush);
        }
    }

    bool create(HINSTANCE instance, int show) {
        if (local_file_.empty()) {
            volume_ = load_volume();
            last_track_ = load_resume_track();
            discord_application_ = load_discord_id();
            if (!valid_discord_id(discord_application_)) discord_application_.clear();
            discord_.configure(discord_application_);
        }
        background_brush_ = CreateSolidBrush(ui::background);
        header_brush_ = CreateSolidBrush(ui::header);
        panel_brush_ = CreateSolidBrush(ui::panel);
        field_brush_ = CreateSolidBrush(ui::field);
        dpi_ = GetDpiForSystem();
        WNDCLASSEXW cls{};
        cls.cbSize = sizeof(cls);
        cls.lpfnWndProc = dispatch;
        cls.hInstance = instance;
        cls.hCursor = LoadCursor(nullptr, IDC_ARROW);
        auto icon = MAKEINTRESOURCEW(IDI_LITEMUSIC);
        cls.hIcon = LoadIconW(instance, icon);
        cls.hIconSm = static_cast<HICON>(LoadImageW(instance, icon, IMAGE_ICON,
                                                    GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        cls.hbrBackground = background_brush_;
        cls.lpszClassName = window_class;
        if (!RegisterClassExW(&cls)) return false;
        auto size = outer_size(default_width, default_height);
        hwnd_ = CreateWindowExW(WS_EX_CONTROLPARENT, window_class, app_title,
                                window_style, CW_USEDEFAULT, CW_USEDEFAULT,
                                size.cx, size.cy, nullptr, nullptr, instance, this);
        if (!hwnd_) return false;
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
        COLORREF caption = ui::header, edge = ui::line, title = ui::ink;
        DwmSetWindowAttribute(hwnd_, 34, &edge, sizeof(edge));
        DwmSetWindowAttribute(hwnd_, 35, &caption, sizeof(caption));
        DwmSetWindowAttribute(hwnd_, 36, &title, sizeof(title));
        if (!verify_) media_keys_.connect(hwnd_, media_key_notice);
        if (!verify_) ShowWindow(hwnd_, show);
        if (local_file_.empty()) restore_login();
        else {
            start_audio({{}, L"Локальное аудио", {}}, {local_file_, L"Локальный файл"}, false);
            if (verify_) {
                if (!player_ || !SetTimer(hwnd_, test_timer, 200, nullptr)) exit_test(4);
                else test_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(15);
            }
        }
        return true;
    }

    int run() {
        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            if (msg.message == WM_KEYDOWN && msg.wParam == 'M' && GetKeyState(VK_CONTROL) < 0) {
                toggle_mini();
            } else if (msg.message == WM_KEYDOWN && msg.wParam == 'F' && GetKeyState(VK_CONTROL) < 0) {
                if (mini_) toggle_mini();
                if (IsWindowEnabled(control(query_id))) SetFocus(control(query_id));
            } else if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == control(query_id)) {
                command(search_id);
            } else if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN && msg.hwnd == control(links_id)) {
                command(open_id);
            } else if (!IsDialogMessageW(hwnd_, &msg)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
        return static_cast<int>(msg.wParam);
    }

private:
    enum class Job { none, login, action };
    using Finish = std::function<void()>;
    using Work = std::function<Finish(std::stop_token)>;

    HWND hwnd_{};
    HWND tips_{};
    UINT dpi_ = 96;
    HFONT body_font_{}, medium_font_{}, title_font_{}, mono_font_{};
    HBRUSH background_brush_{}, header_brush_{}, panel_brush_{}, field_brush_{};
    bool mini_ = false;
    bool mini_pinned_ = false;
    bool restore_maximized_ = false;
    SIZE full_size_{default_width, default_height};
    std::wstring local_file_;
    bool verify_;
    media_keys::Session media_keys_;
    DiscordPresence discord_;
    std::string discord_application_;
    std::optional<ResumeTrack> last_track_;
    bool resume_available_ = false;
    LONGLONG resume_position_ = 0;
    bool restoring_position_ = false;
    std::chrono::steady_clock::time_point test_deadline_;
    std::jthread worker_;
    Job job_ = Job::none;
    Finish finish_;
    std::wstring error_;
    DWORD http_status_ = 0;
    bool closing_ = false;
    std::string token_;
    Profile profile_;
    std::optional<std::vector<std::string>> liked_;
    bool requested_likes_ = false;
    View view_ = View::none;
    std::wstring search_text_, section_;
    size_t page_ = 0;
    bool more_ = false;
    Playlist selected_playlist_;
    std::vector<Track> rows_;
    std::vector<Playlist> playlists_;

    IMFPMediaPlayer* player_{};
    DWORD generation_ = 0;
    bool ready_ = false;
    bool playing_ = false;
    LONGLONG duration_ = 0;
    int volume_ = 100;
    int displayed_second_ = -1;
    bool adjusting_seek_ = false;
    std::optional<LONGLONG> seek_in_progress_;
    std::optional<LONGLONG> queued_seek_;
    std::optional<TrackQueue> queue_;
    bool shuffle_ = false;
    Repeat repeat_ = Repeat::off;
    bool advance_pending_ = false;
    bool wave_playing_ = false;
    bool wave_started_ = false;
    std::chrono::steady_clock::time_point wave_play_since_{};
    double wave_played_seconds_ = 0;
    Track current_;
    std::deque<WavePosition> wave_history_;
    size_t wave_cursor_ = 0;
    std::vector<std::function<void(const Music&)>> pending_feedback_;

    static LRESULT CALLBACK dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        auto app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            app = static_cast<App*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
            app->hwnd_ = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        }
        if (app) return app->handle(message, wparam, lparam);
        return DefWindowProcW(window, message, wparam, lparam);
    }

    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam) {
        try {
            switch (message) {
            case WM_CREATE:
                dpi_ = GetDpiForWindow(hwnd_);
                try { create_controls(); } catch (const std::exception&) { return -1; }
                return 0;
            case WM_SIZE:
                layout(LOWORD(lparam), HIWORD(lparam));
                update_progress_timer();
                if (wparam != SIZE_MINIMIZED && ready_) update_progress();
                return 0;
            case WM_GETMINMAXINFO: {
                auto limits = reinterpret_cast<MINMAXINFO*>(lparam);
                auto minimum = outer_size(mini_ ? mini_width : min_width, mini_ ? player_height : min_height);
                limits->ptMinTrackSize = {minimum.cx, minimum.cy};
                if (mini_) limits->ptMaxTrackSize = limits->ptMinTrackSize;
                return 0;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wparam);
                setup_fonts();
                auto rect = reinterpret_cast<const RECT*>(lparam);
                auto size = outer_size(mini_width, player_height);
                SetWindowPos(hwnd_, nullptr, rect->left, rect->top, mini_ ? size.cx : rect->right - rect->left,
                             mini_ ? size.cy : rect->bottom - rect->top, SWP_NOACTIVATE | SWP_NOZORDER);
                return 0;
            }
            case WM_ERASEBKGND: return 1;
            case WM_PAINT: paint(); return 0;
            case WM_DRAWITEM: return draw_item(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
            case WM_NOTIFY: {
                auto header = reinterpret_cast<const NMHDR*>(lparam);
                if (header->hwndFrom == tips_ && header->code == TTN_GETDISPINFOW) {
                    auto info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
                    auto text = window_text(reinterpret_cast<HWND>(header->idFrom));
                    lstrcpynW(info->szText, text.c_str(), ARRAYSIZE(info->szText));
                    return 0;
                }
                break;
            }
            case WM_CTLCOLORSTATIC:
            case WM_CTLCOLOREDIT:
            case WM_CTLCOLORLISTBOX:
                return control_color(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));
            case WM_CTLCOLORBTN:
                return reinterpret_cast<LRESULT>(panel_brush_);
            case WM_SYSCOMMAND:
                if (mini_ && (wparam & 0xfff0) == SC_MAXIMIZE) return 0;
                break;
            case WM_HSCROLL:
                if (lparam == reinterpret_cast<LPARAM>(control(seek_id))) seek_scroll(LOWORD(wparam));
                if (lparam == reinterpret_cast<LPARAM>(control(volume_id))) change_volume();
                return 0;
            case WM_COMMAND:
                if (LOWORD(wparam) == links_id) {
                    if (HIWORD(wparam) == LBN_DBLCLK) command(open_id);
                    else if (HIWORD(wparam) == LBN_SELCHANGE) enable_controls();
                } else if (HIWORD(wparam) == BN_CLICKED && lparam) {
                    command(LOWORD(wparam));
                }
                return 0;
            case work_done:
                complete_work();
                return 0;
            case media_notice:
                if (wparam == generation_) media_event(static_cast<INT_PTR>(lparam));
                return 0;
            case seek_notice:
                if (wparam == generation_) seek_completed(static_cast<HRESULT>(lparam));
                return 0;
            case media_key_notice:
                media_key(static_cast<media_keys::SystemMediaTransportControlsButton>(wparam));
                return 0;
            case WM_APPCOMMAND:
                if (!media_keys_ && (ready_ || resume_available_)) {
                    switch (GET_APPCOMMAND_LPARAM(lparam)) {
                    case APPCOMMAND_MEDIA_PLAY_PAUSE: command(pause_id); return TRUE;
                    case APPCOMMAND_MEDIA_NEXTTRACK: command(next_id); return TRUE;
                    case APPCOMMAND_MEDIA_PREVIOUSTRACK: command(previous_id); return TRUE;
                    default: break;
                    }
                }
                break;
            case device_notice:
                show_device(std::unique_ptr<DeviceCode>(reinterpret_cast<DeviceCode*>(lparam)));
                return 0;
            case WM_TIMER:
                if (wparam == progress_timer) update_progress();
                if (wparam == test_timer && verify_) {
                    if (position_seconds() >= 0.5) exit_test(0);
                    else if (std::chrono::steady_clock::now() >= test_deadline_) exit_test(6);
                }
                return 0;
            case WM_CLOSE:
                closing_ = true;
                persist_track();
                release_player();
                if (worker_.joinable()) {
                    ShowWindow(hwnd_, SW_HIDE);
                    worker_.request_stop();
                } else {
                    DestroyWindow(hwnd_);
                }
                return 0;
            case WM_ENDSESSION:
                if (wparam) persist_track();
                return 0;
            case WM_DESTROY:
                media_keys_.close();
                PostQuitMessage(0);
                return 0;
            }
        } catch (const std::exception& error) {
            status(widen(error.what()));
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND control(Control id) const { return GetDlgItem(hwnd_, id); }
    int px(int value) const { return ui::px(value, dpi_); }
    SIZE outer_size(int width, int height) const {
        RECT rect{0, 0, px(width), px(height)};
        AdjustWindowRectExForDpi(&rect, window_style, FALSE, WS_EX_CONTROLPARENT, dpi_);
        return {rect.right - rect.left, rect.bottom - rect.top};
    }

    void free_fonts() {
        for (auto font : {body_font_, medium_font_, title_font_, mono_font_}) {
            if (font) DeleteObject(font);
        }
    }

    void setup_fonts() {
        auto make_font = [this](int height, int weight, const wchar_t* name) {
            return CreateFontW(-px(height), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                               OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                               DEFAULT_PITCH, name);
        };
        auto old_body = std::exchange(body_font_, make_font(13, FW_NORMAL, L"Segoe UI"));
        auto old_medium = std::exchange(medium_font_, make_font(13, FW_SEMIBOLD, L"Segoe UI"));
        auto old_title = std::exchange(title_font_, make_font(16, FW_SEMIBOLD, L"Segoe UI"));
        auto old_mono = std::exchange(mono_font_, make_font(11, FW_NORMAL, L"Consolas"));
        for (auto id : {login_id, query_id, search_id, favorites_id, playlists_id, wave_id, links_id, open_id,
                       next_page_id, pause_id, next_id, previous_id, account_id, heading_id, now_id, status_id,
                       seek_id, elapsed_id, duration_id, volume_id, volume_label_id, like_id, meta_id, mini_id, empty_id,
                       pin_id, shuffle_id, repeat_id, discord_id}) {
            auto window = control(id);
            if (!window) continue;
            auto font = id == heading_id || id == now_id ? title_font_ :
                        id == elapsed_id || id == duration_id || id == volume_label_id ? mono_font_ : body_font_;
            SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
        SendMessageW(control(links_id), LB_SETITEMHEIGHT, 0, px(30));
        SendMessageW(control(query_id), EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(px(6), px(6)));
        for (auto font : {old_body, old_medium, old_title, old_mono}) {
            if (font) DeleteObject(font);
        }
    }

    void set(Control id, std::wstring_view text) const {
        auto window = control(id);
        if (window_text(window) != text) {
            SetWindowTextW(window, std::wstring(text).c_str());
            InvalidateRect(window, nullptr, FALSE);
        }
    }

    void status(std::wstring_view text) const { set(status_id, text); }

    void remember_track(bool ended = false) {
        if (!ready_ || current_.id.empty() || profile_.uid.empty()) return;
        auto position = queued_seek_ ? *queued_seek_ :
                        seek_in_progress_ ? *seek_in_progress_ : media_time(&IMFPMediaPlayer::GetPosition);
        last_track_ = ResumeTrack{profile_.uid, current_, ended ? 0 : position};
    }

    void persist_track() {
        if (!local_file_.empty()) return;
        remember_track();
        if (last_track_) save_resume_track(*last_track_);
    }

    void make(const wchar_t* cls, const wchar_t* title, DWORD flags, Control id, DWORD extended = 0) {
        auto win = CreateWindowExW(extended, cls, title, WS_CHILD | WS_VISIBLE | flags, 0, 0, 0, 0,
                                   hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                   GetModuleHandleW(nullptr), nullptr);
        if (!win) throw std::runtime_error("Не удалось создать элементы окна");
        if ((flags & BS_TYPEMASK) == BS_OWNERDRAW && std::wstring_view(cls) == L"BUTTON") {
            SetWindowSubclass(win, ui::button_proc, 1, 0);
        }
    }

    void create_controls() {
        constexpr auto button = WS_TABSTOP | BS_OWNERDRAW;
        make(L"BUTTON", L"Войти", button, login_id);
        make(L"BUTTON", discord_application_.empty() ? L"Discord" : L"Discord ✓", button, discord_id);
        make(L"STATIC", L"Вход не выполнен", SS_CENTERIMAGE | SS_RIGHT | SS_ENDELLIPSIS, account_id);
        make(L"BUTTON", L"Любимые", button, favorites_id);
        make(L"BUTTON", L"Плейлисты", button, playlists_id);
        make(L"BUTTON", L"Моя волна", button, wave_id);
        make(L"BUTTON", L"Вперемешку", button, shuffle_id);
        make(L"BUTTON", L"Повтор: выкл.", button, repeat_id);
        make(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, query_id);
        SendMessageW(control(query_id), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Название трека или исполнитель"));
        SendMessageW(control(query_id), EM_SETLIMITTEXT, 300, 0);
        make(L"BUTTON", L"Найти", button, search_id);
        make(L"STATIC", L"Коллекция", SS_CENTERIMAGE | SS_ENDELLIPSIS, heading_id);
        make(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS, links_id);
        make(L"STATIC", L"Войдите, чтобы открыть коллекцию", SS_CENTERIMAGE | SS_CENTER | SS_ENDELLIPSIS, empty_id);
        make(L"BUTTON", L"Слушать", button, open_id);
        make(L"BUTTON", L"След. стр.", button, next_page_id);
        make(L"STATIC", L"Ничего не играет", SS_CENTERIMAGE | SS_ENDELLIPSIS, now_id);
        make(L"STATIC", L"Выберите трек", SS_CENTERIMAGE | SS_ENDELLIPSIS, meta_id);
        make(L"STATIC", L"0:00", SS_CENTERIMAGE, elapsed_id);
        make(TRACKBAR_CLASSW, L"Перемотка", WS_TABSTOP | TBS_NOTICKS, seek_id);
        make(L"STATIC", L"—", SS_CENTERIMAGE | SS_RIGHT, duration_id);
        SendMessageW(control(seek_id), TBM_SETRANGEMAX, TRUE, seek_steps);
        if (!SetWindowSubclass(control(seek_id), ui::slider_proc, 1, 0)) {
            throw std::runtime_error("Не удалось настроить перемотку");
        }
        make(L"BUTTON", L"Предыдущий", button, previous_id);
        make(L"BUTTON", L"Воспроизвести", button, pause_id);
        make(L"BUTTON", L"Следующий", button, next_id);
        make(L"BUTTON", L"Нравится", button, like_id);
        make(L"STATIC", L"", SS_CENTERIMAGE | SS_RIGHT, volume_label_id);
        make(TRACKBAR_CLASSW, L"Громкость", WS_TABSTOP | TBS_NOTICKS, volume_id);
        if (!SetWindowSubclass(control(volume_id), ui::slider_proc, 1, 0)) {
            throw std::runtime_error("Не удалось настроить громкость");
        }
        SendMessageW(control(volume_id), TBM_SETPOS, TRUE, volume_);
        SendMessageW(control(volume_id), TBM_SETPAGESIZE, 0, 10);
        update_volume_label();
        make(L"STATIC", L"Войдите, чтобы слушать музыку.", SS_CENTERIMAGE | SS_ENDELLIPSIS, status_id);
        make(L"BUTTON", L"Мини", button, mini_id);
        make(L"BUTTON", L"Закрепить поверх окон", button, pin_id);
        ShowWindow(control(pin_id), SW_HIDE);
        setup_fonts();
        tips_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                                 hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (tips_) {
            for (auto id : {search_id, previous_id, pause_id, next_id, like_id, seek_id, volume_id, mini_id, pin_id,
                            shuffle_id, repeat_id}) {
                TOOLINFOW tool{};
                tool.cbSize = sizeof(tool);
                tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
                tool.hwnd = hwnd_;
                tool.uId = reinterpret_cast<UINT_PTR>(control(id));
                tool.lpszText = LPSTR_TEXTCALLBACKW;
                SendMessageW(tips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
            }
        }
        enable_controls();
    }

    void layout(int width, int height) const {
        if (!control(mini_id) || width < px(300) || height < px(100)) return;
        width = MulDiv(width, 96, dpi_);
        height = MulDiv(height, 96, dpi_);
        auto place = [this](Control id, int x, int y, int w, int h) {
            const auto left = px(x), top = px(y);
            MoveWindow(control(id), left, top, px(x + std::max(0, w)) - left, px(y + std::max(0, h)) - top, TRUE);
        };
        auto deck = height - player_height;
        if (!mini_) {
            place(login_id, width - 86, 14, 72, 26);
            place(account_id, 163, 13, width - 340, 28);
            place(discord_id, width - 165, 14, 73, 26);
            place(favorites_id, 10, 85, 110, 31);
            place(playlists_id, 10, 122, 110, 31);
            place(wave_id, 10, 159, 110, 31);
            if (deck >= 263) {
                place(shuffle_id, 10, 200, 110, 26);
                place(repeat_id, 10, 231, 110, 26);
            } else {
                place(shuffle_id, 10, deck - 25, 48, 24);
                place(repeat_id, 62, deck - 25, 58, 24);
            }
            place(query_id, 151, 68, width - 228, 22);
            place(search_id, width - 68, 62, 54, 30);
            place(heading_id, 141, 101, width - 155, 24);
            place(links_id, 141, 130, width - 155, std::max(30, deck - 173));
            place(empty_id, 143, 131, width - 159, std::max(28, deck - 175));
            place(open_id, 141, deck - 35, 88, 25);
            place(next_page_id, width - 97, deck - 35, 83, 25);
        }
        const auto mini_button = mini_ ? 104 : 68;
        place(now_id, 60, deck + 8, width - mini_button - (mini_ ? 114 : 82), 24);
        place(meta_id, 60, deck + 32, width - (mini_ ? mini_button + 115 : 74), 18);
        place(mini_id, width - mini_button - 14, deck + 14, mini_button, 26);
        if (mini_) place(pin_id, width - mini_button - 47, deck + 14, 27, 26);
        place(elapsed_id, 14, deck + 54, 36, 20);
        place(seek_id, 48, deck + 52, width - 96, 24);
        place(duration_id, width - 50, deck + 54, 36, 20);
        place(previous_id, 14, deck + 78, 35, 32);
        place(pause_id, 54, deck + 76, 42, 36);
        place(next_id, 101, deck + 78, 35, 32);
        place(like_id, 144, deck + 78, 35, 32);
        place(volume_id, width - 138, deck + 82, 88, 24);
        place(volume_label_id, width - 48, deck + 82, 34, 24);
        place(status_id, 14, deck + 113, width - 28, 21);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void toggle_mini() {
        if (!mini_ && IsZoomed(hwnd_)) {
            restore_maximized_ = true;
            ShowWindow(hwnd_, SW_RESTORE);
        }
        if (!mini_) {
            RECT client{};
            GetClientRect(hwnd_, &client);
            full_size_ = {MulDiv(client.right, 96, dpi_), MulDiv(client.bottom, 96, dpi_)};
        }
        mini_ = !mini_;
        for (auto id : {login_id, account_id, favorites_id, playlists_id, wave_id, query_id, search_id,
                       heading_id, links_id, empty_id, open_id, next_page_id, shuffle_id, repeat_id, discord_id}) {
            bool visible = !mini_ && (id != empty_id || selection() == LB_ERR);
            ShowWindow(control(id), visible ? SW_SHOWNA : SW_HIDE);
        }
        set(mini_id, mini_ ? L"Развернуть" : L"Мини");
        ShowWindow(control(pin_id), mini_ ? SW_SHOWNA : SW_HIDE);
        apply_pin();
        auto size = mini_ ? outer_size(mini_width, player_height) : outer_size(full_size_.cx, full_size_.cy);
        RECT current{};
        GetWindowRect(hwnd_, &current);
        MONITORINFO monitor{};
        monitor.cbSize = sizeof(monitor);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            const auto& area = monitor.rcWork;
            current.left = std::clamp(current.left, area.left, std::max(area.left, area.right - size.cx));
            current.top = std::clamp(current.top, area.top, std::max(area.top, area.bottom - size.cy));
        }
        SetWindowPos(hwnd_, nullptr, current.left, current.top, size.cx, size.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        if (!mini_ && std::exchange(restore_maximized_, false)) ShowWindow(hwnd_, SW_MAXIMIZE);
        SetFocus(control(mini_id));
    }

    void apply_pin() const {
        SetWindowPos(hwnd_, mini_ && mini_pinned_ ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        set(pin_id, mini_pinned_ ? L"Открепить от верха" : L"Закрепить поверх окон");
    }

    void paint() const {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(hwnd_, &ps);
        RECT client{};
        GetClientRect(hwnd_, &client);
        const auto w = client.right, h = client.bottom;
        const auto deck = h - px(player_height);
        auto icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd_, GCLP_HICON));
        ui::fill(dc, client, ui::background);
        if (!mini_) {
            ui::fill(dc, {0, 0, w, px(53)}, ui::header);
            ui::fill(dc, {0, px(53), w, px(53)+1}, ui::line);
            DrawIconEx(dc, px(13), px(11), icon, px(31), px(31), 0, nullptr, DI_NORMAL);
            ui::text(dc, {px(53), px(9), px(155), px(30)}, L"LiteMusic", ui::ink, title_font_);
            ui::text(dc, {px(53), px(29), px(156), px(45)}, L"Яндекс Музыка", ui::muted, body_font_);
            ui::text(dc, {px(14), px(61), px(119), px(82)}, L"КОЛЛЕКЦИЯ", ui::dim, mono_font_);
            ui::fill(dc, {px(130), px(54), px(130)+1, deck}, ui::line);
            ui::box(dc, {px(141), px(62), w-px(74), px(92)}, px(9), ui::field, ui::line);
            ui::fill(dc, {px(141), px(126), w-px(14), px(127)}, ui::line);
        }
        ui::fill(dc, {0, deck, w, h}, ui::panel);
        ui::fill(dc, {0, deck, w, deck+1}, ui::line);
        DrawIconEx(dc, px(14), deck+px(10), icon, px(36), px(36), 0, nullptr, DI_NORMAL);
        ui::glyph(dc, w-px(151), deck+px(94), dpi_, ui::Glyph::volume, ui::muted);
        EndPaint(hwnd_, &ps);
    }

    LRESULT control_color(HDC dc, HWND window) const {
        auto id = GetDlgCtrlID(window);
        auto color = ui::ink;
        auto brush = background_brush_;
        auto bg = ui::background;
        if (id == query_id) {
            bg = ui::field;
            brush = field_brush_;
            if (!IsWindowEnabled(window)) color = ui::dim;
        } else if (id == account_id) {
            bg = ui::header;
            brush = header_brush_;
            color = ui::muted;
        } else if (id == now_id || id == meta_id || id == elapsed_id || id == duration_id ||
                   id == status_id || id == volume_label_id) {
            bg = ui::panel;
            brush = panel_brush_;
            if (id != now_id) color = ui::muted;
        } else if (id == empty_id) {
            color = ui::muted;
        }
        SetBkMode(dc, OPAQUE);
        SetBkColor(dc, bg);
        SetTextColor(dc, color);
        return reinterpret_cast<LRESULT>(brush);
    }

    LRESULT draw_item(const DRAWITEMSTRUCT& item) const {
        if (item.CtlType == ODT_BUTTON) {
            draw_button(item);
            return TRUE;
        }
        if (item.CtlID != links_id || item.itemID == UINT(-1)) return FALSE;
        auto r = item.rcItem;
        const auto index = static_cast<size_t>(item.itemID);
        const bool folder = view_ == View::playlists;
        if (index >= (folder ? playlists_.size() : rows_.size())) return TRUE;
        const bool chosen = (item.itemState & ODS_SELECTED) != 0;
        const bool current = !folder && player_ && !current_.id.empty() && rows_[index].id == current_.id;
        const auto bg = current ? ui::active : chosen ? ui::selected : ui::background;
        ui::fill(item.hDC, r, bg);
        ui::fill(item.hDC, {r.left + px(8), r.bottom - 1, r.right - px(4), r.bottom}, ui::line);
        if (current || chosen) ui::fill(item.hDC, {r.left, r.top + px(3), r.left + px(2), r.bottom - px(3)}, current ? ui::mint : ui::rail);
        const auto center_y = (r.top + r.bottom) / 2;
        if (folder || current) {
            ui::glyph(item.hDC, r.left + px(17), center_y, dpi_,
                      folder ? ui::Glyph::folder : playing_ ? ui::Glyph::wave : ui::Glyph::play, current ? ui::mint : ui::muted);
        } else {
            ui::text(item.hDC, {r.left + px(7), r.top, r.left + px(26), r.bottom}, std::to_wstring(index + 1),
                     chosen ? ui::muted : ui::dim, mono_font_);
        }
        r.left += px(34);
        r.right -= px(8);
        if (folder) {
            ui::text(item.hDC, r, playlists_[index].name, ui::ink, body_font_);
        } else {
            auto& track = rows_[index];
            auto title = r;
            auto artist = r;
            if (!track.artists.empty()) {
                const auto artist_width = (r.right - r.left) * 36 / 100;
                title.right -= artist_width + px(8);
                artist.left = artist.right - artist_width;
                ui::text(item.hDC, artist, track.artists, ui::muted, body_font_);
            }
            ui::text(item.hDC, title, track.name, current ? ui::mint : ui::ink, chosen || current ? medium_font_ : body_font_);
        }
        if (chosen && (item.itemState & ODS_FOCUS)) {
            auto edge = item.rcItem;
            InflateRect(&edge, -px(1), -px(1));
            SetDCBrushColor(item.hDC, ui::dim);
            FrameRect(item.hDC, &edge, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        return TRUE;
    }

    void draw_button(const DRAWITEMSTRUCT& item) const {
        auto id = static_cast<Control>(item.CtlID);
        auto r = item.rcItem;
        auto dc = item.hDC;
        const bool deck = id == pause_id || id == previous_id || id == next_id || id == like_id || id == mini_id || id == pin_id;
        const bool nav = id == favorites_id || id == playlists_id || id == wave_id;
        const bool current = (id == favorites_id && view_ == View::favorites) ||
                             (id == playlists_id && (view_ == View::playlists || view_ == View::playlist)) ||
                             (id == wave_id && view_ == View::wave) || (id == pin_id && mini_pinned_) ||
                             (id == shuffle_id && shuffle_) || (id == repeat_id && repeat_ != Repeat::off) ||
                             (id == discord_id && !discord_application_.empty());
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool focus = (item.itemState & ODS_FOCUS) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        const bool hovered = GetPropW(item.hwndItem, L"LiteMusic.Hover") != nullptr;
        const bool primary = id == pause_id || (id == login_id && token_.empty());
        const auto surface = deck ? ui::panel : id == login_id ? ui::header : ui::background;
        const auto bg = disabled ? surface :
                        primary ? (hovered || pressed ? ui::mint_hover : ui::mint) :
                        current ? ui::active : hovered || pressed ? ui::hover : nav ? surface : ui::button;
        const auto border = focus ? ui::mint : current ? ui::rail : disabled || nav || primary ? bg : ui::line;
        auto color = disabled ? ui::dim : primary ? ui::on_mint : current ? ui::mint : ui::ink;
        ui::fill(dc, r, surface);
        ui::box(dc, r, px(9), bg, border);

        using ui::Glyph;
        std::optional<Glyph> glyph;
        switch (id) {
        case favorites_id: glyph = Glyph::heart; break;
        case playlists_id: glyph = Glyph::folder; break;
        case wave_id: glyph = Glyph::wave; break;
        case search_id: glyph = Glyph::search; break;
        case pause_id: glyph = playing_ ? Glyph::pause : Glyph::play; break;
        case previous_id: glyph = Glyph::previous; break;
        case next_id: glyph = Glyph::next; break;
        case like_id:
            glyph = is_liked(current_.id) ? Glyph::liked : Glyph::heart;
            if (!disabled && glyph == Glyph::liked) color = ui::mint;
            break;
        case mini_id: glyph = mini_ ? Glyph::expand : Glyph::mini; break;
        case pin_id: glyph = Glyph::pin; break;
        default: break;
        }
        const auto x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
        if (glyph && !nav && id != mini_id) {
            ui::glyph(dc, x, y, dpi_, *glyph, color);
        } else {
            auto label = window_text(item.hwndItem);
            if (r.right - r.left <= px(60)) {
                if (id == shuffle_id) label = L"Случ.";
                if (id == repeat_id) label = repeat_ == Repeat::track ? L"↻ 1" :
                                              repeat_ == Repeat::collection ? L"↻ ∞" : L"Повтор";
            }
            if (glyph) {
                ui::glyph(dc, r.left + px(nav ? 15 : 13), y, dpi_, *glyph, color);
                r.left += px(nav ? 29 : 25);
                r.right -= px(3);
            }
            ui::text(dc, r, label, color, medium_font_,
                     DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | (glyph ? DT_LEFT : DT_CENTER));
        }
    }

    int selection() const { return static_cast<int>(SendMessageW(control(links_id), LB_GETCURSEL, 0, 0)); }

    void enable_controls() {
        bool free = job_ == Job::none;
        bool enabled = !token_.empty() && free;
        for (auto id : {query_id, search_id, favorites_id, playlists_id, wave_id}) {
            EnableWindow(control(id), enabled);
        }
        EnableWindow(control(login_id), free || job_ == Job::login);
        EnableWindow(control(open_id), enabled && selection() != LB_ERR);
        EnableWindow(control(next_page_id), enabled && more_);
        auto modes = enabled && ((queue_ && queue_->source.collection()) ||
                                  view_ == View::favorites || view_ == View::playlist);
        EnableWindow(control(shuffle_id), modes);
        EnableWindow(control(repeat_id), modes);
        set(repeat_id, repeat_ == Repeat::collection ? L"Повтор: список" :
                       repeat_ == Repeat::track ? L"Повтор: трек" : L"Повтор: выкл.");
        const bool can_play = ready_ || (enabled && resume_available_);
        EnableWindow(control(pause_id), can_play);
        EnableWindow(control(seek_id), ready_ && duration_ > 0);
        EnableWindow(control(like_id), enabled && ready_ && !current_.id.empty());
        set(like_id, ready_ && is_liked(current_.id) ? L"Убрать лайк" : L"Нравится");
        EnableWindow(control(previous_id), free && ready_);
        auto next = enabled && (wave_playing_ ? ready_ : has_next_track());
        EnableWindow(control(next_id), next);
        media_keys_.update(ready_ || resume_available_ || player_, playing_, next, free && ready_);
        for (auto id : {favorites_id, playlists_id, wave_id, login_id, shuffle_id, repeat_id}) {
            InvalidateRect(control(id), nullptr, FALSE);
        }
    }

    void work(Job kind, std::wstring_view progress, Work action) {
        if (job_ != Job::none) return;
        finish_ = {};
        error_.clear();
        http_status_ = 0;
        worker_ = std::jthread([this, action = std::move(action)](std::stop_token stop) {
            try {
                finish_ = action(stop);
            } catch (const HttpError& error) {
                http_status_ = error.status;
                error_ = widen(error.what());
            } catch (const std::exception& error) {
                error_ = widen(error.what());
            }
            PostMessageW(hwnd_, work_done, 0, 0);
        });
        job_ = kind;
        if (!progress.empty()) status(progress);
        if (kind == Job::login) set(login_id, L"Отмена");
        enable_controls();
    }

    void complete_work() {
        bool cancelled = worker_.get_stop_token().stop_requested();
        worker_.join();
        auto finish = std::exchange(finish_, {});
        job_ = Job::none;
        set(login_id, token_.empty() ? L"Войти" : L"Выйти");
        if (closing_) {
            DestroyWindow(hwnd_);
            return;
        }
        if (cancelled) {
            set(account_id, L"Вход в Яндекс Музыку");
            status(L"Вход отменён.");
        } else if (http_status_ == 401) {
            sign_out();
            status(L"Яндекс отклонил вход. Попробуйте войти ещё раз.");
        } else if (!error_.empty()) {
            if (token_.empty()) set(account_id, L"Вход в Яндекс Музыку");
            status(error_);
        } else if (finish) {
            finish();
        }
        enable_controls();
        if (advance_pending_ && free_to_advance()) advance_playback();
        else {
            flush_feedback();
            refresh_likes();
        }
    }

    bool is_liked(const std::string& track) const {
        return liked_ && std::binary_search(liked_->begin(), liked_->end(), track);
    }

    void refresh_likes() {
        if (job_ != Job::none || token_.empty() || !ready_ || requested_likes_) return;
        requested_likes_ = true;
        Music music(token_);
        work(Job::action, L"", [this, music, uid = profile_.uid](std::stop_token) -> Finish {
            try {
                auto ids = music.liked_tracks(uid);
                return [this, ids = std::move(ids)]() mutable { liked_ = std::move(ids); };
            } catch (const std::exception&) {
                return {};
            }
        });
    }

    void toggle_like() {
        if (!ready_ || current_.id.empty()) return;
        Music music(token_);
        work(Job::action, L"Сохраняем отметку…", [this, music, uid = profile_.uid, track = current_.id](std::stop_token) {
            auto ids = liked_ ? *liked_ : music.liked_tracks(uid);
            auto where = std::lower_bound(ids.begin(), ids.end(), track);
            bool add = where == ids.end() || *where != track;
            if (add) ids.insert(where, track);
            else ids.erase(where);
            music.set_track_like(uid, track, add);
            return [this, add, ids = std::move(ids)]() mutable {
                liked_ = std::move(ids);
                status(add ? L"Трек добавлен в любимые." : L"Лайк снят.");
                if (view_ == View::favorites) load_view(View::favorites, page_);
            };
        });
    }

    void signed_in(std::string token, Profile profile, bool persist) {
        token_ = std::move(token);
        profile_ = std::move(profile);
        set(account_id, profile_.name.empty() ? L"Вход выполнен" : profile_.name);
        set(empty_id, L"Откройте коллекцию или найдите трек");
        set(login_id, L"Выйти");
        status(L"Выберите раздел или найдите трек.");
        if (persist) {
            try {
                save_token(token_);
            } catch (const std::exception&) {
                status(L"Вход выполнен. Windows не сохранила токен между запусками.");
            }
        }
        if (last_track_ && last_track_->account == profile_.uid) {
            resume_available_ = true;
            set(now_id, last_track_->track.name);
            set(meta_id, last_track_->track.artists);
            show_position(last_track_->position);
            media_keys_.describe(last_track_->track.name, last_track_->track.artists);
            status(L"Нажмите «Играть», чтобы продолжить.");
        }
    }

    void restore_login() {
        try {
            auto token = saved_token();
            if (token.empty()) return;
            work(Job::action, L"Проверяем сохранённый вход…", [this, token = std::move(token)](std::stop_token) {
                auto profile = Music(token).profile();
                return [this, token, profile = std::move(profile)]() mutable {
                    signed_in(std::move(token), std::move(profile), false);
                };
            });
        } catch (const std::exception&) {
            status(L"Хранилище Windows недоступно. Можно войти заново.");
        }
    }

    void sign_in() {
        // The worker delivers the code to the window's message loop.
        work(Job::login, L"Получаем код входа…", [this](std::stop_token stop) -> Finish {
            auto token = Music::sign_in([&] { return stop.stop_requested(); }, [this](DeviceCode device) {
                auto code = std::make_unique<DeviceCode>(std::move(device));
                if (PostMessageW(hwnd_, device_notice, 0, reinterpret_cast<LPARAM>(code.get()))) code.release();
            });
            if (stop.stop_requested() || token.empty()) return {};
            auto profile = Music(token).profile();
            return [this, token = std::move(token), profile = std::move(profile)]() mutable {
                signed_in(std::move(token), std::move(profile), true);
            };
        });
    }

    void show_device(std::unique_ptr<DeviceCode> device) {
        if (closing_ || job_ != Job::login || worker_.get_stop_token().stop_requested()) return;
        set(account_id, L"Код входа · " + device->code);
        status(L"Введите код на странице Яндекса в браузере.");
        auto result = reinterpret_cast<INT_PTR>(ShellExecuteW(hwnd_, L"open", device->url.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (result <= 32) status(L"Откройте " + device->url + L" и введите код.");
    }

    void sign_out() {
        release_player();
        last_track_.reset();
        resume_available_ = false;
        erase_resume_track();
        token_.clear();
        liked_.reset();
        requested_likes_ = false;
        reset_wave();
        queue_.reset();
        profile_ = {};
        rows_.clear();
        playlists_.clear();
        more_ = false;
        view_ = View::none;
        SendMessageW(control(links_id), LB_RESETCONTENT, 0, 0);
        set(open_id, L"Слушать");
        set(login_id, L"Войти");
        set(account_id, L"Вход не выполнен");
        set(heading_id, L"Коллекция");
        set(empty_id, L"Войдите, чтобы открыть коллекцию");
        ShowWindow(control(empty_id), mini_ ? SW_HIDE : SW_SHOWNA);
        current_ = {};
        show_player_idle();
        status(L"Вы вышли из аккаунта.");
        enable_controls();
        forget_token();
    }

    void display_tracks(View view, std::wstring name, TracksPage result, size_t page) {
        view_ = view;
        section_ = std::move(name);
        page_ = page;
        more_ = result.more;
        rows_ = std::move(result.tracks);
        playlists_.clear();
        set(heading_id, section_ + (page ? L" · стр. " + std::to_wstring(page + 1) : L""));
        set(open_id, L"Слушать");
        set(next_page_id, L"След. стр.");
        refill(rows_);
        set(empty_id, L"На этой странице нет треков");
        if (!player_) status(rows_.empty() ? L"Треки не найдены." : L"Двойной щелчок или Enter — слушать.");
    }

    template<class Rows>
    void refill(const Rows& rows) {
        const auto list = control(links_id);
        SendMessageW(list, WM_SETREDRAW, FALSE, 0);
        SendMessageW(list, LB_RESETCONTENT, 0, 0);
        for (const auto& row : rows) {
            auto label = row.name;
            if constexpr (requires { row.artists; }) {
                if (!row.artists.empty()) label += L" — " + row.artists;
            }
            SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        if (!rows.empty()) SendMessageW(list, LB_SETCURSEL, 0, 0);
        SendMessageW(list, WM_SETREDRAW, TRUE, 0);
        ShowWindow(list, mini_ ? SW_HIDE : SW_SHOWNA);
        ShowWindow(control(empty_id), rows.empty() && !mini_ ? SW_SHOWNA : SW_HIDE);
        InvalidateRect(list, nullptr, TRUE);
        enable_controls();
    }

    void load_view(View view, size_t page, std::wstring query = {}, Playlist list = {}) {
        Music music(token_);
        auto uid = profile_.uid;
        work(Job::action, L"Загружаем…", [this, music, uid, page, source = TrackSource{view, std::move(query), std::move(list)}]
             (std::stop_token) -> Finish {
            if (source.view == View::playlists) {
                auto rows = music.playlists(uid);
                return [this, rows = std::move(rows)]() mutable {
                    view_ = View::playlists;
                    more_ = false;
                    rows_.clear();
                    playlists_ = std::move(rows);
                    set(heading_id, L"Плейлисты");
                    set(open_id, L"Открыть");
                    refill(playlists_);
                    set(empty_id, L"Плейлистов пока нет");
                    status(playlists_.empty() ? L"Плейлистов пока нет." : L"Откройте плейлист, чтобы выбрать трек.");
                };
            }
            std::wstring title;
            switch (source.view) {
            case View::search: title = L"Поиск · " + source.query; break;
            case View::favorites: title = L"Любимые треки"; break;
            case View::playlist: title = source.playlist.name; break;
            default: return {};
            }
            auto tracks = source.fetch(music, uid, page);
            return [this, source, title = std::move(title), tracks = std::move(tracks), page]() mutable {
                search_text_ = std::move(source.query);
                selected_playlist_ = std::move(source.playlist);
                display_tracks(source.view, std::move(title), std::move(tracks), page);
            };
        });
    }

    void command(int id) {
        if (id == discord_id) {
            auto application_id = discord_application_;
            if (DialogBoxParamW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDD_DISCORD), hwnd_, discord_dialog,
                                reinterpret_cast<LPARAM>(&application_id)) == IDOK) {
                discord_application_ = std::move(application_id);
                save_discord_id(discord_application_);
                discord_.configure(discord_application_);
                set(discord_id, discord_application_.empty() ? L"Discord" : L"Discord ✓");
                update_discord();
            }
            return;
        }
        if (id == mini_id) {
            toggle_mini();
            return;
        }
        if (id == pin_id && mini_) {
            mini_pinned_ = !mini_pinned_;
            apply_pin();
            return;
        }
        if (id == login_id) {
            if (job_ == Job::login) {
                worker_.request_stop();
                EnableWindow(control(login_id), FALSE);
                status(L"Отменяем вход…");
            } else if (job_ == Job::none) {
                if (token_.empty()) sign_in();
                else sign_out();
            }
            return;
        }
        if (id == pause_id) {
            if (ready_) set_playing(!playing_);
            else if (resume_available_ && free_to_advance()) resume_last_track();
            return;
        }
        if (id == previous_id && ready_ && job_ == Job::none) {
            previous_track();
            return;
        }
        if (token_.empty() || job_ != Job::none) return;
        switch (id) {
        case like_id: toggle_like(); break;
        case shuffle_id:
            shuffle_ = !shuffle_;
            if (queue_) queue_->shuffle.reset();
            enable_controls();
            break;
        case repeat_id:
            repeat_ = next_repeat(repeat_);
            enable_controls();
            break;
        case search_id: {
            auto query = window_text(control(query_id));
            if (query.find_first_not_of(L" \t") == std::wstring::npos) status(L"Введите название трека или исполнителя.");
            else load_view(View::search, 0, std::move(query));
            break;
        }
        case favorites_id: load_view(View::favorites, 0); break;
        case playlists_id: load_view(View::playlists, 0); break;
        case wave_id: begin_wave(); break;
        case next_page_id: if (more_) load_view(view_, page_ + 1, search_text_, selected_playlist_); break;
        case next_id:
            if (wave_playing_ ? ready_ : has_next_track()) advance_playback();
            break;
        case open_id: {
            int idx = selection();
            if (idx == LB_ERR) break;
            if (view_ == View::playlists) load_view(View::playlist, 0, {}, playlists_.at(idx));
            else if (view_ == View::wave && wave_playing_) {
                const auto& position = wave_history_[wave_cursor_];
                if (static_cast<size_t>(idx) != position.index) {
                    open_wave_item({position.batch, static_cast<size_t>(idx)});
                }
            } else {
                open_track(static_cast<size_t>(idx));
            }
            break;
        }
        }
    }

    void media_key(media_keys::SystemMediaTransportControlsButton button) {
        if (closing_) return;
        using namespace media_keys;
        switch (button) {
        case SystemMediaTransportControlsButton_Play:
            if (!playing_) command(pause_id);
            break;
        case SystemMediaTransportControlsButton_Pause:
            if (playing_) set_playing(false);
            break;
        case SystemMediaTransportControlsButton_Next: command(next_id); break;
        case SystemMediaTransportControlsButton_Previous: command(previous_id); break;
        default: break;
        }
    }

    void set_playing(bool play) {
        auto hr = play ? player_->Play() : player_->Pause();
        if (FAILED(hr)) status(media_error(hr));
    }

    void resume_last_track() {
        auto resume = *last_track_;
        Music music(token_);
        work(Job::action, L"Восстанавливаем трек…", [this, music, resume = std::move(resume)](std::stop_token) mutable {
            auto audio = music.stream(resume.track.id);
            return [this, resume = std::move(resume), audio = std::move(audio)]() mutable {
                start_audio(std::move(resume.track), std::move(audio), false, resume.position);
            };
        });
    }

    void release_player() {
        discord_.clear();
        remember_track();
        ++generation_;
        advance_pending_ = false;
        ready_ = false;
        playing_ = false;
        duration_ = 0;
        resume_position_ = 0;
        restoring_position_ = false;
        adjusting_seek_ = false;
        seek_in_progress_.reset();
        queued_seek_.reset();
        update_progress_timer();
        show_position(0);
        SendMessageW(control(seek_id), TBM_SETPOS, TRUE, 0);
        set(duration_id, L"—");
        if (player_) {
            player_->Shutdown();
            player_->Release();
            player_ = nullptr;
        }
        set(pause_id, L"Воспроизвести");
        InvalidateRect(control(links_id), nullptr, FALSE);
    }

    void reset_wave() {
        wave_playing_ = false;
        wave_started_ = false;
        wave_history_.clear();
        wave_cursor_ = 0;
        if (token_.empty()) pending_feedback_.clear();
    }

    void start_audio(Track track, Audio audio, bool wave, LONGLONG start_position = 0) {
        release_player();
        resume_available_ = false;
        resume_position_ = start_position;
        wave_playing_ = wave;
        wave_started_ = false;
        wave_played_seconds_ = 0;
        wave_play_since_ = {};
        current_ = std::move(track);
        media_keys_.describe(current_.name, current_.artists);
        set(now_id, current_.name);
        set(meta_id, current_.artists.empty() ? audio.format : current_.artists + L"  ·  " + audio.format);
        InvalidateRect(control(links_id), nullptr, FALSE);
        auto callback = new MediaCallback(hwnd_, generation_);
        auto hr = MFPCreateMediaPlayer(nullptr, FALSE, 0, callback, nullptr, &player_);
        callback->Release();
        if (SUCCEEDED(hr)) hr = player_->SetVolume(player_volume(volume_));
        if (SUCCEEDED(hr) && verify_) hr = player_->SetMute(TRUE);
        if (SUCCEEDED(hr)) hr = player_->CreateMediaItemFromURL(audio.url.c_str(), FALSE, 0, nullptr);
        if (FAILED(hr)) {
            release_player();
            reset_wave();
            status(media_error(hr));
        } else {
            status(L"Открываем аудиопоток…");
        }
        enable_controls();
    }

    void show_player_idle() const {
        set(now_id, L"Ничего не играет");
        set(meta_id, L"Выберите трек");
    }

    bool has_next_track() const {
        if (!queue_) return false;
        if (queue_->source.collection()) {
            bool wrap = repeat_ == Repeat::collection;
            if (queue_->shuffle) return queue_->shuffle->order.step(true, wrap).has_value();
            if (shuffle_) return wrap || queue_->page > 0 || queue_->list.more || queue_->list.tracks.size() > 1;
            if (wrap) return true;
        }
        return queue_->index + 1 < queue_->list.tracks.size() || queue_->list.more;
    }

    bool repeat_track() const {
        return queue_ && queue_->source.collection() && repeat_ == Repeat::track;
    }

    void previous_track() {
        auto position = media_time(&IMFPMediaPlayer::GetPosition);
        if (queued_seek_) position = *queued_seek_;
        else if (seek_in_progress_) position = *seek_in_progress_;
        if (position < restart_threshold) {
            if (wave_playing_ && wave_cursor_ > 0) {
                auto cursor = wave_cursor_ - 1;
                open_wave_item(wave_history_[cursor], cursor);
                return;
            }
            bool previous = queue_ && (queue_->source.collection() && shuffle_ ?
                queue_->shuffle && queue_->shuffle->order.step(false, false).has_value() :
                queue_->index > 0 || queue_->page > 0);
            if (previous) {
                step_queue(false);
                return;
            }
        }
        seek_to(0);
    }

    bool free_to_advance() const {
        return job_ == Job::none && !token_.empty();
    }

    bool showing_queue_page() const {
        const auto& source = queue_->source;
        if (view_ != source.view || page_ != queue_->page) return false;
        if (view_ == View::search) return search_text_ == source.query;
        if (view_ == View::playlist) {
            return selected_playlist_.owner == source.playlist.owner && selected_playlist_.kind == source.playlist.kind;
        }
        return true;
    }

    void play_queue_track(Audio audio) {
        auto index = queue_->index;
        auto track = queue_->list.tracks.at(index);
        if (showing_queue_page()) {
            if (index >= rows_.size() || rows_[index].id != track.id) {
                auto row = std::find_if(rows_.begin(), rows_.end(), [&track](const Track& item) {
                    return item.id == track.id;
                });
                index = static_cast<size_t>(row - rows_.begin());
            }
            if (index < rows_.size()) SendMessageW(control(links_id), LB_SETCURSEL, index, 0);
        }
        start_audio(std::move(track), std::move(audio), false);
    }

    void open_track(size_t index) {
        feedback_end("skip");
        release_player();
        resume_available_ = false;
        reset_wave();
        queue_.reset();
        show_player_idle();
        Music music(token_);
        TrackQueue queue{{view_, search_text_, selected_playlist_}, {rows_, more_}, page_, index, {}};
        work(Job::action, L"Получаем поток…", [this, music, queue = std::move(queue)](std::stop_token) mutable {
            auto audio = music.stream(queue.list.tracks.at(queue.index).id);
            return [this, queue = std::move(queue), audio = std::move(audio)]() mutable {
                queue_ = std::move(queue);
                play_queue_track(std::move(audio));
            };
        });
    }

    void advance_playback() {
        if (!free_to_advance()) return;
        bool finished = std::exchange(advance_pending_, false);
        if (wave_playing_) {
            advance_wave();
        } else if (finished && repeat_track()) {
            const auto id = queue_->list.tracks[queue_->index].id;
            work(Job::action, L"Повторяем трек…", [this, music = Music(token_), id](std::stop_token) {
                auto audio = music.stream(id);
                return [this, audio = std::move(audio)]() mutable { play_queue_track(std::move(audio)); };
            });
        } else if (has_next_track()) {
            step_queue(true);
        }
    }

    void play_queue_page(size_t page, size_t index, TracksPage list, Audio audio) {
        bool update_list = showing_queue_page() && queue_->page != page;
        queue_->list = std::move(list);
        queue_->page = page;
        queue_->index = index;
        if (update_list) display_tracks(view_, section_, queue_->list, page);
        play_queue_track(std::move(audio));
    }

    void step_shuffle(bool forward, bool wrap) {
        auto history = queue_->shuffle;
        work(Job::action, L"Открываем трек…", [this, music = Music(token_), uid = profile_.uid,
            source = queue_->source, page = queue_->page, index = queue_->index,
            id = queue_->list.tracks[queue_->index].id, history, forward, wrap](std::stop_token stop) mutable -> Finish {
            bool current_removed = false;
            if (!history) {
                auto canceled = [&] { return stop.stop_requested(); };
                auto pages = source.view == View::favorites ? music.favorite_pages(uid, canceled) :
                                                              music.playlist_pages(source.playlist, canceled);
                if (canceled()) return {};
                std::vector<std::pair<size_t, size_t>> positions;
                std::optional<size_t> selected, same_id;
                for (size_t p = 0; p < pages.size(); ++p) {
                    for (size_t i = 0; i < pages[p].tracks.size(); ++i) {
                        if (pages[p].tracks[i].id == id) {
                            if (p == page && i == index) selected = positions.size();
                            if (!same_id) same_id = positions.size();
                        }
                        positions.emplace_back(p, i);
                    }
                }
                if (positions.empty()) return [this] { status(L"Треки закончились."); };
                // The playing track may have been removed from the collection since it was opened.
                if (!selected) selected = same_id;
                current_removed = !selected;
                auto count = positions.size();
                history = std::make_shared<ShuffledPages>(ShuffledPages{
                    std::move(pages), std::move(positions), ShuffleOrder(count, selected)});
            }
            auto cursor = current_removed ? std::optional<size_t>(0) : history->order.step(forward, wrap);
            if (!cursor) return [this, history] {
                queue_->shuffle = history;
                status(L"Треки закончились.");
            };
            auto [p, i] = history->positions[history->order.tracks[*cursor]];
            auto audio = music.stream(history->pages[p].tracks[i].id);
            return [this, history, cursor = *cursor, p, i, audio = std::move(audio)]() mutable {
                history->order.cursor = cursor;
                queue_->shuffle = history;
                play_queue_page(p, i, history->pages[p], std::move(audio));
            };
        });
    }

    void step_queue(bool forward) {
        bool collection = queue_->source.collection();
        bool wrap = collection && repeat_ == Repeat::collection;
        if (collection && shuffle_) {
            step_shuffle(forward, wrap);
            return;
        }
        release_player();
        Music music(token_);
        auto loading = forward ? L"Получаем следующий трек…" : L"Получаем предыдущий трек…";
        if (forward ? queue_->index + 1 < queue_->list.tracks.size() : queue_->index > 0) {
            auto index = forward ? queue_->index + 1 : queue_->index - 1;
            work(Job::action, loading, [this, music, index, id = queue_->list.tracks[index].id](std::stop_token) {
                auto audio = music.stream(id);
                return [this, index, audio = std::move(audio)]() mutable {
                    queue_->index = index;
                    play_queue_track(std::move(audio));
                };
            });
            return;
        }

        work(Job::action, loading, [this, music, uid = profile_.uid, source = queue_->source, page = queue_->page,
                                   id = queue_->list.tracks[queue_->index].id, forward, wrap,
                                   more = queue_->list.more]
             (std::stop_token stop) mutable -> Finish {
            TracksPage list;
            bool wrapping = forward && !more && wrap;
            if (wrapping) page = 0;
            else if (forward) ++page;
            else --page;
            do {
                if (stop.stop_requested()) return {};
                list = source.fetch(music, uid, page);
                if (!list.tracks.empty()) break;
                if (!forward && page > 0) --page;
                else if (forward && list.more) ++page;
                else if (forward && wrap && !wrapping) {
                    page = 0;
                    wrapping = true;
                } else break;
            } while (true);
            if (list.tracks.empty()) {
                if (!forward) {
                    auto audio = music.stream(id);
                    return [this, audio = std::move(audio)]() mutable { play_queue_track(std::move(audio)); };
                }
                return [this] {
                    queue_->list.more = false;
                    status(L"Треки закончились.");
                };
            }
            auto index = forward ? size_t{0} : list.tracks.size() - 1;
            auto audio = music.stream(list.tracks[index].id);
            return [this, page, index, list = std::move(list), audio = std::move(audio)]() mutable {
                play_queue_page(page, index, std::move(list), std::move(audio));
            };
        });
    }

    LONGLONG media_time(HRESULT (STDMETHODCALLTYPE IMFPMediaPlayer::*read)(REFGUID, PROPVARIANT*)) const {
        if (!player_) return 0;
        PROPVARIANT pos{};
        auto hr = (player_->*read)(MFP_POSITIONTYPE_100NS, &pos);
        LONGLONG ticks = 0;
        if (SUCCEEDED(hr)) {
            if (pos.vt == VT_I8) ticks = pos.hVal.QuadPart;
            if (pos.vt == VT_UI8 && pos.uhVal.QuadPart <= LLONG_MAX) ticks = static_cast<LONGLONG>(pos.uhVal.QuadPart);
        }
        PropVariantClear(&pos);
        return std::max(0LL, ticks);
    }

    double position_seconds() const {
        return static_cast<double>(media_time(&IMFPMediaPlayer::GetPosition)) / ticks_per_second;
    }

    void update_discord(bool seek = false) {
        if (!playing_ || current_.id.empty()) {
            discord_.clear();
        } else if (!discord_application_.empty()) {
            discord_.playing(current_, position_seconds(), static_cast<double>(duration_) / ticks_per_second, seek);
        }
    }

    void update_progress_timer() const {
        if (playing_ && !verify_ && !IsIconic(hwnd_)) SetTimer(hwnd_, progress_timer, 500, nullptr);
        else KillTimer(hwnd_, progress_timer);
    }

    void show_position(LONGLONG ticks) {
        int second = static_cast<int>(ticks / ticks_per_second);
        if (second == displayed_second_) return;
        displayed_second_ = second;
        set(elapsed_id, time_label(ticks));
    }

    void update_progress() {
        if (!ready_ || adjusting_seek_ || seek_in_progress_) return;
        auto position = media_time(&IMFPMediaPlayer::GetPosition);
        show_position(position);
        if (duration_ > 0) {
            auto step = static_cast<int>(std::clamp(static_cast<double>(position) / duration_, 0.0, 1.0) * seek_steps);
            if (step != SendMessageW(control(seek_id), TBM_GETPOS, 0, 0)) {
                SendMessageW(control(seek_id), TBM_SETPOS, TRUE, step);
            }
        }
    }

    void seek_scroll(unsigned notification) {
        if (!ready_ || duration_ <= 0) return;
        if (notification == TB_ENDTRACK && !adjusting_seek_) return;
        auto position = SendMessageW(control(seek_id), TBM_GETPOS, 0, 0);
        auto ticks = static_cast<LONGLONG>(static_cast<double>(duration_) * position / seek_steps);
        show_position(ticks);
        if (notification == TB_THUMBTRACK) {
            adjusting_seek_ = true;
            return;
        }
        adjusting_seek_ = false;
        seek_to(std::min(ticks, duration_ - 1));
    }

    void seek_to(LONGLONG ticks) {
        if (seek_in_progress_) {
            queued_seek_ = ticks;
            return;
        }
        PROPVARIANT position{};
        position.vt = VT_I8;
        position.hVal.QuadPart = ticks;
        auto hr = player_->SetPosition(MFP_POSITIONTYPE_100NS, &position);
        if (SUCCEEDED(hr)) seek_in_progress_ = ticks;
        else seek_completed(hr);
    }

    void seek_completed(HRESULT hr) {
        seek_in_progress_.reset();
        if (std::exchange(restoring_position_, false)) {
            set_playing(true);
            return;
        }
        auto next = std::exchange(queued_seek_, std::nullopt);
        if (FAILED(hr)) status(L"Перемотка недоступна для этого потока.");
        if (next && SUCCEEDED(hr)) seek_to(*next);
        else {
            update_progress();
            if (SUCCEEDED(hr)) update_discord(true);
        }
    }

    void change_volume() {
        auto slider = control(volume_id);
        int level = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
        if (level == volume_) return;
        auto hr = player_ ? player_->SetVolume(player_volume(level)) : S_OK;
        if (FAILED(hr)) {
            SendMessageW(slider, TBM_SETPOS, TRUE, volume_);
            status(L"Не удалось изменить громкость.");
            return;
        }
        volume_ = level;
        if (local_file_.empty()) save_volume(volume_);
        update_volume_label();
    }

    void update_volume_label() const {
        set(volume_label_id, std::to_wstring(volume_) + L"%");
    }

    void exit_test(int exit_code) {
        KillTimer(hwnd_, test_timer);
        release_player();
        PostQuitMessage(exit_code);
    }

    void media_event(INT_PTR event) {
        if (event < 0) {
            if (verify_) {
                exit_test(5);
                return;
            }
            release_player();
            reset_wave();
            status(media_error(static_cast<HRESULT>(event)));
        } else if (event == MFP_EVENT_TYPE_MEDIAITEM_SET) {
            if (auto start = std::exchange(resume_position_, 0LL); start > 0) {
                auto length = media_time(&IMFPMediaPlayer::GetDuration);
                if (length == 0 || start < length - ticks_per_second) {
                    restoring_position_ = true;
                    seek_to(start);
                    return;
                }
            }
            set_playing(true);
        } else if (event == MFP_EVENT_TYPE_PLAY) {
            if (!ready_) {
                duration_ = media_time(&IMFPMediaPlayer::GetDuration);
                set(duration_id, duration_ > 0 ? time_label(duration_) : L"—");
                if (duration_ > 0) {
                    auto step = static_cast<int>(std::clamp(5.0 * ticks_per_second / duration_ * seek_steps,
                                                           1.0, static_cast<double>(seek_steps)));
                    SendMessageW(control(seek_id), TBM_SETLINESIZE, 0, step);
                    SendMessageW(control(seek_id), TBM_SETPAGESIZE, 0, step);
                }
            }
            ready_ = playing_ = true;
            update_discord();
            InvalidateRect(control(links_id), nullptr, FALSE);
            update_progress_timer();
            update_progress();
            set(pause_id, L"Пауза");
            status(L"Воспроизведение");
            if (wave_playing_ && !wave_started_) {
                queue_feedback("trackStarted", 0);
                wave_started_ = true;
            }
            if (wave_playing_ && wave_play_since_ == std::chrono::steady_clock::time_point{}) {
                wave_play_since_ = std::chrono::steady_clock::now();
            }
        } else if (event == MFP_EVENT_TYPE_PAUSE) {
            playing_ = false;
            update_discord();
            InvalidateRect(control(links_id), nullptr, FALSE);
            stop_wave_clock();
            update_progress_timer();
            update_progress();
            set(pause_id, L"Продолжить");
            status(L"Пауза");
        } else if (event == MFP_EVENT_TYPE_PLAYBACK_ENDED) {
            if (verify_) {
                exit_test(7);
                return;
            }
            feedback_end("trackFinished");
            release_player();
            if (last_track_) last_track_->position = 0;
            status(L"Трек закончился.");
            advance_pending_ = wave_playing_ || repeat_track() || has_next_track();
            if (advance_pending_) advance_playback();
        }
        enable_controls();
        flush_feedback();
        refresh_likes();
    }

    void queue_feedback(std::string type, double seconds) {
        pending_feedback_.push_back([batch = wave_history_[wave_cursor_].batch->batch, track = current_.id,
                                     type = std::move(type), seconds](const Music& music) {
            music.wave_event(type, batch, track, seconds);
        });
    }

    void feedback_end(std::string type) {
        if (wave_playing_ && wave_started_) {
            stop_wave_clock();
            queue_feedback(std::move(type), wave_played_seconds_);
            wave_started_ = false;
        }
    }

    void stop_wave_clock() {
        if (wave_play_since_ == std::chrono::steady_clock::time_point{}) return;
        wave_played_seconds_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - wave_play_since_).count();
        wave_play_since_ = {};
    }

    static void send_feedback(const Music& music, const std::vector<std::function<void(const Music&)>>& events) {
        for (auto& event : events) {
            try { event(music); } catch (const std::exception&) {
                // A rejected feedback report must not interrupt the music.
            }
        }
    }

    void flush_feedback() {
        if (job_ != Job::none || token_.empty() || pending_feedback_.empty()) return;
        Music music(token_);
        auto events = std::exchange(pending_feedback_, {});
        work(Job::action, L"", [music, events = std::move(events)](std::stop_token) -> Finish {
            send_feedback(music, events);
            return {};
        });
    }

    void begin_wave() {
        feedback_end("skip");
        auto events = std::exchange(pending_feedback_, {});
        release_player();
        resume_available_ = false;
        reset_wave();
        queue_.reset();
        show_player_idle();
        Music music(token_);
        work(Job::action, L"Запускаем «Мою волну»…", [this, music, events = std::move(events)](std::stop_token) {
            send_feedback(music, events);
            auto wave = music.wave();
            try { music.wave_event("radioStarted", wave.batch); } catch (const std::exception&) {}
            auto audio = music.stream(wave.tracks.front().id);
            return [this, wave = std::move(wave), audio = std::move(audio)]() mutable {
                accept_wave(std::move(wave), std::move(audio));
            };
        });
    }

    void accept_wave(Wave wave, Audio audio) {
        play_wave_position({std::make_shared<Wave>(std::move(wave)), 0}, std::move(audio), {}, true);
    }

    void play_wave_position(WavePosition position, Audio audio, std::optional<size_t> cursor, bool show_wave = false) {
        bool different_batch = wave_history_.empty() || wave_history_[wave_cursor_].batch != position.batch;
        if (cursor) {
            wave_cursor_ = *cursor;
        } else {
            if (!wave_history_.empty()) wave_history_.resize(wave_cursor_ + 1);
            wave_history_.push_back(position);
            if (wave_history_.size() > wave_history_limit) wave_history_.pop_front();
            wave_cursor_ = wave_history_.size() - 1;
        }
        if (show_wave || (view_ == View::wave && different_batch)) {
            display_tracks(View::wave, L"Моя волна", {position.batch->tracks, false}, 0);
        }
        if (view_ == View::wave) SendMessageW(control(links_id), LB_SETCURSEL, position.index, 0);
        start_audio(position.batch->tracks.at(position.index), std::move(audio), true);
    }

    void open_wave_item(WavePosition position, std::optional<size_t> cursor = {}) {
        auto loading = cursor && *cursor < wave_cursor_ ? L"Получаем предыдущий трек…" : L"Получаем трек…";
        feedback_end("skip");
        auto events = std::exchange(pending_feedback_, {});
        wave_playing_ = false;
        release_player();
        show_player_idle();
        Music music(token_);
        work(Job::action, loading, [this, music, events = std::move(events), position, cursor](std::stop_token) {
            send_feedback(music, events);
            auto audio = music.stream(position.batch->tracks.at(position.index).id);
            return [this, position, cursor, audio = std::move(audio)]() mutable {
                play_wave_position(std::move(position), std::move(audio), cursor);
            };
        });
    }

    void advance_wave() {
        if (job_ != Job::none || !wave_playing_) return;
        if (wave_cursor_ + 1 < wave_history_.size()) {
            auto cursor = wave_cursor_ + 1;
            open_wave_item(wave_history_[cursor], cursor);
            return;
        }
        const auto& position = wave_history_[wave_cursor_];
        if (position.index + 1 < position.batch->tracks.size()) {
            open_wave_item({position.batch, position.index + 1});
            return;
        }
        feedback_end("skip");
        auto events = std::exchange(pending_feedback_, {});
        auto previous = current_.id;
        wave_playing_ = false;
        release_player();
        show_player_idle();
        Music music(token_);
        work(Job::action, L"Продолжаем «Мою волну»…", [this, music, previous, events = std::move(events)](std::stop_token) {
            send_feedback(music, events);
            auto wave = music.wave(previous);
            auto audio = music.stream(wave.tracks.front().id);
            return [this, wave = std::move(wave), audio = std::move(audio)]() mutable {
                play_wave_position({std::make_shared<Wave>(std::move(wave)), 0}, std::move(audio), {});
            };
        });
    }
};
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    int argc = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring local_file;
    bool verify = false;
    if (args && argc == 3) {
        verify = std::wstring_view(args[1]) == L"--verify-audio";
        if (verify || std::wstring_view(args[1]) == L"--audio-test") local_file = args[2];
    }
    if (args) LocalFree(args);

    auto hr = RoInitialize(RO_INIT_SINGLETHREADED);
    if (FAILED(hr)) return 3;
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    if (!InitCommonControlsEx(&controls)) {
        RoUninitialize();
        return 3;
    }
    int code = 3;
    {
        App app(std::move(local_file), verify);
        if (app.create(instance, show)) code = app.run();
    }
    RoUninitialize();
    return code;
}
