#!/usr/bin/env python3
"""Per-function line coverage from a gcovr JSON report.

gcovr tags every source line with the function that owns it, so grouping those
lines by function gives exact per-function line coverage without the STL
template noise that `gcov -f` emits.

Usage: function_coverage.py <gcovr.json> [--max-name N] [--low PERCENT]
"""

import argparse
import json
import sys

STD_STRING = ("std::__cxx11::basic_string<char, std::char_traits<char>, "
              "std::allocator<char> >")


def shorten(name, limit):
    name = name.replace(STD_STRING, "std::string")
    name = name.replace("std::allocator<", "alloc<")
    if len(name) <= limit:
        return name
    return name[: limit - 3] + "..."


def project_functions(entry):
    """True for functions defined in bjorne_agent sources, not STL instantiations."""
    demangled = entry.get("demangled_name") or entry.get("name") or ""
    head = demangled.split("(", 1)[0]
    return head.startswith("bjorne_agent::")


def collect(report):
    rows = []
    for file_entry in report["files"]:
        path = file_entry["file"]
        names = {}
        for function in file_entry.get("functions", []):
            names[function["name"]] = function
        per_function = {}
        for line in file_entry.get("lines", []):
            mangled = line.get("function_name")
            if not mangled:
                continue
            stats = per_function.setdefault(mangled, [0, 0])
            stats[1] += 1
            if line.get("count", 0) > 0:
                stats[0] += 1
        for mangled, (covered, total) in per_function.items():
            entry = names.get(mangled, {})
            if not project_functions(entry):
                continue
            rows.append({
                "file": path,
                "name": entry.get("demangled_name", mangled),
                "line": entry.get("lineno", 0),
                "covered": covered,
                "total": total,
            })
    rows.sort(key=lambda row: (row["file"], row["line"], row["name"]))
    return rows


def percent(covered, total):
    if total == 0:
        return 100.0
    return 100.0 * covered / total


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("report", help="gcovr JSON report path")
    parser.add_argument("--max-name", type=int, default=96, help="function name width")
    parser.add_argument("--low", type=float, default=80.0,
                        help="report functions below this line coverage")
    args = parser.parse_args()

    with open(args.report, encoding="utf-8") as handle:
        report = json.load(handle)

    rows = collect(report)
    if not rows:
        print("No bjorne_agent functions found in the report", file=sys.stderr)
        return 1

    current_file = None
    print(f"{'Function':<{args.max_name}} {'Line':>5} {'Lines':>6} {'Exec':>5} {'Cover':>6}")
    print("-" * (args.max_name + 28))
    for row in rows:
        if row["file"] != current_file:
            current_file = row["file"]
            print(f"\n{current_file}")
        name = shorten(row["name"], args.max_name)
        print(f"  {name:<{args.max_name - 2}} {row['line']:>5} {row['total']:>6} "
              f"{row['covered']:>5} {percent(row['covered'], row['total']):>5.0f}%")

    total_lines = sum(row["total"] for row in rows)
    covered_lines = sum(row["covered"] for row in rows)
    uncovered = [row for row in rows if row["covered"] == 0]
    low = [row for row in rows if percent(row["covered"], row["total"]) < args.low]

    print()
    print(f"Functions: {len(rows)}  Lines: {covered_lines}/{total_lines} "
          f"({percent(covered_lines, total_lines):.0f}%)  "
          f"Uncovered: {len(uncovered)}  Below {args.low:.0f}%: {len(low)}")
    for row in low:
        name = shorten(row["name"], args.max_name)
        print(f"  {percent(row['covered'], row['total']):>5.0f}%  {name} "
              f"({row['file']}:{row['line']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
