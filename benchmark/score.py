"""Scores an LLM-reconstructed C program against the ground-truth binary it
was reconstructed from: compiles the reconstruction and compares its
behavior, input-by-input, against the original.
"""
import pathlib
import subprocess
import tempfile


def compile_reconstruction(code: str, out_path: pathlib.Path, cc: str = "cc"):
    """Compiles `code` to `out_path`. Returns (success, compiler_stderr)."""
    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".c", delete=False
    ) as src_file:
        src_file.write(code)
        src_path = src_file.name

    try:
        proc = subprocess.run(
            [cc, "-O0", src_path, "-o", str(out_path)],
            capture_output=True,
            text=True,
            timeout=30,
        )
        return proc.returncode == 0, proc.stderr
    finally:
        pathlib.Path(src_path).unlink(missing_ok=True)


def run_binary(binary: pathlib.Path, args: list):
    try:
        proc = subprocess.run(
            [str(binary)] + args, capture_output=True, text=True, timeout=10
        )
        return proc.returncode, proc.stdout
    except (subprocess.TimeoutExpired, OSError) as e:
        return None, str(e)


def behavioral_score(
    ground_truth_binary: pathlib.Path,
    reconstructed_code: str,
    test_cases: list,
    work_dir: pathlib.Path,
    cc: str = "cc",
) -> dict:
    """Compiles `reconstructed_code` and runs it against `test_cases`
    (a list of argv lists), comparing (exit_code, stdout) to the ground
    truth binary for each case.

    Returns a dict: {
      "compiled": bool,
      "compiler_error": str | None,
      "total_cases": int,
      "matching_cases": int,
      "accuracy": float,   # matching_cases / total_cases, 0.0 if it didn't compile
      "details": [ {args, ground_truth: (rc, stdout), reconstructed: (rc, stdout) | None, match: bool} ]
    }
    """
    recon_binary = work_dir / "reconstructed"
    compiled, compiler_error = compile_reconstruction(
        reconstructed_code, recon_binary, cc=cc
    )

    result = {
        "compiled": compiled,
        "compiler_error": None if compiled else compiler_error,
        "total_cases": len(test_cases),
        "matching_cases": 0,
        "accuracy": 0.0,
        "details": [],
    }

    for args in test_cases:
        gt = run_binary(ground_truth_binary, args)
        if not compiled:
            result["details"].append(
                {"args": args, "ground_truth": gt, "reconstructed": None, "match": False}
            )
            continue
        recon = run_binary(recon_binary, args)
        match = recon == gt
        if match:
            result["matching_cases"] += 1
        result["details"].append(
            {"args": args, "ground_truth": gt, "reconstructed": recon, "match": match}
        )

    if result["total_cases"] > 0:
        result["accuracy"] = result["matching_cases"] / result["total_cases"]

    return result
