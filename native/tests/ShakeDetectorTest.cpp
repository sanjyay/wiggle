#include "ShakeDetector.hpp"

#include <chrono>

namespace {
using namespace std::chrono_literals;

bool feedShake(ShakeDetector& detector, std::chrono::milliseconds spacing) {
    const auto start = ShakeDetector::Clock::time_point{};
    bool       activated = false;
    for (std::size_t index = 0; index < 7; ++index) {
        const double x = index % 2 == 0 ? 0.0 : 60.0;
        activated = detector.update(Vector2D{x, 0.0}, start + spacing * index) || activated;
    }
    return activated;
}
} // namespace

int main() {
    {
        ShakeDetector detector;
        const auto    start = ShakeDetector::Clock::time_point{};
        for (std::size_t index = 0; index < 100; ++index)
            if (detector.update(Vector2D{static_cast<double>(index % 2), 0.0}, start + 5ms * index))
                return 1;
    }

    {
        ShakeDetector detector;
        const auto    start = ShakeDetector::Clock::time_point{};
        for (std::size_t index = 0; index < 20; ++index)
            if (detector.update(Vector2D{static_cast<double>(index * 10), 0.0}, start + 10ms * index))
                return 2;
    }

    {
        ShakeDetector detector;
        if (!feedShake(detector, 8ms) || detector.retainedSamples() > 2)
            return 3;
    }

    {
        ShakeDetector detector;
        if (!feedShake(detector, 16ms))
            return 4;
    }

    {
        ShakeDetector detector;
        const auto    start = ShakeDetector::Clock::time_point{};
        for (std::size_t index = 0; index < 400; ++index)
            detector.update(Vector2D{static_cast<double>(index % 3) * 3.0, 0.0}, start + 2ms * index);
        if (detector.retainedSamples() > ShakeDetector::Settings{}.maximumSamples)
            return 5;
    }

    return 0;
}
