#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>
#include <openssl/crypto.h>
#include <openssl/core_names.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
struct work_struct { bool queued; };
struct net_device { u8 dev_addr[6]; void *ieee80211_ptr; };
struct sk_buff { u8 *data; unsigned int len; };
struct ieee80211_hdr { u8 data[24]; };
struct wl_cfg80211_priv { unsigned long status; void *profile; };
typedef struct { u8 ea[6]; u32 val; } scb_val_t;
static struct wl_cfg80211_priv priv;
struct {
    struct net_device *dev;
    u8 bssid[6], saq_frame[320], dd_frame[96];
    unsigned int saq_len, dd_len, freq, dd_reason;
    bool armed, pmf_active, tk_set, saq_busy, dd_busy, saq_pending, sae_ok;
    u8 tk_key_idx;
    u16 saq_tid;
    int dd_kind, tx_pn, saq_tries;
    struct work_struct saq_work, dd_work, saq_chk;
} wl_sae;
static int wl_key_epoch, wl_bip_lock, wl_re_fast_lock, wl_saq_lock;
static int re_pmf_ba = 2, re_verbose;
static unsigned int re_ba_rxclr;
static u64 wl_mgmt_pn;
static bool wl_re_fast_ok = true;
static void *wl_re_fast_tfm;

static unsigned int passed, dropped, disconnected, responses, auth_delivered;
static u16 disconnect_reason;
static u8 last_pass[128], last_response[44];
static const u8 tk[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
#define READ_ONCE(x) (x)
#define WRITE_ONCE(x, value) ((x) = (value))
#define GFP_ATOMIC 0
#define GFP_KERNEL 0
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(7, 2, 0)
#define WL_STATUS_CONNECTED 0
#define ETHER_ADDR_LEN 6
#define WLC_DISASSOC 1
#define htod32(x) (x)
#define WLAN_STATUS_REQUEST_DECLINED 37
#define WLAN_REASON_UNSPECIFIED 1
#define WL_SAQ_RETRY_MS 300
#define WL_SAQ_MAX_TRIES 3
#define msecs_to_jiffies(x) (x)
#define atomic_read(x) (*(x))
#define atomic64_fetch_inc(x) ((*(x))++)
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
static void re_log(const char *fmt, ...) { (void)fmt; }
static void skb_trim(struct sk_buff *skb, unsigned int n) { skb->len = n; }
static unsigned int skb_headlen(struct sk_buff *skb) { return skb->len; }
static void schedule_work(struct work_struct *w) { w->queued = true; }
static void schedule_delayed_work(struct work_struct *w, unsigned int delay) { (void)delay; w->queued = true; }
static void cancel_delayed_work(struct work_struct *w) { w->queued = false; }
static bool _ieee80211_is_robust_mgmt_frame(struct ieee80211_hdr *h)
{
    u8 *f = (u8 *)h;
    u8 subtype = f[0] & 0xfc;
    if (subtype == 0xc0 || subtype == 0xa0) return true;
    if (subtype != 0xd0) return false;
    if (f[1] & 0x40) return true;
    switch (f[24]) {
    case 4: case 7: case 11: case 15: case 20: case 21: case 22: case 127:
        return false;
    default:
        return true;
    }
}
struct aes_cmac_key { u8 raw[16]; };
struct aes_cmac_ctx { EVP_MAC_CTX *ctx; };
static int crypto_memneq(const void *a, const void *b, size_t n) { return CRYPTO_memcmp(a, b, n); }
static int aes_cmac_preparekey(struct aes_cmac_key *key, const u8 *raw, size_t len)
{ if (len != 16) return -EINVAL; memcpy(key->raw, raw, len); return 0; }
static void aes_cmac_init(struct aes_cmac_ctx *ctx, const struct aes_cmac_key *key)
{
    EVP_MAC *mac = EVP_MAC_fetch(NULL, "CMAC", NULL);
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_CIPHER, "AES-128-CBC", 0),
        OSSL_PARAM_construct_end()
    };
    assert(mac);
    ctx->ctx = EVP_MAC_CTX_new(mac);
    EVP_MAC_free(mac);
    assert(ctx->ctx && EVP_MAC_init(ctx->ctx, key->raw, 16, params));
}
static void aes_cmac_update(struct aes_cmac_ctx *ctx, const u8 *data, size_t len)
{ assert(EVP_MAC_update(ctx->ctx, data, len)); }
static void aes_cmac_final(struct aes_cmac_ctx *ctx, u8 *out)
{
    size_t len;
    assert(EVP_MAC_final(ctx->ctx, out, &len, 16) && len == 16);
    EVP_MAC_CTX_free(ctx->ctx);
}
#include "../wl-src/src/wl/sys/wl_pmf.h"
static struct wl_pmf_key wl_bip_keys[2];
static void wl_re_pktfree(struct net_device *dev, void *pkt, bool tx) { (void)dev; (void)pkt; assert(!tx); dropped++; }
static void wlc_recv(void *wlc, void *pkt)
{
    struct sk_buff *skb = pkt;
    (void)wlc;
    assert(skb->len <= sizeof(last_pass));
    memcpy(last_pass, skb->data, skb->len);
    passed++;
}
static bool cfg80211_rx_mgmt(void *dev, unsigned int freq, int sig, const u8 *f, unsigned int len, int flags)
{
    (void)dev; (void)freq; (void)sig; (void)flags;
    assert(len == 46 && f[0] == 0xb0 && f[24] == 3);
    auth_delivered++;
    return true;
}
static void (*during_setup)(void), (*during_probe)(void), (*during_decrypt)(void);
static void inject(void (**hook)(void))
{
    void (*fn)(void) = *hook;
    *hook = NULL;
    if (fn) fn();
}
static int wl_re_ccm_setup(void) { inject(&during_setup); return 0; }
static int wl_re_ccm_with(void *tfm, int gfp, bool enc, const u8 *nonce, const u8 *aad,
                          unsigned int aadlen, u8 *data, unsigned int len)
{
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    u8 out[68];
    int n, ok;
    (void)tfm; (void)gfp;
    assert(ctx && len <= 60 && !enc);
    assert(EVP_DecryptInit_ex(ctx, EVP_aes_128_ccm(), NULL, NULL, NULL));
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN, 13, NULL));
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG, 8, data + len));
    assert(EVP_DecryptInit_ex(ctx, NULL, NULL, tk, nonce));
    assert(EVP_DecryptUpdate(ctx, NULL, &n, NULL, (int)len));
    assert(EVP_DecryptUpdate(ctx, NULL, &n, aad, (int)aadlen));
    ok = EVP_DecryptUpdate(ctx, out, &n, data, (int)len);
    if (ok) memcpy(data, out, len);
    EVP_CIPHER_CTX_free(ctx);
    inject(&during_decrypt);
    return ok ? 0 : -1;
}
static int wl_re_ccm(const u8 *nonce, const u8 *aad, unsigned int aadlen, u8 *data, unsigned int len)
{ (void)nonce; (void)aad; (void)aadlen; (void)data; (void)len; responses++; return 0; }
static int wl_re_tx_prot_locked(struct net_device *dev, u8 fc0, const u8 *body, unsigned int len)
{ (void)dev; (void)fc0; (void)body; (void)len; responses++; return 0; }
static void *wl_re_find_scb(struct net_device *dev, const u8 *mac) { (void)dev; (void)mac; return NULL; }
static int wl_re_mgmt_tx(struct net_device *dev, const u8 *f, int len, void *scb)
{ (void)dev; (void)scb; assert(len == 44); memcpy(last_response, f, 44); return 0; }
static void get_random_bytes(void *out, size_t len) { memset(out, 0x56, len); }
static void wl_re_saq_send(struct net_device *dev)
{ (void)dev; wl_sae.saq_tries++; inject(&during_probe); }
static struct wl_cfg80211_priv *ndev_to_wl(struct net_device *dev) { (void)dev; return &priv; }
static bool test_bit(unsigned int bit, unsigned long *status) { return (*status & (1UL << bit)) != 0; }
static void clear_bit(unsigned int bit, unsigned long *status) { *status &= ~(1UL << bit); }
static void wl_dev_ioctl(struct net_device *dev, int cmd, const scb_val_t *value, size_t len)
{ (void)dev; assert(cmd == WLC_DISASSOC && len == sizeof(*value)); disconnect_reason = value->val; }
static void cfg80211_disconnected(struct net_device *dev, u16 reason, const void *ie, size_t len, bool local, int gfp)
{ (void)dev; (void)ie; (void)len; (void)local; (void)gfp; assert(reason == disconnect_reason); disconnected++; }
static void wl_link_down(struct wl_cfg80211_priv *wl) { (void)wl; }
static void wl_init_prof(void *profile) { (void)profile; }
#include "rx-under-test.h"

static u8 storage[128], wlc[0x6c0];
static struct sk_buff packet = { storage, 0 };
static struct net_device dev = { { 2, 0, 0, 0, 0, 1 }, NULL };
static const u8 ap[6] = { 2, 0, 0, 0, 0, 2 };
static void receive(u8 subtype, u64 pn, const u8 *body, unsigned int len, bool corrupt)
{
    u8 *f = storage + 20;
    u8 aad[22] = { 0 }, nonce[13] = { 0x10 };
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    int n;
    memset(storage, 0, sizeof(storage));
    f[0] = subtype; f[1] = 0x40;
    memcpy(f + 4, dev.dev_addr, 6); memcpy(f + 10, ap, 6); memcpy(f + 16, ap, 6);
    aad[0] = subtype; aad[1] = 0x40; memcpy(aad + 2, f + 4, 18);
    memcpy(nonce + 1, ap, 6);
    for (unsigned int i = 0; i < 6; i++) nonce[12 - i] = (u8)(pn >> (8 * i));
    f[24] = (u8)pn; f[25] = (u8)(pn >> 8); f[27] = 0x20;
    for (unsigned int i = 0; i < 4; i++) f[28 + i] = (u8)(pn >> (16 + 8 * i));
    assert(ctx && len <= 60);
    assert(EVP_EncryptInit_ex(ctx, EVP_aes_128_ccm(), NULL, NULL, NULL));
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_IVLEN, 13, NULL));
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_SET_TAG, 8, NULL));
    assert(EVP_EncryptInit_ex(ctx, NULL, NULL, tk, nonce));
    assert(EVP_EncryptUpdate(ctx, NULL, &n, NULL, (int)len));
    assert(EVP_EncryptUpdate(ctx, NULL, &n, aad, sizeof(aad)));
    assert(EVP_EncryptUpdate(ctx, f + 32, &n, body, (int)len));
    assert(EVP_EncryptFinal_ex(ctx, f + 32 + len, &n));
    assert(EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_CCM_GET_TAG, 8, f + 32 + len));
    EVP_CIPHER_CTX_free(ctx);
    if (corrupt) f[32 + len] ^= 1;
    memset(f + 40 + len, 0x5a, 4);
    packet.len = 20 + 44 + len;
    __wrap_wlc_recv(wlc, &packet);
}
static void receive_group(u8 subtype, u64 pn, u16 reason, bool corrupt)
{
    u8 *f = storage + 20;
    u8 message[40], tag[16];
    size_t n;
    memset(storage, 0, sizeof(storage));
    f[0] = subtype;
    memset(f + 4, 0xff, 6);
    memcpy(f + 10, ap, 6); memcpy(f + 16, ap, 6);
    f[24] = (u8)reason; f[25] = reason >> 8;
    f[26] = 76; f[27] = 16; f[28] = 4;
    for (unsigned int i = 0; i < 6; i++) f[30 + i] = (u8)(pn >> (8 * i));
    message[0] = subtype; message[1] = 0;
    memcpy(message + 2, f + 4, 18);
    memcpy(message + 20, f + 24, 20);
    assert(EVP_Q_mac(NULL, "CMAC", NULL, "AES-128-CBC", NULL, tk, sizeof(tk),
                    message, sizeof(message), tag, sizeof(tag), &n) && n == 16);
    memcpy(f + 36, tag, 8);
    if (corrupt) f[36] ^= 1;
    packet.len = 20 + 44 + 4;
    __wrap_wlc_recv(wlc, &packet);
}
static void reset(void)
{
    memset(&wl_sae, 0, sizeof(wl_sae));
    memcpy(wl_sae.bssid, ap, 6);
    wl_sae.dev = &dev;
    wl_sae.pmf_active = wl_sae.tk_set = true;
    wl_mgmt_pn = 0; wl_key_epoch = 0; wl_re_fast_ok = true;
    passed = dropped = disconnected = responses = auth_delivered = 0;
    dev.ieee80211_ptr = NULL;
    priv.status = 1;
    during_setup = during_probe = during_decrypt = NULL;
    memset(wl_bip_keys, 0, sizeof(wl_bip_keys));
    assert(!wl_pmf_install(wl_bip_keys, 4, tk, sizeof(tk), NULL, 0));
    u32 offset = 14;
    memcpy(wlc + 0x6bc, &offset, sizeof(offset));
}
static const u8 matching_response[] = { 8, 1, 0x34, 0x12 };
static bool burst_group;
static u8 burst_subtype;
static void response_during_worker(void)
{
    receive(0xd0, 11, matching_response, sizeof(matching_response), false);
    assert(!wl_sae.saq_pending && wl_sae.saq_busy);
}
static void teardown_during_probe(void)
{
    const u8 body[] = { 3, 0 };
    if (burst_group) receive_group(burst_subtype, 10, 3, false);
    else receive(burst_subtype, 10, body, sizeof(body), false);
}
static void stop_session(void) { wl_re_pmf_stop(); }
static void replace_key(void) { wl_key_epoch += 2; }
static void drain_teardown(void)
{
    assert(wl_sae.dd_work.queued);
    wl_sae.dd_work.queued = false;
    wl_re_dd_work(&wl_sae.dd_work);
}
static void bursts(void)
{
    const u8 request[] = { 8, 0, 0x78, 0x56 };
    const u8 unrelated[] = { 8, 1, 0xcd, 0xab };
    const u8 teardown[] = { 3, 0 }, later_teardown[] = { 4, 0 };
    reset();
    wl_sae.saq_pending = true; wl_sae.saq_tid = 0x1234;
    wl_sae.saq_chk.queued = true; wl_sae.saq_tries = WL_SAQ_MAX_TRIES;
    receive(0xd0, 10, request, sizeof(request), false);
    assert(wl_sae.saq_work.queued && wl_sae.saq_busy);
    receive(0xd0, 11, unrelated, sizeof(unrelated), false);
    assert(wl_sae.saq_pending);
    receive(0xd0, 12, matching_response, sizeof(matching_response), false);
    assert(!wl_sae.saq_pending && !wl_sae.saq_chk.queued && wl_sae.saq_busy);
    wl_re_saq_chk_work(&wl_sae.saq_chk);
    assert(!disconnected);
    wl_sae.saq_work.queued = false; wl_re_saq_work(&wl_sae.saq_work);
    assert(responses == 1 && last_response[34] == 0x78 && last_response[35] == 0x56);

    reset();
    wl_sae.saq_pending = true; wl_sae.saq_tid = 0x1234;
    receive(0xd0, 10, request, sizeof(request), false);
    during_setup = response_during_worker;
    wl_sae.saq_work.queued = false; wl_re_saq_work(&wl_sae.saq_work);
    assert(!wl_sae.saq_pending && !wl_sae.saq_work.queued && !wl_sae.saq_busy && responses == 1);

    for (unsigned int group = 0; group < 2; group++) {
        for (unsigned int subtype = 0; subtype < 2; subtype++) {
            burst_group = group; burst_subtype = subtype ? 0xa0 : 0xc0;
            for (unsigned int kind = 0; kind < 3; kind++) {
                reset();
                wl_sae.dd_kind = (int)kind; wl_sae.dd_busy = true;
                wl_sae.dd_len = 26; wl_sae.dd_frame[24] = 6;
                schedule_work(&wl_sae.dd_work);
                teardown_during_probe();
                if (group) receive_group(burst_subtype, 11, 4, false);
                else receive(burst_subtype, 11, later_teardown, sizeof(later_teardown), false);
                drain_teardown();
                assert(disconnected == 1 && disconnect_reason == 3 && !wl_sae.dd_busy);
            }
            reset();
            wl_sae.dd_kind = 1; wl_sae.dd_busy = true;
            schedule_work(&wl_sae.dd_work);
            during_probe = teardown_during_probe;
            drain_teardown();
            assert(!disconnected && wl_sae.dd_work.queued);
            wl_sae.saq_tries = WL_SAQ_MAX_TRIES;
            wl_re_saq_chk_work(&wl_sae.saq_chk);
            assert(!disconnected);
            drain_teardown();
            assert(disconnected == 1 && disconnect_reason == 3);
        }
    }
    for (unsigned int group = 0; group < 2; group++) {
        for (unsigned int maximum = 0; maximum < 2; maximum++) {
            u16 reason = maximum ? 65535 : 0;
            u8 body[] = { (u8)reason, reason >> 8 };
            reset();
            if (group) receive_group(0xc0, 1, reason, false);
            else receive(0xc0, 1, body, sizeof(body), false);
            drain_teardown();
            assert(disconnected == 1 && disconnect_reason == reason);
        }
    }
    reset();
    wl_sae.dd_kind = 1; wl_sae.dd_busy = true;
    schedule_work(&wl_sae.dd_work);
    receive_group(0xc0, 20, 3, true);
    receive(0xc0, 20, teardown, sizeof(teardown), true);
    assert(!wl_sae.dd_reason);
    drain_teardown();
    assert(!disconnected && wl_sae.saq_pending);

    reset();
    wl_sae.saq_pending = true; wl_sae.saq_tid = 0x1234;
    during_decrypt = replace_key;
    receive(0xd0, 10, matching_response, sizeof(matching_response), false);
    assert(wl_sae.saq_pending && wl_mgmt_pn == 0);
    reset();
    during_decrypt = stop_session;
    receive(0xc0, 10, teardown, sizeof(teardown), false);
    assert(!wl_sae.dd_reason && !wl_sae.dd_work.queued);
    reset();
    receive(0xd0, 10, request, sizeof(request), false);
    wl_re_pmf_stop();
    wl_sae.saq_work.queued = false; wl_re_saq_work(&wl_sae.saq_work);
    assert(!responses && !wl_sae.saq_busy);
    reset();
    teardown_during_probe();
    wl_re_pmf_stop();
    drain_teardown();
    assert(!disconnected);
}
static void frame_versions(void)
{
    const u8 subtypes[] = { 0xc0, 0xa0, 0xd0, 0xb0 };
    unsigned int failures = 0;
    for (unsigned int i = 0; i < sizeof(subtypes); i++) {
        for (unsigned int version = 0; version < 4; version++) {
            reset();
            wl_sae.armed = subtypes[i] == 0xb0;
            wl_sae.pmf_active = !wl_sae.armed;
            dev.ieee80211_ptr = &dev;
            memset(storage, 0, sizeof(storage));
            u8 *f = storage + 20;
            f[0] = subtypes[i] | version;
            memcpy(f + 4, dev.dev_addr, 6);
            memcpy(f + 10, ap, 6); memcpy(f + 16, ap, 6);
            f[24] = 3; f[26] = 1;
            packet.len = 70;
            __wrap_wlc_recv(wlc, &packet);
            unsigned int expected_auth = subtypes[i] == 0xb0 && version == 0;
            printf("Frame 0x%02x version %u: blob=%u drop=%u auth=%u\n",
                   subtypes[i], version, passed, dropped, auth_delivered);
            failures += passed != 0 || dropped != 1 || auth_delivered != expected_auth;
            assert(!disconnected && !wl_sae.dd_work.queued && wl_mgmt_pn == 0);
        }
    }
    fflush(stdout);
    assert(!failures);
}
int main(void)
{
    frame_versions();
    bursts();
    const u8 saq_response[] = { 8, 1, 0x34, 0x12 };
    const u8 saq_request[] = { 8, 0, 0x78, 0x56 };
    const u8 ba[] = { 3, 2, 0, 0, 0, 0 };
    const u8 teardown[] = { 3, 0 };
    reset();
    wl_sae.saq_pending = true; wl_sae.saq_tid = 0x1234;
    receive(0xd0, 10, saq_response, sizeof(saq_response), false);
    assert(!wl_sae.saq_work.queued && wl_mgmt_pn == 10 && !passed && !wl_sae.saq_pending);
    receive(0xd0, 11, ba, sizeof(ba), false);
    assert(passed == 1 && wl_mgmt_pn == 11);
    assert(last_pass[20] == 0xd0 && !(last_pass[21] & 0x40));
    assert(!memcmp(last_pass + 44, ba, sizeof(ba)) && last_pass[50] == 0x5a);
    assert(!wl_sae.saq_pending && !wl_sae.saq_busy && wl_mgmt_pn == 11);
    receive(0xd0, 10, saq_response, sizeof(saq_response), false);
    assert(!wl_sae.saq_work.queued && !wl_sae.saq_pending);
    for (unsigned int i = 0; i < 2; i++) {
        reset();
        receive(i ? 0xa0 : 0xc0, 10, teardown, sizeof(teardown), false);
        receive(0xd0, 11, ba, sizeof(ba), false);
        assert(wl_sae.dd_work.queued && passed == 1);
        wl_sae.dd_work.queued = false; wl_re_dd_work(&wl_sae.dd_work);
        assert(disconnected == 1 && disconnect_reason == 3 && !wl_sae.dd_busy);
    }
    reset();
    receive(0xd0, 20, saq_request, sizeof(saq_request), true);
    assert(!wl_sae.saq_work.queued && wl_mgmt_pn == 0 && !passed);
    receive(0xd0, 10, saq_request, sizeof(saq_request), false);
    assert(wl_sae.saq_work.queued);
    wl_sae.saq_work.queued = false; wl_re_saq_work(&wl_sae.saq_work);
    assert(responses == 1 && last_response[32] == 8 && last_response[33] == 1);
    receive(0xd0, 10, saq_request, sizeof(saq_request), false);
    assert(!wl_sae.saq_work.queued && responses == 1 && wl_mgmt_pn == 10);
    receive(0xd0, 11, ba, sizeof(ba), false);
    receive(0xd0, 11, ba, sizeof(ba), false);
    assert(passed == 1 && wl_mgmt_pn == 11);
    reset(); wl_key_epoch = 1;
    receive(0xd0, 10, saq_response, sizeof(saq_response), false);
    assert(wl_mgmt_pn == 0 && !wl_sae.saq_work.queued);
    reset(); wl_re_fast_ok = false;
    receive(0xc0, 10, teardown, sizeof(teardown), false);
    assert(wl_mgmt_pn == 0 && !wl_sae.dd_work.queued);
    reset(); wl_sae.pmf_active = false;
    receive(0xd0, 1, ba, sizeof(ba), false);
    assert(passed == 1 && wl_mgmt_pn == 0);
    const u16 statuses[] = { 0, 76, 77, 126, 127 };
    unsigned int failures = 0;
    for (unsigned int i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        reset();
        wl_sae.armed = true; wl_sae.pmf_active = false;
        dev.ieee80211_ptr = &dev;
        memset(storage, 0, sizeof(storage));
        u8 *f = storage + 20;
        f[0] = 0xb0;
        memcpy(f + 4, dev.dev_addr, 6);
        memcpy(f + 10, ap, 6); memcpy(f + 16, ap, 6);
        f[24] = 3; f[26] = 1;
        f[28] = (u8)statuses[i]; f[29] = statuses[i] >> 8;
        packet.len = 70;
        __wrap_wlc_recv(wlc, &packet);
        printf("SAE status %u: delivered=%u expected=1\n", statuses[i], auth_delivered);
        failures += auth_delivered != 1 || passed != 0 || dropped != 1;
    }
    if (failures) return 1;
    puts("RX: authenticated burst effects, queued/running workers, group/unicast teardown, session/key boundaries, replay and WPA2 pass-through passed");
    return 0;
}
