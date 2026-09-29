"""Record a testing session from the wearable, with a label before each trial.

    python3 tools/trial_capture.py                    # /dev/ttyACM0
    python3 tools/trial_capture.py --port /dev/ttyACM1

Linux version of tools/trial_capture.ps1, same log format, so
tools/analyse_trials.py reads either. Type a label and press Enter before
each trial, then do the movement. The label is stamped with the board's own
clock (the last CSV row seen). Ctrl+C when you are done.

Name trials so the first word says what they are:

    fall 1      a simulated fall                (should be detected)
    walk 1      walking on the spot             (should not be)
    sit 1       sitting down firmly
    bend 1      bending to pick something up
    lower 1     lowering yourself slowly to the floor
    shake 1     shaking the board about
"""
import argparse
import os
import re
import select
import sys
import termios
import time

ROW = re.compile(r"^(\d+),")

CYAN = "\033[36m"
YELLOW = "\033[33m"
RESET = "\033[0m"


def open_serial(port, baud):
    """Raw 8N1 serial port without pyserial."""
    speed = getattr(termios, "B%d" % baud)
    fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[0] = 0                                        # iflag
    attrs[1] = 0                                        # oflag
    attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    attrs[3] = 0                                        # lflag: raw
    attrs[4] = speed
    attrs[5] = speed
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIFLUSH)
    return fd


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", default="/dev/ttyACM0")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    out = args.out
    if not out:
        here = os.path.dirname(os.path.abspath(__file__))
        stamp = time.strftime("%Y%m%d_%H%M%S")
        out = os.path.join(here, "..", "trials", "trial_%s.log" % stamp)
    out = os.path.normpath(out)
    os.makedirs(os.path.dirname(out), exist_ok=True)

    try:
        fd = open_serial(args.port, args.baud)
    except OSError as err:
        print("Could not open %s: %s" % (args.port, err))
        return 1

    print("Recording to %s" % out)
    print("Type a label and press Enter before each trial, e.g. 'fall 1' or 'walk 1'.")
    print("Ctrl+C when you are done.\n")

    pending = b""
    last_tick = 0

    with open(out, "w", encoding="utf-8") as log:
        try:
            while True:
                ready, _, _ = select.select([fd, sys.stdin], [], [], 0.2)

                if fd in ready:
                    try:
                        pending += os.read(fd, 4096)
                    except BlockingIOError:
                        pass
                    *lines, pending = pending.split(b"\n")
                    for raw in lines:
                        line = raw.decode("utf-8", "replace").rstrip("\r")
                        log.write(line + "\n")

                        hit = ROW.match(line)
                        if hit:
                            last_tick = int(hit.group(1))

                        # CSV rows would scroll everything else away
                        if line.startswith(("##", ">>", "net:", "WARNING")):
                            print(CYAN + line + RESET)
                    log.flush()

                if sys.stdin in ready:
                    label = sys.stdin.readline()
                    if not label:
                        break                   # stdin closed
                    label = label.strip()
                    if label:
                        mark = "# MARK t=%d %s" % (last_tick, label)
                        log.write(mark + "\n")
                        log.flush()
                        print(YELLOW + mark + RESET)
        except KeyboardInterrupt:
            pass
        finally:
            os.close(fd)

    print("\nsaved %s" % out)
    print("analyse with:  python3 tools/analyse_trials.py \"%s\"" % out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
