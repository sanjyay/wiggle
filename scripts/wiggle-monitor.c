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
#include <limits.h>
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
#define TRACK_INTERVAL_MS       4      // Faster than common 60-240 Hz displays
#define TRACK_ACTIVE_WINDOW_MS  2500
#define TRACK_AFTER_SHOW_MS     600    // Outlasts the shell's 1x proxy release delay
#define IPC_RESPONSE_TIMEOUT_MS 300
#define HIDE_APPLY_TIMEOUT_MS   600    // Hyprland's cursor tick is 500 ms
#define HIDE_CHECK_INTERVAL_MS  10
#define TRAVEL_SAMPLE_COUNT     32
#define TRAVEL_INTERVAL_MS      50
#define MIN_POINTER_TRAVEL_PX   48.0   // Compositor pixels the pointer must cover during a shake
#define IGNORED_LOG_INTERVAL_MS 10000
#define TOUCHPAD_COUNTS_PER_MM  40.0   // Roughly a 1000 DPI mouse
#define TOUCHPAD_FALLBACK_MM    100.0  // Assumed pad width when no resolution is reported

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
    double scale_x;
    double scale_y;
    int    x;
    int    y;
    int    last_x;
    int    last_y;
    bool   has_last;
    bool   touching;
    int    fingers;
} TouchpadState;

typedef struct {
    struct libevdev *dev;
    int              fd;
    int              pending_dx;
    int              pending_dy;
    char             path[280];
    bool             is_touchpad;
    TouchpadState    touchpad;
} DeviceState;

typedef struct {
    int     x;
    int     y;
    int64_t timestamp_ms;
} PointerSample;

typedef struct {
    PointerSample samples[TRAVEL_SAMPLE_COUNT];
    int           next;
    int           count;
} PointerTravel;

/* ─── Globals ─── */

static volatile sig_atomic_t g_running = 1;
static ShakeDetector g_detector = {0};
static PointerTravel g_travel = {0};

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

/*
 * Sends one request to Hyprland's command socket and reads the full reply.
 * Replies larger than the buffer are truncated.
 */
static bool hypr_request(const char *request, char *response, size_t size) {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const char *signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (!runtime || !signature) return false;

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int length = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/hypr/%s/.socket.sock",
                          runtime, signature);
    if (length < 0 || (size_t)length >= sizeof(addr.sun_path)) return false;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        !write_all(fd, request, strlen(request)) ||
        shutdown(fd, SHUT_WR) < 0) {
        close(fd);
        return false;
    }

    size_t used = 0;
    while (used < size - 1) {
        struct pollfd response_poll = { .fd = fd, .events = POLLIN };
        if (poll(&response_poll, 1, IPC_RESPONSE_TIMEOUT_MS) <= 0) break;

        ssize_t n = read(fd, response + used, size - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    close(fd);
    response[used] = '\0';
    return used > 0;
}

static bool set_compositor_cursor_invisible(bool invisible) {
    char response[128];
    const char *request = invisible
        ? "/eval hl.config({ cursor = { invisible = true } })"
        : "/eval hl.config({ cursor = { invisible = false } })";
    return hypr_request(request, response, sizeof(response)) && response_is_ok(response);
}

static int query_compositor_cursor(int *x, int *y) {
    char response[256];
    if (!hypr_request("j/cursorpos", response, sizeof(response))) return 0;

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

static const char *query_monitors(void) {
    static char response[65536];
    return hypr_request("j/monitors", response, sizeof(response)) ? response : NULL;
}

/*
 * Monitors list HW_CURSOR under tearingBlockedBy while a hardware cursor is
 * actually on screen. Unlike the cursor:invisible option, which Hyprland only
 * applies on its 500 ms cursor tick, this follows what is really drawn.
 */
static bool hardware_cursor_visible(void) {
    const char *monitors = query_monitors();
    return monitors && strstr(monitors, "\"HW_CURSOR\"");
}

/*
 * Whether Hyprland draws a cursor at all. Games hide it by not setting a
 * cursor image, which also clears HW_CURSOR. With software cursors (SW under
 * directScanoutBlockedBy) this can't be told, so the cursor counts as drawn.
 */
static bool cursor_drawn(void) {
    const char *monitors = query_monitors();
    return !monitors || strstr(monitors, "\"HW_CURSOR\"") || strstr(monitors, "\"SW\"");
}

/*
 * Wine/Proton games confine the pointer and draw a transparent cursor, so
 * neither the travel nor the drawn-cursor check catches them. They run as
 * fullscreen XWayland windows, unlike browsers and video players, which are
 * native Wayland clients. Some clients also mark themselves as games.
 */
static bool focused_window_is_game(void) {
    char response[8192];
    if (!hypr_request("j/activewindow", response, sizeof(response))) return false;

    if (strstr(response, "\"contentType\": \"game\"")) return true;

    int fullscreen = 0;
    const char *key = strstr(response, "\"fullscreen\":");
    if (key) sscanf(key + strlen("\"fullscreen\":"), "%d", &fullscreen);

    // Mode 1 is maximized; bit 2 is real fullscreen
    return (fullscreen & 2) && strstr(response, "\"xwayland\": true");
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

/* ─── Pointer Travel ─── */

static void travel_add(PointerTravel *travel, int x, int y, int64_t ts) {
    travel->samples[travel->next] = (PointerSample){ .x = x, .y = y, .timestamp_ms = ts };
    travel->next = (travel->next + 1) % TRAVEL_SAMPLE_COUNT;
    if (travel->count < TRAVEL_SAMPLE_COUNT) travel->count++;
}

// Diagonal of the area the compositor pointer covered within the shake window
static double travel_extent(const PointerTravel *travel, int64_t ts) {
    bool found = false;
    int left = 0, right = 0, top = 0, bottom = 0;

    for (int i = 0; i < travel->count; i++) {
        const PointerSample *sample = &travel->samples[i];
        if (ts - sample->timestamp_ms >= HISTORY_INTERVAL_MS) continue;

        if (!found || sample->x < left) left = sample->x;
        if (!found || sample->x > right) right = sample->x;
        if (!found || sample->y < top) top = sample->y;
        if (!found || sample->y > bottom) bottom = sample->y;
        found = true;
    }

    double width = right - left;
    double height = bottom - top;
    return sqrt(width * width + height * height);
}

/*
 * KWin ignores shakes while the pointer is constrained. Hyprland doesn't
 * expose that over IPC, so look at what games do instead: a locked pointer
 * is held in place and never travels, a confined one is usually hidden, and
 * the window is fullscreen. While the proxy is up we hide the cursor
 * ourselves, so the drawn-cursor check only applies to the first shake.
 */
static const char *shake_rejection(int64_t ts, bool proxy_engaged) {
    if (travel_extent(&g_travel, ts) < MIN_POINTER_TRAVEL_PX) return "pointer did not move";
    if (!proxy_engaged && !cursor_drawn()) return "cursor is hidden";
    if (focused_window_is_game()) return "focused window is a fullscreen XWayland game";
    return NULL;
}

/* ─── Touchpad Motion ─── */

static double abs_units_per_mm(const struct input_absinfo *abs) {
    if (abs->resolution > 0) return abs->resolution;
    double range = abs->maximum - abs->minimum;
    return range > 0 ? range / TOUCHPAD_FALLBACK_MM : 1.0;
}

static void touchpad_sync_state(TouchpadState *tp, const struct libevdev *dev) {
    static const unsigned int tools[] = {
        BTN_TOOL_FINGER, BTN_TOOL_DOUBLETAP, BTN_TOOL_TRIPLETAP,
        BTN_TOOL_QUADTAP, BTN_TOOL_QUINTTAP,
    };
    tp->x = libevdev_get_event_value(dev, EV_ABS, ABS_X);
    tp->y = libevdev_get_event_value(dev, EV_ABS, ABS_Y);
    tp->touching = libevdev_get_event_value(dev, EV_KEY, BTN_TOUCH) != 0;
    tp->fingers = 0;
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (libevdev_get_event_value(dev, EV_KEY, tools[i]))
            tp->fingers = (int)i + 1;
    }
    // A recovered position establishes a baseline, never a movement jump.
    tp->has_last = false;
}

static void touchpad_init(TouchpadState *tp, const struct libevdev *dev) {
    const struct input_absinfo *abs_x = libevdev_get_abs_info(dev, ABS_X);
    const struct input_absinfo *abs_y = libevdev_get_abs_info(dev, ABS_Y);
    double units_x = abs_units_per_mm(abs_x);
    double units_y = abs_y->resolution > 0 ? abs_y->resolution : units_x;

    memset(tp, 0, sizeof(*tp));
    tp->scale_x = TOUCHPAD_COUNTS_PER_MM / units_x;
    tp->scale_y = TOUCHPAD_COUNTS_PER_MM / units_y;
    touchpad_sync_state(tp, dev);
}

static void touchpad_handle_event(TouchpadState *tp, const struct input_event *ev) {
    if (ev->type == EV_ABS) {
        if (ev->code == ABS_X) tp->x = ev->value;
        else if (ev->code == ABS_Y) tp->y = ev->value;
        return;
    }

    // Only contact and finger-count state is kept; no other key data is read.
    switch (ev->code) {
    case BTN_TOUCH:          tp->touching = ev->value != 0; break;
    case BTN_TOOL_FINGER:    if (ev->value) tp->fingers = 1; break;
    case BTN_TOOL_DOUBLETAP: if (ev->value) tp->fingers = 2; break;
    case BTN_TOOL_TRIPLETAP: if (ev->value) tp->fingers = 3; break;
    case BTN_TOOL_QUADTAP:   if (ev->value) tp->fingers = 4; break;
    case BTN_TOOL_QUINTTAP:  if (ev->value) tp->fingers = 5; break;
    default: break;
    }
}

/*
 * Converts the absolute position of a single finger into a relative delta at
 * the end of each frame. Multi-finger scrolling and gestures are ignored, and
 * the first frame of a new contact only records its position so lifting and
 * re-placing a finger never registers as a jump.
 */
static void touchpad_finish_frame(TouchpadState *tp, int *dx, int *dy) {
    bool tracking = tp->touching && tp->fingers == 1;

    if (tracking && tp->has_last) {
        *dx += (int)lround((tp->x - tp->last_x) * tp->scale_x);
        *dy += (int)lround((tp->y - tp->last_y) * tp->scale_y);
    }

    if (!tp->touching) tp->fingers = 0;
    tp->last_x = tp->x;
    tp->last_y = tp->y;
    tp->has_last = tracking;
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

    int has_mt = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X);
    int is_direct = libevdev_has_property(dev, INPUT_PROP_DIRECT);

    /*
     * Some composite mouse receivers expose alphanumeric keys on the same
     * event node as REL_X/REL_Y (notably Logitech HID++ devices). Classify by
     * pointer axes here and discard all EV_KEY events in the read loop below.
     */
    bool is_mouse = has_rel && !has_mt;

    /*
     * Same rule udev uses for ID_INPUT_TOUCHPAD: absolute X/Y with finger
     * tools and no pen. Touchscreens are excluded by INPUT_PROP_DIRECT.
     */
    bool is_touchpad = !has_rel &&
                       libevdev_has_event_code(dev, EV_ABS, ABS_X) &&
                       libevdev_has_event_code(dev, EV_ABS, ABS_Y) &&
                       libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_FINGER) &&
                       !libevdev_has_event_code(dev, EV_KEY, BTN_TOOL_PEN);

    if (is_direct || (!is_mouse && !is_touchpad)) {
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
    devices[idx].is_touchpad = is_touchpad;
    if (is_touchpad)
        touchpad_init(&devices[idx].touchpad, dev);
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
    int last_pos_x = INT_MIN;
    int last_pos_y = INT_MIN;
    bool hide_pending = false;
    int64_t hide_deadline = 0;
    int64_t next_hide_check = 0;
    int64_t next_travel_sample = 0;
    int64_t last_ignored_log = -IGNORED_LOG_INTERVAL_MS;

    while (g_running) {
        // Place inotify and stdin at the end of pollfds
        int inotify_idx = active_count;
        pfds[inotify_idx].fd = (inotify_fd >= 0) ? inotify_fd : -1;
        pfds[inotify_idx].events = POLLIN;

        int stdin_idx = active_count + 1;
        pfds[stdin_idx].fd = STDIN_FILENO;
        pfds[stdin_idx].events = POLLIN | POLLHUP | POLLERR;

        int total_pfds = active_count + 2;

        // While the proxy follows the pointer, wake up for the next sample even without input
        int timeout = -1;
        int64_t poll_ts = now_ms();
        if (poll_ts < track_until || compositor_cursor_hidden)
            timeout = next_track > poll_ts ? (int)(next_track - poll_ts) : 0;

        int ret = poll(pfds, total_pfds, timeout);

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
                        bool was_visible = hardware_cursor_visible();
                        if (!set_compositor_cursor_invisible(true)) {
                            printf("HIDE_FAILED\n");
                        } else if (was_visible) {
                            /* Confirm once the cursor is really gone, see below */
                            hide_pending = true;
                            hide_deadline = now_ms() + HIDE_APPLY_TIMEOUT_MS;
                        } else {
                            printf("HIDDEN\n");
                        }
                        fflush(stdout);
                    } else if (strcmp(command, "SHOW") == 0) {
                        hide_pending = false;
                        bool shown = set_compositor_cursor_invisible(false);
                        if (shown) {
                            compositor_cursor_hidden = false;
                            track_until = now_ms() + TRACK_AFTER_SHOW_MS;
                        }
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
                else if (devices[d].is_touchpad && (ev.type == EV_ABS || ev.type == EV_KEY)) {
                    touchpad_handle_event(&devices[d].touchpad, &ev);
                }
                else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    if (devices[d].is_touchpad) {
                        touchpad_finish_frame(&devices[d].touchpad,
                                              &devices[d].pending_dx,
                                              &devices[d].pending_dy);
                    }

                    int dx = devices[d].pending_dx;
                    int dy = devices[d].pending_dy;
                    devices[d].pending_dx = 0;
                    devices[d].pending_dy = 0;

                    if (dx == 0 && dy == 0)
                        continue;

                    if (ts >= next_travel_sample) {
                        int cur_x, cur_y;
                        if (query_compositor_cursor(&cur_x, &cur_y))
                            travel_add(&g_travel, cur_x, cur_y, ts);
                        next_travel_sample = ts + TRAVEL_INTERVAL_MS;
                    }

                    if (detector_update(&g_detector, dx, dy, ts)) {
                        int cur_x, cur_y;
                        if (query_compositor_cursor(&cur_x, &cur_y)) {
                            travel_add(&g_travel, cur_x, cur_y, ts);

                            const char *rejection = shake_rejection(
                                ts, compositor_cursor_hidden || ts < track_until);
                            if (rejection) {
                                if (ts - last_ignored_log >= IGNORED_LOG_INTERVAL_MS) {
                                    fprintf(stderr, "wiggle-monitor: ignored shake, %s\n", rejection);
                                    last_ignored_log = ts;
                                }
                                continue;
                            }

                            printf("SHAKE %d %d\n", cur_x, cur_y);
                            fflush(stdout);
                            last_pos_x = cur_x;
                            last_pos_y = cur_y;
                            track_until = ts + TRACK_ACTIVE_WINDOW_MS;
                            next_track = ts + TRACK_INTERVAL_MS;
                            handoff_pending = true;
                            break;
                        } else {
                            fprintf(stderr, "wiggle-monitor: discarded shake without an exact cursor position\n");
                        }
                    }
                }
                // Everything else, including all EV_KEY and EV_ABS events from mice, is discarded
            }

            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                devices[d].pending_dx = 0;
                devices[d].pending_dy = 0;
                while (libevdev_next_event(devices[d].dev,
                        LIBEVDEV_READ_FLAG_SYNC, &ev) == LIBEVDEV_READ_STATUS_SYNC)
                    ;
                if (devices[d].is_touchpad)
                    touchpad_sync_state(&devices[d].touchpad, devices[d].dev);
            } else if (rc < 0 && rc != -EAGAIN) {
                remove_device(devices, pfds, &active_count, d);
                continue;
            }

            if (handoff_pending) break;
        }

        /*
         * Sample the compositor position on a fixed clock instead of once per
         * input event. Per-event queries race Hyprland's own handling of the
         * same event, and the rate limit often dropped the last position of a
         * movement, leaving the proxy short of the real pointer.
         */
        ts = now_ms();

        if (!handoff_pending && (ts < track_until || compositor_cursor_hidden) &&
            ts >= next_track) {
            int cur_x, cur_y;
            if (query_compositor_cursor(&cur_x, &cur_y) &&
                (cur_x != last_pos_x || cur_y != last_pos_y)) {
                printf("POS %d %d\n", cur_x, cur_y);
                fflush(stdout);
                last_pos_x = cur_x;
                last_pos_y = cur_y;
            }
            next_track = ts + TRACK_INTERVAL_MS;
        }

        /*
         * The shell keeps the proxy at 1x until HIDDEN arrives, so it never
         * grows while the small native cursor is still drawn on top.
         */
        if (hide_pending && ts >= next_hide_check) {
            if (ts >= hide_deadline || !hardware_cursor_visible()) {
                hide_pending = false;
                printf("HIDDEN\n");
                fflush(stdout);
            }
            next_hide_check = ts + HIDE_CHECK_INTERVAL_MS;
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
