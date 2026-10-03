/* Sample program: a toy license-key validator.
 *
 * Deliberately has nested conditionals, a loop, and early returns, so
 * flattening has real branchy structure to work on and a decompiler has
 * real logic to try to reconstruct.
 */
#include <stdio.h>
#include <string.h>

/* Very small checksum: sum of digit values mod 97, must equal the last two
 * characters (as a two-digit number) for the key to be considered valid.
 * Format expected: "XXXX-XXXX-CC" where X are digits and CC is the
 * checksum, zero-padded.
 */
static int compute_checksum(const char *key, int len) {
  int sum = 0;
  for (int i = 0; i < len; i++) {
    char c = key[i];
    if (c >= '0' && c <= '9') {
      sum += (c - '0');
    } else if (c == '-') {
      continue;
    } else {
      return -1; /* invalid character */
    }
  }
  return sum % 97;
}

int validate_license_key(const char *key) {
  if (key == NULL)
    return 0;

  size_t len = strlen(key);
  if (len != 14)
    return 0;

  if (key[4] != '-' || key[9] != '-')
    return 0;

  int checksum = compute_checksum(key, 9); /* digits + first dash only */
  if (checksum < 0)
    return 0;

  int provided = (key[12] - '0') * 10 + (key[13] - '0');
  if (key[12] < '0' || key[12] > '9' || key[13] < '0' || key[13] > '9')
    return 0;

  if (checksum != provided)
    return 0;

  return 1;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <license-key>\n", argv[0]);
    return 2;
  }
  int ok = validate_license_key(argv[1]);
  printf("%s\n", ok ? "VALID" : "INVALID");
  return ok ? 0 : 1;
}
