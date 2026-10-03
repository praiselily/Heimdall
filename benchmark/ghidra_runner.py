"""Thin wrapper around Ghidra's `analyzeHeadless` that decompiles a single
binary to {function_name: pseudocode} JSON using
ghidra_scripts/DecompileFunctions.py.

Requires the GHIDRA_INSTALL_DIR environment variable (or --ghidra-dir) to
point at a Ghidra install, i.e. the directory containing
support/analyzeHeadless.
"""
import json
import os
import pathlib
import subprocess
import tempfile


def decompile_binary(binary_path: pathlib.Path, ghidra_dir: pathlib.Path) -> dict:
    analyze_headless = ghidra_dir / "support" / "analyzeHeadless"
    if os.name == "nt":
        analyze_headless = analyze_headless.with_suffix(".bat")
    if not analyze_headless.exists():
        raise FileNotFoundError(
            f"analyzeHeadless not found at {analyze_headless}; check "
            "GHIDRA_INSTALL_DIR / --ghidra-dir"
        )

    scripts_dir = pathlib.Path(__file__).parent / "ghidra_scripts"

    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = pathlib.Path(tmp)
        out_json = tmp_path / "decompiled.json"
        project_dir = tmp_path / "ghidra_project"
        project_dir.mkdir()

        cmd = [
            str(analyze_headless),
            str(project_dir),
            "HeimdallBenchmark",
            "-import",
            str(binary_path),
            "-postScript",
            "DecompileFunctions.py",
            str(out_json),
            "-scriptPath",
            str(scripts_dir),
            "-deleteProject",
        ]
        subprocess.run(cmd, check=True, capture_output=True, text=True)

        with open(out_json) as f:
            return json.load(f)
