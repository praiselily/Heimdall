# Plan: Starter prompt for an LLVM-based "AI-resistant" obfuscation project

## Context

The user wants to build a GitHub-traction-worthy cybersecurity project in C++, in the
code-obfuscation / software-protection space (legitimate IP-protection tooling, same
category as OLLVM/Hikari — not an evasion or malware tool). Based on their own
observation that AI/LLM tools are increasingly used to decompile and reverse-engineer
applications, the project should specifically target resisting **AI-assisted
decompilation**, which is a differentiator current tools don't measure or market
against.

Working directory (`C:\Users\ingeu\Github Projects`) has no existing code for this —
it's a fresh project. The deliverable of this planning session is not code, but a
well-scoped starter prompt the user will paste into a new chat to kick off the actual
build.

Scope decisions from the user (via AskUserQuestion):
- **v1 breadth**: one obfuscation pass, done well — not a multi-pass framework yet.
- **AI-resistance**: first-class and measured — the project must include a benchmark
  harness comparing obfuscated vs. plain binaries against real decompilation/AI-assist
  tooling, with results reported in the README.
- **LLVM target**: latest stable LLVM release, using the new Pass Manager API (avoids
  OLLVM's main weakness of being stuck on an ancient LLVM fork).

## Deliverable: the prompt

Give the user this text (as the recommended content for a new chat):

---

> I want to build a C++/LLVM project called an obfuscation pass for software
> IP-protection (same category as OLLVM/Hikari — legitimate anti-tampering/anti-reverse-engineering
> tooling, not an evasion tool), with a specific, differentiating angle: measuring and
> improving resistance to **AI-assisted decompilation** (LLM-based decompilers,
> AI-assist plugins for Ghidra/IDA/Binary Ninja that summarize or rename decompiled
> pseudocode).
>
> **Scope for v1 — ship one pass well, not a framework:**
> Implement control-flow flattening as an LLVM pass: transform each function's CFG
> into a single dispatcher loop driven by a state variable and a switch statement, so
> static analysis and decompiler output no longer reflects the original control flow.
>
> **Technical requirements:**
> - Target the current stable LLVM release, written against the new Pass Manager
>   (`PassInfoMixin`), buildable as an out-of-tree `opt`/Clang plugin via CMake.
> - Take a `-mllvm`/pass-pipeline flag to enable the pass per-function or per-module.
> - Must preserve program semantics exactly — correctness tests (existing test
>   programs run before/after the pass, output diffed) are non-negotiable.
> - Should degrade gracefully on edge cases (irreducible CFGs, exception-handling
>   landing pads, etc.) rather than miscompiling — bail out of transforming a function
>   if it hits an unsupported shape.
>
> **The AI-resistance benchmark (first-class deliverable, not an afterthought):**
> Build a small harness that, for a set of sample test binaries:
> 1. Compiles each test program both with and without the pass.
> 2. Decompiles both versions with an open decompiler (e.g. Ghidra headless or
>    Binary Ninja's free tier) to get pseudocode.
> 3. Feeds the pseudocode to an LLM and asks it to reconstruct/explain the function's
>    behavior (or asks a code-focused LLM decompiler model directly, if a suitable
>    open one is accessible).
> 4. Scores how close the LLM's reconstruction is to the real semantics (e.g. does it
>    correctly describe the algorithm, does it produce code that behaves the same on a
>    test input set) — flattened vs. plain, same test programs.
> Report the comparison in the README with concrete before/after examples, not just a
> summary claim.
>
> **Deliverables:**
> - The pass plugin + CMake build.
> - A handful of sample test programs chosen to exercise realistic logic (not toy
>   loops) — e.g. a small license-check function, a basic parser, a simple crypto
>   routine.
> - The benchmark harness and its results.
> - A README with build instructions, before/after disassembly snippets, and the
>   benchmark findings.
>
> **Explicitly out of scope for v1** (future work, don't build now): instruction
> substitution, bogus control flow / opaque predicates, string encryption,
> VM-based obfuscation, chaining multiple passes. Flag these as a roadmap in the
> README instead of building them.
>
> Please start by proposing a concrete technical design (pass structure, how the
> dispatcher/state-variable transform will be built on LLVM IR, and how the benchmark
> harness will be wired up end-to-end) before writing code.

---

## Notes for the user

- This framing (software IP-protection / anti-tampering, benchmarked defensively
  against AI-assisted RE) is the legitimate, publishable angle — keep it in the
  README positioning when the project goes public.
- Starting with one well-executed pass plus a real benchmark is more likely to get
  attention than a half-finished multi-pass framework — the benchmark results are
  the hook for a launch post (r/ReverseEngineering, r/netsec, HN).
- When the user opens the new chat with this prompt, the first reply from Claude
  should be a design proposal, not code — matches the "propose design first" closing
  line in the prompt above.
