/* Sample program: a small `key=value;key=value` config-line parser.
 *
 * Exercises loops with multiple exit conditions, a small state machine
 * (looking for key, '=', value, ';'), and nested branches -- representative
 * of real parsing code rather than a toy arithmetic loop.
 */
#include <stdio.h>
#include <string.h>

#define MAX_PAIRS 8
#define MAX_TOKEN 32

typedef struct {
  char key[MAX_TOKEN];
  char value[MAX_TOKEN];
} kv_pair;

/* Returns the number of key=value pairs parsed, or -1 on malformed input.
 * `out` must have room for MAX_PAIRS entries.
 */
int parse_config(const char *input, kv_pair *out) {
  int count = 0;
  size_t i = 0;
  size_t len = strlen(input);

  while (i < len) {
    if (count >= MAX_PAIRS)
      return -1;

    size_t key_start = i;
    while (i < len && input[i] != '=' && input[i] != ';')
      i++;
    size_t key_len = i - key_start;
    if (i >= len || input[i] != '=' || key_len == 0 || key_len >= MAX_TOKEN)
      return -1;

    memcpy(out[count].key, input + key_start, key_len);
    out[count].key[key_len] = '\0';
    i++; /* skip '=' */

    size_t val_start = i;
    while (i < len && input[i] != ';')
      i++;
    size_t val_len = i - val_start;
    if (val_len >= MAX_TOKEN)
      return -1;

    memcpy(out[count].value, input + val_start, val_len);
    out[count].value[val_len] = '\0';
    count++;

    if (i < len && input[i] == ';')
      i++;
    else
      break;
  }

  return count;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <config-string>\n", argv[0]);
    return 2;
  }

  kv_pair pairs[MAX_PAIRS];
  int n = parse_config(argv[1], pairs);
  if (n < 0) {
    printf("PARSE_ERROR\n");
    return 1;
  }

  for (int i = 0; i < n; i++)
    printf("%s=%s\n", pairs[i].key, pairs[i].value);
  printf("COUNT=%d\n", n);
  return 0;
}
