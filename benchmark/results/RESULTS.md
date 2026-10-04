# Heimdall AI-resistance benchmark: results

Run date 2026-10-04. Ghidra 12.1.4 headless, LLVM/clang 22.1.8 (MSYS2
clang64), `heimdall-cff` built from this repo.

No `ANTHROPIC_API_KEY` was set up for this run, so I read the pseudocode
below myself and did the reconstruction by hand instead of through
`harness.py`'s API call. Same input a model would get, no other access to
the source. I also wrote the pass, so this isn't independent
verification. Call it a dry run of the methodology, not the real thing.

Two things were broken first:

- `DecompileFunctions.py` never ran. Ghidra 11.3+ needs PyGhidra for a
  `.py` postScript. Replaced with `DecompileFunctions.java`.
- The binaries statically link their CRT, so each decompile returns
  around 28 functions, almost all CRT startup noise untouched by the
  pass. Scoring below only uses the function(s) that matter per program.

## Result

| Program | Variant | Ghidra decompile | Behavioral match |
|---|---|---|---|
| license_check | plain | clean, readable C | 3/3 |
| license_check | flattened | failed ("Could not recover jumptable") | 2/3, coincidental |
| simple_parser | plain | clean, readable C | 3/3 |
| simple_parser | flattened | failed, same error | 1/3, coincidental |
| tiny_crypto | plain | clean, readable C | 3/3 |
| tiny_crypto | flattened | failed, same error | 0/3 |

The flattened matches aren't real recoveries. Details below.

## Ghidra's decompiler fails outright

Plain binaries decompile to clean, accurate C. Reconstructing them was
mechanical. Flattened binaries don't decompile to obfuscated-but-readable
code; Ghidra gives up. `validate_license_key` flattened, in full:

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

That's the whole function. The length check, dash positions, checksum
math: none of it shows up. Even the return type is wrong (`void` instead
of `bool`), because Ghidra can't follow the dispatch far enough to see
what's actually returned. Same thing happened to `parse_config` and to
`main` in `tiny_crypto`, whose round function and encrypt/decrypt loops
sit entirely inside the flattened region.

Why: Ghidra's jump-table recovery needs a range check before the indirect
jump to bound how many table entries are safe to read. The dispatcher has
none. LLVM knows the state variable is always in range, since the
switch's default case is unreachable, so it skips the bounds check.
Ghidra can't bound the table and bails with "too many branches." There's
nothing left in the text for a model to reason about.

Given nothing to recover, the honest answer is "can't determine this"
plus whatever a visible branch shows, usually a null check. That defaults
to always rejecting, which matches ground truth whenever the correct
answer also happens to be reject. `simple_parser`/flattened's one match
is this exactly: it always prints `PARSE_ERROR`, right for `"malformed"`,
wrong for the other two. Zero real recovery across any flattened
function.

## Raw disassembly

Disassembling `validate_license_key` directly shows ordinary x86-64:
every block jumps to the same address, the dispatcher. A human reading
the listing, or a tool reading disassembly instead of one pseudocode
dump, would likely still spot that pattern. What this measures is
narrower: whether an LLM reading Ghidra's default decompiler output
recovers the behavior. It doesn't. Raw disassembly, a second decompiler,
or a dedicated deflattening tool might still succeed; that's a different
question. See [DESIGN.md](../../DESIGN.md#known-limitations) on plain CFF
being a known pattern.

## Raw test cases

```
license_check / plain   --> 3/3  (1234-5678-0036, 1234-5678-0000, not-a-key)
license_check / flat    --> 2/3  (fails the one VALID case; recon never returns VALID)
simple_parser / plain   --> 3/3  (a=1;b=2;c=3, malformed, "")
simple_parser / flat    --> 1/3  (only "malformed", coincidental)
tiny_crypto   / plain   --> 3/3  (00000000, deadbeef, 12345678)
tiny_crypto   / flat    --> 0/3
```

Three programs, one decompiler: a starting sample, not a statistically
powerful one (see [benchmark/README.md](../README.md#limitations)).
`tiny_crypto`'s subkey bytes aren't in Ghidra's C output at all, only an
opaque `DAT_...` reference. The plain reconstruction used the actual
bytes from the binary's `.rdata` section, which a real Ghidra session
would also have.
