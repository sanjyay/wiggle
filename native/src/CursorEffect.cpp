#include "CursorEffect.hpp"

#include <algorithm>
#include <cmath>

#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/pointer/PointerManager.hpp>

namespace {
constexpr double SCALE_EPSILON = 0.0001;
}

CursorEffect::CursorEffect() : CursorEffect(Settings{}) {}

CursorEffect::CursorEffect(Settings settings) : settings_(settings) {}

CursorEffect::~CursorEffect() {
    restore();
}

bool CursorEffect::trigger(Clock::time_point now) {
    if (!active_) {
        const auto& cursor = Pointer::mgr()->currentCursorImage();
        if (!cursor.pBuffer || cursor.surface) {
            Log::logger->log(Log::WARN, "[wiggle-native] cursor is not backed by a compositor buffer; native effect skipped");
            return false;
        }

        originalBuffer_      = cursor.pBuffer;
        originalHotspot_     = cursor.hotspot;
        originalBufferScale_ = cursor.scale;
        appliedHotspot_      = originalHotspot_;
        appliedBufferScale_  = originalBufferScale_;
        currentScale_        = 1.0;
        targetScale_         = 1.0;
        active_              = true;

        Pointer::mgr()->lockSoftwareAll();
        softwareLocked_ = true;
        beginTransition(std::min(settings_.initialScale, settings_.maximumScale), now);
    } else {
        beginTransition(std::min(targetScale_ + settings_.scaleStep, settings_.maximumScale), now);
    }

    decayAt_ = now + settings_.hold;
    return true;
}

void CursorEffect::tick(Clock::time_point now) {
    if (!active_)
        return;

    if (!cursorStateIsOwned()) {
        abortForCursorChange();
        return;
    }

    if (now >= decayAt_ && targetScale_ != 1.0)
        beginTransition(1.0, now);

    const auto elapsed  = now - transitionStarted_;
    const auto duration = std::chrono::duration<double>(settings_.transition).count();
    const auto progress = duration > 0.0 ? std::clamp(std::chrono::duration<double>(elapsed).count() / duration, 0.0, 1.0) : 1.0;
    const auto eased    = easeInOutCubic(progress);
    currentScale_       = transitionFrom_ + (targetScale_ - transitionFrom_) * eased;
    apply(currentScale_);

    if (progress >= 1.0 && targetScale_ == 1.0)
        restore();
}

void CursorEffect::restore() {
    if (!active_ && !softwareLocked_)
        return;

    if (active_ && cursorStateIsOwned())
        Pointer::mgr()->setCursorBuffer(originalBuffer_, originalHotspot_, originalBufferScale_);

    if (softwareLocked_)
        Pointer::mgr()->unlockSoftwareAll();

    originalBuffer_.reset();
    softwareLocked_      = false;
    active_              = false;
    transitionFrom_      = 1.0;
    targetScale_         = 1.0;
    currentScale_        = 1.0;
    originalBufferScale_ = 1.0F;
    appliedBufferScale_  = 1.0F;
}

void CursorEffect::setMaximumScale(double maximumScale) {
    settings_.maximumScale = std::max(maximumScale, 1.0);
    settings_.initialScale = std::min(settings_.initialScale, settings_.maximumScale);
    if (active_ && targetScale_ > settings_.maximumScale)
        beginTransition(settings_.maximumScale, Clock::now());
}

bool CursorEffect::active() const {
    return active_;
}

double CursorEffect::scale() const {
    return currentScale_;
}

void CursorEffect::beginTransition(double target, Clock::time_point now) {
    transitionFrom_    = currentScale_;
    targetScale_       = std::clamp(target, 1.0, settings_.maximumScale);
    transitionStarted_ = now;
}

void CursorEffect::apply(double scale) {
    const auto bufferScale = static_cast<float>(static_cast<double>(originalBufferScale_) / scale);
    const auto hotspot     = originalHotspot_ * scale;

    if (std::abs(static_cast<double>(bufferScale - appliedBufferScale_)) < SCALE_EPSILON && hotspot == appliedHotspot_)
        return;

    Pointer::mgr()->setCursorBuffer(originalBuffer_, hotspot, bufferScale);
    appliedHotspot_     = hotspot;
    appliedBufferScale_ = bufferScale;
}

void CursorEffect::abortForCursorChange() {
    Log::logger->log(Log::INFO, "[wiggle-native] cursor changed during effect; leaving the new cursor untouched");

    if (softwareLocked_)
        Pointer::mgr()->unlockSoftwareAll();

    originalBuffer_.reset();
    softwareLocked_ = false;
    active_         = false;
    targetScale_    = 1.0;
    currentScale_   = 1.0;
}

bool CursorEffect::cursorStateIsOwned() const {
    const auto& cursor = Pointer::mgr()->currentCursorImage();
    return cursor.pBuffer == originalBuffer_ && !cursor.surface && cursor.hotspot == appliedHotspot_ &&
        std::abs(static_cast<double>(cursor.scale - appliedBufferScale_)) < SCALE_EPSILON;
}

double CursorEffect::easeInOutCubic(double progress) {
    if (progress < 0.5)
        return 4.0 * progress * progress * progress;

    const auto shifted = -2.0 * progress + 2.0;
    return 1.0 - shifted * shifted * shifted / 2.0;
}
