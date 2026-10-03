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

## Out of scope for v1

See [README.md](README.md#roadmap).
