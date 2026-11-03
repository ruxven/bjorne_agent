#!/usr/bin/env python3
"""
Convert Lizard CSV output to JSON format for pain point analysis.
"""

import csv
import json
import sys
from pathlib import Path


def convert_csv_to_json(csv_file, json_file):
    """Convert Lizard CSV output to JSON format."""
    result = []

    with open(csv_file, "r") as f:
        reader = csv.DictReader(f)

        # Group functions by file
        files = {}
        for row in reader:
            filename = row.get("file", "")
            if filename not in files:
                files[filename] = {"filename": filename, "function_list": []}

            # Extract function information
            function_info = {
                "name": row.get("function", ""),
                "cyclomatic_complexity": int(row.get("CCN", 0)),
                "start_line": int(row.get("start", 0)),
                "end_line": int(row.get("end", 0)),
                "nloc": int(row.get("NLOC", 0)),
                "token_count": int(row.get("token", 0)),
                "parameter_count": int(row.get("PARAM", 0)),
            }

            files[filename]["function_list"].append(function_info)

        # Convert to list format
        result = list(files.values())

    # Write JSON output
    with open(json_file, "w") as f:
        json.dump(result, f, indent=2)

    print(f"Converted {csv_file} to {json_file}")
    print(f"Processed {len(result)} files")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python csv_to_json_complexity.py <input.csv> <output.json>")
        sys.exit(1)

    csv_file = sys.argv[1]
    json_file = sys.argv[2]

    if not Path(csv_file).exists():
        print(f"Error: CSV file '{csv_file}' not found")
        sys.exit(1)

    convert_csv_to_json(csv_file, json_file)
