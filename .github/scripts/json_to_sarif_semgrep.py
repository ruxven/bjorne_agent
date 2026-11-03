#!/usr/bin/env python3
"""
Convert Semgrep JSON output to SARIF format for pain point analysis.
"""

import json
import sys
from pathlib import Path


def convert_json_to_sarif(json_file, sarif_file):
    """Convert Semgrep JSON output to SARIF format."""

    with open(json_file, "r") as f:
        semgrep_data = json.load(f)

    # Create SARIF structure
    sarif = {
        "$schema": "https://raw.githubusercontent.com/oasis-tcs/sarif-spec/master/Schemata/sarif-schema-2.1.0.json",
        "version": "2.1.0",
        "runs": [
            {
                "tool": {
                    "driver": {
                        "name": "Semgrep",
                        "version": semgrep_data.get("version", "unknown"),
                        "rules": [],
                    }
                },
                "results": [],
            }
        ],
    }

    # Track unique rules
    rules_map = {}

    # Process results
    if "results" in semgrep_data:
        for result in semgrep_data["results"]:
            # Extract rule information
            rule_id = result.get("check_id", "unknown")

            # Add rule to rules list if not already present
            if rule_id not in rules_map:
                rule_info = {
                    "id": rule_id,
                    "name": rule_id,
                    "shortDescription": {
                        "text": result.get("extra", {}).get("message", rule_id)
                    },
                    "fullDescription": {
                        "text": result.get("extra", {})
                        .get("metadata", {})
                        .get("message", rule_id)
                    },
                    "defaultConfiguration": {
                        "level": result.get("extra", {})
                        .get("severity", "warning")
                        .lower()
                    },
                }
                sarif["runs"][0]["tool"]["driver"]["rules"].append(rule_info)
                rules_map[rule_id] = (
                    len(sarif["runs"][0]["tool"]["driver"]["rules"]) - 1
                )

            # Create result entry
            sarif_result = {
                "ruleId": rule_id,
                "ruleIndex": rules_map[rule_id],
                "level": result.get("extra", {}).get("severity", "warning").lower(),
                "message": {
                    "text": result.get("extra", {}).get("message", "Issue found")
                },
                "locations": [
                    {
                        "physicalLocation": {
                            "artifactLocation": {"uri": result.get("path", "")},
                            "region": {
                                "startLine": result.get("start", {}).get("line", 1),
                                "startColumn": result.get("start", {}).get("col", 1),
                                "endLine": result.get("end", {}).get("line", 1),
                                "endColumn": result.get("end", {}).get("col", 1),
                            },
                        }
                    }
                ],
            }

            sarif["runs"][0]["results"].append(sarif_result)

    # Write SARIF output
    with open(sarif_file, "w") as f:
        json.dump(sarif, f, indent=2)

    print(f"Converted {json_file} to {sarif_file}")
    print(f"Processed {len(sarif['runs'][0]['results'])} findings")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print("Usage: python json_to_sarif_semgrep.py <input.json> <output.sarif>")
        sys.exit(1)

    json_file = sys.argv[1]
    sarif_file = sys.argv[2]

    if not Path(json_file).exists():
        print(f"Error: JSON file '{json_file}' not found")
        sys.exit(1)

    convert_json_to_sarif(json_file, sarif_file)
