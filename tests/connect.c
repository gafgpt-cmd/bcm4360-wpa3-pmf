#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
struct ether_addr { u8 octet[6]; };
struct net_device { int unused; };
struct delayed_work { bool queued; };
struct cfg80211_bss { int unused; };
struct wl_cfg80211_connect_info { u8 *req_ie, *resp_ie; u32 req_ie_len, resp_ie_len; };
struct wl_cfg80211_profile { struct { u8 SSID[32]; } ssid; u8 bssid[6]; };
struct wl_cfg80211_priv {
    void *scan_request;
    unsigned long status;
    struct ether_addr bssid;
    struct wl_cfg80211_profile *profile;
    struct wl_cfg80211_connect_info conn_info;
};
typedef struct { u32 event_type, status; struct ether_addr addr; } wl_event_msg_t;
static struct {
    unsigned int join_len, retries, assoc_status;
    bool sae_conn, sae_ok, ptk_done;
    struct delayed_work assoc_wd, retry_work, hs_wd;
} wl_sae;
static int re_verbose;

#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(7, 2, 0)
#define WL_STATUS_CONNECTING 1
#define WL_STATUS_CONNECTED 2
#define WLC_E_SET_SSID 0
#define WLC_E_LINK 16
#define WLC_E_STATUS_SUCCESS 0
#define WLC_E_STATUS_FAIL 1
#define WL_SAE_MAX_RETRIES 2
#define WL_SAE_RETRY_DELAY_MS 100
#define WL_HS_TIMEOUT_MS 1000
#define WLC_GET_RATE 12
#define WLAN_STATUS_SUCCESS 0
#define WLAN_STATUS_AUTH_TIMEOUT 16
#define GFP_KERNEL 0
#define ETHER_ADDR_LEN 6
#define KERN_ERR ""
#define DUMP_PREFIX_OFFSET 0
#define dtoh32(x) (x)
#define EVENT_TYPE(e) dtoh32((e)->event_type)
#define EVENT_STATUS(e) dtoh32((e)->status)
#define WL_DBG(args)
#define wl_to_conn(w) (&(w)->conn_info)
#define wl_to_wiphy(w) ((void *)(w))
#define msecs_to_jiffies(x) (x)
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define test_bit(bit, addr) ((*(addr) >> (bit)) & 1)
#define set_bit(bit, addr) (*(addr) |= 1UL << (bit))
static bool test_and_clear_bit(int bit, unsigned long *addr)
{ bool was = test_bit(bit, addr); *addr &= ~(1UL << bit); return was; }
static void re_log(const char *fmt, ...) { (void)fmt; }
static void cancel_delayed_work(struct delayed_work *w) { w->queued = false; }
static void schedule_delayed_work(struct delayed_work *w, unsigned int ms) { (void)ms; w->queued = true; }
struct cfg80211_scan_info { bool aborted; };
static void cfg80211_scan_done(void *req, struct cfg80211_scan_info *info) {}
static void print_hex_dump(const char *l, const char *p, int t, int r, int g, const void *b, u32 n, bool a) {}
static void wl_dev_intvar_get(struct net_device *dev, const char *name, s32 *v) {}
static void wl_dev_ioctl(struct net_device *dev, int cmd, void *v, int len) {}
static int wl_rsn_ie_has_sae(const u8 *ie, u32 len) { return 1; }
static void wl_refuse_assoc(struct net_device *dev, const u8 *addr, const char *why) { assert(0); }
static void wl_update_bss_info(struct wl_cfg80211_priv *wl) {}
static struct cfg80211_bss bss;
static struct cfg80211_bss *wl_get_ess_bss(struct wl_cfg80211_priv *wl) { return &bss; }
static void cfg80211_put_bss(void *wiphy, struct cfg80211_bss *b) {}

static u8 assoc_ies[4] = { 0x30, 2, 1, 0 }, ies_loaded;
static void wl_get_assoc_ies(struct wl_cfg80211_priv *wl)
{
    wl->conn_info.req_ie = wl->conn_info.resp_ie = assoc_ies;
    wl->conn_info.req_ie_len = wl->conn_info.resp_ie_len = sizeof(assoc_ies);
    ies_loaded++;
}

static unsigned int reports;
static bool reported_bssid;
static u8 last_bssid[6];
static const u8 *last_req_ie;
static u32 last_req_ie_len;
static u16 last_status;
static void cfg80211_connect_result(struct net_device *dev, const u8 *bssid, const u8 *req_ie, u32 req_ie_len,
                                    const u8 *resp_ie, u32 resp_ie_len, u16 status, int gfp)
{
    reports++;
    reported_bssid = bssid;
    if (bssid) memcpy(last_bssid, bssid, 6);
    last_req_ie = req_ie; last_req_ie_len = req_ie_len;
    last_status = status;
}

#include "connect-under-test.h"

static struct net_device dev;
static struct wl_cfg80211_profile profile;
static struct wl_cfg80211_priv wl = { .profile = &profile };

static void join(const u8 *ap, u32 type, u32 status, bool completed)
{
    wl_event_msg_t e = { .event_type = type, .status = status };
    memcpy(e.addr.octet, ap, 6);
    reports = 0;
    set_bit(WL_STATUS_CONNECTING, &wl.status);
    assert(wl_bss_connect_done(&wl, &dev, &e, NULL, completed) == 0);
}

int main(void)
{
    const u8 ap_5g[6] = { 2, 0, 0, 0, 0x35, 0xa4 }, ap_2g[6] = { 2, 0, 0, 0, 0x35, 0x94 };

    join(ap_5g, WLC_E_LINK, WLC_E_STATUS_SUCCESS, true);
    assert(reports == 1 && last_status == WLAN_STATUS_SUCCESS && reported_bssid && !memcmp(last_bssid, ap_5g, 6));
    assert(last_req_ie == assoc_ies && last_req_ie_len == sizeof(assoc_ies) && test_bit(WL_STATUS_CONNECTED, &wl.status));

    /* link lost; the next join, on the other band, fails: blame nobody, so the supplicant charges ap_2g */
    wl.status = 0;
    join(ap_2g, WLC_E_SET_SSID, WLC_E_STATUS_FAIL, false);
    printf("failed join after prior link: reported bssid %s\n",
           !reported_bssid ? "none" : !memcmp(last_bssid, ap_5g, 6) ? "previous AP (stale)" : "other");
    assert(reports == 1 && last_status == WLAN_STATUS_AUTH_TIMEOUT);
    assert(!reported_bssid && !last_req_ie && !last_req_ie_len && !test_bit(WL_STATUS_CONNECTED, &wl.status));

    join(ap_2g, WLC_E_LINK, WLC_E_STATUS_FAIL, false);
    assert(reports == 1 && !reported_bssid && last_status == WLAN_STATUS_AUTH_TIMEOUT);

    /* SAE join failure with retries left: retry silently, no report */
    wl_sae.join_len = 1;
    join(ap_2g, WLC_E_SET_SSID, WLC_E_STATUS_FAIL, false);
    assert(reports == 0 && wl_sae.retry_work.queued);
    wl_sae.join_len = 0;
    wl.status = 0;

    /* not connecting: nothing to report */
    wl_event_msg_t e = { .event_type = WLC_E_SET_SSID, .status = WLC_E_STATUS_FAIL };
    reports = 0;
    assert(wl_bss_connect_done(&wl, &dev, &e, NULL, false) == 0 && reports == 0);

    join(ap_2g, WLC_E_LINK, WLC_E_STATUS_SUCCESS, true);
    assert(reports == 1 && last_status == WLAN_STATUS_SUCCESS && !memcmp(last_bssid, ap_2g, 6) && ies_loaded == 2);
    puts("CONNECT: success reports the joined AP; failed joins report no BSSID or stale IEs");
    return 0;
}
