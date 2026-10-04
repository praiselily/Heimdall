# Heimdall: Design

## Goal

One LLVM obfuscation pass (control flow flattening, same category as
OLLVM/Hikari) for software IP protection, plus a benchmark for how much it
degrades AI-assisted decompilation rather than traditional static-analysis
metrics.

v1 ships one pass. Bogus control flow, opaque predicates, instruction
substitution, string encryption, VM-based obfuscation, and pass chaining
are future work. See [README.md](README.md#roadmap).

## Pass structure

`heimdall::ControlFlowFlatteningPass` is a `PassInfoMixin` function pass,
registered through the new Pass Manager's plugin interface and run under
`-passes=heimdall-cff`.

### Transform

For an eligible function `F`:

1. Collect every block except entry, which stays in place and falls
   through into the dispatcher.
2. Assign each collected block an `i32` state ID, drawn from a shuffled
   permutation of `[0, N)` rather than handed out in block order
   (sequential IDs would leak the original layout back through the
   constants alone, even with the real edges gone). Seeded on the
   function name so it's stable across rebuilds.
3. Build a dispatcher block: a `switch` over a stack-allocated state
   variable, one case per block, `unreachable` default.
4. Rewrite each block's terminator to store its successor's state ID
   (`select(cond, trueId, falseId)` for a conditional branch) and jump to
   the dispatcher instead of its real target.
5. Rewrite the entry block's terminator the same way.
6. Demote any value used across blocks to a stack slot with
   `DemoteRegToStack`/`DemotePHIToStack`, since flattening breaks the
   dominance PHI nodes depend on.

### Eligibility

Skipped (not transformed) when a function:

- has fewer than two basic blocks,
- contains `landingpad`/`invoke`/`resume`/`catchswitch`/`catchpad`/
  `cleanuppad` (exception handling isn't flattened in v1),
- contains `indirectbr` or `callbr`,
- is a declaration, `optnone`, or its entry block doesn't end in a
  `br`/`switch`.

Loop structure isn't checked. The transform rewrites terminators
independently of `LoopInfo`, and flattens irreducible CFGs the same as
anything else (`irreducible_dispatch.c` covers this).

Bail-outs are logged at `-debug-only=heimdall-cff` and counted via
`NumFunctionsFlattened` / `NumFunctionsSkipped`.

### Correctness

- Only the mechanism reaching a block changes, never what it computes.
- `DemoteRegToStack` is reused rather than reimplemented.
- An existing `alloca` is excluded from demotion: it already dominates
  everything, and wrapping its pointer in another slot breaks
  `llvm.lifetime.start`/`end`, which require the literal alloca.
- Every transformed function runs through `verifyFunction`; a failure
  aborts rather than shipping bad IR.
- `test/correctness` builds each sample plain and flattened, runs both
  against the same inputs, diffs output.

## Benchmark harness

Per sample program: build plain and flattened (both `-O1`), decompile both
with Ghidra, ask an LLM to explain and rewrite each from pseudocode alone,
score the rewrite behaviorally (compiled and run against real test
inputs) and descriptively (does the explanation match a ground-truth
description). Results go to `benchmark/results/`.

The harness only needs the two binaries, so it isn't tied to this one
pass.

## Known limitations

- Flattened functions still match one literal structural signature,
  regardless of naming: alloca, load-into-switch block, unreachable
  default, every other block storing then branching back to it. That's a
  fingerprint a tool built for CFF specifically could match without
  reasoning about anything. See
  [benchmark/README.md](benchmark/README.md#limitations); opaque
  predicates and state-variable encoding on the roadmap address this.
- No minimum block-count threshold: a trivial two-block `if`/`else` still
  gets "flattened" into a one-case switch, which is pure overhead.
- No bound on the select/icmp chain for a terminator's state computation,
  so a very large switch means a correspondingly large chain.
- Coroutines and `convergent` operations (GPU/SIMT) aren't excluded by
  eligibility. Not a concern for the native-CPU target this is built for,
  but a real gap if pointed at that kind of code.
- Running further optimization after flattening (e.g. a later `-O2` or
  LTO pass) doesn't restore direct edges, but loop-rotate/loop-simplify
  will reshape the dispatcher into canonical loop form. Harmless to
  correctness but means this pass should run last, right before codegen.

## Out of scope for v1

See [README.md](README.md#roadmap).
