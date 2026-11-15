#!/usr/bin/env python3
"""Line and function coverage summary from a gcovr JSON report.

The report counts the project functions only, so the STL template
instantiations that gcov emits do not dilute the figure. A function counts as
covered when the report gives it an execution count above zero.

Usage: coverage_summary.py <gcovr.json> [--uncovered]
"""

import argparse
import json
import sys


def project_function(entry):
    """True for functions defined in bjorne_agent sources, not STL instantiations."""
    demangled = entry.get("demangled_name") or entry.get("name") or ""
    head = demangled.split("(", 1)[0]
    return head.startswith("bjorne_agent::")


def collect(report):
    """Returns the covered and total line and function counts, and the uncovered functions."""
    lines_covered = lines_total = 0
    functions_covered = functions_total = 0
    uncovered = []

    for file_entry in report["files"]:
        for line in file_entry.get("lines", []):
            lines_total += 1
            if line.get("count", 0) > 0:
                lines_covered += 1
        for function in file_entry.get("functions", []):
            if not project_function(function):
                continue
            functions_total += 1
            if function.get("execution_count", 0) > 0:
                functions_covered += 1
            else:
                uncovered.append((file_entry["file"], function.get("lineno", 0),
                                  function.get("demangled_name", function.get("name", ""))))

    return (lines_covered, lines_total, functions_covered, functions_total, uncovered)


def percent(covered, total):
    if total == 0:
        return 100.0
    return 100.0 * covered / total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", help="gcovr JSON report path")
    parser.add_argument("--uncovered", action="store_true",
                        help="list the functions that no test reached")
    args = parser.parse_args()

    with open(args.report, encoding="utf-8") as handle:
        report = json.load(handle)

    lines_covered, lines_total, functions_covered, functions_total, uncovered = collect(report)
    if lines_total == 0:
        print("No lines in the report", file=sys.stderr)
        return 1

    print(f"Lines: {lines_covered}/{lines_total} ({percent(lines_covered, lines_total):.1f}%)")
    print(f"Functions: {functions_covered}/{functions_total} "
          f"({percent(functions_covered, functions_total):.1f}%)")

    if args.uncovered:
        for path, line, name in uncovered:
            head = name.split("(", 1)[0]
            print(f"  uncovered: {path}:{line} {head}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
