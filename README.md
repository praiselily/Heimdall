# Heimdall

An LLVM obfuscation pass for software IP protection, built around control
flow flattening, with a benchmark that measures how much it degrades
AI-assisted decompilation instead of just claiming it does.

Heimdall sits in the same space as [OLLVM](https://github.com/obfuscator-llvm/obfuscator)
and [Hikari](https://github.com/HikariObfuscator/Hikari): anti-tampering
tooling for protecting shipped binaries, not an evasion or malware tool.
It targets current stable LLVM and the new Pass Manager instead of an old
fork, and it treats AI-assisted reverse engineering (LLM decompiler
plugins, pseudocode summarizers) as something worth measuring directly
rather than assuming traditional obfuscation metrics still apply.

## What it does

`heimdall-cff` rewrites an eligible function's control flow into a single
dispatcher loop driven by a state variable and a switch, so a decompiler
sees one flat loop instead of the function's real branch and loop
structure. The full transform, eligibility rules, and correctness strategy
are in [DESIGN.md](DESIGN.md).

v1 scope is one pass, done properly, rather than a partial framework. See
[Roadmap](#roadmap).

## Build

Requires LLVM (new Pass Manager, which has been the default for years) and
CMake 3.20+. Built and tested against LLVM 22.

```bash
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build
```

This builds the `HeimdallCFF` plugin (`build/lib/HeimdallCFF.so` / `.dylib`
/ `.dll`).

## Usage

`-fpass-plugin` registers the pass with clang but doesn't splice it into
clang's default `-O` pipeline on its own, so running it is a two-step
process: emit optimized IR, run `opt` with the plugin loaded, then hand the
result back to clang for codegen.

```bash
clang -O1 -S -emit-llvm input.c -o input.ll
opt -load-pass-plugin=build/lib/HeimdallCFF.so -passes=heimdall-cff input.ll -S -o input.flat.ll
clang input.flat.ll -o output
```

Functions that fail the eligibility checks (exception handling, indirect
control flow, irreducible CFGs, `optnone`) are left alone instead of risking
a miscompile. Run `opt` with `-debug-only=heimdall-cff` (needs an
assertions-enabled LLVM build) to see what was skipped and why.

## Correctness

`test/correctness` compiles each sample program plain and flattened, runs
both against the same inputs, and diffs the output byte for byte. The
sample set covers a license-key validator, a config-line parser, a small
Feistel cipher, and three targeted stress cases (entry-block value
demotion, nested loops with a switch, and an irreducible CFG). See
[DESIGN.md](DESIGN.md#known-limitations) for what each stress case is
checking.

```bash
cmake --build build --target test
# or directly:
python test/correctness/run_correctness.py \
  --clang clang --plugin build/lib/HeimdallCFF.so \
  --programs-dir test/correctness/programs --work-dir /tmp/heimdall-correctness
```

## AI-resistance benchmark

For each sample program, the plain and flattened binary are both decompiled
with Ghidra, an LLM is asked to reconstruct the program's behavior and
explain its purpose from the pseudocode alone, and the result is scored on
two axes: whether the rewritten code actually behaves the same (compiled
and run against the real test inputs), and whether the explanation
correctly identifies what the program does. See
[benchmark/README.md](benchmark/README.md) for how to run it and
[DESIGN.md](DESIGN.md#benchmark-harness) for the methodology.

Results: [`benchmark/results/RESULTS.md`](benchmark/results/RESULTS.md).
Headline finding so far: Ghidra's decompiler doesn't just struggle with a
flattened function, it fails outright (its jump-table recovery can't
bound the dispatcher's indirect jump without an explicit range check,
which LLVM omits since the switch's default case is unreachable by
construction) -- producing no usable pseudocode for an LLM to work from at
all. The writeup also covers the important caveat that goes with that
result: it's specific to the decompiler's generated C, not a claim that
the binary becomes unanalyzable by other means.

## Roadmap

Out of scope for v1, on purpose:

- Instruction substitution
- Bogus control flow / opaque predicates
- String encryption
- VM-based obfuscation
- Chaining multiple passes together

## License

[MIT](LICENSE)
