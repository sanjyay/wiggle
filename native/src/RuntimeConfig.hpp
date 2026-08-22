#pragma once

#include <functional>
#include <string>

struct wl_event_source;

class RuntimeConfig {
  public:
    struct Values {
        bool   enabled     = true;
        double sensitivity = 4.0;
        double maximumScale = 4.0;
    };

    using Callback = std::function<void(const Values&)>;

    explicit RuntimeConfig(Callback callback);
    ~RuntimeConfig();

    RuntimeConfig(const RuntimeConfig&) = delete;
    RuntimeConfig& operator=(const RuntimeConfig&) = delete;

    bool start();

    [[nodiscard]] const std::string& path() const;

  private:
    static int onReadable(int fd, unsigned int mask, void* data);

    void drainEvents();
    void readCurrent();

    Callback         callback_;
    std::string      directory_;
    std::string      filename_ = "wiggle-native.conf";
    std::string      path_;
    int              inotifyFd_ = -1;
    int              watch_ = -1;
    wl_event_source* eventSource_ = nullptr;
};

