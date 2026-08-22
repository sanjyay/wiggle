#include "ShakeDetector.hpp"

#include <algorithm>
#include <cmath>

ShakeDetector::ShakeDetector() : ShakeDetector(Settings{}) {}

ShakeDetector::ShakeDetector(Settings settings) : settings_(settings) {}

bool ShakeDetector::sameDirection(double previous, double current) {
    constexpr double tolerance = 1.0;
    return (previous >= -tolerance && current >= -tolerance) || (previous <= tolerance && current <= tolerance);
}

bool ShakeDetector::update(Vector2D position, Clock::time_point timestamp) {
    while (!history_.empty() && timestamp - history_.front().timestamp >= settings_.historyWindow)
        history_.pop_front();

    if (!history_.empty() && history_.back().position.distance(position) < settings_.noiseThreshold)
        return false;

    if (history_.size() >= 2) {
        auto&       last     = history_.back();
        const auto& previous = history_[history_.size() - 2];
        const auto  oldDelta = last.position - previous.position;
        const auto  newDelta = position - last.position;

        if (sameDirection(oldDelta.x, newDelta.x) && sameDirection(oldDelta.y, newDelta.y)) {
            last = Sample{.position = position, .timestamp = timestamp};
            return false;
        }
    }

    if (history_.size() == settings_.maximumSamples)
        history_.pop_front();
    history_.push_back(Sample{.position = position, .timestamp = timestamp});

    if (history_.size() < 3)
        return false;

    double left       = history_.front().position.x;
    double right      = left;
    double top        = history_.front().position.y;
    double bottom     = top;
    double pathLength = 0.0;

    for (std::size_t index = 1; index < history_.size(); ++index) {
        pathLength += history_[index - 1].position.distance(history_[index].position);
        left        = std::min(left, history_[index].position.x);
        right       = std::max(right, history_[index].position.x);
        top         = std::min(top, history_[index].position.y);
        bottom      = std::max(bottom, history_[index].position.y);
    }

    const double diagonal = std::hypot(right - left, bottom - top);
    if (diagonal < settings_.minimumTravel)
        return false;

    if (pathLength / diagonal <= settings_.sensitivity)
        return false;

    reset();
    return true;
}

void ShakeDetector::reset() {
    history_.clear();
}

void ShakeDetector::setSensitivity(double sensitivity) {
    settings_.sensitivity = std::max(sensitivity, 1.0);
    reset();
}

std::size_t ShakeDetector::retainedSamples() const {
    return history_.size();
}
