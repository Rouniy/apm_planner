"""Independent semantic comparisons; never changes either input.

  python3 CompareDataFlashTools.py json MP10.json QT.json
  python3 CompareDataFlashTools.py gpx MP10.track.json QT.gpx
"""
import json
import math
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def differences(expected, actual, path="$", found=None):
    found = [] if found is None else found
    if type(expected) is not type(actual):
        found.append((path, expected, actual))
    elif isinstance(expected, dict):
        if expected.keys() != actual.keys():
            found.append((path + ".keys", list(expected), list(actual)))
        for key in expected.keys() & actual.keys():
            differences(expected[key], actual[key], path + "." + key, found)
    elif isinstance(expected, list):
        if len(expected) != len(actual):
            found.append((path + ".length", len(expected), len(actual)))
        for index, (left, right) in enumerate(zip(expected, actual)):
            differences(left, right, f"{path}[{index}]", found)
    elif expected != actual:
        found.append((path, expected, actual))
    return found


def main():
    mode, reference, result = sys.argv[1:]
    expected = json.loads(Path(reference).read_text(encoding="utf-8-sig"))
    if mode == "json":
        actual = json.loads(Path(result).read_text(encoding="utf-8-sig"))
    elif mode == "gpx":
        namespace = "{http://www.topografix.com/GPX/1/1}"
        root = ET.parse(result).getroot()
        assert root.tag == namespace + "gpx"
        assert root.attrib == {"version": "1.1", "creator": "Mission Planner"}
        assert len(root.findall(namespace + "trk")) == 1
        assert len(root.findall(f"{namespace}trk/{namespace}trkseg")) == 1
        actual = []
        for point in root.findall(f"{namespace}trk/{namespace}trkseg/{namespace}trkpt"):
            actual.append({"lat": float(point.attrib["lat"]),
                           "lon": float(point.attrib["lon"]),
                           "ele": float(point.findtext(namespace + "ele")),
                           "time": point.findtext(namespace + "time")})
        # Qt deliberately excludes non-finite coordinates admitted by MP10's
        # range comparisons; this is a disclosed validity fix, not a tolerance.
        expected = [point for point in expected if all(
            math.isfinite(float(point[key])) for key in ("lat", "lon", "ele"))]
        for point in expected:
            for key in ("lat", "lon", "ele"):
                point[key] = float(point[key])
    else:
        raise ValueError("Expected json or gpx mode")
    errors = differences(expected, actual)
    for path, left, right in errors[:30]:
        print(f"DIFF {path}: MP10={left!r} QT={right!r}")
    print(f"{mode}: {len(expected)} rows, {len(errors)} differences")
    return bool(errors)


if __name__ == "__main__":
    sys.exit(main())
