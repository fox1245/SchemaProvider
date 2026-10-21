#!/usr/bin/env python3
"""Every committed tables.md under results/ is exactly what render.py produces from the records beside it."""
import subprocess
import sys
import unittest
from pathlib import Path

HARNESS = Path(__file__).resolve().parents[1]


class ResultsTests(unittest.TestCase):
    def test_tables_are_generated_from_the_records(self):
        found = sorted((HARNESS / "results").glob("*/tables.md"))
        for tables in found:
            produced = subprocess.run([sys.executable, str(HARNESS / "render.py"), str(tables.parent)], capture_output=True, text=True, check=True).stdout
            self.assertEqual(produced, tables.read_text(), f"{tables.parent.name}: tables.md differs from the render of its records")

    def test_every_cell_record_names_its_provenance_and_samples(self):
        import json

        for cells in sorted((HARNESS / "results").glob("*/cells.jsonl")):
            for line in cells.read_text().splitlines():
                r = json.loads(line)
                if r["status"] == "n/a":
                    continue
                self.assertIn("provenance", r)
                if r["status"] in ("ok", "invalid"):
                    self.assertTrue((cells.parent / r["samples_path"]).exists(), r["cell"])


if __name__ == "__main__":
    unittest.main()
