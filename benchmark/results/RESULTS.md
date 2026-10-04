# Heimdall AI-resistance benchmark: results

Run date 2026-10-04. Ghidra 12.1.4 (headless), LLVM/clang 22.1.8 (MSYS2
clang64), `heimdall-cff` built from this repository.

No `ANTHROPIC_API_KEY` was configured for this run, so the
reconstruction/judging steps were done manually: reading only the
pseudocode shown below, with no access to this project's actual source,
the same constraint `harness.py` would place on an API call, just without
making the call. Worth saying plainly: whoever did the reconstruction
here also built the pass being tested, so this isn't an arms-length
result the way a real API run would be. The conclusion (nothing in the
flattened pseudocode to reconstruct from) doesn't change depending on who
reads it, since the information just isn't in the input either way, but
take this run as a methodology check rather than a substitute for the
automated one.

Two things were broken and got fixed before any of this ran:

- `DecompileFunctions.py` never ran. Ghidra 11.3+ requires PyGhidra for a
  `.py` postScript and fails headless analysis without it. Replaced with
  `DecompileFunctions.java`.
- The sample binaries statically link their CRT, so each decompile
  returns around 28 functions, almost all CRT startup code untouched by
  the obfuscation pass. Reconstruction and scoring below use only the
  function(s) that matter per program.

## Result

| Program | Variant | Ghidra decompile | Behavioral match |
|---|---|---|---|
| license_check | plain | clean, readable C | 3/3 |
| license_check | flattened | failed ("Could not recover jumptable") | 2/3, coincidental |
| simple_parser | plain | clean, readable C | 3/3 |
| simple_parser | flattened | failed, same error | 1/3, coincidental |
| tiny_crypto | plain | clean, readable C | 3/3 |
| tiny_crypto | flattened | failed, same error | 0/3 |

Read the next two sections before treating this as "67% understood" — the
flattened matches aren't real recoveries.

## Ghidra's decompiler fails outright

For the plain binaries, Ghidra produces clean, accurate C; reconstructing
each one was close to mechanical. For every flattened binary, Ghidra
doesn't produce obfuscated-but-readable code, it gives up entirely. In
full, `validate_license_key` flattened decompiles to:

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

That's the whole function as Ghidra sees it. The length check, the dash
positions, and the checksum arithmetic don't appear anywhere in it. Even
the return type comes out wrong (`void` instead of `bool`), because
Ghidra can't follow the dispatch far enough to see what's actually
returned. The same happened to `parse_config` and to `main` in
`tiny_crypto`, whose round function and encrypt/decrypt loops sit entirely
inside the flattened region.

The reason is specific and mechanical, not just "it's more confusing."
Ghidra's switch/jump-table recovery needs a bound on how many table
entries are safe to read, normally taken from an explicit range check
right before the indirect jump. The dispatcher has none: LLVM knows the
state variable can never hold an out-of-range value, since the switch's
default is `unreachable`, so it never emits a bounds check at all. Without
one, Ghidra can't bound the table and bails with "too many branches." The
reconstruction scores below aren't really testing reasoning, because
there's nothing left in the text to reason about.

Given nothing recoverable, the honest reconstruction is "this can't be
determined" plus whatever the one or two visible branches show, usually a
null check. That produces a program that always rejects, and it then
matches ground truth on any test case whose correct answer also happens
to be reject. Not because anything was recovered, just because "no" is
sometimes the right answer by default. `simple_parser`/flattened's one
match is exactly this: the reconstruction always prints `PARSE_ERROR`,
correct for `"malformed"`, wrong for the other two inputs. Zero genuine
algorithm recovery happened for any flattened function in this run.

## What the raw disassembly still shows

Disassembling `validate_license_key` directly, not through the
decompiler, is ordinary, legible x86-64: every basic block ends in a
`jmp` to the exact same address, the dispatcher. A human reading the raw
listing, or a tool that looks at disassembly instead of one pseudocode
dump, would plausibly still notice every path converges on one address
and flag this as control-flow flattening without reconstructing the exact
logic. What this run actually measured is narrower than "can this be
broken": specifically, whether an LLM layered on Ghidra's default
decompiler output recovers the behavior. It doesn't. Whether a more
thorough pass, raw disassembly, a second decompiler, or a dedicated
deflattening tool would still succeed is a separate question this run
doesn't answer. See [DESIGN.md](../../DESIGN.md#known-limitations) on
plain CFF being a known pattern.

## Raw test cases

```
license_check / plain   --> 3/3  (1234-5678-0036, 1234-5678-0000, not-a-key)
license_check / flat    --> 2/3  (fails the one VALID case; recon never returns VALID)
simple_parser / plain   --> 3/3  (a=1;b=2;c=3, malformed, "")
simple_parser / flat    --> 1/3  (only "malformed", coincidental)
tiny_crypto   / plain   --> 3/3  (00000000, deadbeef, 12345678)
tiny_crypto   / flat    --> 0/3
```

Three programs and one decompiler is a starting sample, not a
statistically powerful one; see
[benchmark/README.md](../README.md#limitations). `tiny_crypto`'s subkey
constants aren't visible in Ghidra's C output at all, only an opaque
`DAT_...` reference, so the plain-variant reconstruction used the actual
bytes read from the binary's `.rdata` section. That's a realistic
extension of what a Ghidra-based workflow has available, not something
inferred from the pseudocode text alone.
