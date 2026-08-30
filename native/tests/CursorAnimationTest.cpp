#include "CursorAnimation.hpp"
#include <chrono>
#include <cmath>
#include <cstdlib>

using namespace std::chrono_literals;
static bool near(double a, double b) { return std::abs(a - b) < 0.0001; }
#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition))                                                          \
      return EXIT_FAILURE;                                                     \
  } while (false)

int main() {
  const CursorAnimation::Clock::time_point start{};
  CursorAnimation animation;
  CHECK(!animation.active() && near(animation.scale(), 1.0));
  animation.trigger(start);
  animation.tick(start + 100ms);
  CHECK(near(animation.scale(), 2.0));
  animation.tick(start + 200ms);
  CHECK(near(animation.scale(), 3.0));
  animation.trigger(start + 300ms);
  animation.trigger(start + 400ms);
  CHECK(near(animation.targetScale(), 4.0));
  animation.tick(start + 600ms);
  CHECK(near(animation.scale(), 4.0));
  animation.tick(start + 2399ms);
  CHECK(animation.active());
  animation.tick(start + 2400ms);
  animation.tick(start + 2500ms);
  CHECK(animation.scale() < 4.0 && animation.scale() > 1.0);
  animation.tick(start + 2600ms);
  CHECK(!animation.active() && animation.finished() &&
        near(animation.scale(), 1.0));
  animation.reset();
  CHECK(!animation.active() && !animation.finished());
  animation.trigger(start);
  animation.setMaximumScale(2.0, start + 50ms);
  CHECK(near(animation.targetScale(), 2.0));
}
