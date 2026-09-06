"""Compare actual MP10 GeoRef oracle with GeoRefProbe on private fixtures.

Usage: python CompareGeoRef.py REFERENCE/oracle.json QT_PROBE.json OPTIONS.json
Requires Pillow. Matching/report numeric tolerance is explicitly 1e-12 absolute
(decimal-to-binary conversion/formatting), EXIF DMS 1e-9 degrees, altitude 1e-6m.
Does not claim byte-identical XML formatting or reference metadata preservation.
"""
import json
import hashlib
import math
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

from PIL import Image


def equal_number(actual, expected, tolerance=1e-12):
    assert math.isfinite(float(actual)) and math.isfinite(float(expected))
    assert abs(float(actual) - float(expected)) <= tolerance, (actual, expected, tolerance)


def compare_xml(actual, expected):
    assert actual.tag == expected.tag and actual.attrib == expected.attrib
    left, right = (actual.text or "").strip(), (expected.text or "").strip()
    if actual.tag.endswith("}coordinates"):
        a, b = left.replace(",", " ").split(), right.replace(",", " ").split()
        assert len(a) == len(b)
        for x, y in zip(a, b):
            equal_number(x, y)
    else:
        assert left == right, (actual.tag, left, right)
    assert len(actual) == len(expected)
    for a, b in zip(actual, expected):
        compare_xml(a, b)


def coordinate(gps, number):
    d, m, s = gps[number]
    result = float(d) + float(m) / 60 + float(s) / 3600
    ref = gps[number - 1]
    if isinstance(ref, bytes):
        ref = ref.decode("ascii").rstrip("\0")
    return -result if ref in ("S", "W") else result


def compare(reference, qt, options):
    assert reference["error"] is None, reference["error"]
    inputs = reference["inputs"]
    def unchanged(path, record):
        data = Path(path).read_bytes()
        assert len(data) == record["size"]
        assert hashlib.sha256(data).hexdigest() == record["sha256"], str(path)
    unchanged(options["logPath"], inputs["logCopy"])
    for record in inputs["photos"]:
        unchanged(Path(options["photoDirectory"]) / record["name"], record)
    if "estimate" in reference:
        assert qt["success"] and qt["hasEstimate"] and not qt["error"], qt
        assert qt["offsetSeconds"] == reference["estimate"]["offsetSeconds"]
        return {"estimated_offset_seconds": qt["offsetSeconds"], "inputs_unchanged": True}
    folder = Path(options.get("outputDirectory") or Path(options["photoDirectory"]) / "geotagged")
    expected = reference["matches"]
    if not expected:
        assert not qt["success"] and qt["error"], qt
        assert not qt.get("matches") and not qt.get("publishedPaths") and not qt.get("taggedPhotos")
        assert not folder.exists() or not list(folder.iterdir())
        return {"matches": 0, "empty_reference_rejected": True}
    assert qt["success"], qt
    assert not qt["cancelled"] and not qt["error"] and not qt["failedPaths"]
    assert len(qt["matches"]) == len(expected)
    assert qt["taggedPhotos"] == len(expected) and qt["failedPhotos"] == 0
    outputs = [folder / "location.txt", folder / "location.kml"] + [
        folder / (Path(m["name"]).stem + "_geotag" + Path(m["name"]).suffix) for m in expected]
    assert qt["publishedPaths"] == [str(p) for p in outputs]
    assert set(folder.iterdir()) == set(outputs), list(folder.iterdir())
    for a, b in zip(qt["matches"], expected):
        assert Path(a["sourcePath"]) == Path(options["photoDirectory"]) / b["name"]
        assert Path(a["outputPath"]) == folder / (Path(b["name"]).stem + "_geotag" + Path(b["name"]).suffix)
        assert a["timeUtc"] == b["timeUtc"], (a["timeUtc"], b["timeUtc"])
        for key in ("latitude", "longitude", "altitude", "roll", "pitch", "yaw"):
            equal_number(a[key], b[key])
        with Image.open(a["sourcePath"]) as src, Image.open(a["outputPath"]) as dst:
            assert (src.mode, src.size, src.tobytes()) == (dst.mode, dst.size, dst.tobytes())
            old, new = src.getexif(), dst.getexif()
            for tag in (0x010E, 0x0112):
                assert new.get(tag) == old.get(tag), (a["sourcePath"], tag)
            for tag in (0x9003, 0x9004, 0x927C):
                assert new.get_ifd(0x8769).get(tag) == old.get_ifd(0x8769).get(tag)
            gps = new.get_ifd(0x8825)
            equal_number(coordinate(gps, 2), b["latitude"], 1e-9)
            equal_number(coordinate(gps, 4), b["longitude"], 1e-9)
            altitude = float(gps[6])
            if gps[5] in (1, b"\x01"):
                altitude = -altitude
            equal_number(altitude, b["altitude"], 1e-6)
    # The reference's negative-altitude EXIF writer can fail separately from
    # its reports; reports are still compared whenever actually produced.
    report = reference["geotag"]
    actual_lines = (folder / "location.txt").read_text().splitlines()
    expected_lines = report["locationTxt"].splitlines()
    assert actual_lines[0] == expected_lines[0]
    assert len(actual_lines) == len(expected_lines)
    for a, b in zip(actual_lines[1:], expected_lines[1:]):
        a, b = a.split(), b.split()
        assert len(a) == len(b) and a[0] == b[0]
        for x, y in zip(a[1:], b[1:]):
            equal_number(x, y)
    compare_xml(ET.fromstring((folder / "location.kml").read_bytes()),
                ET.fromstring(report["locationKml"]))
    return {"matches": len(expected), "decoded_pixels_and_selected_metadata_preserved": len(expected),
            "reports": 2, "numeric_absolute_tolerance": 1e-12, "inputs_unchanged": True,
            "negative_altitude_is_qt_fix_not_reference_exif_parity": any(m["altitude"] < 0 for m in expected)}


if __name__ == "__main__":
    ref, qt, options = (json.loads(Path(p).read_text()) for p in sys.argv[1:])
    print(json.dumps(compare(ref, qt, options), sort_keys=True))
