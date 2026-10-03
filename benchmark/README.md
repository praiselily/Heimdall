# AI-resistance benchmark

Measures how much `heimdall-cff` degrades **AI-assisted decompilation** --
an LLM's ability to reconstruct a function's real behavior and purpose from
decompiler pseudocode alone -- compared to the same program compiled without
obfuscation.

Results from the first real run: [`results/RESULTS.md`](results/RESULTS.md).
Headline finding: Ghidra's decompiler doesn't just make flattened functions
harder to read, it fails outright (its jump-table analyzer can't bound the
dispatcher's indirect jump without an explicit range check, which LLVM
omits since the switch's default case is unreachable by construction), so
there's nothing left in the pseudocode for an LLM to reconstruct from. Read
the full writeup for the important caveat that goes with that result.

## Pipeline

```
 source.c ──clang──┬─► plain.bin ──Ghidra──► plain pseudocode ──LLM──► reconstruction ──┐
                    │                                                                    ├─► score (plain vs. flattened)
                    └─► flat.bin ───Ghidra──► flat pseudocode ───LLM──► reconstruction ──┘
                        (+ heimdall-cff)
```

Full description of each stage: [`../DESIGN.md`](../DESIGN.md#benchmark-harness).

## Requirements

- A built `HeimdallCFF` plugin (see the top-level [README](../README.md#build))
  and `opt` on `PATH` (or pass `--opt`).
- [Ghidra](https://ghidra-sre.org/) installed locally; set `GHIDRA_INSTALL_DIR`
  or pass `--ghidra-dir`. Confirmed working against 12.1.4. Note: Ghidra
  11.3+ requires PyGhidra to run a `.py` postScript at all (a Jython-style
  script fails headless analysis outright without it), which is why
  `ghidra_scripts/DecompileFunctions.java` is a plain `GhidraScript`
  instead -- no extra setup needed.
- `pip install -r requirements.txt`
- `ANTHROPIC_API_KEY` set in the environment.

## Running

```bash
python harness.py \
  --clang clang \
  --plugin ../build/lib/HeimdallCFF.so \
  --ghidra-dir "$GHIDRA_INSTALL_DIR" \
  --programs-dir ../test/correctness/programs \
  --out-dir results
```

Writes `results/results.json` (raw data) and `results/RESULTS.md` (the
human-readable report, linked from the top-level README once generated).

## Scoring

- **Behavioral accuracy**: the LLM's reconstructed C is compiled and run
  against a fixed set of CLI inputs per sample program; its (exit code,
  stdout) is compared byte-for-byte against the ground-truth binary.
- **Descriptive score**: a second LLM call judges (0-5) whether the first
  LLM's plain-English explanation of the program correctly identifies its
  actual purpose, against a human-written ground-truth description.

Both are computed separately for the plain and flattened binary of each
sample program; the gap between them is the headline result.

## Limitations

- Three sample programs is a starting point, not a statistically powerful
  benchmark. Treat results as illustrative and grow the sample set before
  citing numbers as general claims.
- The harness reconstructs the whole decompiled program in one LLM call
  rather than function-by-function, so it also measures the LLM's ability to
  infer `main`'s argv handling, not just the obfuscated function body.
- Ghidra is one decompiler; results may not generalize to IDA/Binary Ninja
  AI-assist plugins, which use different decompilation output.
- A binary that statically links its CRT decompiles to 25+ functions,
  almost all identical CRT startup code neither variant's obfuscation
  touches. `run_one` filters to each program's `relevant_functions` before
  handing anything to the model; a new sample program needs that list
  populated (check which of its functions actually show up standalone
  after `-O1` -- small static helpers are often inlined into their caller
  and never appear on their own).
- The first real run found that Ghidra's decompiler can fail outright on a
  flattened function rather than merely produce harder-to-read code (see
  [`results/RESULTS.md`](results/RESULTS.md)). When that happens, the
  "behavioral"/"descriptive" scores stop measuring the model's reasoning
  and start measuring "what does a reconstruction built from almost no
  information default to" -- still a meaningful result for this project's
  threat model, but worth checking for per sample rather than assumed.
