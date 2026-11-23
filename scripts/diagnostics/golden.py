"""Compare integer results from a homebrew reference and a native implementation."""
import argparse
import json
from pathlib import Path


def load(path):
    cases = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        row = json.loads(line)
        if not isinstance(row, dict) or not isinstance(row.get("case"), str) or type(row.get("result")) is not int:
            raise ValueError(f"{path}:{line_number}: expected a string case and an integer result")
        key = row["case"]
        if not key or key in cases:
            raise ValueError(f"{path}:{line_number}: empty or repeated case {key!r}")
        cases[key] = row["result"]
    if not cases:
        raise ValueError(f"{path}: empty input")
    return cases


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("reference", type=Path)
    p.add_argument("actual", type=Path)
    args = p.parse_args()
    try:
        expected, actual = load(args.reference), load(args.actual)
    except (ValueError, OSError) as error:
        p.error(str(error))
    failures = 0
    for key in sorted(expected.keys() | actual.keys()):
        if expected.get(key) != actual.get(key):
            print(f"{key}: expected {expected.get(key, 'missing')}, actual {actual.get(key, 'missing')}")
            failures += 1
    print(f"{len(expected.keys() | actual.keys())} cases, {failures} differences")
    return int(bool(failures))


if __name__ == "__main__":
    raise SystemExit(main())
