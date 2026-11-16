#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <openssl/evp.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
struct work_struct { bool queued; };
struct net_device { u8 dev_addr[6]; void *ieee80211_ptr; };
struct sk_buff { u8 *data; unsigned int len; };
struct ieee80211_hdr { u8 data[24]; };
struct {
    struct net_device *dev;
    u8 bssid[6], saq_frame[320], dd_frame[96];
    unsigned int saq_len, dd_len, freq;
    bool armed, pmf_active, tk_set, saq_busy, dd_busy, saq_pending;
    u8 tk_key_idx;
    u16 saq_tid;
    int dd_kind, tx_pn;
    struct work_struct saq_work, dd_work, saq_chk;
} wl_sae;
static int wl_key_epoch, wl_bip_lock, wl_re_fast_lock, wl_saq_lock;
static int re_pmf_ba = 2, re_verbose;
static unsigned int re_ba_rxclr;
static u64 wl_mgmt_pn;
static bool wl_re_fast_ok = true;
static void *wl_re_fast_tfm;
static int wl_bip_keys[2];
static unsigned int passed, dropped, disconnected, responses;
static u16 disconnect_reason;
static u8 last_pass[128], last_response[44];
static const u8 tk[16] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
#define READ_ONCE(x) (x)
#define GFP_ATOMIC 0
#define WLAN_STATUS_REQUEST_DECLINED 37
#define WLAN_REASON_UNSPECIFIED 1
#define atomic_read(x) (*(x))
#define atomic64_fetch_inc(x) ((*(x))++)
#define spin_lock_irqsave(lock, flags) ((void)(lock), (flags) = 0)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define mutex_lock(lock) ((void)(lock))
#define mutex_unlock(lock) ((void)(lock))
static void re_log(const char *fmt, ...) { (void)fmt; }
static void skb_trim(struct sk_buff *skb, unsigned int n) { skb->len = n; }
static unsigned int skb_headlen(struct sk_buff *skb) { return skb->len; }
static void schedule_work(struct work_struct *w) { assert(!w->queued); w->queued = true; }
static void cancel_delayed_work(struct work_struct *w) { w->queued = false; }
static bool _ieee80211_is_robust_mgmt_frame(struct ieee80211_hdr *h)
{
    u8 *f = (u8 *)h;
    return f[0] == 0xc0 || f[0] == 0xa0 || (f[0] == 0xd0 && ((f[1] & 0x40) || f[24] == 3 || f[24] == 8));
}
static bool wl_pmf_verify(int *keys, const u8 *f, size_t len) { (void)keys; (void)f; (void)len; return false; }
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
{ (void)dev; (void)freq; (void)sig; (void)f; (void)len; (void)flags; assert(false); return false; }
static int wl_re_ccm_setup(void) { return 0; }
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
    return ok ? 0 : -1;
}
static int wl_re_ccm(const u8 *nonce, const u8 *aad, unsigned int aadlen, u8 *data, unsigned int len)
{ (void)nonce; (void)aad; (void)aadlen; (void)data; (void)len; responses++; return 0; }
static int wl_re_tx_prot_locked(struct net_device *dev, u8 fc0, const u8 *body, unsigned int len)
{ (void)dev; (void)fc0; (void)body; (void)len; responses++; return 0; }
static void *wl_re_find_scb(struct net_device *dev, const u8 *mac) { (void)dev; (void)mac; return NULL; }
static int wl_re_mgmt_tx(struct net_device *dev, const u8 *f, int len, void *scb)
{ (void)dev; (void)scb; assert(len == 44); memcpy(last_response, f, 44); return 0; }
static void wl_re_saq_start(struct net_device *dev, const char *why) { (void)dev; (void)why; wl_sae.saq_pending = true; }
static void wl_re_assoc_lost(struct net_device *dev, u16 reason, const char *why)
{ (void)dev; (void)why; disconnected++; disconnect_reason = reason; }
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
static void reset(void)
{
    memset(&wl_sae, 0, sizeof(wl_sae));
    memcpy(wl_sae.bssid, ap, 6);
    wl_sae.dev = &dev;
    wl_sae.pmf_active = wl_sae.tk_set = true;
    wl_mgmt_pn = 0; wl_key_epoch = 0; wl_re_fast_ok = true;
    passed = dropped = disconnected = responses = 0;
    u32 offset = 14;
    memcpy(wlc + 0x6bc, &offset, sizeof(offset));
}
int main(void)
{
    const u8 saq_response[] = { 8, 1, 0x34, 0x12 };
    const u8 saq_request[] = { 8, 0, 0x78, 0x56 };
    const u8 ba[] = { 3, 2, 0, 0, 0, 0 };
    const u8 teardown[] = { 3, 0 };
    reset();
    wl_sae.saq_pending = true; wl_sae.saq_tid = 0x1234;
    receive(0xd0, 10, saq_response, sizeof(saq_response), false);
    assert(wl_sae.saq_work.queued && wl_mgmt_pn == 10 && !passed);
    receive(0xd0, 11, ba, sizeof(ba), false);
    assert(passed == 1 && wl_mgmt_pn == 11);
    assert(last_pass[20] == 0xd0 && !(last_pass[21] & 0x40));
    assert(!memcmp(last_pass + 44, ba, sizeof(ba)) && last_pass[50] == 0x5a);
    wl_sae.saq_work.queued = false; wl_re_saq_work(&wl_sae.saq_work);
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
    puts("RX: CCMP authentication/replay before deferred dispatch, SA-Query, both teardown subtypes, Block Ack and WPA2 pass-through passed");
    return 0;
}
