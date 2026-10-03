#!/usr/bin/env python3
"""End-to-end AI-resistance benchmark.

For each sample program: compiles plain + heimdall-cff-flattened binaries,
decompiles both with Ghidra, asks an LLM to reconstruct each from pseudocode
alone, scores the reconstructions (behavioral + descriptive), and writes a
comparison report to benchmark/results/.

Usage:
  python benchmark/harness.py \
      --clang clang --plugin build/lib/HeimdallCFF.so \
      --ghidra-dir "$GHIDRA_INSTALL_DIR" \
      --programs-dir test/correctness/programs \
      --out-dir benchmark/results

Requires ANTHROPIC_API_KEY in the environment.
"""
import argparse
import json
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))

import anthropic
from ghidra_runner import decompile_binary
from reconstruct import judge_explanation, reconstruct_program
from score import behavioral_score

# One-line ground truth for the "descriptive" score, and the CLI test cases
# used for the "behavioral" score. Kept in sync by hand with
# test/correctness/run_correctness.py's TEST_CASES -- see that file for the
# authoritative correctness-test inputs; this is a representative subset
# plus a human description an LLM judge checks explanations against.
PROGRAMS = {
    "license_check": {
        "ground_truth": (
            "Validates a license key of the form XXXX-XXXX-XXCC (14 chars): "
            "checks length and dash positions, sums the digit characters in "
            "the first two groups mod 97, and requires the last two digits "
            "(CC) to equal that checksum."
        ),
        "test_cases": [
            ["1234-5678-0036"],
            ["1234-5678-0000"],
            ["not-a-key"],
        ],
    },
    "simple_parser": {
        "ground_truth": (
            "Parses a string of semicolon-separated key=value pairs into a "
            "list, rejecting malformed entries (missing '=', empty/too-long "
            "key or value, too many pairs), and prints each pair plus a "
            "count, or PARSE_ERROR."
        ),
        "test_cases": [
            ["a=1;b=2;c=3"],
            ["malformed"],
            [""],
        ],
    },
    "tiny_crypto": {
        "ground_truth": (
            "A 6-round Feistel block cipher operating on a 32-bit block "
            "with a branchy non-linear round function (rotate/complement "
            "chosen by low bits, plus a fixed additive constant); encrypts "
            "then decrypts the input and reports whether the roundtrip "
            "recovers the original plaintext."
        ),
        "test_cases": [
            ["00000000"],
            ["deadbeef"],
            ["12345678"],
        ],
    },
}


def compile_variant(clang, plugin, src, out, flatten):
    cmd = [clang, "-O1", str(src), "-o", str(out)]
    if flatten:
        cmd += [f"-fpass-plugin={plugin}", "-mllvm", "-passes=heimdall-cff"]
    subprocess.run(cmd, check=True)


def run_one(name, cfg, clang, plugin, ghidra_dir, programs_dir, work_dir, cc, model):
    src = programs_dir / f"{name}.c"
    plain_bin = work_dir / f"{name}.plain"
    flat_bin = work_dir / f"{name}.flat"
    compile_variant(clang, plugin, src, plain_bin, flatten=False)
    compile_variant(clang, plugin, src, flat_bin, flatten=True)

    client = anthropic.Anthropic()
    report = {"program": name, "ground_truth": cfg["ground_truth"], "variants": {}}

    for variant, binary in (("plain", plain_bin), ("flattened", flat_bin)):
        print(f"  [{name}/{variant}] decompiling with Ghidra...")
        decompiled = decompile_binary(binary, ghidra_dir)

        print(f"  [{name}/{variant}] asking {model} to reconstruct...")
        explanation, code = reconstruct_program(client, model, decompiled)

        behavioral = None
        if code:
            print(f"  [{name}/{variant}] scoring behavioral equivalence...")
            behavioral = behavioral_score(
                binary, code, cfg["test_cases"], work_dir, cc=cc
            )

        print(f"  [{name}/{variant}] judging explanation quality...")
        judged = judge_explanation(client, model, cfg["ground_truth"], explanation)

        report["variants"][variant] = {
            "decompiled_function_count": len(decompiled),
            "explanation": explanation,
            "reconstructed_code": code,
            "behavioral": behavioral,
            "descriptive": judged,
        }

    return report


def write_markdown_report(reports, out_path):
    lines = ["# Heimdall AI-resistance benchmark results", ""]
    lines.append(
        "Plain vs. `heimdall-cff`-flattened binaries, decompiled with Ghidra "
        "and reconstructed by an LLM from pseudocode alone. See "
        "[DESIGN.md](../../DESIGN.md#benchmark-harness-ai-resistance) for "
        "methodology."
    )
    lines.append("")
    lines.append(
        "| Program | Variant | Behavioral accuracy | Descriptive score (0-5) |"
    )
    lines.append("|---|---|---|---|")
    for r in reports:
        for variant, v in r["variants"].items():
            acc = (
                f"{v['behavioral']['accuracy']*100:.0f}% "
                f"({v['behavioral']['matching_cases']}/{v['behavioral']['total_cases']})"
                if v["behavioral"]
                else "n/a (did not compile)"
            )
            lines.append(
                f"| {r['program']} | {variant} | {acc} | {v['descriptive'].get('score', 'n/a')} |"
            )
    lines.append("")

    for r in reports:
        lines.append(f"## {r['program']}")
        lines.append("")
        lines.append(f"**Ground truth:** {r['ground_truth']}")
        lines.append("")
        for variant, v in r["variants"].items():
            lines.append(f"### {variant}")
            lines.append("")
            lines.append(f"**LLM explanation:** {v['explanation']}")
            lines.append("")
            lines.append(
                f"**Descriptive judge:** {v['descriptive'].get('score', 'n/a')}/5 "
                f"-- {v['descriptive'].get('justification', '')}"
            )
            lines.append("")
            if v["behavioral"]:
                lines.append(
                    f"**Behavioral accuracy:** "
                    f"{v['behavioral']['matching_cases']}/{v['behavioral']['total_cases']} "
                    "test cases matched ground-truth output."
                )
            else:
                lines.append("**Behavioral accuracy:** reconstruction did not compile.")
            lines.append("")

    out_path.write_text("\n".join(lines), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--plugin", required=True)
    ap.add_argument("--ghidra-dir", required=True, type=pathlib.Path)
    ap.add_argument(
        "--programs-dir",
        default=pathlib.Path("test/correctness/programs"),
        type=pathlib.Path,
    )
    ap.add_argument(
        "--out-dir", default=pathlib.Path("benchmark/results"), type=pathlib.Path
    )
    ap.add_argument("--work-dir", default=pathlib.Path("/tmp/heimdall-benchmark"), type=pathlib.Path)
    ap.add_argument("--cc", default="cc", help="Compiler used to build LLM reconstructions")
    ap.add_argument("--model", default="claude-sonnet-5-5")
    args = ap.parse_args()

    args.work_dir.mkdir(parents=True, exist_ok=True)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    reports = []
    for name, cfg in PROGRAMS.items():
        print(f"== {name} ==")
        reports.append(
            run_one(
                name,
                cfg,
                args.clang,
                args.plugin,
                args.ghidra_dir,
                args.programs_dir,
                args.work_dir,
                args.cc,
                args.model,
            )
        )

    (args.out_dir / "results.json").write_text(json.dumps(reports, indent=2))
    write_markdown_report(reports, args.out_dir / "RESULTS.md")
    print(f"\nWrote {args.out_dir / 'results.json'} and {args.out_dir / 'RESULTS.md'}")


if __name__ == "__main__":
    main()
