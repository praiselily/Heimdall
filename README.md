# Heimdall

An LLVM control-flow flattening pass for software IP protection, with a
benchmark measuring how much it actually degrades AI-assisted
decompilation rather than just claiming it does.

Same space as [OLLVM](https://github.com/obfuscator-llvm/obfuscator) and
[Hikari](https://github.com/HikariObfuscator/Hikari): anti-tampering
tooling for shipped binaries, not an evasion tool. Targets current stable
LLVM and the new Pass Manager instead of an old fork, and treats
AI-assisted reverse engineering as something to measure directly.

## What it does

`heimdall-cff` rewrites an eligible function's control flow into a single
dispatcher loop driven by a state variable and a switch. A decompiler sees
one flat loop instead of the function's real branches and loops. Full
transform and eligibility rules in [DESIGN.md](DESIGN.md).

v1 is one pass, done properly, instead of a half-built framework. See
[Roadmap](#roadmap).

## Build

LLVM with the new Pass Manager (the default for years now) and CMake
3.20+. Built and tested against LLVM 22.

```bash
cmake -S . -B build -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm
cmake --build build
```

Produces the `HeimdallCFF` plugin (`build/lib/HeimdallCFF.so` / `.dylib` /
`.dll`).

## Usage

`-fpass-plugin` registers the pass with clang but won't splice it into the
default `-O` pipeline by itself. Running it is a two-step process: emit
optimized IR, run `opt` with the plugin loaded, hand the result back to
clang for codegen.

```bash
clang -O1 -S -emit-llvm input.c -o input.ll
opt -load-pass-plugin=build/lib/HeimdallCFF.so -passes=heimdall-cff input.ll -S -o input.flat.ll
clang input.flat.ll -o output
```

Functions that fail the eligibility checks (exception handling, indirect
control flow, `optnone`) are left alone rather than risking a miscompile.
`-debug-only=heimdall-cff` (assertions build) shows what got skipped.

## Correctness

`test/correctness` compiles each sample program plain and flattened, runs
both against the same inputs, and diffs the output byte for byte. Sample
set: a license-key validator, a config-line parser, a small Feistel
cipher, and a few targeted stress cases (entry-block value demotion,
nested loops with a switch, an irreducible CFG). See
[DESIGN.md](DESIGN.md#known-limitations) for what each one checks.

```bash
cmake --build build --target test
# or directly:
python test/correctness/run_correctness.py \
  --clang clang --plugin build/lib/HeimdallCFF.so \
  --programs-dir test/correctness/programs --work-dir /tmp/heimdall-correctness
```

## AI-resistance benchmark

For each sample program, both binaries get decompiled with Ghidra, an LLM
reconstructs the program's behavior and purpose from the pseudocode, and
the result is scored on whether the rewrite actually behaves the same and
whether the explanation is right. See
[benchmark/README.md](benchmark/README.md) for how to run it and
[DESIGN.md](DESIGN.md#benchmark-harness) for the methodology.

Results so far: [`benchmark/results/RESULTS.md`](benchmark/results/RESULTS.md).
Ghidra's decompiler doesn't just struggle with a flattened function, it
fails outright. Its jump-table recovery can't bound the dispatcher's
indirect jump without a range check, and LLVM omits that check since the
switch's default case is unreachable by construction. That leaves no
usable pseudocode for a model to reconstruct anything from. The writeup
covers the caveat that goes with it too.

## Roadmap

Out of scope for v1, on purpose:

- Instruction substitution
- Bogus control flow / opaque predicates
- String encryption
- VM-based obfuscation
- Chaining multiple passes together

## License

[MIT](LICENSE)
