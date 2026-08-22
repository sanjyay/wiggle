#include <stdexcept>
#include <string>

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include "CursorTracker.hpp"
#include "ShakeDetector.hpp"

namespace {
constexpr auto PLUGIN_NAME    = "wiggle-native";
constexpr auto PLUGIN_VERSION = "0.1.0-experimental";

HANDLE pluginHandle = nullptr;
CursorTracker cursorTracker;
ShakeDetector shakeDetector;
CHyprSignalListener cursorMoveListener;
} // namespace

APICALL EXPORT std::string PLUGIN_API_VERSION() {
    return HYPRLAND_API_VERSION;
}

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    pluginHandle = handle;

    const std::string compositorABI = __hyprland_api_get_hash();
    const std::string pluginABI     = __hyprland_api_get_client_hash();
    if (compositorABI != pluginABI) {
        HyprlandAPI::addNotification(
            pluginHandle,
            "[Wiggle] Native backend ABI mismatch; refusing to load",
            CHyprColor{1.0F, 0.2F, 0.2F, 1.0F},
            5000.0F);
        throw std::runtime_error("wiggle-native: Hyprland ABI mismatch");
    }

    cursorMoveListener = Event::bus()->m_events.input.mouse.move.listen([](Vector2D position, Event::SCallbackInfo&) {
        cursorTracker.record(position);
        if (shakeDetector.update(position))
            Log::logger->log(Log::INFO, "[wiggle-native] shake detected (rendering disabled at checkpoint C)");
    });

    Log::logger->log(Log::INFO, "[wiggle-native] loaded version {} (ABI {})", PLUGIN_VERSION, pluginABI);
    return {PLUGIN_NAME, "Experimental native cursor wiggle backend", "sanjyay", PLUGIN_VERSION};
}

APICALL EXPORT void PLUGIN_EXIT() {
    cursorMoveListener.reset();

    Log::logger->log(Log::INFO, "[wiggle-native] unloaded version {}", PLUGIN_VERSION);
    pluginHandle = nullptr;
}
