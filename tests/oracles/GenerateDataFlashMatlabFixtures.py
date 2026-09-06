"""Generate small, deterministic binary oracle fixtures in a NEW directory."""
import math
import struct
import sys
from pathlib import Path


def fmt(number, length, name, layout, columns):
    return b"\xa3\x95\x80" + struct.pack("<BB4s16s64s", number, length,
        name.encode("ascii"), layout.encode("ascii"), columns.encode("ascii"))


def packet(number, layout, *values):
    return b"\xa3\x95" + bytes([number]) + struct.pack("<" + layout, *values)


def main():
    root = Path(sys.argv[1])
    root.mkdir(exist_ok=False)
    header = fmt(128, 89, "FMT", "BBnNZ", "Type,Length,Name,Format,Columns")
    # Modes are resolved through the same initialized MP10 GUI callback.
    log = header
    log += fmt(150, 75, "MSG", "QZ", "TimeUS,Message")
    log += fmt(151, 14, "MODE", "QMBB", "TimeUS,Mode,ModeNum,Rsn")
    log += fmt(152, 71, "VAL", "QfdcCeELqQN", "TimeUS,F,D,C,U,E,V,Lat,S,Q,Text")
    log += fmt(153, 31, "PARM", "QNf", "TimeUS,Name,Value")
    log += packet(150, "Q64s", 1_000_000, b"ArduCopter V4.5.7")
    log += packet(151, "QBBB", 1_000_001, 3, 3, 1)
    log += packet(152, "QfdhHiIi qQ16s".replace(" ", ""), 1_000_002,
                  0.1, 0.10000000000000001, -125, 65535, -267, 4294967295,
                  473977419, -9223372036854775808, 18446744073709551615, b"123.25")
    log += packet(152, "QfdhHiIiqQ16s", 1_100_002,
                  -0.0, -math.inf, 0, 0, 0, 0, 0, -1, 0, b"text")
    log += packet(153, "Q16sf", 1_200_000, b"NEG_ZERO", -0.0)
    log += packet(153, "Q16sf", 1_200_001, b"FLOAT_PARAM", 0.1)
    log += packet(150, "Q64s", 1_300_000, b"[1 2 -3 NaN Infinity]")
    with (root / "binary-values.bin").open("xb") as stream:
        stream.write(log)
    # Every resolver entry can change numeric MODE to zero. Keep one unresolved
    # row before firmware detection, then exercise all common mode numbers.
    for firmware in ("ArduCopter", "ArduPlane", "ArduRover"):
        modes = header + fmt(150, 75, "MSG", "QZ", "TimeUS,Message")
        modes += fmt(151, 14, "MODE", "QMBB", "TimeUS,Mode,ModeNum,Rsn")
        modes += packet(151, "QBBB", 100, 3, 3, 1)
        modes += packet(150, "Q64s", 200, (firmware + " V4.5.7").encode("ascii"))
        for mode in range(32):
            modes += packet(151, "QBBB", 300 + mode, mode, mode, 1)
        with (root / ("modes-" + firmware + ".bin")).open("xb") as stream:
            stream.write(modes)
    # Source-order labels, duplicated identical FMT and sparse logical line ids.
    log = header + fmt(155, 11, "TST", "Q", "TimeUS")
    log += packet(155, "Q", 100)
    log += fmt(155, 11, "TST", "Q", "TimeUS")
    log += packet(155, "Q", 200)
    with (root / "duplicate-fmt.bin").open("xb") as stream:
        stream.write(log)
    # MP10 re-emits the next record for each empty binary Z field. Qt omits
    # malformed records, never inventing numeric rows with another frame's id.
    log = header + fmt(150, 67, "MSG", "Z", "Message")
    log += fmt(151, 67, "FILE", "Z", "Data")
    log += fmt(152, 11, "VAL", "Q", "TimeUS")
    log += fmt(153, 76, "UNIT", "QBZ", "TimeUS,Id,Label")
    log += packet(151, "64s", b"") + packet(151, "64s", b"")
    log += packet(152, "Q", 123456)
    log += packet(153, "QB64s", 100, 99, b"")
    log += packet(150, "64s", b"hello")
    with (root / "empty-blob-recovery.bin").open("xb") as stream:
        stream.write(log)
    # Complete empty log has PARM 0x2 and Seen 0x1, no invented numeric matrix.
    with (root / "empty.log").open("xb"):
        pass
    # The source reference excludes an unterminated final ASCII record.
    with (root / "unterminated.log").open("xb") as stream:
        stream.write(b"FMT,128,89,FMT,BBnNZ,Type,Length,Name,Format,Columns\n"
                     b"FMT,155,11,TST,Q,TimeUS\nTST,100\nTST,200")
    if "--large" in sys.argv[2:]:
        with (root / "large-x64.bin").open("xb") as stream:
            stream.write(header + fmt(155, 11, "TST", "Q", "TimeUS"))
            # 2,000,003 logical records must still produce one x64 MAT file.
            for start in range(0, 2_000_001, 8192):
                stream.write(b"".join(packet(155, "Q", index * 1000)
                    for index in range(start, min(start + 8192, 2_000_001))))
    print(root)


if __name__ == "__main__":
    main()
