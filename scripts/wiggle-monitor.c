/*
 * wiggle-monitor.c — Clean-room KDE-style Shake Detector and Pointer Monitor for Wiggle
 *
 * Implements KDE/KWin-style shake detection semantics:
 *   - 1000ms rolling motion history
 *   - Same-direction motion coalescing
 *   - Total path distance / bounding-box diagonal ratio evaluation (sensitivity = 4.0)
 *   - Minimum diagonal threshold to ignore sensor jitter
 *   - Immediate history reset on trigger to allow continuous fluid re-triggering
 *   - Compositor pointer tracking during active shake effect
 *
 * MIT License
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <signal.h>
#include <time.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <linux/input.h>
#include <libevdev/libevdev.h>

/* ─── Tunable Constants ─── */

#define HISTORY_INTERVAL_MS     1000
#define DEFAULT_SENSITIVITY     4.0
#define MIN_DIAGONAL_COUNTS     40.0
#define MAX_HISTORY_POINTS      512
#define MAX_DEVICES             16
#define TRACK_INTERVAL_MS       16
#define TRACK_ACTIVE_WINDOW_MS  2500
#define IPC_RESPONSE_TIMEOUT_MS 300

/* ─── Data Structures ─── */

typedef struct {
    double  x;
    double  y;
    int64_t timestamp_ms;
} HistoryPoint;

typedef struct {
    HistoryPoint history[MAX_HISTORY_POINTS];
    int          count;
    double       cum_x;
    double       cum_y;
    int64_t      interval_ms;
    double       sensitivity;
    double       min_diagonal;
} ShakeDetector;

typedef struct {
    struct libevdev *dev;
    int              fd;
    int              pending_dx;
    int              pending_dy;
    char             path[280];
} DeviceState;

/* ─── Globals ─── */

static volatile sig_atomic_t g_running = 1;
static ShakeDetector g_detector = {0};

/* ─── Compositor IPC & Fail-Safe ─── */

static bool response_is_ok(const char *response) {
    while (*response == ' ' || *response == '\t' || *response == '\r' || *response == '\n') {
        response++;
    }
    return strncmp(response, "ok", 2) == 0;
}

static bool write_all(int fd, const char *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
        ssize_t written = write(fd, data + offset, length - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        offset += (size_t)written;
    }
    return true;
}

static bool set_compositor_cursor_invisible(bool invisible) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !signature) return false;

    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int length = snprintf(path, sizeof(path), "%s/hypr/%s/.socket.sock",
                          runtime, signature);
    if (length < 0 || (size_t)length >= sizeof(path)) return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
    char request[128];
    int req_len = snprintf(request, sizeof(request),
                           "/eval hl.config({ cursor = { invisible = %s } })",
                           invisible ? "true" : "false");
    if (req_len <= 0 || !write_all(fd, request, (size_t)req_len)) {
        close(fd);
        return false;
    }

    if (shutdown(fd, SHUT_WR) < 0) {
        close(fd);
        return false;
    }

    struct pollfd response_poll = { .fd = fd, .events = POLLIN };
    int ready = poll(&response_poll, 1, IPC_RESPONSE_TIMEOUT_MS);
    if (ready <= 0 || !(response_poll.revents & POLLIN)) {
        close(fd);
        return false;
    }

    char response[128];
    ssize_t used = read(fd, response, sizeof(response) - 1);
    close(fd);
    if (used <= 0) return false;
    response[used] = '\0';
    return response_is_ok(response);
}

static int query_compositor_cursor(int *x, int *y) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !signature) return 0;

    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    int length = snprintf(path, sizeof(path), "%s/hypr/%s/.socket.sock",
                          runtime, signature);
    if (length < 0 || (size_t)length >= sizeof(path)) return 0;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return 0;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return 0;
    }
    static const char request[] = "j/cursorpos";
    if (!write_all(fd, request, sizeof(request) - 1)) {
        close(fd);
        return 0;
    }
    if (shutdown(fd, SHUT_WR) < 0) {
        close(fd);
        return 0;
    }
    struct pollfd response_poll = { .fd = fd, .events = POLLIN };
    int ready = poll(&response_poll, 1, IPC_RESPONSE_TIMEOUT_MS);
    if (ready <= 0 || !(response_poll.revents & POLLIN)) {
        close(fd);
        return 0;
    }
    char response[256];
    ssize_t used = read(fd, response, sizeof(response) - 1);
    close(fd);
    if (used <= 0) return 0;
    response[used] = '\0';

    const char *px = strstr(response, "\"x\"");
    const char *py = strstr(response, "\"y\"");
    if (px && py) {
        const char *cx = strchr(px, ':');
        const char *cy = strchr(py, ':');
        if (cx && cy && sscanf(cx + 1, "%d", x) == 1 && sscanf(cy + 1, "%d", y) == 1) {
            return 1;
        }
    }
    return 0;
}

/* ─── Monotonic Clock ─── */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void emergency_cleanup(void) {
    (void)set_compositor_cursor_invisible(false);
}

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ─── Shake Detector ─── */

static void detector_init(ShakeDetector *det) {
    memset(det, 0, sizeof(*det));
    det->interval_ms = HISTORY_INTERVAL_MS;
    det->sensitivity = DEFAULT_SENSITIVITY;
    det->min_diagonal = MIN_DIAGONAL_COUNTS;
}

static inline bool same_direction(double a, double b) {
    if (a > 0.0 && b > 0.0) return true;
    if (a < 0.0 && b < 0.0) return true;
    if (fabs(a) < 1e-6 && fabs(b) < 1e-6) return true;
    return false;
}

static bool detector_update(ShakeDetector *det, double dx, double dy, int64_t ts) {
    det->cum_x += dx;
    det->cum_y += dy;
    double cur_x = det->cum_x;
    double cur_y = det->cum_y;

    // Prune entries older than the rolling window interval
    int write_idx = 0;
    for (int i = 0; i < det->count; i++) {
        if (ts - det->history[i].timestamp_ms < det->interval_ms) {
            if (write_idx != i) {
                det->history[write_idx] = det->history[i];
            }
            write_idx++;
        }
    }
    det->count = write_idx;

    // Coalesce continuous movement in the same direction into the last point
    if (det->count >= 2) {
        HistoryPoint *last = &det->history[det->count - 1];
        const HistoryPoint *prev = &det->history[det->count - 2];
        if (same_direction(last->x - prev->x, cur_x - last->x) &&
            same_direction(last->y - prev->y, cur_y - last->y)) {
            last->x = cur_x;
            last->y = cur_y;
            last->timestamp_ms = ts;
            return false;
        }
    }

    if (det->count < MAX_HISTORY_POINTS) {
        det->history[det->count].x = cur_x;
        det->history[det->count].y = cur_y;
        det->history[det->count].timestamp_ms = ts;
        det->count++;
    }

    if (det->count < 2) return false;

    double left = det->history[0].x;
    double right = det->history[0].x;
    double top = det->history[0].y;
    double bottom = det->history[0].y;
    double distance = 0;

    for (int i = 1; i < det->count; i++) {
        double d_x = det->history[i].x - det->history[i - 1].x;
        double d_y = det->history[i].y - det->history[i - 1].y;
        distance += sqrt(d_x * d_x + d_y * d_y);

        if (det->history[i].x < left) left = det->history[i].x;
        if (det->history[i].x > right) right = det->history[i].x;
        if (det->history[i].y < top) top = det->history[i].y;
        if (det->history[i].y > bottom) bottom = det->history[i].y;
    }

    double bounds_w = right - left;
    double bounds_h = bottom - top;
    double diagonal = sqrt(bounds_w * bounds_w + bounds_h * bounds_h);

    if (diagonal < det->min_diagonal) {
        return false;
    }

    double shake_factor = distance / diagonal;
    if (shake_factor > det->sensitivity) {
        det->count = 0; // Immediate reset allows continuous rapid re-detection
        return true;
    }

    return false;
}

/* ─── Device Discovery & Hotplug ─── */

static int add_device(const char *path, DeviceState *devices, struct pollfd *pfds, int *active_count) {
    if (*active_count >= MAX_DEVICES) return 0;

    for (int i = 0; i < *active_count; i++) {
        if (strcmp(devices[i].path, path) == 0) return 0; // Already added
    }

    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        close(fd);
        return 0;
    }

    int has_rel = libevdev_has_event_type(dev, EV_REL) &&
                  libevdev_has_event_code(dev, EV_REL, REL_X) &&
                  libevdev_has_event_code(dev, EV_REL, REL_Y);

    int is_touchpad = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X);
    int is_direct = libevdev_has_property(dev, INPUT_PROP_DIRECT);

    // Reject keyboards (devices with alphanumeric text keys)
    int has_keyboard_keys = libevdev_has_event_code(dev, EV_KEY, KEY_A) ||
                           libevdev_has_event_code(dev, EV_KEY, KEY_SPACE) ||
                           libevdev_has_event_code(dev, EV_KEY, KEY_B) ||
                           libevdev_has_event_code(dev, EV_KEY, KEY_Z);

    if (!has_rel || is_touchpad || is_direct || has_keyboard_keys) {
        libevdev_free(dev);
        close(fd);
        return 0;
    }

    fprintf(stderr, "wiggle-monitor: monitoring %s (%s)\n",
            path, libevdev_get_name(dev));

    int idx = *active_count;
    devices[idx].dev = dev;
    devices[idx].fd = fd;
    devices[idx].pending_dx = 0;
    devices[idx].pending_dy = 0;
    snprintf(devices[idx].path, sizeof(devices[idx].path), "%s", path);

    pfds[idx].fd = fd;
    pfds[idx].events = POLLIN;

    (*active_count)++;
    return 1;
}

static void remove_device(DeviceState *devices, struct pollfd *pfds, int *active_count, int index) {
    if (index < 0 || index >= *active_count) return;
    fprintf(stderr, "wiggle-monitor: device %s disconnected\n", devices[index].path);
    libevdev_free(devices[index].dev);
    close(devices[index].fd);
    for (int i = index; i < *active_count - 1; i++) {
        devices[i] = devices[i + 1];
        pfds[i] = pfds[i + 1];
    }
    (*active_count)--;
}

static void discover_all_mice(DeviceState *devices, struct pollfd *pfds, int *active_count) {
    DIR *dir = opendir("/dev/input");
    if (!dir) return;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && *active_count < MAX_DEVICES) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char path[280];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        add_device(path, devices, pfds, active_count);
    }
    closedir(dir);
}

/* ─── Main ─── */

int main(int argc, char *argv[]) {
    // Deliver SIGTERM automatically if parent (Quickshell) terminates/crashes
    prctl(PR_SET_PDEATHSIG, SIGTERM);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    sigaction(SIGQUIT, &sa, NULL);
    sigaction(SIGPIPE, &sa, NULL);

    setvbuf(stdout, NULL, _IOLBF, 0);

    detector_init(&g_detector);

    DeviceState devices[MAX_DEVICES];
    struct pollfd pfds[MAX_DEVICES + 2];
    int active_count = 0;

    if (argc > 1) {
        for (int i = 1; i < argc && active_count < MAX_DEVICES; i++) {
            add_device(argv[i], devices, pfds, &active_count);
        }
    } else {
        discover_all_mice(devices, pfds, &active_count);
    }

    if (active_count == 0) {
        fprintf(stderr, "wiggle-monitor: no mouse devices found on startup, waiting for devices...\n");
    }

    // Setup inotify for hotplug device discovery in /dev/input
    int inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0) {
        inotify_add_watch(inotify_fd, "/dev/input", IN_CREATE | IN_ATTRIB);
    }

    // Monitor stdin for pipe closure / EOF from parent Quickshell process
    int stdin_flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (stdin_flags >= 0) {
        fcntl(STDIN_FILENO, F_SETFL, stdin_flags | O_NONBLOCK);
    }

    fprintf(stderr, "wiggle-monitor: ready (%d device%s)\n",
            active_count, active_count == 1 ? "" : "s");

    int64_t track_until = 0;
    int64_t next_track = 0;
    bool compositor_cursor_hidden = false;

    while (g_running) {
        // Place inotify and stdin at the end of pollfds
        int inotify_idx = active_count;
        pfds[inotify_idx].fd = (inotify_fd >= 0) ? inotify_fd : -1;
        pfds[inotify_idx].events = POLLIN;

        int stdin_idx = active_count + 1;
        pfds[stdin_idx].fd = STDIN_FILENO;
        pfds[stdin_idx].events = POLLIN | POLLHUP | POLLERR;

        int total_pfds = active_count + 2;

        int ret = poll(pfds, total_pfds, -1);

        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // Check if parent closed the pipe (EOF / HUP)
        if (pfds[stdin_idx].revents & (POLLHUP | POLLERR)) {
            break;
        }
        if (pfds[stdin_idx].revents & POLLIN) {
            char cmd_buf[64];
            ssize_t n = read(STDIN_FILENO, cmd_buf, sizeof(cmd_buf) - 1);
            if (n == 0) {
                // EOF: parent process exited
                break;
            }
            if (n > 0) {
                cmd_buf[n] = '\0';
                char *saveptr = NULL;
                for (char *command = strtok_r(cmd_buf, "\r\n", &saveptr);
                    command != NULL;
                    command = strtok_r(NULL, "\r\n", &saveptr)) {
                    if (strcmp(command, "HIDE") == 0) {
                        /* Track conservatively until SHOW is positively acknowledged. */
                        compositor_cursor_hidden = true;
                        bool hidden = set_compositor_cursor_invisible(true);
                        printf(hidden ? "HIDDEN\n" : "HIDE_FAILED\n");
                        fflush(stdout);
                    } else if (strcmp(command, "SHOW") == 0) {
                        bool shown = set_compositor_cursor_invisible(false);
                        if (shown) compositor_cursor_hidden = false;
                        printf(shown ? "SHOWN\n" : "SHOW_FAILED\n");
                        fflush(stdout);
                    }
                }
            }
        }

        int64_t ts = now_ms();
        bool handoff_pending = false;

        // Handle inotify events (new mouse plugged in or created)
        if (inotify_fd >= 0 && (pfds[inotify_idx].revents & POLLIN)) {
            char inotify_buf[1024];
            while (read(inotify_fd, inotify_buf, sizeof(inotify_buf)) > 0)
                ;
            discover_all_mice(devices, pfds, &active_count);
        }

        for (int d = active_count - 1; d >= 0; d--) {
            if (pfds[d].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                remove_device(devices, pfds, &active_count, d);
                continue;
            }

            if (!(pfds[d].revents & POLLIN))
                continue;

            struct input_event ev;
            int rc;

            while ((rc = libevdev_next_event(devices[d].dev,
                        LIBEVDEV_READ_FLAG_NORMAL, &ev)) == LIBEVDEV_READ_STATUS_SUCCESS) {

                if (ev.type == EV_REL) {
                    if (ev.code == REL_X)
                        devices[d].pending_dx += ev.value;
                    else if (ev.code == REL_Y)
                        devices[d].pending_dy += ev.value;
                }
                else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    int dx = devices[d].pending_dx;
                    int dy = devices[d].pending_dy;
                    devices[d].pending_dx = 0;
                    devices[d].pending_dy = 0;

                    if (dx == 0 && dy == 0)
                        continue;

                    if (detector_update(&g_detector, dx, dy, ts)) {
                        int cur_x, cur_y;
                        if (query_compositor_cursor(&cur_x, &cur_y)) {
                            printf("SHAKE %d %d\n", cur_x, cur_y);
                            fflush(stdout);
                            track_until = ts + TRACK_ACTIVE_WINDOW_MS;
                            next_track = ts + TRACK_INTERVAL_MS;
                            handoff_pending = true;
                            break;
                        } else {
                            fprintf(stderr, "wiggle-monitor: discarded shake without an exact cursor position\n");
                        }
                    } else if ((ts < track_until || compositor_cursor_hidden) && ts >= next_track) {
                        int cur_x, cur_y;
                        if (query_compositor_cursor(&cur_x, &cur_y)) {
                            printf("POS %d %d\n", cur_x, cur_y);
                            fflush(stdout);
                        }
                        next_track = ts + TRACK_INTERVAL_MS;
                    }
                }
                // All other event types (EV_KEY, EV_ABS, EV_MSC, etc.) are explicitly discarded
            }

            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                devices[d].pending_dx = 0;
                devices[d].pending_dy = 0;
                while (libevdev_next_event(devices[d].dev,
                        LIBEVDEV_READ_FLAG_SYNC, &ev) == LIBEVDEV_READ_STATUS_SYNC)
                    ;
            } else if (rc < 0 && rc != -EAGAIN) {
                remove_device(devices, pfds, &active_count, d);
                continue;
            }

            if (handoff_pending) break;
        }
    }

    for (int d = 0; d < active_count; d++) {
        libevdev_free(devices[d].dev);
        close(devices[d].fd);
    }
    if (inotify_fd >= 0) close(inotify_fd);

    emergency_cleanup();
    fprintf(stderr, "wiggle-monitor: stopped\n");
    return 0;
}
