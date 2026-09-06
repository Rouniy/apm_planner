"""Test narrow oracle exceptions; run with the isolated SciPy interpreter."""
import tempfile
import unittest
from pathlib import Path

import numpy as np

from CompareDataFlashMatlab import empty_blob_recovery, remove_proven_recovery_duplicates
from GenerateDataFlashMatlabFixtures import fmt, packet


class RecoveryProofTest(unittest.TestCase):
    def test_raw_framing_and_consecutive_empty_blobs(self):
        raw = fmt(150, 67, "FILE", "Z", "Data") + fmt(151, 11, "VAL", "Q", "TimeUS")
        raw += packet(150, "64s", b"") * 2 + packet(151, "Q", 100)
        raw += packet(150, "64s", b"")  # no successor: nothing to normalize
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "fixture.bin"
            source.write_bytes(raw)
            self.assertEqual(empty_blob_recovery(source), {3: (5, "VAL"), 4: (5, "VAL")})
            source.write_bytes(raw[:-1])
            with self.assertRaises(ValueError):
                empty_blob_recovery(source)

    def test_removes_only_proven_rows_and_preserves_bits(self):
        values = {"VAL": np.array([[3, -0.0], [4, -0.0], [5, -0.0], [7, 2.0]])}
        self.assertEqual(remove_proven_recovery_duplicates(values,
            {3: (5, "VAL"), 4: (5, "VAL")}), {"VAL": 2})
        self.assertEqual(values["VAL"].shape, (2, 2))
        self.assertEqual(values["VAL"][0, 1:].tobytes(), np.array([-0.0]).tobytes())

    def test_changed_payload_or_type_is_not_hidden(self):
        for values, proof in (([[3, 0.0], [4, -0.0]], {3: (4, "VAL")}),
                              ([[3, 1.0], [4, 1.0]], {3: (4, "GPS")}),
                              ([[3, 1.0]], {3: (4, "VAL")})):
            with self.assertRaises(ValueError):
                remove_proven_recovery_duplicates({"VAL": np.array(values)}, proof)

    def test_custom_cells_must_also_be_duplicates(self):
        for same in (True, False):
            cells = np.empty((1, 2), dtype=object)
            cells[0, 0] = np.array([list("hello")])
            cells[0, 1] = np.array([list("hello" if same else "world")])
            values = {"MSG": np.array([[3., 0.], [4., 0.]]), "MSG1": cells}
            if same:
                self.assertEqual(remove_proven_recovery_duplicates(values,
                    {3: (4, "MSG")}), {"MSG": 1, "MSG1": 1})
                self.assertEqual(values["MSG1"].shape, (1, 1))
            else:
                with self.assertRaises(ValueError):
                    remove_proven_recovery_duplicates(values, {3: (4, "MSG")})


if __name__ == "__main__":
    unittest.main()
