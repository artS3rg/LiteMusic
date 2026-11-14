#include "portable_media.hpp"
#import <MediaPlayer/MediaPlayer.h>
#include <array>
#include <utility>

struct MediaKeys::Impl {
    std::shared_ptr<Handler> handler;
    std::array<MPRemoteCommand*, 5> commands;
    std::array<id, 5> tokens{};
    MediaState state;

    explicit Impl(Handler callback) : handler(std::make_shared<Handler>(std::move(callback))) {
        auto* center = [MPRemoteCommandCenter sharedCommandCenter];
        commands = {center.playCommand, center.pauseCommand, center.togglePlayPauseCommand,
                    center.nextTrackCommand, center.previousTrackCommand};
        constexpr MediaAction actions[] = {MediaAction::play, MediaAction::pause, MediaAction::toggle,
                                            MediaAction::next, MediaAction::previous};
        std::weak_ptr<Handler> target = handler;
        for (size_t i = 0; i < commands.size(); ++i) {
            auto action = actions[i];
            commands[i].enabled = NO;
            tokens[i] = [commands[i] addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (auto callback = target.lock()) (*callback)(action, 0);
                });
                return MPRemoteCommandHandlerStatusSuccess;
            }];
        }
    }

    ~Impl() {
        handler.reset();
        for (size_t i = 0; i < commands.size(); ++i) {
            [commands[i] removeTarget:tokens[i]];
            commands[i].enabled = NO;
        }
        auto* info = [MPNowPlayingInfoCenter defaultCenter];
        info.playbackState = MPNowPlayingPlaybackStateStopped;
        info.nowPlayingInfo = nil;
    }
};

MediaKeys::MediaKeys(Handler handler) : impl_(std::make_unique<Impl>(std::move(handler))) {}
MediaKeys::~MediaKeys() = default;

void MediaKeys::update(MediaState state) {
    // Volume is controlled by LiteMusic and is not displayed in the system card.
    state.volume = 1;
    auto old = impl_->state;
    old.position = state.position;
    if (old == state) return;
    impl_->state = std::move(state);
    const auto& next = impl_->state;
    bool playing = next.status == MediaState::Status::playing;
    bool paused = next.status == MediaState::Status::paused;
    impl_->commands[0].enabled = next.can_play;
    impl_->commands[1].enabled = next.can_pause;
    impl_->commands[2].enabled = next.can_play || next.can_pause;
    impl_->commands[3].enabled = next.can_next;
    impl_->commands[4].enabled = next.can_previous;

    auto* info = [MPNowPlayingInfoCenter defaultCenter];
    if (next.title.empty()) {
        info.playbackState = MPNowPlayingPlaybackStateStopped;
        info.nowPlayingInfo = nil;
        return;
    }
    auto* title = [NSString stringWithUTF8String:next.title.c_str()] ?: @"";
    auto* artist = [NSString stringWithUTF8String:next.artist.c_str()] ?: @"";
    auto* metadata = [NSMutableDictionary dictionaryWithDictionary:@{
        MPMediaItemPropertyTitle: title,
        MPMediaItemPropertyArtist: artist,
        MPNowPlayingInfoPropertyPlaybackRate: @(playing ? 1.0 : 0.0),
        MPNowPlayingInfoPropertyElapsedPlaybackTime: @(next.position)
    }];
    if (next.duration > 0) metadata[MPMediaItemPropertyPlaybackDuration] = @(next.duration);
    info.nowPlayingInfo = metadata;
    info.playbackState = playing ? MPNowPlayingPlaybackStatePlaying :
                         paused ? MPNowPlayingPlaybackStatePaused : MPNowPlayingPlaybackStateStopped;
}

void MediaKeys::seeked(double seconds) {
    auto* info = [MPNowPlayingInfoCenter defaultCenter];
    auto* metadata = [info.nowPlayingInfo mutableCopy];
    if (!metadata) return;
    metadata[MPNowPlayingInfoPropertyElapsedPlaybackTime] = @(seconds);
    info.nowPlayingInfo = metadata;
}
