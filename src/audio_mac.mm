#include "audio.hpp"
#include <AVFoundation/AVFoundation.h>
#include <cmath>

struct Player::Impl {
    AVPlayer* player = [AVPlayer new];
    id end_notice = nil;
    bool ended = false;
    bool desired = false;
    double start = 0;
};

Player::Player(bool) : impl_(std::make_unique<Impl>()) {}
Player::~Player() { clear(); }

void Player::open(const std::string& url, double position) {
    clear();
    auto* item = [AVPlayerItem playerItemWithURL:[NSURL URLWithString:[NSString stringWithUTF8String:url.c_str()]]];
    impl_->start = position;
    auto* state = impl_.get();
    impl_->end_notice = [[NSNotificationCenter defaultCenter] addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
        object:item queue:[NSOperationQueue mainQueue] usingBlock:^(NSNotification*) { state->ended = true; }];
    [impl_->player replaceCurrentItemWithPlayerItem:item];
    play(true);
}

void Player::clear() {
    if (impl_->end_notice) {
        [[NSNotificationCenter defaultCenter] removeObserver:impl_->end_notice];
        impl_->end_notice = nil;
    }
    [impl_->player pause];
    [impl_->player replaceCurrentItemWithPlayerItem:nil];
    impl_->desired = false;
    impl_->ended = false;
    impl_->start = 0;
}

void Player::play(bool enabled) {
    impl_->desired = enabled;
    if (enabled && impl_->start == 0) [impl_->player play];
    else [impl_->player pause];
}

void Player::seek(double seconds) {
    [impl_->player seekToTime:CMTimeMakeWithSeconds(seconds, 1000)
            toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero];
}

void Player::volume(double level) { impl_->player.volume = level; }

Playback Player::poll() {
    auto* player = impl_->player;
    auto* item = player.currentItem;
    if (!item) return {};
    if (item.status == AVPlayerItemStatusFailed || player.status == AVPlayerStatusFailed) return {Playback::failed};
    if (impl_->ended) return {Playback::ended};
    if (item.status == AVPlayerItemStatusReadyToPlay && impl_->start > 0) {
        seek(impl_->start);
        impl_->start = 0;
        play(impl_->desired);
    }
    auto real = [](CMTime time) {
        auto value = CMTimeGetSeconds(time);
        return std::isfinite(value) && value >= 0 ? value : 0.0;
    };
    return {player.timeControlStatus == AVPlayerTimeControlStatusPlaying ? Playback::playing :
            item.status == AVPlayerItemStatusReadyToPlay && !impl_->desired ? Playback::paused : Playback::loading,
            real(player.currentTime), real(item.duration)};
}
