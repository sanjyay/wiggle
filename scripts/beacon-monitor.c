/*
 * beacon-monitor.c — Shake detection helper for Beacon
 *
 * Reads relative mouse motion from evdev devices using libevdev.
 * Detects deliberate "find my cursor" shake gestures internally.
 * Outputs only "SHAKE\n" on stdout when a shake is detected.
 *
 * Event-driven: blocks on read(), zero CPU when mouse is idle.
 * No polling, no keylogging, no button capture, no network, no persistence.
 *
 * Usage: beacon-monitor [/dev/input/eventN ...]
 *   If no devices specified, auto-detects mouse devices.
 *
 * Build: gcc -O2 -o beacon-monitor beacon-monitor.c $(pkg-config --cflags --libs libevdev) -lm
 *
 * MIT License — see LICENSE
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
#include <linux/input.h>
#include <libevdev/libevdev.h>

/* ─── Tunable Constants ─── */

/* Rolling window duration for shake detection (milliseconds) */
#define WINDOW_MS           600

/* Minimum direction reversals required within the window */
#define MIN_REVERSALS       3

/* Minimum cumulative travel distance (pixels) within the window */
#define MIN_TRAVEL          150

/* Minimum velocity for a segment to count (pixels/ms) */
#define MIN_VELOCITY        0.3

/* Minimum distance for a single movement segment to count as intentional */
#define MIN_SEGMENT_DIST    15

/* Maximum samples in the rolling window */
#define MAX_SAMPLES         256

/* Cooldown after a SHAKE event (milliseconds) — prevents retriggering */
#define COOLDOWN_MS         400

/* Maximum number of mouse devices to monitor simultaneously */
#define MAX_DEVICES         8

/* ─── Data Structures ─── */

typedef struct {
    int64_t timestamp_ms;  /* monotonic timestamp in milliseconds */
    int     dx;            /* relative X delta */
    int     dy;            /* relative Y delta */
} MotionSample;

typedef struct {
    MotionSample samples[MAX_SAMPLES];
    int          head;     /* next write position (circular) */
    int          count;    /* number of valid samples */
    int64_t      last_shake_ms;  /* timestamp of last SHAKE output */
} ShakeDetector;

typedef struct {
    struct libevdev *dev;
    int              fd;
    int              pending_dx;  /* accumulated dx within current SYN frame */
    int              pending_dy;  /* accumulated dy within current SYN frame */
} DeviceState;

/* ─── Globals ─── */

static volatile sig_atomic_t g_running = 1;
static ShakeDetector g_detector = {0};

/* ─── Utility ─── */

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

/* ─── Shake Detection Algorithm ───
 *
 * The algorithm works on a rolling time window of relative motion samples.
 * It tracks "segments" — continuous movements in roughly the same direction.
 * A reversal is counted when the dominant axis direction flips.
 *
 * To qualify as a shake:
 *   1. Multiple direction reversals within the time window (MIN_REVERSALS)
 *   2. Sufficient cumulative travel distance (MIN_TRAVEL)
 *   3. Sufficient velocity on reversal segments (MIN_VELOCITY)
 *   4. Not in cooldown period
 *
 * This rejects:
 *   - Slow movement (insufficient velocity)
 *   - Single fast swipe (only 0-1 reversals)
 *   - Small jitter (insufficient travel distance and segment distance)
 *   - Normal navigation (insufficient reversals)
 */

static void detector_add_sample(ShakeDetector *det, int dx, int dy, int64_t ts) {
    int idx = det->head;
    det->samples[idx].timestamp_ms = ts;
    det->samples[idx].dx = dx;
    det->samples[idx].dy = dy;
    det->head = (det->head + 1) % MAX_SAMPLES;
    if (det->count < MAX_SAMPLES)
        det->count++;
}

static int detector_check_shake(ShakeDetector *det, int64_t now) {
    /* Cooldown check */
    if (det->last_shake_ms > 0 && (now - det->last_shake_ms) < COOLDOWN_MS)
        return 0;

    /* Trim old samples and find valid window */
    int64_t window_start = now - WINDOW_MS;

    /* We'll walk through samples in chronological order within the window.
     * Since it's circular, we need to find the oldest valid sample. */
    int valid_count = 0;
    int start_idx = -1;

    /* Find all samples within the window */
    typedef struct { int dx; int dy; int64_t ts; } ValidSample;
    ValidSample valid[MAX_SAMPLES];

    for (int i = 0; i < det->count; i++) {
        int idx = (det->head - det->count + i + MAX_SAMPLES) % MAX_SAMPLES;
        if (det->samples[idx].timestamp_ms >= window_start) {
            if (start_idx < 0) start_idx = i;
            valid[valid_count].dx = det->samples[idx].dx;
            valid[valid_count].dy = det->samples[idx].dy;
            valid[valid_count].ts = det->samples[idx].timestamp_ms;
            valid_count++;
        }
    }

    if (valid_count < 4)
        return 0;

    /* Accumulate into segments by tracking sign of dominant axis.
     * A segment is a continuous run of motion in roughly the same direction. */

    /* First, determine which axis has more total motion (to handle both
     * horizontal and vertical shaking) */
    int total_abs_dx = 0, total_abs_dy = 0;
    for (int i = 0; i < valid_count; i++) {
        total_abs_dx += abs(valid[i].dx);
        total_abs_dy += abs(valid[i].dy);
    }

    /* Use the dominant axis for reversal detection */
    int use_x = (total_abs_dx >= total_abs_dy);

    /* Build segments: accumulate consecutive samples with same sign on dominant axis */
    typedef struct {
        int     travel;     /* signed travel on dominant axis */
        int     abs_travel; /* absolute travel */
        int64_t start_ts;
        int64_t end_ts;
    } Segment;

    Segment segments[MAX_SAMPLES];
    int seg_count = 0;

    int cur_sign = 0;
    int cur_travel = 0;
    int cur_abs = 0;
    int64_t seg_start = valid[0].ts;

    for (int i = 0; i < valid_count; i++) {
        int val = use_x ? valid[i].dx : valid[i].dy;
        if (val == 0) continue;

        int sign = (val > 0) ? 1 : -1;

        if (cur_sign == 0) {
            /* First sample */
            cur_sign = sign;
            cur_travel = val;
            cur_abs = abs(val);
            seg_start = valid[i].ts;
        } else if (sign == cur_sign) {
            /* Same direction — extend segment */
            cur_travel += val;
            cur_abs += abs(val);
        } else {
            /* Direction changed — close current segment */
            if (cur_abs >= MIN_SEGMENT_DIST) {
                segments[seg_count].travel = cur_travel;
                segments[seg_count].abs_travel = cur_abs;
                segments[seg_count].start_ts = seg_start;
                segments[seg_count].end_ts = valid[i > 0 ? i - 1 : 0].ts;
                seg_count++;
            }
            /* Start new segment */
            cur_sign = sign;
            cur_travel = val;
            cur_abs = abs(val);
            seg_start = valid[i].ts;
        }
    }
    /* Close last segment */
    if (cur_abs >= MIN_SEGMENT_DIST && seg_count < MAX_SAMPLES) {
        segments[seg_count].travel = cur_travel;
        segments[seg_count].abs_travel = cur_abs;
        segments[seg_count].start_ts = seg_start;
        segments[seg_count].end_ts = valid[valid_count - 1].ts;
        seg_count++;
    }

    if (seg_count < 2)
        return 0;

    /* Count reversals and validate velocity */
    int reversals = 0;
    int total_travel = 0;
    int fast_segments = 0;

    for (int i = 0; i < seg_count; i++) {
        total_travel += segments[i].abs_travel;

        /* Check velocity of this segment */
        int64_t dt = segments[i].end_ts - segments[i].start_ts;
        if (dt <= 0) dt = 1;
        double velocity = (double)segments[i].abs_travel / (double)dt;
        if (velocity >= MIN_VELOCITY)
            fast_segments++;

        /* Check for reversal */
        if (i > 0) {
            int prev_sign = (segments[i-1].travel > 0) ? 1 : -1;
            int curr_sign = (segments[i].travel > 0) ? 1 : -1;
            if (prev_sign != curr_sign)
                reversals++;
        }
    }

    /* Final decision */
    if (reversals >= MIN_REVERSALS &&
        total_travel >= MIN_TRAVEL &&
        fast_segments >= MIN_REVERSALS) {
        det->last_shake_ms = now;
        return 1;
    }

    return 0;
}

/* ─── Device Discovery ─── */

static int is_mouse_device(const char *path) {
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) return 0;

    struct libevdev *dev = NULL;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        close(fd);
        return 0;
    }

    /* A mouse has REL_X and REL_Y and usually BTN_LEFT */
    int has_rel = libevdev_has_event_type(dev, EV_REL) &&
                  libevdev_has_event_code(dev, EV_REL, REL_X) &&
                  libevdev_has_event_code(dev, EV_REL, REL_Y);

    /* Exclude touchpads (they have ABS_MT_POSITION_X) and joysticks */
    int is_touchpad = libevdev_has_event_code(dev, EV_ABS, ABS_MT_POSITION_X);

    /* Exclude devices that are purely touchpads */
    int is_direct = libevdev_has_property(dev, INPUT_PROP_DIRECT);

    libevdev_free(dev);
    close(fd);

    /* Accept if it has relative axes and is not a touchpad with multi-touch */
    return has_rel && !is_touchpad && !is_direct;
}

static int discover_mice(char paths[][280], int max) {
    int count = 0;
    DIR *dir = opendir("/dev/input");
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < max) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;

        char path[280];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);

        if (is_mouse_device(path)) {
            snprintf(paths[count], 280, "%s", path);
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* ─── Main ─── */

int main(int argc, char *argv[]) {
    /* Set up signal handlers for clean shutdown */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    /* Line-buffered stdout for immediate SHAKE delivery */
    setvbuf(stdout, NULL, _IOLBF, 0);

    /* Determine devices to monitor */
    char device_paths[MAX_DEVICES][280];
    int device_count = 0;

    if (argc > 1) {
        /* Use command-line specified devices */
        for (int i = 1; i < argc && device_count < MAX_DEVICES; i++) {
            snprintf(device_paths[device_count], 280, "%s", argv[i]);
            device_count++;
        }
    } else {
        /* Auto-discover mouse devices */
        device_count = discover_mice(device_paths, MAX_DEVICES);
    }

    if (device_count == 0) {
        fprintf(stderr, "beacon-monitor: no mouse devices found\n");
        return 1;
    }

    /* Open all devices */
    DeviceState devices[MAX_DEVICES];
    struct pollfd pfds[MAX_DEVICES];
    int active_count = 0;

    for (int i = 0; i < device_count; i++) {
        int fd = open(device_paths[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            fprintf(stderr, "beacon-monitor: cannot open %s: %s\n",
                    device_paths[i], strerror(errno));
            continue;
        }

        struct libevdev *dev = NULL;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            fprintf(stderr, "beacon-monitor: libevdev error for %s: %s\n",
                    device_paths[i], strerror(-rc));
            close(fd);
            continue;
        }

        /* Only grab REL events — ignore everything else.
         * We do NOT use EVIOCGRAB (exclusive grab) because that would
         * steal input from the compositor. We just read passively. */

        fprintf(stderr, "beacon-monitor: monitoring %s (%s)\n",
                device_paths[i], libevdev_get_name(dev));

        devices[active_count].dev = dev;
        devices[active_count].fd = fd;
        devices[active_count].pending_dx = 0;
        devices[active_count].pending_dy = 0;

        pfds[active_count].fd = fd;
        pfds[active_count].events = POLLIN;

        active_count++;
    }

    if (active_count == 0) {
        fprintf(stderr, "beacon-monitor: no devices opened successfully\n");
        return 1;
    }

    fprintf(stderr, "beacon-monitor: ready (%d device%s)\n",
            active_count, active_count > 1 ? "s" : "");

    /* Main event loop */
    while (g_running) {
        int ret = poll(pfds, active_count, -1);  /* Block indefinitely */

        if (ret < 0) {
            if (errno == EINTR)
                continue;  /* Signal received — check g_running */
            break;
        }

        int64_t ts = now_ms();

        for (int d = 0; d < active_count; d++) {
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
                    /* End of an input frame — process accumulated deltas */
                    int dx = devices[d].pending_dx;
                    int dy = devices[d].pending_dy;
                    devices[d].pending_dx = 0;
                    devices[d].pending_dy = 0;

                    if (dx == 0 && dy == 0)
                        continue;

                    detector_add_sample(&g_detector, dx, dy, ts);

                    if (detector_check_shake(&g_detector, ts)) {
                        printf("SHAKE\n");
                        /* Stdout is line-buffered, flushed on \n */
                    }
                }
            }

            /* Handle SYN_DROPPED (we lost events — reset pending) */
            if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                devices[d].pending_dx = 0;
                devices[d].pending_dy = 0;
                /* Drain sync events */
                while (libevdev_next_event(devices[d].dev,
                        LIBEVDEV_READ_FLAG_SYNC, &ev) == LIBEVDEV_READ_STATUS_SYNC)
                    ;
            }
        }
    }

    /* Cleanup */
    for (int d = 0; d < active_count; d++) {
        libevdev_free(devices[d].dev);
        close(devices[d].fd);
    }

    fprintf(stderr, "beacon-monitor: stopped\n");
    return 0;
}
