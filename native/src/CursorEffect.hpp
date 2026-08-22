#pragma once

#include <chrono>

#include <aquamarine/buffer/Buffer.hpp>
#include <hyprland/src/helpers/math/Math.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>

class CursorEffect {
  public:
    using Clock = std::chrono::steady_clock;

    struct Settings {
        double                    initialScale = 3.0;
        double                    scaleStep    = 1.0;
        double                    maximumScale = 4.0;
        std::chrono::milliseconds transition{200};
        std::chrono::milliseconds hold{2000};
    };

    CursorEffect();
    explicit CursorEffect(Settings settings);
    ~CursorEffect();

    bool trigger(Clock::time_point now = Clock::now());
    void tick(Clock::time_point now = Clock::now());
    void restore();
    void setMaximumScale(double maximumScale);

    [[nodiscard]] bool active() const;
    [[nodiscard]] double scale() const;

  private:
    void beginTransition(double target, Clock::time_point now);
    void apply(double scale);
    void abortForCursorChange();

    [[nodiscard]] bool cursorStateIsOwned() const;
    [[nodiscard]] static double easeInOutCubic(double progress);

    Settings settings_;

    SP<Aquamarine::IBuffer> originalBuffer_;
    Vector2D                originalHotspot_;
    float                   originalBufferScale_ = 1.0F;
    Vector2D                appliedHotspot_;
    float                   appliedBufferScale_ = 1.0F;

    Clock::time_point transitionStarted_{};
    Clock::time_point decayAt_{};
    double            transitionFrom_ = 1.0;
    double            targetScale_    = 1.0;
    double            currentScale_   = 1.0;
    bool              softwareLocked_ = false;
    bool              active_         = false;
};
