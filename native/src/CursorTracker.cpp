#include "CursorTracker.hpp"

#include <algorithm>

void CursorTracker::record(Vector2D position, Clock::time_point timestamp) {
    samples_[head_] = Sample{.position = position, .timestamp = timestamp};
    head_           = (head_ + 1) % CAPACITY;
    size_           = std::min(size_ + 1, CAPACITY);
    ++totalSamples_;
}

std::size_t CursorTracker::size() const {
    return size_;
}

std::size_t CursorTracker::totalSamples() const {
    return totalSamples_;
}

const CursorTracker::Sample* CursorTracker::latest() const {
    if (size_ == 0)
        return nullptr;

    const auto index = (head_ + CAPACITY - 1) % CAPACITY;
    return &samples_[index];
}

