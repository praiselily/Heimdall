/* Stress test: nested loops + a switch inside the inner loop, so the
 * flattened dispatcher has to juggle many blocks and several independent
 * "resume points" feeding back into it (back-edges from both the inner and
 * outer loop, plus a switch's multiple successors). Representative of real
 * state-machine-shaped code (parsers, VM interpreters, protocol handlers)
 * rather than a single simple loop.
 */
#include <stdio.h>
#include <stdlib.h>

static long run_machine(int rows, int cols, int seed) {
  long acc = 0;
  int state = seed % 4;

  for (int r = 0; r < rows; r++) {
    for (int c = 0; c < cols; c++) {
      switch (state) {
      case 0:
        acc += r + c;
        state = (c % 2 == 0) ? 1 : 2;
        break;
      case 1:
        acc -= (r * c) % 7;
        state = 3;
        break;
      case 2:
        acc += (r ^ c);
        if (acc % 5 == 0)
          state = 0;
        else
          state = 1;
        break;
      case 3:
      default:
        acc *= 1; /* no-op, but keeps a real default case in the switch */
        state = (r + c) % 4;
        break;
      }
    }
  }

  return acc;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s <rows> <cols> <seed>\n", argv[0]);
    return 2;
  }
  int rows = atoi(argv[1]);
  int cols = atoi(argv[2]);
  int seed = atoi(argv[3]);
  printf("ACC=%ld\n", run_machine(rows, cols, seed));
  return 0;
}
