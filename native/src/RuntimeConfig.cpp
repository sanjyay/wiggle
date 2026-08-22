#include "RuntimeConfig.hpp"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-server-core.h>

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>

RuntimeConfig::RuntimeConfig(Callback callback) : callback_(std::move(callback)) {}

RuntimeConfig::~RuntimeConfig() {
    if (eventSource_)
        wl_event_source_remove(eventSource_);
    if (watch_ >= 0 && inotifyFd_ >= 0)
        inotify_rm_watch(inotifyFd_, watch_);
    if (inotifyFd_ >= 0)
        close(inotifyFd_);
}

bool RuntimeConfig::start() {
    const char* runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
    if (!runtimeDirectory || runtimeDirectory[0] != '/')
        return false;

    directory_ = runtimeDirectory;
    path_      = directory_ + "/" + filename_;

    inotifyFd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotifyFd_ < 0)
        return false;

    watch_ = inotify_add_watch(inotifyFd_, directory_.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
    if (watch_ < 0)
        return false;

    eventSource_ = wl_event_loop_add_fd(g_pCompositor->m_wlEventLoop, inotifyFd_, WL_EVENT_READABLE, onReadable, this);
    if (!eventSource_)
        return false;

    readCurrent();
    return true;
}

const std::string& RuntimeConfig::path() const {
    return path_;
}

int RuntimeConfig::onReadable(int, unsigned int, void* data) {
    static_cast<RuntimeConfig*>(data)->drainEvents();
    return 0;
}

void RuntimeConfig::drainEvents() {
    alignas(inotify_event) std::array<char, 4096> buffer{};

    while (true) {
        const auto bytes = read(inotifyFd_, buffer.data(), buffer.size());
        if (bytes < 0 && errno == EINTR)
            continue;
        if (bytes <= 0)
            break;

        std::size_t offset = 0;
        while (offset + sizeof(inotify_event) <= static_cast<std::size_t>(bytes)) {
            const auto* event = reinterpret_cast<const inotify_event*>(buffer.data() + offset);
            if (event->len > 0 && filename_ == event->name && (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)))
                readCurrent();
            offset += sizeof(inotify_event) + event->len;
        }
    }
}

void RuntimeConfig::readCurrent() {
    struct stat metadata {};
    if (lstat(path_.c_str(), &metadata) != 0)
        return;

    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != getuid() || metadata.st_size < 0 || metadata.st_size > 256) {
        Log::logger->log(Log::WARN, "[wiggle-native] refusing unsafe runtime configuration at {}", path_);
        return;
    }

    std::ifstream input{path_};
    int           version = 0;
    int           enabled = 0;
    Values        values;
    std::string   trailing;

    if (!(input >> version >> enabled >> values.sensitivity >> values.maximumScale) || (input >> trailing) || version != 1 || (enabled != 0 && enabled != 1) ||
        values.sensitivity < 1.0 || values.sensitivity > 10.0 || values.maximumScale < 1.0 || values.maximumScale > 8.0) {
        Log::logger->log(Log::WARN, "[wiggle-native] ignoring invalid runtime configuration at {}", path_);
        return;
    }

    values.enabled = enabled == 1;
    callback_(values);
}

