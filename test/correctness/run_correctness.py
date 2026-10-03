#!/usr/bin/env python3
"""Correctness harness: compiles each sample program plain and flattened,
runs both against a fixed set of inputs, and diffs stdout/exit-code output
byte-for-byte.

Usage:
  run_correctness.py --clang /path/to/clang \
                      --plugin /path/to/libHeimdallCFF.so \
                      --programs-dir test/correctness/programs \
                      --work-dir /tmp/heimdall-correctness

Exits non-zero (and prints a diff) on the first mismatch.
"""
import argparse
import pathlib
import subprocess
import sys

# argv (excluding program name) to run against each sample, chosen to
# exercise both "valid"/accept and "invalid"/reject code paths.
TEST_CASES = {
    "license_check": [
        ["1234-5678-0036"],  # valid per DESIGN.md example
        ["1234-5678-0000"],  # invalid checksum
        ["not-a-key"],  # wrong length / format
        ["1234x5678-0036"],  # bad separator
    ],
    "simple_parser": [
        ["a=1;b=2;c=3"],
        ["a=1;b=2;c=3;"],  # trailing separator
        ["malformed"],  # no '='
        [""],  # empty input
        ["k=" + "x" * 40],  # value too long -> parse error
    ],
    "tiny_crypto": [
        ["00000000"],
        ["deadbeef"],
        ["ffffffff"],
        ["12345678"],
    ],
    # Regression test for a bug found in code review: see
    # entry_value_stress.c for why entry-block-defined, cross-block-used
    # values need special care during demotion.
    "entry_value_stress": [
        ["10", "20", "5"],
        ["0", "0", "0"],
        ["-30", "40", "7"],
        ["200", "1", "1"],
    ],
    # Nested loops + an inner switch -- stresses the dispatcher with many
    # blocks and several independent back-edges feeding it.
    "nested_state_machine": [
        ["5", "5", "0"],
        ["20", "3", "7"],
        ["1", "1", "3"],
        ["0", "10", "2"],
    ],
    # Irreducible CFG (see irreducible_dispatch.c) -- the pass is expected
    # to SKIP this function entirely, so plain == flattened output is the
    # whole point of this case: it confirms the bail-out path is safe, not
    # that flattening happened.
    "irreducible_dispatch": [
        ["50"],
        ["0"],
        ["-500"],
        ["99"],
    ],
}


def compile_variant(clang, plugin, src, out, flatten):
    cmd = [clang, "-O1", str(src), "-o", str(out)]
    if flatten:
        cmd += [
            f"-fpass-plugin={plugin}",
            "-mllvm",
            "-passes=heimdall-cff",
        ]
    subprocess.run(cmd, check=True)


def run(binary, args):
    proc = subprocess.run(
        [str(binary)] + args, capture_output=True, text=True, timeout=10
    )
    return proc.returncode, proc.stdout, proc.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clang", required=True)
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--programs-dir", required=True, type=pathlib.Path)
    ap.add_argument("--work-dir", required=True, type=pathlib.Path)
    args = ap.parse_args()

    args.work_dir.mkdir(parents=True, exist_ok=True)

    failures = []
    for name, cases in TEST_CASES.items():
        src = args.programs_dir / f"{name}.c"
        plain_bin = args.work_dir / f"{name}.plain"
        flat_bin = args.work_dir / f"{name}.flat"

        print(f"== {name} ==")
        compile_variant(args.clang, args.plugin, src, plain_bin, flatten=False)
        compile_variant(args.clang, args.plugin, src, flat_bin, flatten=True)

        for case_args in cases:
            plain_result = run(plain_bin, case_args)
            flat_result = run(flat_bin, case_args)

            if plain_result != flat_result:
                failures.append((name, case_args, plain_result, flat_result))
                print(f"  MISMATCH for args={case_args!r}")
                print(f"    plain:     rc={plain_result[0]} stdout={plain_result[1]!r}")
                print(f"    flattened: rc={flat_result[0]} stdout={flat_result[1]!r}")
            else:
                print(f"  OK args={case_args!r} rc={plain_result[0]}")

    if failures:
        print(f"\n{len(failures)} correctness mismatch(es) found.", file=sys.stderr)
        return 1

    print("\nAll correctness checks passed: flattened output == plain output.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
