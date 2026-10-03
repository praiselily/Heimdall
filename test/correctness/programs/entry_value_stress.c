/* Regression test for entry-block value demotion: values computed in the
 * ENTRY block and used by later blocks ("int x = compute(); if (x > 0)
 * ...", one of the most common shapes in real code) must be demoted to a
 * stack slot whose alloca sits at the top of the entry block, not just
 * somewhere inside it. An alloca placed later than a store that feeds it
 * produces invalid IR, which the pass's verifier should catch immediately
 * if this regresses.
 *
 * This program is built so several values are computed in the entry block
 * itself (not a pre-header the compiler might insert) and are consumed by
 * both earlier-in-text and later-in-text sibling blocks, plus loop
 * back-edges, to exercise that path hard.
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
