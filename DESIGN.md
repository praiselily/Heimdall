# Heimdall: Design

## Goal

Heimdall ships one LLVM obfuscation pass, control flow flattening, built
for software IP protection (same category as OLLVM/Hikari), with a
reproducible benchmark measuring how much it degrades AI-assisted
decompilation (LLM-based reconstruction of decompiler pseudocode), not just
traditional static-analysis metrics.

v1 ships exactly one pass. Bogus control flow, opaque predicates,
instruction substitution, string encryption, VM-based obfuscation, and pass
chaining are future work. See [README.md](README.md#roadmap).

## Pass structure

`heimdall::ControlFlowFlatteningPass` is a `PassInfoMixin`-based function
pass registered through the new Pass Manager's plugin interface
(`llvmGetPassPluginInfo`), loaded into `opt`/`clang` via `-fpass-plugin=` /
`-load-pass-plugin=` and run under `-passes=heimdall-cff`.

### Transform

For a function `F` that passes eligibility checks:

1. **Collect basic blocks.** Every block in `F` except the entry block,
   which stays in place and falls through into the dispatcher, so PHI nodes
   and arguments tied to the entry block's identity aren't disturbed.
2. **Assign state IDs.** Each collected block gets an `i32` state constant,
   drawn from a shuffled permutation of `[0, N)` rather than handed out in
   block order. Block order at this point is still close to source order,
   so a sequential assignment would leave the constant each block stores as
   a stand-in position index: the direct edges are gone, but "lower values
   run earlier" would still leak most of the original layout. The
   permutation is seeded from the function's name, so it's fixed for a
   given input rather than different every build.
3. **Build the dispatcher.** A new block holds a `switch` over a
   stack-allocated `state` variable, one case per collected block, falling
   through to an `unreachable` default (never hit, since every store to
   `state` writes a valid case value).
4. **Rewrite terminators.** Each collected block's original terminator
   (`br`, conditional `br`, `switch`) is replaced with a write of the next
   state value to the `state` alloca, followed by an unconditional branch
   back to the dispatcher. A conditional branch becomes
   `select(cond, trueStateId, falseStateId)` stored into `state`, so the
   condition still runs and the semantics are unchanged, but the edge it
   produces is no longer a direct edge in the function's graph. Every
   block's only textual successor becomes the dispatcher.
5. **Entry block rewrite.** The entry block's terminator is rewritten the
   same way, pointing into the dispatcher instead of its original target.
6. **Memory instead of SSA for cross-block values.** Any value defined in
   one block and used in another is demoted from an SSA/PHI edge to a
   stack slot, the same approach `-reg2mem` uses, since flattening breaks
   the dominance relationships PHI nodes depend on. Done with LLVM's
   `DemoteRegToStack` rather than a hand-rolled version.

### Eligibility

Flattening is opt-in per function: bail out rather than risk a miscompile.
A function is skipped when:

- it has fewer than two basic blocks;
- it or any block in it contains a `landingpad`, `invoke`, `resume`,
  `catchswitch`/`catchpad`/`cleanuppad` (exception handling isn't flattened
  in v1);
- it contains an `indirectbr` or `callbr`;
- it's a declaration, `optnone`, or its entry block doesn't end in a
  `br`/`switch`.

Loop structure isn't an eligibility condition. The transform rewrites each
block's terminator independently and never consults `LoopInfo`, so it
flattens irreducible CFGs the same way it flattens anything else;
`irreducible_dispatch.c` confirms this against a hand-built irreducible
cycle (two distinct external entries into the same loop body).

Every bail-out is logged at `-debug-only=heimdall-cff` and counted via
LLVM's `Statistic` machinery (`NumFunctionsFlattened` /
`NumFunctionsSkipped`, printed with `-stats`).

### Correctness strategy

- Flattening changes how control reaches a block, never what the block
  computes. Each branch condition still runs in its original block; only
  the edge is redirected through the dispatcher's state variable.
- `DemoteRegToStack` is reused rather than reimplemented. Hand-rolled PHI
  elimination is the easiest place to introduce a subtle miscompile.
- Demotion only applies to a value's *users*, never to an existing `alloca`
  itself. An alloca already lives in the entry block and already dominates
  everything; wrapping its pointer in another stack slot replaces direct
  uses of it with a loaded copy, which breaks anything that requires the
  literal alloca, `llvm.lifetime.start`/`end` in particular (their pointer
  argument must be an alloca or poison, not an arbitrary pointer value).
  `simple_parser.c`'s local array caught this directly: compiling it
  through the full pipeline produced a verifier error until the alloca
  case was excluded from demotion.
- Every transformed function runs through `llvm::verifyFunction` before the
  pass returns. A verification failure means a bug in the pass; it aborts
  rather than shipping malformed IR.
- `test/correctness` compiles each sample program plain and flattened, runs
  both against the same inputs, and diffs output byte for byte.

## Benchmark harness

Per sample program:

1. **Build** twice: plain (`-O1`) and flattened (`-O1` then `heimdall-cff`,
   so flattening survives at the optimization level a shipped binary would
   actually use).
2. **Decompile** both binaries with Ghidra headless analysis to get
   per-function pseudocode. Ghidra is free, scriptable, and the usual
   backend behind "AI decompiler" plugins that summarize Ghidra/IDA output
   with an LLM.
3. **Reconstruct**: feed the pseudocode to an LLM (model configurable,
   defaults to a Claude model via the Anthropic API) and ask it to explain
   what the function does and rewrite it as equivalent C.
4. **Score**:
   - Behavioral: compile the LLM's rewritten C, run it against the same
     test inputs used in `test/correctness`, compare outputs to the ground
     truth, aggregate to a pass/fail percentage.
   - Descriptive: an LLM judge scores, against the original source as
     ground truth, whether the explanation identifies the function's actual
     purpose rather than a vague guess.
5. **Report**: plain vs. flattened scores plus a few concrete before/after
   examples, written to `benchmark/results/`.

The harness only needs the two compiled binaries, so it's decoupled from
the pass and could later be pointed at other obfuscation passes for
comparison.

## Known limitations

### Fixed

- **State IDs leaked block order.** State constants were handed out as a
  plain `0..N-1` walk over the function's blocks in their original (still
  essentially source) order. Edges were hidden, but the constants
  themselves weren't: a block storing a lower state value than another
  reliably ran earlier in the source, letting an analyst partially
  reconstruct the original layout from the state values alone, without
  needing the edges at all. Fixed by drawing IDs from a shuffled
  permutation instead of a sequential one (see the transform's step 2,
  above).
- **The pass's own output was self-identifying.** Every synthetic value and
  block carried a literal `heimdall.*` name (`heimdall.state`,
  `heimdall.dispatch`, `heimdall.sel`, ...). These never reach a normally
  linked native binary, since LLVM doesn't preserve local value names past
  codegen, but they survive intact in any intermediate LLVM IR or bitcode:
  confirmed by compiling through the documented `opt`-based pipeline and
  grepping the output, where every one of those strings shows up in plain
  text. That matters more than it might look: LTO object files embed an
  LLVM bitcode section by design (`.llvmbc` / `__LLVM,__bitcode`), and
  `-flto` is an ordinary, common flag for exactly the kind of release build
  that would use an obfuscator. A bitcode section with `strings` turning up
  "heimdall.dispatch" defeats the entire point before an analyst runs a
  single real analysis pass. Fixed by not naming any of the pass's
  generated values or blocks, so they look like any other anonymous
  compiler-generated temporary.
- **Running further optimization after flattening reshapes the
  dispatcher.** Feeding the flattened IR through a standard `-O2` pipeline
  (tested directly: `opt -passes='default<O2>'` on already-flattened IR)
  doesn't undo the flattening or restore direct edges, but LLVM's
  loop-rotate and loop-simplify passes recognize the dispatcher as a
  genuine loop and reshape it into canonical form (dedicated preheader and
  latch blocks), growing the function somewhat in the process. Harmless to
  correctness, confirmed by re-running the test suite's inputs against the
  re-optimized binary, but it means `heimdall-cff` should run as the last
  step before codegen, not loaded into the middle of a larger pipeline or
  followed by further optimization (including a second, later LTO
  optimization pass) without expecting its output's shape to shift.

### Open

- **Plain CFF is still a known, fingerprintable pattern**, independent of
  the naming fix above. Every flattened function has the same literal
  structural signature regardless of what anything is named: one `alloca`
  near the top of the function, a block holding exactly one `load` feeding
  a `switch` with an `unreachable` default, and every other block ending in
  a `store` to that same slot followed by an unconditional branch back to
  it. That's a trivial, high-precision structural match for any tool built
  to look for it (a simple IR-pattern scanner, let alone a deobfuscator
  specifically targeting CFF), and published deflattening techniques
  (symbolic-execution-based and pattern-based) target exactly this
  signature. The benchmark measures resistance to a general-purpose LLM
  reading raw pseudocode, not resistance to a tool built specifically to
  undo CFF; see [benchmark/README.md](benchmark/README.md#limitations).
  Opaque predicates on the dispatch condition, state-variable encoding, and
  multiple interleaved dispatchers are the roadmap items that address this
  directly, not just breadth for its own sake.
- **Flattening a trivial function barely obscures anything.** The
  eligibility check only requires 2+ basic blocks, so a simple two-block
  `if`/`else` gets turned into a one-case switch: strictly more code, a
  textbook-recognizable dispatcher shape, and essentially no extra
  analysis cost for a reader, since there was only ever one decision to
  begin with. Worth a minimum-block-count threshold below which flattening
  is skipped as not worth its own signature; not implemented.
- **No bound on dispatcher/select-chain size.** A function with a very
  large switch turns into a correspondingly long chain of sequential
  `select`/`icmp` pairs per rewritten terminator. Not a correctness issue,
  but code-size and compile-time blowup on pathological input hasn't been
  measured.
- **Coroutines and convergent operations aren't special-cased.** A
  function built around `llvm.coro.*` intrinsics, or containing `convergent`
  calls (common on GPU/SIMT targets, where flattening can violate the
  uniform-control-flow assumptions those targets rely on), isn't excluded
  by the eligibility check. Low priority for the project's actual target
  (native CPU IP protection), but a real gap if this pass is ever pointed
  at that kind of code.

## Out of scope for v1

See [README.md](README.md#roadmap).
