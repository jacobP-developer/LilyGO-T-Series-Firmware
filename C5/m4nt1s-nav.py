#!/usr/bin/env python3
"""
m4nt1s-nav.py — arrow-key controller for M4NT1S firmware (ESP32-C5).

This is a transparent proxy: it opens the USB-Serial-JTAG CDC-ACM
(which appears as /dev/ttyACM0 or COMx) and forwards stdin bytes
straight to it. The firmware parses the ANSI escape sequences and
posts input events.

Keys:
  ↑ / ↓       move selection
  Enter       select
  ← or ESC    back
  Ctrl+C      quit
"""
import argparse
import os
import sys
import time
import select

try:
    import serial
except ImportError:
    print("pyserial required: pip install pyserial", file=sys.stderr)
    sys.exit(1)


def detect_env():
    if sys.platform == "win32":
        return "windows"
    if sys.platform == "darwin":
        return "macos"
    if ("com.termux" in os.environ.get("PREFIX", "")
            or os.environ.get("TERMUX_VERSION")
            or os.environ.get("ANDROID_ROOT")):
        return "android"
    return "linux"


ENV = detect_env()

# Espressif VID — USB-Serial-JTAG devices report this
TARGET_VID = 0x303A


def pick_port(arg_port):
    if arg_port:
        return arg_port

    if ENV == "android":
        for node in ("/dev/ttyACM0", "/dev/ttyACM1", "/dev/ttyUSB0"):
            if os.path.exists(node):
                return node
        return "/dev/ttyACM0"

    try:
        import serial.tools.list_ports
        ports = list(serial.tools.list_ports.comports())
        # Match Espressif VID (USB-Serial-JTAG)
        for p in ports:
            if p.vid == TARGET_VID:
                return p.device
        # Generic ttyACM / COM fallback
        for p in ports:
            if "ttyACM" in p.device or "COM" in p.device:
                return p.device
    except Exception:
        pass

    return "COM3" if ENV == "windows" else "/dev/ttyACM0"


def open_serial(port):
    for attempt in range(1, 3):
        try:
            return serial.Serial(port, 115200, timeout=0.1)
        except Exception as e:
            if attempt < 2:
                print(f"[serial] {port} failed: {e} — retrying in 5s",
                      file=sys.stderr)
                time.sleep(5)
            else:
                print(f"[serial] cannot open {port}: {e}", file=sys.stderr)
    return None


def input_windows(ser, running):
    import msvcrt
    while running[0]:
        if not msvcrt.kbhit():
            time.sleep(0.01)
            continue
        ch = msvcrt.getch()
        if ch == b'\x03':
            running[0] = False
            return
        if ch in (b'\x00', b'\xe0'):
            ch2 = msvcrt.getch()
            if   ch2 == b'H': ser.write(b'\x1b[A')   # up
            elif ch2 == b'P': ser.write(b'\x1b[B')   # down
            elif ch2 == b'K': ser.write(b'\x1b[D')   # left → back
            elif ch2 == b'M': ser.write(b'\x1b[C')   # right (reserved)
            continue
        if ch in (b'\r', b'\n'):
            ser.write(b'\r')
        elif ch == b'\x1b':
            ser.write(b'\x1b[D')
        # everything else ignored


def input_posix(ser, running):
    import termios
    import tty
    fd = sys.stdin.fileno()
    old = termios.tcgetattr(fd)
    try:
        tty.setraw(fd)
        while running[0]:
            r, _, _ = select.select([sys.stdin], [], [], 0.1)
            if not r:
                continue
            data = os.read(fd, 64)
            if not data:
                continue
            if b'\x03' in data:
                running[0] = False
                return
            # Forward everything as-is: arrow keys already arrive as ANSI
            ser.write(data)
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default=None,
                    help="Explicit port (e.g. /dev/ttyACM0 or COM5)")
    args = ap.parse_args()

    port = pick_port(args.port)
    ser = open_serial(port)
    if ser is None:
        sys.exit(1)

    print(f"[nav] connected {port} ({ENV})", file=sys.stderr)
    print("[nav] arrows / enter to control. Ctrl+C to quit.", file=sys.stderr)

    running = [True]
    try:
        if ENV == "windows":
            input_windows(ser, running)
        else:
            input_posix(ser, running)
    except KeyboardInterrupt:
        pass

    try:
        ser.close()
    except Exception:
        pass


if __name__ == "__main__":
    main()