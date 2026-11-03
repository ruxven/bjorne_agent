import sys
import json

"""
[
    {
        filename:"aaa.cpp"
        functions:[
            {
                function_name:"aaa:bbb()",
                hit_count:67
                line_start:123
                line_end:133
            }
        ]
    }
]
"""


def find_func_name(line_number, func_dict):
    result = None
    result_line_start = None
    for func_name in func_dict.keys():
        f_d = func_dict[func_name]
        line_start = f_d["line_start"]
        if result:
            if line_start < result_line_start:
                continue
        if line_start <= line_number:
            result = func_name
            result_line_start = line_start

    if not result:
        print(f"cannot find function for line number {line_number}")
        return result
        for func_name in func_dict.keys():
            f_d = func_dict[func_name]
            line_start = f_d["line_start"]
            print(f"  {func_name}:{line_start}")
    # else:
    # print(f"found {result}:{result_line_start} for {line_number}")

    return result


def main():
    if len(sys.argv) != 3:
        print("Usage: python generate_csv.py <path/to/lcov/file> <path/to/json/file>")
        sys.exit(1)

    lcov_file_path = sys.argv[1]
    json_file_path = sys.argv[2]

    current_file = None
    file_dict = {}
    func_dict = {}
    with open(lcov_file_path, "r") as f:
        for line in f:
            # end file
            if line.startswith("end_of_record"):
                file_dict[current_file] = func_dict
                current_file = None
                func_dict = None
                continue

            # source file
            if line.startswith("SF:"):
                current_file = line.strip().split(":", 1)[1]
                func_dict = {}
                func_line_dict = {}
            if not current_file:
                continue

            # TN: test name
            # SF: source file path
            # FN: line number,function name
            # FNF:  number functions found
            # FNH: number hit
            # BRDA: branch data: line, block, (expressions,count)+
            # BRF: branches found
            # DA: line number, hit count
            # LF: lines found
            # LH:  lines hit.

            if line.startswith("FN:"):
                # FN:line_number,function_name
                parts = line.strip().split(":", 1)[1].split(",")
                line_start, func_name = int(parts[0]), str(parts[1])
                line_end = line_start
                line_count = 0
                line_hit = 0
                branch_count = 0
                branch_hit = 0
                func_dict[func_name] = {
                    "line_start": line_start,
                    "line_end": line_end,
                    "line_count": line_count,
                    "line_hit": line_hit,
                    "branch_count": branch_count,
                    "branch_hit": branch_hit,
                }
            elif line.startswith("DA:"):
                if not func_dict:
                    continue
                # DA:line_number,hit_count
                parts = line.strip().split(":", 1)[1].split(",")
                line_number, hit_count = int(parts[0]), int(parts[1])

                func_name = find_func_name(line_number, func_dict)
                if not func_name:
                    print(f"current_file: {current_file}, line: {line}")
                    continue
                f_d = func_dict[func_name]
                line_end = f_d["line_end"]
                if line_number > line_end:
                    f_d["line_end"] = line_number
                f_d["line_count"] += 1
                if hit_count > 0:
                    f_d["line_hit"] += 1
                func_dict[func_name] = f_d
            elif line.startswith("BRDA:"):
                parts = line.strip().split(":", 1)[1].split(",")
                line_number, branch_pair_id, branch_id, hit_count = (
                    int(parts[0]),
                    int(parts[1]),
                    int(parts[2]),
                    parts[3],
                )
                try:
                    hit_count = int(hit_count)
                except Exception as e:
                    # ignore the line and move on
                    pass
                    continue
                func_name = find_func_name(line_number, func_dict)
                if not func_name:
                    print(f"current_file: {current_file}, line: {line}")
                    continue
                f_d = func_dict[func_name]
                f_d["branch_count"] += 1
                if hit_count > 0:
                    f_d["branch_hit"] += 1
                func_dict[func_name] = f_d

            # end if
        # end for line
    # end with open

    output_a = []
    for filename in file_dict.keys():
        print(f"filename: {filename}")
        output_func_a = []
        func_dict = file_dict[filename]
        for func_name in func_dict.keys():
            f_d = func_dict[func_name]
            f_d["function_name"] = func_name
            output_func_a.append(f_d)
        file_d = {}
        file_d["filename"] = filename
        file_d["function_coverage"] = output_func_a
        output_a.append(file_d)

    with open(json_file_path, "w") as f:
        json.dump(output_a, f, indent=2)
        print(f"Wrote out to {json_file_path}")


# end main

if __name__ == "__main__":
    main()
