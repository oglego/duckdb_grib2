#!/usr/bin/env python3
"""Extract one message from a GRIB2 file without decoding it. Needs only the Python standard library.

NBM files hold hundreds of messages and can be tens of megabytes. Test fixtures only need one message, so cut it out:

    python3 scripts/extract_message.py fixtures/nbm/blend.t00z.core.f001.ak.grib2 fixtures/nbm_ak_msg.grib2

By default the first message is extracted. Use --index N for the N-th message (1-based, the same numbering as the
first column of the .idx files).
"""

import argparse
import struct
import sys


def iterate_messages(f):
    """Yield (offset, length) for every GRIB2 message, hopping by the length in each indicator section."""
    offset = 0
    while True:
        f.seek(offset)
        head = f.read(16)
        if len(head) < 16:
            return
        if head[:4] != b"GRIB":
            sys.exit(f"error: no GRIB marker at byte {offset}")
        if head[7] != 2:
            sys.exit(f"error: byte {offset}: GRIB edition {head[7]} is not supported (only edition 2)")
        length = struct.unpack(">Q", head[8:16])[0]
        yield offset, length
        offset += length


def main():
    parser = argparse.ArgumentParser(description="Extract one message from a GRIB2 file")
    parser.add_argument("input")
    parser.add_argument("output")
    parser.add_argument("--index", type=int, default=1, help="1-based message number (default: 1)")
    args = parser.parse_args()

    with open(args.input, "rb") as f:
        for number, (offset, length) in enumerate(iterate_messages(f), start=1):
            if number != args.index:
                continue
            f.seek(offset)
            data = f.read(length)
            if len(data) != length or data[-4:] != b"7777":
                sys.exit(f"error: message {number} is truncated or malformed")
            with open(args.output, "wb") as out:
                out.write(data)
            print(f"wrote message {number} (bytes {offset} to {offset + length - 1}, {length} bytes) to {args.output}")
            return
    sys.exit(f"error: {args.input} has fewer than {args.index} messages")


if __name__ == "__main__":
    main()
