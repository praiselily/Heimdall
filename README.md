# Heimdall

An LLVM control-flow flattening pass for software IP protection. Includes
a benchmark measuring how much it degrades AI-assisted decompilation.

Same space as [OLLVM](https://github.com/obfuscator-llvm/obfuscator) and
[Hikari](https://github.com/HikariObfuscator/Hikari): legitimate
anti-tampering tooling for shipped binaries. Targets current stable LLVM
and the new Pass Manager. Treats AI-assisted reverse engineering as
something worth measuring directly.

## What it does

`heimdall-cff` rewrites an eligible function's control flow into a single
dispatcher loop driven by a state variable and a switch. A decompiler sees
one flat loop where the function's real branches and loops used to be.
Full transform and eligibility rules in [DESIGN.md](DESIGN.md).

v1 ships one pass, built properly. See [Roadmap](#roadmap) for what's
deferred.

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
default `-O` pipeline by itself. Running it takes two steps: emit
optimized IR, run `opt` with the plugin loaded, hand the result back to
clang for codegen.

```bash
clang -O1 -S -emit-llvm input.c -o input.ll
opt -load-pass-plugin=build/lib/HeimdallCFF.so -passes=heimdall-cff input.ll -S -o input.flat.ll
clang input.flat.ll -o output
```

Functions that fail the eligibility checks (exception handling, indirect
control flow, `optnone`) are left alone. `-debug-only=heimdall-cff`
(assertions build) shows what got skipped and why.

## Correctness

`test/correctness` compiles each sample program plain and flattened, runs
both against the same inputs, and diffs the output byte for byte. Sample
set: a license-key validator, a config-line parser, a small Feistel
cipher, and a few targeted stress cases (entry-block value demotion,
nested loops with a switch, an irreducible CFG). What each checks is in
[DESIGN.md](DESIGN.md#known-limitations).

```bash
cmake --build build --target test
# or directly:
python test/correctness/run_correctness.py \
  --clang clang --plugin build/lib/HeimdallCFF.so \
  --programs-dir test/correctness/programs --work-dir /tmp/heimdall-correctness
```

## AI-resistance benchmark

For each sample program, both binaries get decompiled with Ghidra. An LLM
reconstructs the program's behavior and purpose from the pseudocode. The
result is scored on whether the rewrite behaves the same and whether the
explanation is right. How to run it: [benchmark/README.md](benchmark/README.md).
Methodology: [DESIGN.md](DESIGN.md#benchmark-harness).

Results so far: [`benchmark/results/RESULTS.md`](benchmark/results/RESULTS.md).
Ghidra's decompiler fails outright on a flattened function. Its jump-table
recovery needs a range check to bound the dispatcher's indirect jump, and
LLVM omits that check since the switch's default case is unreachable by
construction. No usable pseudocode comes out the other end for a model to
work from. The writeup covers the caveat that goes with this.

## Roadmap

Deferred past v1:

- Instruction substitution
- Bogus control flow / opaque predicates
- String encryption
- VM-based obfuscation
- Chaining multiple passes together

## License

[MIT](LICENSE)
