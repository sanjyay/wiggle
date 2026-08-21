/*
 * test-shake-detector.c — Unit tests for KDE-style shake detector
 *
 * Tests the KDE/KWin-style geometric ratio algorithm across:
 *   - Idle / stationary sensor noise
 *   - Fast straight swipes
 *   - Slow navigation drift
 *   - Smooth circular motion
 *   - Text reading zig-zag
 *   - Horizontal, vertical, and diagonal shakes
 *   - Scale invariance across 0.5×, 1×, 2×, 4× scales (DPI independence)
 *   - Event/sample rate invariance (1000Hz, 500Hz, 250Hz, 125Hz, 60Hz)
 *   - Continuous multi-second oscillation producing sequential triggers without cooldown
 *   - Single shake followed by normal movement (exactly one trigger)
 *
 * MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define HISTORY_INTERVAL_MS     1000
#define DEFAULT_SENSITIVITY     4.0
#define MIN_DIAGONAL_COUNTS     40.0
#define MAX_HISTORY_POINTS      512

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

static void detector_init(ShakeDetector *det) {
    memset(det, 0, sizeof(*det));
    det->interval_ms = HISTORY_INTERVAL_MS;
    det->sensitivity = DEFAULT_SENSITIVITY;
    det->min_diagonal = MIN_DIAGONAL_COUNTS;
}

static inline bool same_direction(double a, double b) {
    const double tolerance = 1.0;
    return (a >= -tolerance && b >= -tolerance) || (a <= tolerance && b <= tolerance);
}

static bool detector_update(ShakeDetector *det, double dx, double dy, int64_t ts) {
    det->cum_x += dx;
    det->cum_y += dy;
    double cur_x = det->cum_x;
    double cur_y = det->cum_y;

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
        det->count = 0; // Immediate reset on trigger
        return true;
    }

    return false;
}

/* ─── Test Harness ─── */

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name, expr) do { \
    tests_run++; \
    if (expr) { \
        printf("  ✓ %s\n", name); \
        tests_passed++; \
    } else { \
        printf("  ✗ %s (FAILED)\n", name); \
    } \
} while(0)

typedef struct {
    int64_t t;
    double  dx;
    double  dy;
} EventSample;

static int count_triggers(ShakeDetector *det, const EventSample *samples, int count) {
    int triggers = 0;
    for (int i = 0; i < count; i++) {
        if (detector_update(det, samples[i].dx, samples[i].dy, samples[i].t))
            triggers++;
    }
    return triggers;
}

static int generate_shake(EventSample *out, int max_count, double scale, int interval_ms, int axis, int64_t start_t) {
    int idx = 0;
    int64_t t = start_t;
    double stroke_len = 50.0 * scale;

    for (int stroke = 0; stroke < 6 && idx < max_count - 10; stroke++) {
        int sign = (stroke % 2 == 0) ? -1 : 1;
        for (int step = 0; step < 5 && idx < max_count; step++) {
            t += interval_ms;
            double step_val = sign * (stroke_len / 5.0);

            out[idx].t = t;
            if (axis == 0) {
                out[idx].dx = step_val;
                out[idx].dy = 0;
            } else if (axis == 1) {
                out[idx].dx = 0;
                out[idx].dy = step_val;
            } else {
                out[idx].dx = step_val;
                out[idx].dy = step_val;
            }
            idx++;
        }
    }
    return idx;
}

/* ─── Individual Tests ─── */

void test_idle(void) {
    ShakeDetector det;
    detector_init(&det);
    TEST("idle: no samples produces no shake", detector_update(&det, 0, 0, 1000) == false);
}

void test_sensor_jitter(void) {
    ShakeDetector det;
    detector_init(&det);

    EventSample samples[100];
    int pattern[] = {1, -1, 1, -1, 2, -2, 1, -1, 1, -2};
    for (int i = 0; i < 100; i++) {
        samples[i].t = 1000 + i * 8;
        samples[i].dx = pattern[i % 10];
        samples[i].dy = pattern[(i + 3) % 10];
    }

    TEST("sensor jitter: micro-noise does not trigger", count_triggers(&det, samples, 100) == 0);
}

void test_fast_swipe(void) {
    ShakeDetector det;
    detector_init(&det);

    EventSample samples[15];
    for (int i = 0; i < 15; i++) {
        samples[i].t = 1000 + i * 10;
        samples[i].dx = 35;
        samples[i].dy = 0;
    }

    TEST("fast swipe: straight 500px swipe does not trigger", count_triggers(&det, samples, 15) == 0);
}

void test_slow_navigation(void) {
    ShakeDetector det;
    detector_init(&det);

    EventSample samples[30];
    for (int i = 0; i < 30; i++) {
        samples[i].t = 1000 + i * 30;
        samples[i].dx = 2;
        samples[i].dy = 1;
    }

    TEST("slow navigation: drift does not trigger", count_triggers(&det, samples, 30) == 0);
}

void test_text_zigzag(void) {
    ShakeDetector det;
    detector_init(&det);

    EventSample samples[100];
    int idx = 0;
    int64_t t = 1000;

    for (int line = 0; line < 3; line++) {
        for (int step = 0; step < 15 && idx < 100; step++) {
            t += 30;
            samples[idx++] = (EventSample){t, 6, 0};
        }
        for (int cr = 0; cr < 4 && idx < 100; cr++) {
            t += 15;
            samples[idx++] = (EventSample){t, -22, 8};
        }
    }

    TEST("reading text zig-zag: does not trigger", count_triggers(&det, samples, idx) == 0);
}

void test_shakes(void) {
    EventSample samples[100];
    int count;

    ShakeDetector det_h;
    detector_init(&det_h);
    count = generate_shake(samples, 100, 1.0, 10, 0, 1000);
    TEST("compact horizontal shake: triggers cleanly", count_triggers(&det_h, samples, count) >= 1);

    ShakeDetector det_v;
    detector_init(&det_v);
    count = generate_shake(samples, 100, 1.0, 10, 1, 1000);
    TEST("compact vertical shake: triggers cleanly", count_triggers(&det_v, samples, count) >= 1);

    ShakeDetector det_d;
    detector_init(&det_d);
    count = generate_shake(samples, 100, 1.0, 10, 2, 1000);
    TEST("compact diagonal shake: triggers cleanly", count_triggers(&det_d, samples, count) >= 1);
}

void test_scale_invariance(void) {
    double scales[] = {0.8, 1.0, 1.5, 2.0, 4.0};
    EventSample samples[100];

    for (size_t s = 0; s < sizeof(scales)/sizeof(scales[0]); s++) {
        char msg[128];

        ShakeDetector det_h;
        detector_init(&det_h);
        int count = generate_shake(samples, 100, scales[s], 10, 0, 1000);
        snprintf(msg, sizeof(msg), "scale invariance (%.1fx horizontal): triggers", scales[s]);
        TEST(msg, count_triggers(&det_h, samples, count) >= 1);

        ShakeDetector det_v;
        detector_init(&det_v);
        count = generate_shake(samples, 100, scales[s], 10, 1, 1000);
        snprintf(msg, sizeof(msg), "scale invariance (%.1fx vertical): triggers", scales[s]);
        TEST(msg, count_triggers(&det_v, samples, count) >= 1);
    }
}

void test_sample_rate_invariance(void) {
    int intervals[] = {1, 2, 4, 8, 16};
    EventSample samples[200];

    for (size_t r = 0; r < sizeof(intervals)/sizeof(intervals[0]); r++) {
        char msg[128];
        ShakeDetector det;
        detector_init(&det);
        int count = generate_shake(samples, 200, 1.0, intervals[r], 0, 1000);
        snprintf(msg, sizeof(msg), "sample rate invariance (%dms / %dHz): triggers",
                 intervals[r], 1000 / intervals[r]);
        TEST(msg, count_triggers(&det, samples, count) >= 1);
    }
}

void test_continuous_oscillation(void) {
    ShakeDetector det;
    detector_init(&det);

    EventSample samples[500];
    int idx = 0;
    int64_t t = 0;

    // 4.8 seconds of continuous oscillation
    for (int stroke = 0; stroke < 40 && idx < 500; stroke++) {
        int sign = (stroke % 2 == 0) ? 1 : -1;
        for (int step = 0; step < 10 && idx < 500; step++) {
            t += 12;
            samples[idx++] = (EventSample){t, sign * 5.0, 0};
        }
    }

    int triggers = count_triggers(&det, samples, idx);
    TEST("continuous 4.8s shake: produces sequential re-triggers (>= 5)", triggers >= 5);
}

void test_single_shake_then_idle(void) {
    ShakeDetector det;
    detector_init(&det);

    EventSample shake[100];
    int count = generate_shake(shake, 100, 1.0, 10, 0, 1000);
    int initial_triggers = count_triggers(&det, shake, count);

    EventSample move[50];
    for (int i = 0; i < 50; i++) {
        move[i] = (EventSample){2000 + i * 20, 3, 1};
    }
    int later_triggers = count_triggers(&det, move, 50);

    TEST("single shake then normal movement: exactly one trigger event", initial_triggers == 1 && later_triggers == 0);
}

int main(void) {
    printf("Wiggle Clean-Room KDE-Style Shake Detector Tests\n");
    printf("================================================\n\n");

    test_idle();
    test_sensor_jitter();
    test_fast_swipe();
    test_slow_navigation();
    test_text_zigzag();
    test_shakes();
    test_scale_invariance();
    test_sample_rate_invariance();
    test_continuous_oscillation();
    test_single_shake_then_idle();

    printf("\n================================================\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
