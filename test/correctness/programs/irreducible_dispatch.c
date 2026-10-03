/* Stress test for an irreducible CFG. Built with `goto` so two different
 * blocks jump into the *same* two-block cycle from outside it -- a
 * textbook irreducible CFG (no single block dominates the whole cycle, so
 * it has no natural loop header LLVM's LoopInfo can agree on).
 *
 * heimdall-cff does not special-case loop structure at all: it rewrites
 * each block's terminator independently, so an irreducible cycle flattens
 * the same way a normal loop does. This test exists to confirm that in
 * practice, since it's an easy case to get wrong by assumption rather than
 * by inspection.
 */
#include <stdio.h>
#include <stdlib.h>

static int irreducible(int x) {
  if (x > 0)
    goto mid;
  goto tail;

mid:
  x -= 1;
  /* falls through to tail */
tail:
  x += 2;
  if (x < 100 && x > -100)
    goto mid; /* back-edge into the cycle from `tail`, entered from either
               * `mid`'s fallthrough OR this goto -- two distinct ways into
               * the {mid, tail} cycle, no single header dominates it. */
  return x;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <x>\n", argv[0]);
    return 2;
  }
  printf("RESULT=%d\n", irreducible(atoi(argv[1])));
  return 0;
}
