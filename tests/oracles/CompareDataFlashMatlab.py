"""Independent SciPy + raw Level-5 comparison for actual MP10 DataFlash exports.

Only Seen enumeration order is intrinsically unspecified by MP10's Hashtable.
Reference-only FMTU bug labels must be named explicitly, never wildcard ignored.
Requires numpy/scipy; reads both files without modifying either.
"""
import argparse
import collections
import json
import struct
import zlib
from pathlib import Path

import numpy as np
from scipy.io import loadmat


def element(data, position):
    first, second = struct.unpack_from("<II", data, position)
    if first >> 16:
        size = first >> 16
        if size > 4:
            raise ValueError("Invalid small element")
        return first & 0xffff, data[position + 4:position + 4 + size], position + 8
    end = position + 8 + second
    if end > len(data):
        raise ValueError("Truncated MAT element")
    return first, data[position + 8:end], (end + 7) & ~7


def top_level(path):
    data = Path(path).read_bytes()
    if len(data) < 128 or data[126:128] != b"IM":
        raise ValueError("Expected little-endian Level-5 MAT")
    rows = []

    def scan(stream, offset):
        while offset < len(stream):
            kind, body, offset = element(stream, offset)
            if kind == 15:
                scan(zlib.decompress(body), 0)
                continue
            if kind != 14:
                raise ValueError(f"Unexpected top-level kind {kind}")
            _, _, at = element(body, 0)  # flags
            _, _, at = element(body, at)  # dimensions
            _, name, _ = element(body, at)
            rows.append((name.decode("utf-8"), body))
    scan(data, 128)
    return rows


def compare(a, b, where, issues):
    if a.shape != b.shape or a.dtype.kind != b.dtype.kind:
        issues.append({"path": where, "reference": [list(a.shape), str(a.dtype)],
                       "qt": [list(b.shape), str(b.dtype)]})
        return
    if a.dtype.kind == "O":
        for index in np.ndindex(a.shape):
            compare(a[index], b[index], where + str(index), issues)
    elif a.dtype.kind == "f":
        # Include signed zero and NaN payload bits, not only tolerant numbers.
        if a.dtype.itemsize != 8 or b.dtype.itemsize != 8:
            issues.append({"path": where, "error": "Expected double matrix"})
        elif a.astype("<f8").tobytes(order="F") != b.astype("<f8").tobytes(order="F"):
            aa, bb = a.astype("<f8").view("<u8"), b.astype("<f8").view("<u8")
            mismatch = np.argwhere(aa != bb)
            issues.append({"path": where, "different_values": len(mismatch),
                           "first": [{"index": ij.tolist(), "reference": repr(float(a[tuple(ij)])),
                                      "qt": repr(float(b[tuple(ij)]))} for ij in mismatch[:3]]})
    elif a.dtype.kind in "US":
        if not np.array_equal(a, b):
            issues.append({"path": where, "reference_text": a.tolist(), "qt_text": b.tolist()})
    elif not np.array_equal(a, b):
        issues.append({"path": where, "error": "Different values"})


def strings(cells):
    return sorted("".join(value.flatten(order="F").tolist())
                  for value in cells.flatten(order="F"))


def empty_blob_recovery(path):
    """Independent strict BIN framing: only prove all-NUL Z recovery sites.

    MP10's byte[].Aggregate throws on empty Z; ReadMessage scans to the next
    record, but DFLogBuffer's next indexed read seeks back and repeats it.
    Do not reinterpret arbitrary shape mismatches as this source defect.
    """
    raw = Path(path).read_bytes()
    widths = dict(zip("bBhHiIqQgfdcCeELnNMZa", (1, 1, 2, 2, 4, 4, 8, 8,
        2, 4, 8, 2, 2, 4, 4, 4, 4, 16, 1, 64, 64)))
    definitions = {}
    records = []
    at = 0
    while at < len(raw):
        if raw[at:at + 2] != b"\xa3\x95" or at + 3 > len(raw):
            raise ValueError("Recovery proof requires fully framed BIN")
        kind = raw[at + 2]
        if kind == 128:
            if at + 89 > len(raw):
                raise ValueError("Incomplete FMT in recovery proof")
            ident, length, name, layout, _ = struct.unpack_from("<BB4s16s64s", raw, at + 3)
            definitions[ident] = (length, name.rstrip(b"\0").decode("ascii"),
                                   layout.rstrip(b"\0").decode("ascii"))
            records.append(("FMT", False))
            at += 89
            continue
        length, name, layout = definitions[kind]
        if at + length > len(raw):
            raise ValueError("Incomplete frame in recovery proof")
        cursor, empty = at + 3, False
        for field in layout:
            width = widths[field]  # unknown/A layouts must not be guessed
            if field == "Z" and not raw[cursor:cursor + width].strip(b"\0"):
                empty = True
            cursor += width
        if cursor != at + length:
            raise ValueError("Unexpected FMT size in recovery proof")
        records.append((name, empty))
        at += length
    recovery, successor = {}, None
    for index in range(len(records) - 1, -1, -1):
        if not records[index][1]:
            successor = (index + 1, records[index][0])
        elif successor is not None:
            recovery[index + 1] = successor
    return recovery


def remove_proven_recovery_duplicates(reference, recovery):
    removed = {}
    for name, matrix in list(reference.items()):
        if name.startswith("__") or matrix.dtype.kind != "f" or matrix.ndim != 2:
            continue
        if matrix.shape[1] < 1 or not np.all(np.isfinite(matrix[:, 0])) or not np.all(matrix[:, 0] == np.floor(matrix[:, 0])):
            raise ValueError("Invalid logical row ids cannot prove recovery")
        rows = {int(row[0]): index for index, row in enumerate(matrix)}
        if len(rows) != matrix.shape[0]:
            raise ValueError("Duplicate logical row ids cannot prove recovery")
        keep = np.ones(matrix.shape[0], dtype=bool)
        for line, (next_line, next_type) in recovery.items():
            if line not in rows:
                continue
            if name != next_type and not name.startswith(next_type + "_"):
                raise ValueError("Recovery successor type mismatch")
            if next_line not in rows or matrix[rows[line], 1:].tobytes() != matrix[rows[next_line], 1:].tobytes():
                raise ValueError("Recovery row is not a bit-exact successor duplicate")
            keep[rows[line]] = False
        count = int(np.count_nonzero(~keep))
        if not count:
            continue
        custom = name + "1"
        if name in ("MSG", "ISBD") and custom in reference:
            cells = reference[custom]
            if cells.shape != (1, len(keep)):
                raise ValueError("Recovery custom-cell alignment mismatch")
            for index in np.flatnonzero(~keep):
                next_line, _ = recovery[int(matrix[index, 0])]
                issues = []
                compare(cells[0, index], cells[0, rows[next_line]], custom, issues)
                if issues:
                    raise ValueError("Recovery custom cell is not a successor duplicate")
            reference[custom] = cells[:, keep]
            removed[custom] = count
        reference[name] = matrix[keep, :]
        removed[name] = count
    return removed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference")
    parser.add_argument("qt")
    parser.add_argument("--ignore-reference-variable", action="append", default=[])
    parser.add_argument("--allow-identical-duplicate-labels", action="store_true")
    parser.add_argument("--reference-empty-blob-source", help="BIN to prove MP10 recovery duplicates; never a blanket row filter")
    args = parser.parse_args()
    reader_options = dict(chars_as_strings=False, uint16_codec="utf-16le",
                          verify_compressed_data_integrity=True)
    reference, qt = [loadmat(path, **reader_options) for path in (args.reference, args.qt)]
    recovered = (remove_proven_recovery_duplicates(reference,
        empty_blob_recovery(args.reference_empty_blob_source))
        if args.reference_empty_blob_source else {})
    ref_rows, qt_rows = top_level(args.reference), top_level(args.qt)
    issues = []
    ignored = set(args.ignore_reference_variable)
    counts = []
    for label, rows in (("reference", ref_rows), ("qt", qt_rows)):
        grouped = collections.defaultdict(list)
        for name, body in rows:
            grouped[name].append(body)
        counts.append(collections.Counter(name for name, _ in rows))
        for name, bodies in grouped.items():
            if len(bodies) < 2:
                continue
            allowed = label == "reference" and (name in ignored or
                args.allow_identical_duplicate_labels and name.endswith("_label")
                and all(body == bodies[0] for body in bodies))
            if not allowed:
                issues.append({"file": label, "duplicate_name": name, "count": len(bodies)})
    ref_names = {name for name in reference if not name.startswith("__")}
    qt_names = {name for name in qt if not name.startswith("__")}
    for name in sorted(ignored):
        if name not in ref_names or name in qt_names:
            issues.append({"invalid_reference_only_exception": name})
    for name in sorted((ref_names - ignored) ^ qt_names):
        issues.append({"variable_set_difference": name, "reference": name in ref_names})
    ref_order = list(dict.fromkeys(name for name, _ in ref_rows if name not in ignored))
    qt_order = [name for name, _ in qt_rows]
    if ref_order != qt_order:
        issues.append({"top_level_order": {"reference": ref_order, "qt": qt_order}})
    numeric = 0
    cells = 0
    for name in sorted((ref_names - ignored) & qt_names):
        a, b = reference[name], qt[name]
        if name == "Seen":
            if a.shape != b.shape or a.dtype.kind != "O" or b.dtype.kind != "O" or strings(a) != strings(b):
                issues.append({"Seen_set_or_shape_mismatch": True})
        else:
            compare(a, b, name, issues)
        numeric += a.dtype.kind == "f"
        cells += a.dtype.kind == "O"
    print(json.dumps({"reference_variables": len(ref_names), "qt_variables": len(qt_names),
                      "numeric_matrices": numeric, "cell_matrices": cells,
                      "Seen_order_normalized": True, "reference_only_exceptions": sorted(ignored),
                      "proven_reference_recovery_duplicates_removed": recovered,
                      "issues": issues}, ensure_ascii=False, indent=2))
    return bool(issues)


if __name__ == "__main__":
    raise SystemExit(main())
