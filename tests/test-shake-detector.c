/*
 * test-shake-detector.c — Unit tests for beacon-monitor shake detection
 *
 * Tests the shake detection algorithm with synthetic motion samples.
 * Does NOT require actual input devices.
 *
 * Build: gcc -O2 -o test-shake-detector test-shake-detector.c -lm
 * Run:   ./test-shake-detector
 *
 * MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* ─── Include the detection constants and structures directly ─── */
/* (We duplicate the core algorithm here to test it in isolation) */

#define WINDOW_MS           600
#define MIN_REVERSALS       3
#define MIN_TRAVEL          150
#define MIN_VELOCITY        0.3
#define MIN_SEGMENT_DIST    15
#define MAX_SAMPLES         256
#define COOLDOWN_MS         400

typedef struct {
    int64_t timestamp_ms;
    int     dx;
    int     dy;
} MotionSample;

typedef struct {
    MotionSample samples[MAX_SAMPLES];
    int          head;
    int          count;
    int64_t      last_shake_ms;
} ShakeDetector;

/* ─── Duplicate the detection algorithm for isolated testing ─── */

static void detector_init(ShakeDetector *det) {
    memset(det, 0, sizeof(*det));
}

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
    if (det->last_shake_ms > 0 && (now - det->last_shake_ms) < COOLDOWN_MS)
        return 0;

    int64_t window_start = now - WINDOW_MS;

    typedef struct { int dx; int dy; int64_t ts; } ValidSample;
    ValidSample valid[MAX_SAMPLES];
    int valid_count = 0;

    for (int i = 0; i < det->count; i++) {
        int idx = (det->head - det->count + i + MAX_SAMPLES) % MAX_SAMPLES;
        if (det->samples[idx].timestamp_ms >= window_start) {
            valid[valid_count].dx = det->samples[idx].dx;
            valid[valid_count].dy = det->samples[idx].dy;
            valid[valid_count].ts = det->samples[idx].timestamp_ms;
            valid_count++;
        }
    }

    if (valid_count < 4)
        return 0;

    int total_abs_dx = 0, total_abs_dy = 0;
    for (int i = 0; i < valid_count; i++) {
        total_abs_dx += abs(valid[i].dx);
        total_abs_dy += abs(valid[i].dy);
    }

    int use_x = (total_abs_dx >= total_abs_dy);

    typedef struct {
        int     travel;
        int     abs_travel;
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
            cur_sign = sign;
            cur_travel = val;
            cur_abs = abs(val);
            seg_start = valid[i].ts;
        } else if (sign == cur_sign) {
            cur_travel += val;
            cur_abs += abs(val);
        } else {
            if (cur_abs >= MIN_SEGMENT_DIST) {
                segments[seg_count].travel = cur_travel;
                segments[seg_count].abs_travel = cur_abs;
                segments[seg_count].start_ts = seg_start;
                segments[seg_count].end_ts = valid[i > 0 ? i - 1 : 0].ts;
                seg_count++;
            }
            cur_sign = sign;
            cur_travel = val;
            cur_abs = abs(val);
            seg_start = valid[i].ts;
        }
    }
    if (cur_abs >= MIN_SEGMENT_DIST && seg_count < MAX_SAMPLES) {
        segments[seg_count].travel = cur_travel;
        segments[seg_count].abs_travel = cur_abs;
        segments[seg_count].start_ts = seg_start;
        segments[seg_count].end_ts = valid[valid_count - 1].ts;
        seg_count++;
    }

    if (seg_count < 2)
        return 0;

    int reversals = 0;
    int total_travel = 0;
    int fast_segments = 0;

    for (int i = 0; i < seg_count; i++) {
        total_travel += segments[i].abs_travel;

        int64_t dt = segments[i].end_ts - segments[i].start_ts;
        if (dt <= 0) dt = 1;
        double velocity = (double)segments[i].abs_travel / (double)dt;
        if (velocity >= MIN_VELOCITY)
            fast_segments++;

        if (i > 0) {
            int prev_sign = (segments[i-1].travel > 0) ? 1 : -1;
            int curr_sign = (segments[i].travel > 0) ? 1 : -1;
            if (prev_sign != curr_sign)
                reversals++;
        }
    }

    if (reversals >= MIN_REVERSALS &&
        total_travel >= MIN_TRAVEL &&
        fast_segments >= MIN_REVERSALS) {
        det->last_shake_ms = now;
        return 1;
    }

    return 0;
}

/* ─── Test Helpers ─── */

/* Feed a series of (dx, dy) samples at a given rate (ms between samples) */
static int feed_and_check(ShakeDetector *det, int samples[][2], int count,
                          int interval_ms, int64_t start_ts) {
    int triggered = 0;
    for (int i = 0; i < count; i++) {
        int64_t ts = start_ts + (int64_t)i * interval_ms;
        detector_add_sample(det, samples[i][0], samples[i][1], ts);
        if (detector_check_shake(det, ts))
            triggered = 1;
    }
    return triggered;
}

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

/* ─── Tests ─── */

/* Test 1: Idle — no input at all */
void test_idle(void) {
    ShakeDetector det;
    detector_init(&det);

    TEST("idle: no samples produces no shake",
         detector_check_shake(&det, 1000) == 0);
}

/* Test 2: Slow movement — gentle cursor drift */
void test_slow_movement(void) {
    ShakeDetector det;
    detector_init(&det);

    /* Slow rightward drift: 1px every 50ms for 500ms */
    int samples[10][2];
    for (int i = 0; i < 10; i++) {
        samples[i][0] = 1;
        samples[i][1] = 0;
    }

    int triggered = feed_and_check(&det, samples, 10, 50, 1000);
    TEST("slow movement: gentle drift does not trigger", triggered == 0);
}

/* Test 3: Normal navigation — smooth cursor arc */
void test_normal_navigation(void) {
    ShakeDetector det;
    detector_init(&det);

    /* Smooth movement to a target: consistent rightward + slight downward */
    int samples[20][2];
    for (int i = 0; i < 20; i++) {
        samples[i][0] = 8 + (i % 3);   /* ~8-10px right */
        samples[i][1] = 2;              /* slight down */
    }

    int triggered = feed_and_check(&det, samples, 20, 10, 1000);
    TEST("normal navigation: smooth arc does not trigger", triggered == 0);
}

/* Test 4: One fast swipe */
void test_fast_swipe(void) {
    ShakeDetector det;
    detector_init(&det);

    /* Single fast rightward swipe: 40px deltas for 100ms */
    int samples[10][2];
    for (int i = 0; i < 10; i++) {
        samples[i][0] = 40;
        samples[i][1] = 0;
    }

    int triggered = feed_and_check(&det, samples, 10, 10, 1000);
    TEST("fast swipe: single direction does not trigger", triggered == 0);
}

/* Test 5: Horizontal shake — should trigger */
void test_horizontal_shake(void) {
    ShakeDetector det;
    detector_init(&det);

    /* ← fast, → fast, ← fast, → fast */
    /* Each direction: 5 samples at 10ms apart = 50ms per direction */
    int samples[40][2];
    int idx = 0;
    for (int dir = 0; dir < 8; dir++) {
        int sign = (dir % 2 == 0) ? -1 : 1;
        for (int i = 0; i < 5; i++) {
            samples[idx][0] = sign * 20;
            samples[idx][1] = 0;
            idx++;
        }
    }

    int triggered = feed_and_check(&det, samples, idx, 10, 1000);
    TEST("horizontal shake: triggers", triggered == 1);
}

/* Test 6: Vertical shake — should trigger */
void test_vertical_shake(void) {
    ShakeDetector det;
    detector_init(&det);

    /* ↑ fast, ↓ fast, ↑ fast, ↓ fast */
    int samples[40][2];
    int idx = 0;
    for (int dir = 0; dir < 8; dir++) {
        int sign = (dir % 2 == 0) ? -1 : 1;
        for (int i = 0; i < 5; i++) {
            samples[idx][0] = 0;
            samples[idx][1] = sign * 20;
            idx++;
        }
    }

    int triggered = feed_and_check(&det, samples, idx, 10, 1000);
    TEST("vertical shake: triggers", triggered == 1);
}

/* Test 7: Diagonal shake — should trigger */
void test_diagonal_shake(void) {
    ShakeDetector det;
    detector_init(&det);

    /* Diagonal shake: both axes reversing */
    int samples[40][2];
    int idx = 0;
    for (int dir = 0; dir < 8; dir++) {
        int sign = (dir % 2 == 0) ? -1 : 1;
        for (int i = 0; i < 5; i++) {
            samples[idx][0] = sign * 15;
            samples[idx][1] = sign * 15;
            idx++;
        }
    }

    int triggered = feed_and_check(&det, samples, idx, 10, 1000);
    TEST("diagonal shake: triggers", triggered == 1);
}

/* Test 8: Tiny jitter — should NOT trigger */
void test_tiny_jitter(void) {
    ShakeDetector det;
    detector_init(&det);

    /* Small random-looking jitter: ±1-3px rapidly */
    int samples[30][2];
    int pattern[] = {1, -2, 1, -1, 2, -1, 1, -2, 1, -1,
                     2, -1, 1, -1, 2, -2, 1, -1, 2, -1,
                     1, -2, 1, -1, 2, -1, 1, -2, 1, -1};
    for (int i = 0; i < 30; i++) {
        samples[i][0] = pattern[i];
        samples[i][1] = pattern[(i + 3) % 30];
    }

    int triggered = feed_and_check(&det, samples, 30, 5, 1000);
    TEST("tiny jitter: does not trigger", triggered == 0);
}

/* Test 9: Insufficient reversals — 2 reversals only */
void test_insufficient_reversals(void) {
    ShakeDetector det;
    detector_init(&det);

    /* Only 2 direction changes: → ← → (not enough) */
    int samples[15][2];
    int idx = 0;
    for (int dir = 0; dir < 3; dir++) {
        int sign = (dir % 2 == 0) ? 1 : -1;
        for (int i = 0; i < 5; i++) {
            samples[idx][0] = sign * 20;
            samples[idx][1] = 0;
            idx++;
        }
    }

    int triggered = feed_and_check(&det, samples, idx, 10, 1000);
    TEST("insufficient reversals: 2 reversals does not trigger", triggered == 0);
}

/* Test 10: Cooldown/retrigger behavior */
void test_cooldown_retrigger(void) {
    ShakeDetector det;
    detector_init(&det);

    /* First shake at t=1000..1390 (40 samples × 10ms) */
    int shake[40][2];
    int idx = 0;
    for (int dir = 0; dir < 8; dir++) {
        int sign = (dir % 2 == 0) ? -1 : 1;
        for (int i = 0; i < 5; i++) {
            shake[idx][0] = sign * 20;
            shake[idx][1] = 0;
            idx++;
        }
    }
    int first = feed_and_check(&det, shake, idx, 10, 1000);
    TEST("cooldown: first shake triggers", first == 1);

    /* Record where the shake was detected (last_shake_ms).
     * It was set during the first feed. The cooldown is 400ms from that point.
     * Try another shake immediately after — the samples at the end of
     * the first shake overlap, so we need to check that a fresh shake
     * within cooldown is blocked. */

    /* Feed a fresh shake entirely within cooldown window.
     * The first shake triggered around t≈1200-1350. Cooldown ≈ t+400.
     * So feeding a shake at t=1400..1790 should be blocked if the trigger
     * was late in the first sequence. But it might slip through if the
     * window still contains old reversal data. Let's explicitly check: */
    ShakeDetector det2;
    detector_init(&det2);
    /* Artificially set last_shake to simulate just-triggered state */
    det2.last_shake_ms = 5000;

    idx = 0;
    for (int dir = 0; dir < 8; dir++) {
        int sign = (dir % 2 == 0) ? -1 : 1;
        for (int i = 0; i < 5; i++) {
            shake[idx][0] = sign * 20;
            shake[idx][1] = 0;
            idx++;
        }
    }
    /* Feed shake within cooldown (5000 + 400 = 5400, our samples end at 5390) */
    int during_cooldown = feed_and_check(&det2, shake, idx, 10, 5000);
    TEST("cooldown: immediate retry blocked", during_cooldown == 0);

    /* Now feed the same shake after cooldown expires */
    idx = 0;
    for (int dir = 0; dir < 8; dir++) {
        int sign = (dir % 2 == 0) ? -1 : 1;
        for (int i = 0; i < 5; i++) {
            shake[idx][0] = sign * 20;
            shake[idx][1] = 0;
            idx++;
        }
    }
    int after_cooldown = feed_and_check(&det2, shake, idx, 10, 6000);
    TEST("cooldown: retrigger after cooldown works", after_cooldown == 1);
}

/* ─── Main ─── */

int main(void) {
    printf("Beacon shake-detector tests\n");
    printf("===========================\n\n");

    test_idle();
    test_slow_movement();
    test_normal_navigation();
    test_fast_swipe();
    test_horizontal_shake();
    test_vertical_shake();
    test_diagonal_shake();
    test_tiny_jitter();
    test_insufficient_reversals();
    test_cooldown_retrigger();

    printf("\n%d/%d tests passed\n", tests_passed, tests_run);

    return (tests_passed == tests_run) ? 0 : 1;
}
