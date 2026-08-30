#pragma once
#include <chrono>

class CursorAnimation {
public:
  using Clock = std::chrono::steady_clock;
  struct Settings {
    double initialScale = 3.0, scaleStep = 1.0, maximumScale = 4.0;
    std::chrono::milliseconds transition{200}, hold{2000};
  };
  CursorAnimation();
  explicit CursorAnimation(Settings settings);
  void trigger(Clock::time_point now = Clock::now());
  void tick(Clock::time_point now = Clock::now());
  void reset();
  void setMaximumScale(double maximumScale,
                       Clock::time_point now = Clock::now());
  [[nodiscard]] bool active() const;
  [[nodiscard]] bool finished() const;
  [[nodiscard]] double scale() const;
  [[nodiscard]] double targetScale() const;

private:
  void beginTransition(double target, Clock::time_point now);
  [[nodiscard]] static double easeInOutCubic(double progress);
  Settings settings_;
  Clock::time_point transitionStarted_{}, decayAt_{};
  double transitionFrom_ = 1.0, targetScale_ = 1.0, currentScale_ = 1.0;
  bool active_ = false, finished_ = false;
};
