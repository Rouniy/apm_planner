"""Independent scipy reader: compare actual MP10 and Qt MAT files by variables."""
import hashlib
import json
import sys
import numpy as np
from scipy.io import loadmat

left, right = (loadmat(path, verify_compressed_data_integrity=True) for path in sys.argv[1:3])
names_left = {name for name in left if not name.startswith("__")}
names_right = {name for name in right if not name.startswith("__")}
issues = []
for name in sorted(names_left - names_right):
    issues.append({"missing_in_qt": name})
for name in sorted(names_right - names_left):
    issues.append({"extra_in_qt": name})
summary = []
for name in sorted(names_left & names_right):
    a, b = left[name], right[name]
    exact = a.shape == b.shape and np.asarray(a, dtype="<f8").tobytes(order="F") == np.asarray(b, dtype="<f8").tobytes(order="F")
    if not exact:
        item = {"variable": name, "reference_shape": list(a.shape), "qt_shape": list(b.shape)}
        if a.shape == b.shape:
            indices = np.argwhere(~((a == b) | (np.isnan(a) & np.isnan(b))))
            item["first_mismatch"] = [{"index": list(map(int, ij)), "reference": float(a[tuple(ij)]), "qt": float(b[tuple(ij)])} for ij in indices[:3]]
        issues.append(item)
    summary.append({"name": name, "shape": list(b.shape), "dtype": str(b.dtype),
        "sha256": hashlib.sha256(np.asarray(b, dtype="<f8").tobytes(order="F")).hexdigest()})
print(json.dumps({"reference_variables": len(names_left), "qt_variables": len(names_right), "issues": issues, "variables": summary}, indent=2))
raise SystemExit(bool(issues))
