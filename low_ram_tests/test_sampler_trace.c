/*
 * test_sampler_trace: differential validation harness for the
 * discrete-Gaussian sampler.
 *
 * Compile both this binary AND the sign_sampler.o it links against with
 * -DFNDSA_SAMPLER_TRACE. At runtime, every sampler_next entry dumps
 * (logn, mu_bits, isigma_bits) to a file; the returned z is appended
 * after the rejection loop accepts.
 *
 * Build twice:
 *   - baseline   (no FNDSA_LOW_RAM):     ./test_sampler_trace
 *   - low-RAM    (-DFNDSA_LOW_RAM=1):    ./test_sampler_trace_lowram
 *
 * Run both with the same args, then diff the trace files. If the two
 * traces are byte-identical, both builds drive the sampler with
 * identical IEEE-754 inputs (and therefore produce identical signatures
 * for any seeded RNG path) — i.e. sampler-bit-exact.
 *
 * Usage:
 *   ./test_sampler_trace <logn> <hex_seed12> <out_file>
 *
 *   logn       9 (FN-DSA-512) or 10 (FN-DSA-1024). Lower logns also work
 *              with weak signing.
 *   hex_seed12 12 hex chars (6 bytes). The seed is fed to SHAKE256 to
 *              derive a 32-byte keygen seed; signing then uses the same
 *              6 bytes with byte 0 flipped to 0x01 as a per-signature seed.
 *   out_file   trace destination.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../inner.h"

extern FILE *fndsa_sampler_trace_fp;
extern unsigned long fndsa_sampler_trace_idx;

static int
hex2byte(const char *p, uint8_t *out)
{
	uint8_t b = 0;
	for (int i = 0; i < 2; i ++) {
		char c = p[i];
		uint8_t v;
		if (c >= '0' && c <= '9') {
			v = c - '0';
		} else if (c >= 'a' && c <= 'f') {
			v = 10 + c - 'a';
		} else if (c >= 'A' && c <= 'F') {
			v = 10 + c - 'A';
		} else {
			return -1;
		}
		b = (b << 4) | v;
	}
	*out = b;
	return 0;
}

int
main(int argc, char *argv[])
{
	if (argc != 4) {
		fprintf(stderr,
			"usage: %s <logn> <hex_seed12> <out_file>\n", argv[0]);
		return EXIT_FAILURE;
	}
	unsigned logn = (unsigned)strtoul(argv[1], NULL, 10);
	if (logn < 2 || logn > 10) {
		fprintf(stderr, "logn out of range: %u\n", logn);
		return EXIT_FAILURE;
	}
	if (strlen(argv[2]) != 12) {
		fprintf(stderr, "seed must be 12 hex chars (6 bytes)\n");
		return EXIT_FAILURE;
	}
	uint8_t seed[6];
	for (int i = 0; i < 6; i ++) {
		if (hex2byte(argv[2] + 2 * i, &seed[i]) != 0) {
			fprintf(stderr, "bad hex seed\n");
			return EXIT_FAILURE;
		}
	}

	FILE *out = fopen(argv[3], "w");
	if (out == NULL) {
		perror("fopen");
		return EXIT_FAILURE;
	}

	fprintf(out, "# logn=%u seed=%02x%02x%02x%02x%02x%02x build=%s\n",
		logn,
		seed[0], seed[1], seed[2], seed[3], seed[4], seed[5],
#if FNDSA_LOW_RAM
		"FNDSA_LOW_RAM"
#else
		"baseline"
#endif
		);

	size_t sk_len = FNDSA_SIGN_KEY_SIZE(logn);
	size_t vk_len = FNDSA_VRFY_KEY_SIZE(logn);
	size_t sig_len = FNDSA_SIGNATURE_SIZE(logn);
	uint8_t *sk = malloc(sk_len);
	uint8_t *vk = malloc(vk_len);
	uint8_t *sig = malloc(sig_len);
	if (sk == NULL || vk == NULL || sig == NULL) {
		perror("malloc");
		return EXIT_FAILURE;
	}

	size_t kgentmp_len = ((size_t)26 << logn) + 31;
	size_t signtmp_len = ((size_t)59 << logn) + 31;
	size_t tmp_len = signtmp_len > kgentmp_len ? signtmp_len : kgentmp_len;
	void *tmp = malloc(tmp_len);
	if (tmp == NULL) {
		perror("malloc tmp");
		return EXIT_FAILURE;
	}

	shake_context pc;
	uint8_t kseed[32];
	shake_init(&pc, 256);
	shake_inject(&pc, seed, 6);
	shake_flip(&pc);
	shake_extract(&pc, kseed, 32);

	if (!fndsa_keygen_seeded_temp(logn, kseed, 32,
		sk, vk, tmp, kgentmp_len))
	{
		fprintf(stderr, "keygen failed\n");
		return EXIT_FAILURE;
	}

	uint8_t sign_seed[6];
	memcpy(sign_seed, seed, 6);
	sign_seed[0] = 0x01;

	const char *raw_msg = "trace-validation";
	const void *msg = raw_msg;
	size_t msg_len = strlen(raw_msg);
	const char *id = FNDSA_HASH_ID_RAW;

	/* Engage the trace hook now: keygen has been completed without
	   tracing, so the trace contains only sampler calls from signing. */
	fndsa_sampler_trace_fp = out;
	fndsa_sampler_trace_idx = 0;

	size_t r;
	if (logn <= 8) {
		r = fndsa_sign_weak_seeded_temp(sk, sk_len,
			"domain", 6, id, msg, msg_len,
			sign_seed, sizeof sign_seed, sig, sig_len,
			tmp, signtmp_len);
	} else {
		r = fndsa_sign_seeded_temp(sk, sk_len,
			"domain", 6, id, msg, msg_len,
			sign_seed, sizeof sign_seed, sig, sig_len,
			tmp, signtmp_len);
	}
	if (r != sig_len) {
		fprintf(stderr, "sign failed (returned %zu, expected %zu)\n",
			r, sig_len);
		return EXIT_FAILURE;
	}

	fndsa_sampler_trace_fp = NULL;

	fprintf(out, "# sampler_calls=%lu sig_len=%zu\n",
		fndsa_sampler_trace_idx / 2, sig_len);
	fprintf(out, "# sig_full=");
	for (size_t i = 0; i < sig_len; i ++) {
		fprintf(out, "%02x", sig[i]);
	}
	fprintf(out, "\n");

	fclose(out);
	free(sk);
	free(vk);
	free(sig);
	free(tmp);
	return EXIT_SUCCESS;
}
