# Beacon

**Shake your mouse to find your cursor.**

Beacon recreates the macOS/KDE-style "find my cursor" behavior on Omarchy/Hyprland. When you rapidly shake your mouse left/right or up/down, the cursor temporarily becomes much larger so it's easy to locate, then smoothly returns to its original size.

## Install

```bash
omarchy plugin add https://github.com/sanjyay/beacon --enable
```

## How It Works

1. A bundled `beacon-monitor` helper reads mouse motion events from the kernel input subsystem (evdev)
2. Shake detection runs inside the helper — zero IPC overhead during normal mouse use
3. When a deliberate shake is detected, the helper outputs `SHAKE` to the QML service
4. The service temporarily enlarges your cursor using `hyprctl setcursor`
5. After movement settles (2 seconds), the original cursor theme and size are restored

## Requirements

- Omarchy with Quattro shell
- Hyprland
- User must be in the `input` group (standard on Omarchy)
- `libevdev` (standard on Arch Linux)
- `gcc` (for building the helper on first install)

## Building the Helper

The monitor helper needs to be compiled once:

```bash
cd scripts/
gcc -O2 -o beacon-monitor beacon-monitor.c $(pkg-config --cflags --libs libevdev) -lm
```

## License

MIT
