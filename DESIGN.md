# Heimdall — Design

## Goal

Heimdall is a single, well-executed LLVM obfuscation pass — control-flow
flattening — built for legitimate software IP-protection (same category as
OLLVM / Hikari), with a first-class, reproducible benchmark measuring how much
it degrades **AI-assisted decompilation** (LLM-based reconstruction of
decompiler pseudocode), not just traditional static-analysis metrics.

v1 ships exactly one pass. Everything else (bogus control flow, opaque
predicates, instruction substitution, string encryption, VM-based
obfuscation, pass chaining) is explicitly future work — see
[README.md](README.md#roadmap).

## Pass structure

`heimdall::ControlFlowFlatteningPass` is a `PassInfoMixin`-based function
pass registered through the new Pass Manager's plugin interface
(`llvmGetPassPluginInfo`), so it loads into `opt`/`clang` via
`-fpass-plugin=` / `-load-pass-plugin=` and runs under
`-passes=heimdall-cff`.

### Transform, at a glance

For a function `F` that passes eligibility checks:

1. **Collect basic blocks.** Take every basic block in `F` except the entry
   block, which is left in place (it falls straight through into the
   dispatcher) to avoid disturbing PHI nodes/arguments that assume the entry
   block's identity.
2. **Assign state IDs.** Give each collected block an `i32` state constant.
3. **Build the dispatcher.** Create a new `dispatcher` block containing a
   `switch` over a stack-allocated `state` variable (`alloca i32`), with one
   case per collected block, falling through to an `unreachable` default
   (shouldn't be hit — state is only ever written to a valid case).
4. **Rewrite terminators.** For every collected block, replace its original
   terminator (`br`, conditional `br`, `switch`, ...) with:
   - a write of the *next* state value(s) to the `state` alloca, and
   - an unconditional `br` back to the `dispatcher`.
   A conditional branch becomes a `select`/branch-free store of
   `select(cond, trueStateId, falseStateId)` into `state`, then jumps to the
   dispatcher — so the real condition still executes (semantics preserved)
   but the CFG edge it produces is no longer visible as a direct edge in the
   function's graph; every block's only successor, textually, is the
   dispatcher.
5. **Entry block rewrite.** The entry block's terminator is rewritten the
   same way, pointing into the dispatcher instead of its original target.
6. **Memory, not SSA, for cross-block values.** Any value defined in one
   flattened block and used in another is demoted from an SSA/PHI edge to a
   stack slot (`alloca` + `load`/`store`) using the same approach as
   `-reg2mem`, since flattening destroys the dominance relationships PHI
   nodes depend on. This is done with LLVM's existing `DemoteRegToStack`
   utility rather than reinventing it.

### Eligibility / bail-out conditions

Flattening is **opt-in per function** (bail out silently, don't miscompile)
when any of the following hold — each is checked before any IR is mutated:

- The function has no basic blocks, or only one.
- The function (or any block in it) contains a `landingpad`, `invoke`,
  `resume`, `catchswitch`/`catchpad`/`cleanuppad` — exception-handling control
  flow is not flattened in v1.
- The function contains an `indirectbr` or a `callbr`.
- The function's CFG is **irreducible** (detected via LLVM's natural loop
  info: a block with multiple back-edges from outside any recognized loop,
  or cycles not captured by `LoopInfo`) — flattening irreducible graphs
  correctly needs a more involved node-splitting transform, deferred.
- The function is a declaration only, is `optnone`, or is marked
  `naked`/`noinline` in a way that conflicts with our instrumentation (we
  respect `optnone` and skip).

Every bail-out is logged at `-debug-only=heimdall-cff` and counted in a
per-module statistic (`NumFunctionsFlattened` / `NumFunctionsSkipped`,
surfaced through LLVM's `Statistic` machinery, printed with `-stats`).

### Correctness strategy

- Flattening never changes *what* a block computes, only the mechanism by
  which control reaches it. The condition driving each branch is still
  evaluated in its original block; only the edge is indirected through the
  dispatcher's `state` variable.
- `DemoteRegToStack` is a well-tested LLVM utility (used by `-reg2mem`) — we
  reuse it instead of hand-rolling PHI elimination, which is the easiest
  place to introduce subtle miscompiles.
- Verification: every transformed function is run through
  `llvm::verifyFunction` before the pass returns; a verification failure is
  treated as a bug and the pass aborts on that function (bail out, emit a
  diagnostic) rather than returning malformed IR.
- End-to-end correctness is enforced by `test/correctness`: each sample
  program is compiled twice (plain vs. `-passes=heimdall-cff`), run against
  the same input set, and outputs are diffed byte-for-byte. A mismatch fails
  the test suite.

## Benchmark harness (AI-resistance)

Pipeline, per sample program:

1. **Build** the program twice: plain (`-O1`, no obfuscation) and flattened
   (`-O1` then `heimdall-cff`, so flattening survives at the same opt level a
   real shipped binary would use).
2. **Decompile** both binaries with Ghidra headless analysis
   (`analyzeHeadless ... -postScript decompile_to_json.py`) to get
   per-function C-like pseudocode. Ghidra is used because it's free, scriptable,
   and widely used as the backend for "AI decompiler" plugins (e.g. the kind
   that summarize Ghidra/IDA pseudocode with an LLM).
3. **Reconstruct**: feed each function's pseudocode to an LLM (model
   configurable; defaults to a Claude model via the Anthropic API) with a
   fixed prompt asking it to (a) explain what the function does and (b)
   rewrite it as clean, equivalent C.
4. **Score**:
   - *Behavioral*: compile the LLM's rewritten C, run it against the same
     test-input set used in `test/correctness`, and compare outputs to the
     ground truth. Binary pass/fail per input, aggregated to a
     reconstruction-accuracy percentage.
   - *Descriptive*: a rubric-scored comparison (LLM-as-judge, with the
     original source as ground truth) of whether the explanation correctly
     identifies the function's actual purpose (e.g. "validates a license
     key against X" vs. a vague or wrong guess).
5. **Report**: aggregate scores, plain vs. flattened, into a table + a couple
   of concrete before/after pseudocode and reconstruction examples, written
   into `benchmark/results/`. The README links the latest results.

This keeps the harness decoupled from the pass itself (it only needs the two
compiled binaries) so it can later be pointed at other obfuscation passes for
comparison.

## Known limitations / weaknesses

Found via a code-level audit (no LLVM toolchain was available in that
session to compile and empirically verify against; treat the "fixed" items
as fixed-on-paper until exercised by `test/correctness` on a real build):

- **Fixed — entry-block demotion dominance bug.** `demoteCrossBlockValues`
  originally inserted its stack-slot allocas immediately before the entry
  block's *terminator* rather than at its *top*. For any value computed
  early in the entry block and used elsewhere — i.e. almost any non-trivial
  function — `DemoteRegToStack` inserts that value's store right after its
  definition, which landed *before* the (too-late) alloca in program order:
  invalid IR, caught by the pass's own `verifyFunction` call, which would
  then abort the whole compilation via `report_fatal_error`. Fixed by
  inserting all demotion allocas at `getFirstInsertionPt()` of the entry
  block instead. `test/correctness/programs/entry_value_stress.c` is a
  regression test for this specific shape (values computed in entry, used
  by later and reconverging blocks).
- **Fixed — stale "unchanged" report on a rare bail-out path.** If
  `flattenFunction` bailed out (unsupported entry terminator) *after*
  `demoteCrossBlockValues` had already mutated the function's IR, it still
  returned `false` ("unchanged"), which would make the pass report
  `PreservedAnalyses::all()` to the New Pass Manager — wrong, since the IR
  had in fact changed, risking stale cached analyses for later passes in
  the same pipeline run. Fixed by moving the entry-terminator shape check
  into `findBailOutReason` (eligibility), before any mutation can occur.
- **Fixed — a stale assertion could abort debug/assertions-enabled
  builds** on a function whose entry block returns directly while another,
  unreachable block exists elsewhere in the same function (dead code left
  by an earlier pass, or hand-written IR) — a shape `F.size() >= 2` does
  not exclude. Removed the incorrect assumption; the existing terminator
  dispatch already handles this shape safely (now via the eligibility
  check above, before it would even reach that code).
- **Over-conservative irreducible-CFG bail-out (coverage weakness, not a
  correctness bug).** On inspection, the flattening transform itself never
  consults `LoopInfo` or relies on any reducibility assumption — it
  rewrites each block's terminator independently, uniformly, regardless of
  loop structure. The irreducibility check was carried over from CFF
  literature as a defensive default rather than a necessity *for this
  specific transform*. Kept as-is for v1 (unverified relaxations are a bad
  trade in a security tool), but it means real-world irreducible code
  (hand-rolled dispatch loops, some compiler-generated state machines) gets
  *less* obfuscation coverage than it could. `irreducible_dispatch.c`
  exercises the bail-out path itself (confirms it's safe, i.e. doesn't
  crash and doesn't miscompile) without yet testing whether relaxing it
  would also be safe — that needs a real build to investigate further.
- **Plain CFF is a known, fingerprintable pattern.** A bare
  dispatcher-loop-plus-switch-over-a-state-variable is exactly the shape
  OLLVM-style CFF has produced for over a decade; published deobfuscation
  techniques (symbolic-execution-based and pattern-based "deflattening")
  specifically target this signature, and a sufficiently capable
  AI-assisted decompiler could plausibly be pattern-matched against known
  CFF shapes rather than needing to "reason" its way through the control
  flow at all. **This means the benchmark should be read as "does this
  resist a general-purpose LLM reading raw pseudocode," not "does this
  resist a tool built specifically to undo CFF."** Said more bluntly in
  [benchmark/README.md](benchmark/README.md#limitations--honesty-notes).
  Mitigations (opaque predicates on the dispatch condition, state-variable
  encoding/encryption, multiple interleaved dispatchers) are exactly the
  "explicitly out of scope for v1" items in the roadmap — this is the
  concrete reason they matter for v2, not just breadth for its own sake.
- **No bound on dispatcher/select-chain size.** A function with a very
  large `switch` (e.g. a generated jump table with hundreds of cases) turns
  into a chain of that many sequential `select`/`icmp` pairs per rewritten
  terminator. Not a correctness issue, but unbounded code-size/compile-time
  blowup on pathological input has not been tested or guarded against.

## Out of scope for v1

See [README.md](README.md#roadmap).
