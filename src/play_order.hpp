#pragma once

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <optional>
#include <random>
#include <vector>

enum class Repeat { off, collection, track };

inline Repeat next_repeat(Repeat mode) {
    switch (mode) {
    case Repeat::off: return Repeat::collection;
    case Repeat::collection: return Repeat::track;
    case Repeat::track: return Repeat::off;
    }
    return Repeat::off;
}

struct ShuffleOrder {
    std::vector<size_t> tracks;
    size_t cursor = 0;

    ShuffleOrder(size_t count, std::optional<size_t> current) : tracks(count) {
        std::iota(tracks.begin(), tracks.end(), size_t{0});
        if (current) std::swap(tracks.at(*current), tracks.front());
        static thread_local std::mt19937 random(std::random_device{}());
        std::shuffle(tracks.begin() + (current ? 1 : 0), tracks.end(), random);
    }

    std::optional<size_t> step(bool forward, bool wrap) const {
        if (!forward) return cursor > 0 ? std::optional(cursor - 1) : std::nullopt;
        if (cursor + 1 < tracks.size()) return cursor + 1;
        return wrap ? std::optional<size_t>(0) : std::nullopt;
    }
};
