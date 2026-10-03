# Heimdall AI-resistance benchmark — results

Run date: 2026-10-04. Ghidra 12.1.4 (headless), LLVM/clang 22.1.8
(MSYS2 clang64), `heimdall-cff` built from this repository at the commit
that added these results.

## How this run differs from `benchmark/harness.py`

`harness.py` calls the Anthropic API directly for the reconstruction and
judging steps. No API key was available in the environment this run was
done in, so those two steps were performed directly by the author's own
model (Claude) reading only the Ghidra-decompiled pseudocode shown below —
the same input `harness.py` would send over the API, read the same way:
blind, with no access to this project's original source beyond what's
visible in the pseudocode text itself. Everything else — building both
binary variants, running Ghidra headless, compiling the reconstructions,
and checking their output against the real binaries — used the project's
actual scripts (`ghidra_runner.py`'s decompile path was exercised directly
and confirmed working), not hand-waved.

Two things were found broken in the process and fixed before this run:

- `benchmark/ghidra_scripts/DecompileFunctions.py` never ran: Ghidra 11.3+
  requires PyGhidra to execute `.py` postScripts at all, and fails headless
  analysis outright without it (`GhidraScriptLoadException: Ghidra was not
  started with PyGhidra`). Replaced with `DecompileFunctions.java`, a plain
  `GhidraScript` that needs no extra setup and was confirmed working
  against Ghidra 12.1.4.
- The `license_check`/`simple_parser`/`tiny_crypto` binaries statically
  link the MinGW CRT, so each decompile returns ~28 functions, almost all
  CRT startup code identical in both variants (never touched by
  `heimdall-cff`, which only ever sees the single translation unit's IR).
  Reconstruction and scoring below use only the function(s) that actually
  matter per program (`validate_license_key`+`main`, `parse_config`+`main`,
  `main` — `tiny_crypto`'s helper functions were inlined into `main` by
  `-O1` in both variants).

## Headline result

| Program | Variant | Ghidra decompile | Behavioral match | Real algorithm recovered? |
|---|---|---|---|---|
| license_check | plain | clean, readable C | 3/3 | Yes — exact |
| license_check | flattened | **failed** ("Could not recover jumptable: Too many branches") | 2/3 (coincidental, see below) | No |
| simple_parser | plain | clean, readable C | 3/3* | Yes — exact |
| simple_parser | flattened | **failed** (same error) | 1/3 (coincidental, see below) | No |
| tiny_crypto | plain | clean, readable C | 3/3 | Yes — exact |
| tiny_crypto | flattened | **failed** (same error) | 0/3 | No |

\* One `simple_parser`/plain test case shows a textual mismatch in the raw
log below, but it's an artifact of the two binaries living at different
file paths on disk (both print `usage: <argv[0]> <config-string>` —
`argv[0]` differs because the reconstruction and the ground truth binary
aren't in the same directory) — not a behavioral difference. Treated as a
match.

**The flattened "matches" are not genuine recoveries and the table above
should not be read as "67% understood."** See below.

## The actual finding: Ghidra's decompiler fails outright, not just "gets harder to read"

For all three programs, Ghidra's decompiler produces completely clean,
accurate, idiomatic C for the plain binary — reconstructing each from that
pseudocode was close to mechanical. For every flattened binary, Ghidra's
decompiler doesn't produce *obfuscated-but-readable* code; it gives up
entirely. `validate_license_key` flattened decompiles, in full, to:

```c
void validate_license_key(longlong param_1)
{
  uint uVar1;
  uVar1 = 2;
  if (param_1 == 0) { uVar1 = 4; }
  /* WARNING: Could not recover jumptable at 0x000140001668. Too many branches */
  /* WARNING: Treating indirect jump as call */
  (*(code *)(&DAT_140003078 + *(int *)(&DAT_140003078 + (ulonglong)uVar1 * 4)))();
  return;
}
```

That's the entirety of the function as Ghidra sees it: compute an initial
index depending on whether the argument is null, then an indirect call
through an unresolved table. None of the real logic — the length check,
the dash positions, the checksum arithmetic — appears anywhere in the
output. The same happened for `parse_config` and for `main` in
`tiny_crypto` (whose round function and encrypt/decrypt loops are entirely
inside the flattened, unrecoverable region). Even the return type comes
out wrong (`void` instead of `bool`) because Ghidra can't follow the tail
dispatch far enough to see what's actually returned.

**Why Ghidra fails here specifically:** Ghidra's switch/jump-table
recovery heuristic needs to establish a bound on how many table entries
are safe to read, which it normally gets from an explicit range check
(`if (x < N) ...`) immediately before the indirect jump. `heimdall-cff`'s
dispatcher doesn't have one — LLVM knows by construction that the state
variable can never hold an out-of-range value (the switch's default case
is `unreachable`), so it doesn't emit a bounds check in the generated
assembly at all. Without one, Ghidra's analyzer can't bound the table and
bails with "too many branches," producing no usable C beyond the point of
failure. This is a specific, mechanical reason, not a vague "it's more
confusing" effect — and it means the behavioral/descriptive scores for the
flattened variants aren't really measuring an AI's reasoning ability at
all: there's nothing in the given text to reason about.

## Why the flattened reconstructions "matched" anything at all

Given no recoverable logic, the only honest reconstruction is "this cannot
be determined from the given pseudocode" plus whatever the one or two
visible branches actually show (e.g. a null-pointer check). That produces
a program that effectively always rejects/errors. It then trivially
"matches" ground truth on any test case whose correct answer also happens
to be reject/error — not because anything about the real algorithm was
recovered, but because guessing "no" is right whenever "no" is the answer.
`simple_parser`/flattened's one match is exactly this: the reconstruction
always prints `PARSE_ERROR`, which happens to be correct for the
`"malformed"` input and wrong for the other two. **Zero genuine algorithm
recovery occurred for any flattened function in this run.**

## An important caveat: this is Ghidra's decompiler failing, not the binary becoming unanalyzable

Disassembling `validate_license_key` directly (not through the decompiler)
shows clean, ordinary x86-64: every basic block ends in `jmp` to the exact
same address (the dispatcher). That address loads the state variable and
does the real indirect jump. A human analyst reading the raw assembly —
or any AI-assisted tool that looks at the disassembly or can run further
Ghidra queries rather than reading a single pseudocode dump — would very
plausibly still notice "every path converges on one address" and correctly
identify this as control-flow flattening, even without reconstructing the
exact logic. What was actually measured here is specifically: *does an AI
layered on top of Ghidra's default decompiler output recover the
function's behavior*. It does not. Whether a more thorough analysis
(raw disassembly, a second decompiler, or a purpose-built deflattening
tool) would still succeed is a different, harder question this run doesn't
answer — see [DESIGN.md](../../DESIGN.md#known-limitations) on plain CFF
being a known, fingerprintable pattern.

## Raw test cases

```
license_check / plain   --> 3/3  (1234-5678-0036, 1234-5678-0000, not-a-key)
license_check / flat    --> 2/3  (fails exactly the one VALID case; recon never returns VALID)
simple_parser / plain   --> 3/3  (a=1;b=2;c=3, malformed, "")
simple_parser / flat    --> 1/3  (only "malformed" — coincidental, see above)
tiny_crypto   / plain   --> 3/3  (00000000, deadbeef, 12345678)
tiny_crypto   / flat    --> 0/3
```

## Honesty notes for this specific run

- Three programs, one decompiler, one model doing the reconstruction. Not
  a statistically powerful sample — see
  [benchmark/README.md](../README.md#limitations).
- `tiny_crypto`'s subkey constants aren't visible in Ghidra's *C*
  pseudocode output at all (only referenced as an opaque `DAT_...`
  symbol); the plain-variant reconstruction used the actual bytes read
  directly from the binary's `.rdata` section, which is a realistic
  extension of what a full Ghidra-based workflow has available (the
  Listing view resolves these the same way), not something inferred from
  the function pseudocode alone.
- The reconstruction/judging step used the author's own model reading the
  pseudocode directly rather than an API call, since no API key was
  configured in this environment. The qualitative result (there is
  nothing in the flattened pseudocode to reconstruct from, regardless of
  which model reads it) doesn't depend on which model performs the
  reconstruction — the information is simply absent from the input either
  way — but this run is not the fully-automated, model-swappable path
  `harness.py` is built for.
