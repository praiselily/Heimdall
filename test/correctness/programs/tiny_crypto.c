/* Sample program: a small block cipher (a reduced-round Feistel network),
 * not a toy XOR loop -- gives flattening real bit-twiddling branchy logic
 * (round function with conditionals) to obscure, and gives the benchmark a
 * concrete "does the AI reconstruct the actual algorithm" test.
 *
 * NOT cryptographically secure. For benchmarking purposes only.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static uint16_t round_function(uint16_t half, uint16_t subkey) {
  uint16_t x = half ^ subkey;
  /* Branchy non-linear step so the round function isn't just XOR. */
  if (x & 0x1) {
    x = (uint16_t)((x << 3) | (x >> 13));
  } else if (x & 0x2) {
    x = (uint16_t)((x >> 2) | (x << 14));
  } else {
    x = (uint16_t)(~x);
  }
  return (uint16_t)(x + 0x9E37);
}

#define NUM_ROUNDS 6

static uint32_t feistel_encrypt(uint32_t block, const uint16_t *subkeys) {
  uint16_t left = (uint16_t)(block >> 16);
  uint16_t right = (uint16_t)(block & 0xFFFF);

  for (int round = 0; round < NUM_ROUNDS; round++) {
    uint16_t new_right = (uint16_t)(left ^ round_function(right, subkeys[round]));
    left = right;
    right = new_right;
  }

  return ((uint32_t)left << 16) | right;
}

static uint32_t feistel_decrypt(uint32_t block, const uint16_t *subkeys) {
  uint16_t left = (uint16_t)(block >> 16);
  uint16_t right = (uint16_t)(block & 0xFFFF);

  for (int round = NUM_ROUNDS - 1; round >= 0; round--) {
    uint16_t new_left = (uint16_t)(right ^ round_function(left, subkeys[round]));
    right = left;
    left = new_left;
  }

  return ((uint32_t)left << 16) | right;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <32-bit-hex-plaintext>\n", argv[0]);
    return 2;
  }

  uint32_t plaintext = (uint32_t)strtoul(argv[1], NULL, 16);
  uint16_t subkeys[NUM_ROUNDS] = {0x1234, 0xABCD, 0x5678,
                                   0xDEAD, 0xBEEF, 0x0F0F};

  uint32_t ciphertext = feistel_encrypt(plaintext, subkeys);
  uint32_t roundtrip = feistel_decrypt(ciphertext, subkeys);

  printf("PLAINTEXT=%08x\n", plaintext);
  printf("CIPHERTEXT=%08x\n", ciphertext);
  printf("ROUNDTRIP=%08x\n", roundtrip);
  printf("ROUNDTRIP_OK=%d\n", roundtrip == plaintext);
  return roundtrip == plaintext ? 0 : 1;
}
