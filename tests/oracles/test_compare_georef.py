"""Comparator rejection tests; no production or reference files are modified."""
import copy
import json
import sys
import unittest
from pathlib import Path

from CompareGeoRef import compare


class ComparatorTests(unittest.TestCase):
    def test_receipts_and_numeric_corruption_are_rejected(self):
        root = Path(sys.argv[1])
        reference = json.loads((root / "net-cam2/oracle.json").read_text())
        qt = json.loads((root / "qt-cam.json").read_text())
        options = json.loads((root / "fixtures2/cam.json").read_text())
        mutations = [lambda j: j.update(cancelled=True),
                     lambda j: j.update(error="unreported failure"),
                     lambda j: j.update(publishedPaths=[]),
                     lambda j: j["matches"][0].update(latitude=0),
                     lambda j: j["matches"][0].update(outputPath="/wrong/photo.jpg")]
        for mutate in mutations:
            value = copy.deepcopy(qt)
            mutate(value)
            with self.subTest(mutation=mutate), self.assertRaises(AssertionError):
                compare(reference, value, options)
        bad_reference = copy.deepcopy(reference)
        bad_reference["inputs"]["logCopy"]["sha256"] = "0" * 64
        with self.assertRaises(AssertionError):
            compare(bad_reference, qt, options)

    def test_empty_reference_cannot_hide_publication(self):
        root = Path(sys.argv[1])
        reference = json.loads((root / "net-trig-count-mismatch/oracle.json").read_text())
        options = json.loads((root / "fixtures2/trig-count-mismatch.json").read_text())
        with self.assertRaises(AssertionError):
            compare(reference, {"success": False, "error": "failed",
                                "publishedPaths": ["unexpected.txt"]}, options)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
