#pragma once

#include <new>
#include <roapi.h>
#include <systemmediatransportcontrolsinterop.h>
// MinGW defines WinRT boolean and BYTE as the same C++ type.
// Skip the unused duplicate IReference specialization in its generated header.
#ifdef __MINGW32__
#define ____FIReference_1_boolean_INTERFACE_DEFINED__
#endif
#include <windows.media.h>
#include <winstring.h>
#include <wrl/client.h>
#include <string>

namespace media_keys {
using namespace ABI::Windows::Media;
using Pressed = ABI::Windows::Foundation::ITypedEventHandler<
    SystemMediaTransportControls*, SystemMediaTransportControlsButtonPressedEventArgs*>;

class ButtonHandler final : public Pressed {
public:
    ButtonHandler(HWND window, UINT message) : window_(window), message_(message) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (!object) return E_POINTER;
        *object = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(Pressed) && iid != __uuidof(IAgileObject)) return E_NOINTERFACE;
        *object = static_cast<Pressed*>(this);
        AddRef();
        return S_OK;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        auto remaining = InterlockedDecrement(&refs_);
        if (!remaining) delete this;
        return remaining;
    }

    HRESULT STDMETHODCALLTYPE Invoke(ISystemMediaTransportControls*, ISystemMediaTransportControlsButtonPressedEventArgs* args) override {
        SystemMediaTransportControlsButton button{};
        if (SUCCEEDED(args->get_Button(&button))) PostMessageW(window_, message_, button, 0);
        return S_OK;
    }

private:
    volatile LONG refs_ = 1;
    HWND window_;
    UINT message_;
};

class Session {
public:
    bool connect(HWND window, UINT message) {
        Microsoft::WRL::ComPtr<ISystemMediaTransportControlsInterop> interop;
        constexpr wchar_t type[] = L"Windows.Media.SystemMediaTransportControls";
        HSTRING name{};
        HSTRING_HEADER header{};
        auto hr = WindowsCreateStringReference(type, ARRAYSIZE(type) - 1, &header, &name);
        if (FAILED(hr)) return false;
        hr = RoGetActivationFactory(name, IID_PPV_ARGS(interop.GetAddressOf()));
        if (FAILED(hr)) return false;
        hr = interop->GetForWindow(window, IID_PPV_ARGS(controls_.GetAddressOf()));
        if (FAILED(hr)) return false;
        auto handler = new (std::nothrow) ButtonHandler(window, message);
        if (!handler) {
            controls_.Reset();
            return false;
        }
        hr = controls_->add_ButtonPressed(handler, &event_);
        handler->Release();
        if (FAILED(hr)) {
            controls_.Reset();
            return false;
        }
        controls_->put_IsEnabled(false);
        return true;
    }

    ~Session() { close(); }

    void close() {
        if (!controls_) return;
        controls_->put_IsEnabled(false);
        controls_->remove_ButtonPressed(event_);
        controls_.Reset();
    }

    void describe(const std::wstring& title, const std::wstring& artist) const {
        if (!controls_) return;
        Microsoft::WRL::ComPtr<ISystemMediaTransportControlsDisplayUpdater> updater;
        Microsoft::WRL::ComPtr<IMusicDisplayProperties> music;
        if (FAILED(controls_->get_DisplayUpdater(updater.GetAddressOf()))) return;
        if (FAILED(updater->put_Type(MediaPlaybackType_Music))) return;
        if (FAILED(updater->get_MusicProperties(music.GetAddressOf()))) return;
        HSTRING title_ref{}, artist_ref{};
        HSTRING_HEADER title_header{}, artist_header{};
        if (SUCCEEDED(WindowsCreateStringReference(title.c_str(), static_cast<UINT32>(title.size()), &title_header, &title_ref))) {
            music->put_Title(title_ref);
        }
        if (SUCCEEDED(WindowsCreateStringReference(artist.c_str(), static_cast<UINT32>(artist.size()), &artist_header, &artist_ref))) {
            music->put_Artist(artist_ref);
        }
        updater->Update();
    }

    void update(bool playable, bool playing, bool next, bool previous) {
        if (!controls_) return;
        unsigned state = (playable ? 1u : 0u) | (playing ? 2u : 0u) | (next ? 4u : 0u) | (previous ? 8u : 0u);
        if (state == state_) return;
        state_ = state;
        controls_->put_IsPlayEnabled(playable);
        controls_->put_IsPauseEnabled(playable);
        controls_->put_IsNextEnabled(next);
        controls_->put_IsPreviousEnabled(previous);
        auto status = playing ? MediaPlaybackStatus_Playing : playable ? MediaPlaybackStatus_Paused : MediaPlaybackStatus_Closed;
        controls_->put_PlaybackStatus(status);
        controls_->put_IsEnabled(playable);
    }

    explicit operator bool() const { return controls_.Get() != nullptr; }

private:
    Microsoft::WRL::ComPtr<ISystemMediaTransportControls> controls_;
    EventRegistrationToken event_{};
    unsigned state_ = 0;
};
}
