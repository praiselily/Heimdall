# Heimdall

An LLVM obfuscation pass for software IP-protection — control-flow
flattening, built against LLVM's new Pass Manager — with a first-class
benchmark measuring how much it degrades **AI-assisted decompilation**
(LLM-based reconstruction of decompiler pseudocode), not just traditional
static-analysis metrics.

Heimdall is in the same category as [OLLVM](https://github.com/obfuscator-llvm/obfuscator)
and [Hikari](https://github.com/HikariObfuscator/Hikari): legitimate
anti-tampering / anti-reverse-engineering tooling for protecting shipped
binaries, not an evasion or malware tool. Unlike those projects, it targets
current stable LLVM (not a years-old fork) and treats AI-assisted
decompilation — the fastest-growing way real binaries get reverse-engineered
today — as something to measure and defend against explicitly, rather than
an afterthought.

## What it does

`heimdall-cff` transforms each eligible function's control-flow graph into a
single dispatcher loop driven by a state variable and a `switch`, so a
decompiler (and any AI layered on top of it) sees one flat loop instead of
the function's real branch/loop structure. See [DESIGN.md](DESIGN.md) for
the full transform description, eligibility/bail-out rules, and correctness
strategy.

**v1 scope is deliberately narrow: one pass, done well.** See
[Roadmap](#roadmap) for what's intentionally not built yet.

## Build

Requires a current stable LLVM (built with the new Pass Manager, which has
been default for years) and CMake 3.20+.

```bash
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build
```

This produces the `HeimdallCFF` plugin (`build/lib/HeimdallCFF.so` /
`.dylib` / `.dll`).

## Usage

```bash
clang -O1 -fpass-plugin=build/lib/HeimdallCFF.so \
      -mllvm -passes=heimdall-cff \
      input.c -o output

# or via opt directly:
opt -load-pass-plugin=build/lib/HeimdallCFF.so -passes=heimdall-cff input.ll -S
```

Functions that don't meet the eligibility checks (exception handling,
indirect control flow, irreducible CFGs, `optnone`) are left untouched
rather than miscompiled — see `-debug-only=heimdall-cff` for which functions
were skipped and why.

## Correctness

Every change to the pass must pass `test/correctness`: sample programs
(a license-key validator, a config-line parser, a small Feistel-cipher
routine, plus targeted stress cases for entry-block value demotion, nested
loops/switches, and an irreducible CFG — see
[DESIGN.md](DESIGN.md#known-limitations--weaknesses) for what each stress
case regression-tests and why) are compiled both plain and flattened, run
against the same inputs, and their output is diffed byte-for-byte.

```bash
cmake --build build --target test
# or directly:
python test/correctness/run_correctness.py \
  --clang clang --plugin build/lib/HeimdallCFF.so \
  --programs-dir test/correctness/programs --work-dir /tmp/heimdall-correctness
```

## AI-resistance benchmark

See [`benchmark/README.md`](benchmark/README.md) for how to run it and
[DESIGN.md](DESIGN.md#benchmark-harness-ai-resistance) for the methodology.
In short: for each sample program, both the plain and flattened binary are
decompiled with Ghidra, an LLM is asked to reconstruct the program's
behavior and explain its purpose from the pseudocode alone, and the
reconstruction is scored for behavioral equivalence (does the LLM's
rewritten code actually behave the same?) and descriptive accuracy (did it
correctly identify what the program does?).

Results, once generated, live in `benchmark/results/RESULTS.md` with
concrete before/after examples — not just a summary claim.

## Roadmap

Explicitly out of scope for v1, to keep it one well-executed pass instead of
a half-finished framework:

- Instruction substitution
- Bogus control flow / opaque predicates
- String encryption
- VM-based obfuscation
- Chaining multiple passes together

## License

[MIT](LICENSE).
