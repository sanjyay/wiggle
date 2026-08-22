#include "CursorTracker.hpp"

#include <chrono>

namespace {
bool require(bool condition) {
    return condition;
}
} // namespace

int main() {
    CursorTracker tracker;
    const auto    start = CursorTracker::Clock::time_point{};

    for (std::size_t index = 0; index < CursorTracker::CAPACITY + 44; ++index) {
        tracker.record(
            Vector2D{static_cast<double>(index), static_cast<double>(index * 2)},
            start + std::chrono::milliseconds{index});
    }

    if (!require(tracker.size() == CursorTracker::CAPACITY) || !require(tracker.totalSamples() == CursorTracker::CAPACITY + 44) ||
        !require(tracker.latest() != nullptr) || !require(tracker.latest()->position == Vector2D{299.0, 598.0}))
        return 1;

    return 0;
}
