#!/usr/bin/env python3
"""
Enumerate USB HID devices and inspect their input-report traffic using hidapi.

Requires the `hidapi` package (Cython bindings):  pip install hidapi
(Not the similarly named `hid` package, which has a different API.)

Examples:
    ./hid_inspect.py list
    ./hid_inspect.py list --vid 0x0e6f
    ./hid_inspect.py monitor --vid 0x0e6f --pid 0x0214
    ./hid_inspect.py monitor --path "DevSrvsID:4294970123" --diff --log pdp.log
    ./hid_inspect.py monitor --vid 0x0e6f --pid 0x0214 --changes-only --dump riffmaster.txt
    ./hid_inspect.py feature --vid 0x0e6f --pid 0x0214 --report-id 0x00 --size 64
"""

import argparse
import sys
import time

try:
    import hid
except ImportError:
    sys.exit("hidapi not installed. Run: pip install hidapi")

if not hasattr(hid, "device"):
    sys.exit("Wrong 'hid' module loaded. Uninstall 'hid' and install 'hidapi': "
             "pip uninstall hid && pip install hidapi")


def parse_int(value):
    return int(value, 0)


def fmt_path(path):
    return path.decode(errors="replace") if isinstance(path, bytes) else str(path)


def hexdump(data):
    return " ".join("%02x" % b for b in data)


def enumerate_devices(vid=0, pid=0, usage_page=None):
    devices = hid.enumerate(vid, pid)
    if usage_page is not None:
        devices = [d for d in devices if d.get("usage_page") == usage_page]
    return devices


def cmd_list(args):
    devices = enumerate_devices(args.vid, args.pid, args.usage_page)
    if not devices:
        print("No HID devices found.")
        return

    for i, d in enumerate(devices):
        print("[%d] %04x:%04x  %s - %s" % (
            i, d["vendor_id"], d["product_id"],
            d.get("manufacturer_string") or "?",
            d.get("product_string") or "?"))
        print("     path:        %s" % fmt_path(d["path"]))
        print("     serial:      %s" % (d.get("serial_number") or "-"))
        print("     release:     0x%04x" % d.get("release_number", 0))
        print("     interface:   %d" % d.get("interface_number", -1))
        print("     usage page:  0x%04x  usage: 0x%04x" % (
            d.get("usage_page", 0), d.get("usage", 0)))
        print()


def open_device(args):
    dev = hid.device()
    if args.path:
        path = args.path.encode()
        dev.open_path(path)
        return dev, args.path

    if not args.vid or not args.pid:
        sys.exit("Specify --path, or both --vid and --pid.")

    matches = enumerate_devices(args.vid, args.pid, args.usage_page)
    if not matches:
        sys.exit("No device %04x:%04x found." % (args.vid, args.pid))
    if len(matches) > 1:
        print("Note: %d interfaces match; opening the first. Use --path or "
              "--usage-page to pick another (see 'list')." % len(matches),
              file=sys.stderr)
    path = matches[0]["path"]
    dev.open_path(path)
    return dev, fmt_path(path)


def describe(dev):
    def safe(fn):
        try:
            return fn() or "?"
        except (IOError, ValueError):
            return "?"
    return "%s - %s (serial %s)" % (
        safe(dev.get_manufacturer_string),
        safe(dev.get_product_string),
        safe(dev.get_serial_number_string))


def diff_marker(prev, cur):
    """Return a line of '^^' under bytes that changed since the previous report."""
    if prev is None:
        return None
    marks = []
    for i, b in enumerate(cur):
        changed = i >= len(prev) or prev[i] != b
        marks.append("^^" if changed else "  ")
    return " ".join(marks).rstrip()


def cmd_monitor(args):
    try:
        dev, path = open_device(args)
    except IOError as e:
        sys.exit("Failed to open device: %s\n(On macOS, keyboards/mice need "
                 "Input Monitoring permission for your terminal; on Linux you "
                 "may need a udev rule or root.)" % e)

    log = open(args.log, "a") if args.log else None
    dump = open(args.dump, "w") if args.dump else None
    print("Opened %s" % path)
    print("Device: %s" % describe(dev))
    print("Reading input reports (Ctrl+C to stop)...\n")
    if dump:
        dump.write("# hid_inspect dump  %s\n" % time.strftime("%Y-%m-%d %H:%M:%S"))
        dump.write("# path:    %s\n" % path)
        dump.write("# device:  %s\n" % describe(dev))
        dump.write("# options: size=%d timeout=%d diff=%s changes_only=%s\n" % (
            args.size, args.timeout, args.diff, args.changes_only))
        dump.write("# columns: elapsed_s  #report  len  bytes(hex, byte 0 first)\n\n")
        dump.flush()
        print("Dumping to %s" % args.dump)

    dev.set_nonblocking(False)
    start = time.monotonic()
    prev = None
    count = 0

    try:
        while True:
            data = dev.read(args.size, timeout_ms=args.timeout)
            if not data:
                continue  # timeout, no report
            if args.changes_only and prev == data:
                continue

            count += 1
            t = time.monotonic() - start
            line = "%10.4f  #%-6d len=%-3d  %s" % (t, count, len(data), hexdump(data))
            print(line)
            if dump:
                dump.write(line + "\n")
            if args.diff:
                marker = diff_marker(prev, data)
                if marker and marker.strip():
                    # Align under the hex bytes
                    marker_line = " " * line.index(hexdump(data)) + marker
                    print(marker_line)
                    if dump:
                        dump.write(marker_line + "\n")
            if dump:
                dump.flush()
            if log:
                log.write(line + "\n")
                log.flush()
            prev = data
    except KeyboardInterrupt:
        print("\nStopped after %d reports." % count)
        if dump:
            dump.write("\n# stopped after %d reports\n" % count)
    except IOError as e:
        print("\nRead error (device unplugged?): %s" % e, file=sys.stderr)
    finally:
        dev.close()
        if log:
            log.close()
        if dump:
            dump.close()


def cmd_feature(args):
    dev, path = open_device(args)
    try:
        data = dev.get_feature_report(args.report_id, args.size)
        print("Feature report 0x%02x from %s (%d bytes):" % (args.report_id, path, len(data)))
        print(hexdump(data))
    except IOError as e:
        sys.exit("get_feature_report failed: %s" % e)
    finally:
        dev.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    def add_selector(p):
        p.add_argument("--vid", type=parse_int, default=0, help="vendor ID, e.g. 0x0e6f")
        p.add_argument("--pid", type=parse_int, default=0, help="product ID")
        p.add_argument("--usage-page", type=parse_int, default=None,
                       help="filter by HID usage page, e.g. 0x01 (Generic Desktop)")

    p_list = sub.add_parser("list", help="enumerate HID devices")
    add_selector(p_list)
    p_list.set_defaults(func=cmd_list)

    p_mon = sub.add_parser("monitor", help="read and print input reports")
    add_selector(p_mon)
    p_mon.add_argument("--path", help="open by device path (from 'list')")
    p_mon.add_argument("--size", type=int, default=64, help="max report size to read (default 64)")
    p_mon.add_argument("--timeout", type=int, default=1000, help="read timeout in ms (default 1000)")
    p_mon.add_argument("--diff", action="store_true", help="mark bytes that changed since last report")
    p_mon.add_argument("--changes-only", action="store_true", help="suppress identical repeated reports")
    p_mon.add_argument("--log", help="append reports to this file")
    p_mon.add_argument("--dump", help="write a capture (header, reports and --diff markers) "
                                      "to this file, overwriting it")
    p_mon.set_defaults(func=cmd_monitor)

    p_feat = sub.add_parser("feature", help="read a feature report")
    add_selector(p_feat)
    p_feat.add_argument("--path", help="open by device path (from 'list')")
    p_feat.add_argument("--report-id", type=parse_int, default=0)
    p_feat.add_argument("--size", type=int, default=64)
    p_feat.set_defaults(func=cmd_feature)

    args = parser.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
