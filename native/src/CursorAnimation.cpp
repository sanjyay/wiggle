#include "CursorAnimation.hpp"
#include <algorithm>

CursorAnimation::CursorAnimation() : CursorAnimation(Settings{}) {}
CursorAnimation::CursorAnimation(Settings settings) : settings_(settings) {}

void CursorAnimation::trigger(Clock::time_point now) {
  if (!active_) {
    active_ = true;
    finished_ = false;
    currentScale_ = 1.0;
    targetScale_ = 1.0;
  }
  const auto next = targetScale_ <= 1.0 ? settings_.initialScale
                                        : targetScale_ + settings_.scaleStep;
  beginTransition(std::min(next, settings_.maximumScale), now);
  decayAt_ = now + settings_.hold;
}

void CursorAnimation::tick(Clock::time_point now) {
  if (!active_)
    return;
  if (now >= decayAt_ && targetScale_ != 1.0)
    beginTransition(1.0, now);
  const auto duration =
      std::chrono::duration<double>(settings_.transition).count();
  const auto elapsed =
      std::chrono::duration<double>(now - transitionStarted_).count();
  const auto progress =
      duration > 0.0 ? std::clamp(elapsed / duration, 0.0, 1.0) : 1.0;
  const auto eased = easeInOutCubic(progress);
  currentScale_ = transitionFrom_ + (targetScale_ - transitionFrom_) * eased;
  if (progress >= 1.0 && targetScale_ == 1.0) {
    active_ = false;
    finished_ = true;
  }
}

void CursorAnimation::reset() {
  transitionFrom_ = targetScale_ = currentScale_ = 1.0;
  active_ = finished_ = false;
}

void CursorAnimation::setMaximumScale(double maximumScale,
                                      Clock::time_point now) {
  settings_.maximumScale = std::max(maximumScale, 1.0);
  if (active_ && targetScale_ > settings_.maximumScale)
    beginTransition(settings_.maximumScale, now);
}

bool CursorAnimation::active() const { return active_; }
bool CursorAnimation::finished() const { return finished_; }
double CursorAnimation::scale() const { return currentScale_; }
double CursorAnimation::targetScale() const { return targetScale_; }

void CursorAnimation::beginTransition(double target, Clock::time_point now) {
  transitionFrom_ = currentScale_;
  targetScale_ = std::clamp(target, 1.0, settings_.maximumScale);
  transitionStarted_ = now;
}

double CursorAnimation::easeInOutCubic(double progress) {
  if (progress < 0.5)
    return 4.0 * progress * progress * progress;
  const auto shifted = -2.0 * progress + 2.0;
  return 1.0 - shifted * shifted * shifted / 2.0;
}
