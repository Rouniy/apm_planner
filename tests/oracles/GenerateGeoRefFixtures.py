"""Generate disposable real JPEG/TIFF and DataFlash GeoRef fixtures (Pillow).

Usage: python GenerateGeoRefFixtures.py NEW_DIRECTORY
Run comparison processes with TZ=UTC unless testing explicit local EXIF zones.
"""
import datetime as dt
import json
import sys
from pathlib import Path

from PIL import Image


def definition(number, name, layout, columns):
    widths = dict(zip("bBhHiIqQfdcCeELnNZa", (1, 1, 2, 2, 4, 4, 8, 8,
        4, 8, 2, 2, 4, 4, 4, 4, 16, 64, 64)))
    return f"FMT,{number},{3 + sum(widths[x] for x in layout)},{name},{layout},{columns}\n"


def image(path, when, big_endian=False):
    exif = Image.Exif()
    exif[0x010E] = "GeoRef independent fixture; preserve this description"
    exif[0x0112] = 6
    exif[0x8769] = {0x9003: when.strftime("%Y:%m:%d %H:%M:%S"),
                     0x9004: "2000:01:01 00:00:00", 0x927C: b"PRESERVE-MAKER-NOTE"}
    if big_endian:
        pixels = Image.new("I;16B", (4, 3), 513)
    else:
        pixels = Image.new("RGB", (8, 6), (14, 120, 70))
    pixels.save(path, exif=exif.tobytes())
    with Image.open(path) as check:
        assert check.getexif().get_ifd(0x8769)[0x9003] == exif[0x8769][0x9003]


def main():
    root = Path(sys.argv[1]); root.mkdir(exist_ok=False)
    photos = root / "photos"; photos.mkdir()
    week, start_ms = 2400, 200000
    cam_epoch = dt.datetime(1980, 1, 6) + dt.timedelta(weeks=week, milliseconds=start_ms, seconds=-17)
    for i in range(7):
        suffix = ".jpg" if i < 4 else ".tif" if i < 6 else ".tiff"
        image(photos / f"photo-{i:02}{suffix}", cam_epoch + dt.timedelta(seconds=12 + i * 2), i == 6)
    short = root / "short-photos"; short.mkdir()
    image(short / "one.jpg", cam_epoch + dt.timedelta(seconds=12))
    image(short / "two.tif", cam_epoch + dt.timedelta(seconds=14))
    log = definition(128, "FMT", "BBnNZ", "Type,Length,Name,Format,Columns")
    log += definition(150, "GPS", "QBIHLLee", "TimeUS,Status,GMS,GWk,Lat,Lng,Alt,RAlt")
    log += definition(151, "GPS2", "QBIHLLee", "TimeUS,Status,GMS,GWk,Lat,Lng,Alt,RAlt")
    log += definition(152, "ATT", "Qfff", "TimeUS,Roll,Pitch,Yaw")
    columns = "TimeUS,GPSTime,GPSWeek,Lat,Lng,Alt,RelAlt,GPSAlt,R,P,Y"
    log += definition(153, "CAM", "QIHLLeeefff", columns)
    log += definition(154, "TRIG", "QIHLLeeefff", columns)
    for i in range(65):
        board = 1000000 + i * 250000
        ms = start_ms + i * 250
        log += f"ATT,{board},1.25,-2.5,{90+i*0.25}\n"
        log += f"GPS,{board},3,{ms},{week},{47.5+i/100000},{8.5+i/100000},{450+i/10},20\n"
        log += f"GPS2,{board},3,{ms},{week},{-35.5+i/100000},{149.5+i/100000},{550+i/10},30\n"
        if i % 8 == 0 and i // 8 < 7:
            row = f"{board},{ms},{week},{47.6+i/100000},{8.6+i/100000},123.5,12.5,456.5,3.25,-4.5,123.75\n"
            log += "CAM," + row + "TRIG," + row
    source = root / "flight.log"; source.write_text(log, encoding="ascii")
    cases = {
        "cam": {"mode": "cam"},
        "trig": {"mode": "trig"},
        "offset": {"mode": "offset", "timeOffsetSeconds": 13},
        "gps2-offset": {"mode": "offset", "timeOffsetSeconds": 13, "useGps2": True},
        "cam-corrected": {"mode": "cam", "shutterLagMilliseconds": 250,
                          "useGpsAltitude": True, "baseAltitudeAdjustmentMeters": -12.5},
        "cam-amsl": {"mode": "cam", "useAmslAltitude": True},
        "trig-gps2": {"mode": "trig", "useGps2": True, "shutterLagMilliseconds": -250,
                      "useGpsAltitude": True, "baseAltitudeAdjustmentMeters": 10.25},
        "negative-altitude": {"mode": "cam", "baseAltitudeAdjustmentMeters": -500},
        "cam-count-mismatch": {"mode": "cam", "photoDirectory": str(short)},
        "trig-count-mismatch": {"mode": "trig", "photoDirectory": str(short)},
    }
    for name, overrides in cases.items():
        values = {"logPath": str(source), "photoDirectory": str(photos),
                  "outputDirectory": str(root / ("qt-" + name))}
        values.update(overrides)
        (root / (name + ".json")).write_text(json.dumps(values, indent=2), encoding="utf-8")
    print(root)


if __name__ == "__main__":
    main()
