#ifndef WL_PMF_H
#define WL_PMF_H

struct wl_pmf_key {
	struct aes_cmac_key cmac;
	u8 raw[16];
	u64 ipn;
	bool valid;
};

static int wl_pmf_frame_offset(const u8 *rx, size_t len, unsigned int offset)
{
	unsigned int skip;
	if (len < 20 || offset > 4096 || offset > len)
		return -EINVAL;
	skip = 6 + ((rx[0x10] & 4) ? 2 : 0);
	if (len - offset < skip + 4)
		return -EINVAL;
	return (int)(offset + skip);
}

static bool wl_pmf_replay_accept(u64 *last, u64 pn)
{
	if (!pn || pn > 0xffffffffffffULL || pn <= *last)
		return false;
	*last = pn;
	return true;
}

static u64 wl_pmf_ipn(const u8 *p)
{
	u64 n = 0;
	int i;
	for (i = 5; i >= 0; i--)
		n = (n << 8) | p[i];
	return n;
}

/* Caller serializes key installation, verification and deletion. */
static int wl_pmf_install(struct wl_pmf_key keys[2], unsigned int id,
			  const u8 *raw, size_t len, const u8 *seq, size_t seq_len)
{
	struct wl_pmf_key *key;
	u64 ipn;
	if (id < 4 || id > 5 || !raw || len != 16 ||
	    (seq_len && (!seq || seq_len != 6)))
		return -EINVAL;
	key = &keys[id - 4];
	ipn = seq_len ? wl_pmf_ipn(seq) : 0;
	if (key->valid && !crypto_memneq(key->raw, raw, 16)) {
		if (ipn > key->ipn)
			key->ipn = ipn;
		return 0;
	}
	if (aes_cmac_preparekey(&key->cmac, raw, len))
		return -EINVAL;
	memcpy(key->raw, raw, 16);
	key->ipn = ipn;
	key->valid = true;
	return 0;
}

static bool wl_pmf_verify(struct wl_pmf_key keys[2], const u8 *f, size_t len)
{
	struct aes_cmac_ctx ctx;
	struct wl_pmf_key *key;
	const u8 *mmie;
	u8 aad[20], mic[16];
	static const u8 zero[8];
	unsigned int id;
	u64 ipn;
	if (len < 24 + 2 + 18 || len > 4096 || !(f[4] & 1) ||
	    (f[1] & 0xc7) || (f[22] & 0x0f) ||
	    (f[0] != 0xc0 && f[0] != 0xa0 && f[0] != 0xd0))
		return false;
	mmie = f + len - 18;
	if (mmie[0] != 76 || mmie[1] != 16)
		return false;
	id = mmie[2] | ((unsigned int)mmie[3] << 8);
	if (id < 4 || id > 5)
		return false;
	key = &keys[id - 4];
	ipn = wl_pmf_ipn(mmie + 4);
	if (!key->valid || ipn <= key->ipn)
		return false;
	aad[0] = f[0];
	aad[1] = f[1] & ~0x38;
	memcpy(aad + 2, f + 4, 18);
	aes_cmac_init(&ctx, &key->cmac);
	aes_cmac_update(&ctx, aad, sizeof(aad));
	aes_cmac_update(&ctx, f + 24, len - 24 - 8);
	aes_cmac_update(&ctx, zero, sizeof(zero));
	aes_cmac_final(&ctx, mic);
	if (crypto_memneq(mic, f + len - 8, 8))
		return false;
	key->ipn = ipn;
	return true;
}

#endif
