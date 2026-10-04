# AI-resistance benchmark

Measures how much `heimdall-cff` degrades an LLM's ability to reconstruct
a function's behavior and purpose from decompiler pseudocode, compared to
the same program built without obfuscation.

Results from the first run: [`results/RESULTS.md`](results/RESULTS.md).
Ghidra's decompiler fails outright on flattened functions. Its jump-table
analyzer can't bound the dispatcher's indirect jump without a range
check, and LLVM omits that check since the switch's default is
unreachable by construction. Nothing is left in the pseudocode to
reconstruct from. The writeup covers the caveat that comes with that.

## Pipeline

```
 source.c ──clang──┬─► plain.bin ──Ghidra──► plain pseudocode ──LLM──► reconstruction ──┐
                    │                                                                    ├─► score (plain vs. flattened)
                    └─► flat.bin ───Ghidra──► flat pseudocode ───LLM──► reconstruction ──┘
                        (+ heimdall-cff)
```

Stage-by-stage description: [`../DESIGN.md`](../DESIGN.md#benchmark-harness).

## Requirements

- A built `HeimdallCFF` plugin (see the top-level [README](../README.md#build))
  and `opt` on `PATH` (or pass `--opt`).
- [Ghidra](https://ghidra-sre.org/), `GHIDRA_INSTALL_DIR` set or
  `--ghidra-dir` passed. Tested against 12.1.4. Ghidra 11.3+ needs
  PyGhidra to run a `.py` postScript at all, which is why
  `ghidra_scripts/DecompileFunctions.java` is plain Java instead.
- `pip install -r requirements.txt`
- `ANTHROPIC_API_KEY` set.

## Running

```bash
python harness.py \
  --clang clang \
  --plugin ../build/lib/HeimdallCFF.so \
  --ghidra-dir "$GHIDRA_INSTALL_DIR" \
  --programs-dir ../test/correctness/programs \
  --out-dir results
```

Writes `results/results.json` and `results/RESULTS.md`.

## Scoring

- **Behavioral accuracy**: the reconstructed C is compiled and run against
  a fixed set of CLI inputs; its (exit code, stdout) is compared to the
  ground-truth binary.
- **Descriptive score**: a second LLM call judges 0-5 whether the
  explanation identifies the program's real purpose against a
  human-written ground-truth description.

Computed separately for plain and flattened; the gap is the headline
result.

## Limitations

- Three sample programs. Illustrative, not a statistically powerful
  sample; grow it before citing these numbers as general claims.
- One LLM call reconstructs the whole decompiled program, so it also
  measures the model's ability to infer `main`'s argv handling, not just
  the obfuscated function.
- Ghidra only; IDA/Binary Ninja AI-assist plugins use different
  decompiler output and may not generalize.
- A binary that statically links its CRT decompiles to 25+ functions,
  most of them CRT startup code neither variant touches. `run_one` filters
  to each program's `relevant_functions`; a new sample needs that list
  populated (check which functions actually show up standalone after
  `-O1`, since small static helpers often get inlined into their caller).
- When Ghidra's decompiler fails outright on a flattened function (see
  results), the behavioral/descriptive scores stop measuring reasoning
  and start measuring "what a near-empty reconstruction defaults to."
  Still meaningful for this project's threat model, but worth checking
  per sample.
