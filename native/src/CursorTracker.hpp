#pragma once

#include <array>
#include <chrono>
#include <cstddef>

#include <hyprland/src/helpers/math/Math.hpp>

class CursorTracker {
  public:
    using Clock = std::chrono::steady_clock;

    struct Sample {
        Vector2D         position;
        Clock::time_point timestamp;
    };

    static constexpr std::size_t CAPACITY = 256;

    void record(Vector2D position, Clock::time_point timestamp = Clock::now());

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t totalSamples() const;
    [[nodiscard]] const Sample* latest() const;

  private:
    std::array<Sample, CAPACITY> samples_{};
    std::size_t                  head_         = 0;
    std::size_t                  size_         = 0;
    std::size_t                  totalSamples_ = 0;
};

