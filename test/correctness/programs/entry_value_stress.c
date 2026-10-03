/* Regression test for a bug found in code review: values computed in the
 * ENTRY block and used by later blocks (extremely common -- "int x =
 * compute(); if (x > 0) ...") must be demoted to a stack slot whose alloca
 * sits at the very TOP of the entry block, not merely "somewhere in" it.
 * An earlier version of heimdall-cff placed the alloca right before the
 * entry block's terminator instead, which could insert a value's store
 * *before* the alloca it stores into in program order -- invalid IR that
 * the pass's own verifier would catch and abort on for almost any
 * non-trivial function.
 *
 * This program is deliberately built so several values are computed in the
 * entry block itself (not a pre-header the compiler might insert) and are
 * then consumed by both earlier-in-text and later-in-text sibling blocks,
 * plus loop back-edges, to exercise that path hard.
 */
#include <stdio.h>
#include <stdlib.h>

static int classify(int a, int b, int c) {
  int sum = a + b + c;        /* computed in entry, used in multiple blocks below */
  int product = a * b;        /* computed in entry, used only in the loop below */
  int result = 0;

  if (sum > 100) {
    result = sum - 100;
  } else if (sum > 50) {
    result = sum - 50;
  } else {
    /* Loop that repeatedly consumes `product`, computed back in entry. */
    for (int i = 0; i < c && i < 8; i++) {
      result += product % (i + 1);
    }
  }

  /* Use `sum` again, far from its definition, after the diamond above has
   * already merged back together -- exercises demotion across a
   * reconverging CFG region, not just a simple linear chain. */
  if (result > sum) {
    result = sum;
  }

  return result;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <a> <b> <c>\n", argv[0]);
    return 2;
  }
  int a = atoi(argv[1]);
  int b = atoi(argv[2]);
  int c = atoi(argv[3]);
  printf("RESULT=%d\n", classify(a, b, c));
  return 0;
}
