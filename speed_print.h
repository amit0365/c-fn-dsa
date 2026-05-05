/* speed_print.h — adapted from pq-crystals/dilithium's
 * ref/test/speed_print.{c,h} (CC0-1.0 / public domain). Computes median,
 * mean, min, and max of consecutive cpucycles() differences across an
 * NTESTS array, and prints one report line in the canonical Dilithium /
 * Kyber test_speed.c format. */

#ifndef SPEED_PRINT_H
#define SPEED_PRINT_H

#include <stddef.h>
#include <stdint.h>

void print_results(const char *label, uint64_t *t, size_t tlen);

#endif
