#include "CursorTracker.hpp"

#include <cassert>
#include <chrono>

int main() {
    CursorTracker tracker;
    const auto    start = CursorTracker::Clock::time_point{};

    for (std::size_t index = 0; index < CursorTracker::CAPACITY + 44; ++index) {
        tracker.record(
            Vector2D{static_cast<double>(index), static_cast<double>(index * 2)},
            start + std::chrono::milliseconds{index});
    }

    assert(tracker.size() == CursorTracker::CAPACITY);
    assert(tracker.totalSamples() == CursorTracker::CAPACITY + 44);
    assert(tracker.latest() != nullptr);
    assert(tracker.latest()->position == Vector2D{299.0, 598.0});
}

