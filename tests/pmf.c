#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/core_names.h>

typedef uint8_t u8;
typedef uint64_t u64;
struct aes_cmac_key { u8 raw[16]; };
struct aes_cmac_ctx { EVP_MAC_CTX *ctx; };
static int crypto_memneq(const void *a, const void *b, size_t n)
{
	return CRYPTO_memcmp(a, b, n);
}
static int aes_cmac_preparekey(struct aes_cmac_key *k, const u8 *raw, size_t n)
{
	if (n != 16) return -EINVAL;
	memcpy(k->raw, raw, n);
	return 0;
}
static void aes_cmac_init(struct aes_cmac_ctx *c, const struct aes_cmac_key *k)
{
	EVP_MAC *mac = EVP_MAC_fetch(NULL, "CMAC", NULL);
	OSSL_PARAM params[] = {
		OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, "AES-128-CBC", 0),
		OSSL_PARAM_construct_end()
	};
	assert(mac);
	c->ctx = EVP_MAC_CTX_new(mac);
	EVP_MAC_free(mac);
	assert(c->ctx && EVP_MAC_init(c->ctx, k->raw, 16, params));
}
static void aes_cmac_update(struct aes_cmac_ctx *c, const u8 *p, size_t n)
{
	assert(EVP_MAC_update(c->ctx, p, n));
}
static void aes_cmac_final(struct aes_cmac_ctx *c, u8 *out)
{
	size_t n;
	assert(EVP_MAC_final(c->ctx, out, &n, 16) && n == 16);
	EVP_MAC_CTX_free(c->ctx);
}
#include "../wl-src/src/wl/sys/wl_pmf.h"

static const u8 key[16] = { 0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
	0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c };

static void sign_frame(u8 f[44], unsigned int id, u64 ipn)
{
	u8 message[40], tag[16];
	size_t n;
	int i;
	memset(f, 0, 44);
	f[0] = 0xc0;
	memset(f + 4, 0xff, 6);
	memcpy(f + 10, "\x02\x11\x22\x33\x44\x55", 6);
	memcpy(f + 16, f + 10, 6);
	f[24] = 3;
	f[26] = 76; f[27] = 16; f[28] = id;
	for (i = 0; i < 6; i++) f[30 + i] = (u8)(ipn >> (i * 8));
	message[0] = f[0]; message[1] = f[1] & ~0x38;
	memcpy(message + 2, f + 4, 18);
	memcpy(message + 20, f + 24, 20);
	assert(EVP_Q_mac(NULL, "CMAC", NULL, "AES-128-CBC", NULL, key, 16,
		message, sizeof(message), tag, sizeof(tag), &n) && n == 16);
	memcpy(f + 36, tag, 8);
}

static void fresh(struct wl_pmf_key keys[2])
{
	memset(keys, 0, sizeof(*keys) * 2);
	assert(wl_pmf_install(keys, 4, key, 16, NULL, 0) == 0);
}

int main(void)
{
	struct wl_pmf_key keys[2], before[2];
	struct aes_cmac_key k;
	struct aes_cmac_ctx c;
	u8 f[44], changed[44], tag[16], seq[6] = { 5 }, other[16] = { 7 };
	const u8 rfc4493_empty[16] = { 0xbb,0x1d,0x69,0x29,0xe9,0x59,0x37,0x28,
		0x7f,0xa3,0x7d,0x12,0x9b,0x75,0x67,0x46 };
	size_t i, bit;
	u64 last = 0;
	u8 rx[128] = { 0 };
	assert(wl_pmf_frame_offset(rx, sizeof(rx), 38) == 44);
	rx[16] = 4;
	assert(wl_pmf_frame_offset(rx, sizeof(rx), 38) == 46);
	assert(wl_pmf_frame_offset(rx, 19, 0) == -EINVAL);
	assert(wl_pmf_frame_offset(rx, 45, 38) == -EINVAL);
	assert(wl_pmf_frame_offset(rx, sizeof(rx), 0xffffffffu) == -EINVAL);
	assert(!wl_pmf_replay_accept(&last, 0));
	assert(wl_pmf_replay_accept(&last, 1));
	assert(!wl_pmf_replay_accept(&last, 1));
	assert(wl_pmf_replay_accept(&last, 3));
	assert(!wl_pmf_replay_accept(&last, 2));
	assert(!wl_pmf_replay_accept(&last, 0x1000000000000ULL));
	assert(!aes_cmac_preparekey(&k, key, 16));
	aes_cmac_init(&c, &k); aes_cmac_final(&c, tag);
	assert(!memcmp(tag, rfc4493_empty, 16));
	fresh(keys); sign_frame(f, 4, 1);
	assert(wl_pmf_verify(keys, f, sizeof(f)));
	assert(!wl_pmf_verify(keys, f, sizeof(f)));
	assert(!wl_pmf_install(keys, 4, key, 16, NULL, 0));
	assert(!wl_pmf_verify(keys, f, sizeof(f)));
	assert(!wl_pmf_install(keys, 5, key, 16, seq, 6));
	sign_frame(f, 5, 5); assert(!wl_pmf_verify(keys, f, sizeof(f)));
	sign_frame(f, 5, 6); assert(wl_pmf_verify(keys, f, sizeof(f)));
	sign_frame(f, 4, 2); assert(wl_pmf_verify(keys, f, sizeof(f)));
	assert(!wl_pmf_install(keys, 4, other, 16, NULL, 0));
	sign_frame(f, 4, 3); assert(!wl_pmf_verify(keys, f, sizeof(f)));
	for (i = 0; i < 44; i++) {
		if (i == 2 || i == 3 || i == 22 || i == 23) continue;
		for (bit = 0; bit < 8; bit++) {
			if (i == 1 && bit >= 3 && bit <= 5) continue;
			fresh(keys); sign_frame(f, 4, 1); memcpy(changed, f, 44);
			changed[i] ^= (u8)(1u << bit);
			assert(!wl_pmf_verify(keys, changed, 44));
			assert(wl_pmf_verify(keys, f, 44));
		}
	}
	for (i = 0; i < 44; i++) {
		fresh(keys); sign_frame(f, 4, 1);
		assert(!wl_pmf_verify(keys, f, i));
	}
	fresh(keys); sign_frame(f, 4, 1);
	f[1] |= 0x38; f[2] = 99; f[22] = 0x10;
	assert(wl_pmf_verify(keys, f, 44));
	fresh(keys); sign_frame(f, 4, 0xffffffffffffULL);
	assert(wl_pmf_verify(keys, f, 44));
	sign_frame(f, 4, 0); assert(!wl_pmf_verify(keys, f, 44));
	fresh(keys); memcpy(before, keys, sizeof(keys));
	assert(wl_pmf_install(keys, 6, key, 16, NULL, 0) == -EINVAL);
	assert(wl_pmf_install(keys, 4, key, 15, NULL, 0) == -EINVAL);
	assert(wl_pmf_install(keys, 4, key, 16, seq, 5) == -EINVAL);
	assert(wl_pmf_install(keys, 4, key, 16, NULL, 6) == -EINVAL);
	assert(!memcmp(before, keys, sizeof(keys)));
	assert(!wl_pmf_verify(keys, f, 4097));
	memset(keys, 0, sizeof(keys)); sign_frame(f, 4, 1);
	assert(!wl_pmf_verify(keys, f, 44));
	puts("PMF: known-answer crypto, integrity mutations, replay, rotation, reinstallation, malformed packets passed");
	return 0;
}
