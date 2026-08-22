#pragma once

#include <chrono>
#include <cstddef>
#include <deque>

#include <hyprland/src/helpers/math/Math.hpp>

class ShakeDetector {
  // Original MIT implementation informed by KWin's interaction model:
  // compare recent path length with its bounds and coalesce direction runs.
  // No KDE source code is incorporated here.
  public:
    using Clock = std::chrono::steady_clock;

    struct Settings {
        std::chrono::milliseconds historyWindow{1000};
        double                    sensitivity      = 4.0;
        double                    minimumTravel   = 40.0;
        double                    noiseThreshold  = 2.0;
        std::size_t               maximumSamples  = 256;
    };

    ShakeDetector();
    explicit ShakeDetector(Settings settings);

    bool update(Vector2D position, Clock::time_point timestamp = Clock::now());
    void reset();
    void setSensitivity(double sensitivity);

    [[nodiscard]] std::size_t retainedSamples() const;

  private:
    struct Sample {
        Vector2D          position;
        Clock::time_point timestamp;
    };

    [[nodiscard]] static bool sameDirection(double previous, double current);

    Settings           settings_;
    std::deque<Sample> history_;
};
