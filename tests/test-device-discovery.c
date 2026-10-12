/*
 * test-device-discovery.c — Unit tests for pointer device classification
 *
 * Builds synthetic libevdev devices (no hardware or uinput needed) and checks
 * is_pointer_device() from scripts/wiggle-monitor.c:
 *   - Mice with 2, 3, 4 and 6 relative axes are accepted
 *   - A composite receiver advertising alphanumeric keys is accepted
 *   - A pointer whose name contains "keyboard" is accepted
 *   - Touchpads and direct (touchscreen) devices are rejected
 *   - Devices without REL_X/REL_Y are rejected
 *   - The listed non-pointer keyboard dial is rejected by USB ID only, and the
 *     same capabilities under another ID are still accepted
 *
 * MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#define main wiggle_monitor_main
#include "../scripts/wiggle-monitor.c"
#undef main

static int failures;

static struct libevdev *make(const char *name, int vendor, int product) {
    struct libevdev *d = libevdev_new();
    libevdev_set_name(d, name);
    libevdev_set_id_vendor(d, vendor);
    libevdev_set_id_product(d, product);
    return d;
}

static void rel(struct libevdev *d, int count) {
    static const int axes[] = { REL_X, REL_Y, REL_WHEEL, REL_HWHEEL, REL_WHEEL_HI_RES, REL_HWHEEL_HI_RES };
    for (int i = 0; i < count; i++) libevdev_enable_event_code(d, EV_REL, axes[i], NULL);
}

static void check(const char *label, struct libevdev *d, bool expected) {
    bool got = is_pointer_device(d);
    printf("  %s %s\n", got == expected ? "ok  " : "FAIL", label);
    if (got != expected) failures++;
    libevdev_free(d);
}

int main(void) {
    struct libevdev *d;

    for (int n = 2; n <= 6; n++) {
        char label[64];
        d = make("Generic Mouse", 0x1234, 0x0001);
        rel(d, n);
        libevdev_enable_event_code(d, EV_KEY, BTN_LEFT, NULL);
        snprintf(label, sizeof(label), "mouse with %d relative axes is accepted", n);
        check(label, d, true);
    }

    d = make("USB Receiver", 0x046d, 0xc547);
    rel(d, 2);
    libevdev_enable_event_code(d, EV_KEY, BTN_LEFT, NULL);
    libevdev_enable_event_code(d, EV_KEY, KEY_A, NULL);
    libevdev_enable_event_code(d, EV_KEY, KEY_SPACE, NULL);
    check("composite receiver advertising keys is accepted", d, true);

    d = make("Vendor Keyboard Trackpoint", 0x1234, 0x0002);
    rel(d, 2);
    libevdev_enable_event_code(d, EV_KEY, BTN_LEFT, NULL);
    check("pointer with 'keyboard' in its name is accepted", d, true);

    d = make("Touchpad", 0x1234, 0x0003);
    rel(d, 2);
    libevdev_enable_event_code(d, EV_ABS, ABS_MT_POSITION_X, &(struct input_absinfo){ .maximum = 100 });
    check("touchpad is rejected", d, false);

    d = make("Touchscreen", 0x1234, 0x0004);
    rel(d, 2);
    libevdev_enable_property(d, INPUT_PROP_DIRECT);
    check("direct device is rejected", d, false);

    d = make("Keyboard", 0x1234, 0x0005);
    libevdev_enable_event_code(d, EV_KEY, KEY_A, NULL);
    check("device without REL_X/REL_Y is rejected", d, false);

    d = make("Asus Keyboard", 0x0b05, 0x196b);
    rel(d, 4);
    libevdev_enable_event_code(d, EV_KEY, BTN_LEFT, NULL);
    libevdev_enable_event_code(d, EV_KEY, BTN_RIGHT, NULL);
    libevdev_enable_event_code(d, EV_KEY, BTN_MIDDLE, NULL);
    check("listed keyboard media dial is rejected", d, false);

    d = make("Asus Keyboard", 0x0b05, 0x196c);
    rel(d, 4);
    libevdev_enable_event_code(d, EV_KEY, BTN_LEFT, NULL);
    check("same capabilities under another product ID are accepted", d, true);

    printf(failures ? "%d failure(s)\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
