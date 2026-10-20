/* 联机监视器实现:订阅下载 + 服务器查询 + UDP 房间扫描 + 后台刷新。
 *
 * 数据口径与 LanPlayMonitor 一致:
 *   graphql 服务器  POST http://host:port/  {"query": PublicRoomSnapshot}
 *   rest 服务器     GET  http://host:port/info
 *   1.0(rest)服务器 /info 不给房间列表,用 lan-play 中继协议的
 *   LDN 主动扫描补房间(对应 LanPlayMonitor 的 active-udp-scan)。
 */
#include "monitor.hpp"
#include "monitor_title_resolver.hpp"
#include "monitor_player_filter.hpp"
#include "monitor_target_generation.hpp"

#include <switch.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <strings.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <curl/curl.h>

/* ------------------------------------------------------------------ */
/* 常量                                                                */
/* ------------------------------------------------------------------ */

static const char *kSubChinesePath = "jieluojun/LanPlayMonitor/refs/heads/main/chinese_db.json";

/* 下载通道:HTTPS 加速镜像优先(https://v6.gh-proxy.org/ 直接加在 GitHub
 * 链接前面;GitHub 已全站强制 HTTPS,纯 HTTP 通道几乎都被 301,仅作兜底)。
 * 服务器列表不再订阅 servers.json —— 只读 relay.cfg。 */
static const char *kUrlPrefixes[] = {
    "https://v6.gh-proxy.org/https://raw.githubusercontent.com/",
    "http://raw.githubusercontent.com/",
    "http://gh-proxy.com/https://raw.githubusercontent.com/",
    "http://ghfast.top/https://raw.githubusercontent.com/",
};

static const char *kGraphqlQuery =
    "query PublicRoomSnapshot { serverInfo { online idle } room {"
    " sessionId contentId advertiseData hostPlayerName nodeCount nodeCountMax"
    " nodes { playerName isConnected } } }";

/* Query variants keep older custom GraphQL schemas working while preferring
 * both per-room title data and NodeInfo.isConnected. The latter is essential:
 * servers may retain names in disconnected node slots. */
static const char *kGraphqlQueryNoAdvert =
    "query PublicRoomSnapshot { serverInfo { online idle } room {"
    " sessionId contentId hostPlayerName nodeCount nodeCountMax"
    " nodes { playerName isConnected } } }";
static const char *kGraphqlQueryNoConnection =
    "query PublicRoomSnapshot { serverInfo { online idle } room {"
    " sessionId contentId advertiseData hostPlayerName nodeCount nodeCountMax"
    " nodes { playerName } } }";
static const char *kGraphqlQueryOldest =
    "query PublicRoomSnapshot { serverInfo { online idle } room {"
    " sessionId contentId hostPlayerName nodeCount nodeCountMax"
    " nodes { playerName } } }";

static const char *kSaveDir         = "sdmc:/config/ldn_mitm";
static const char *kSaveChinesePath = "sdmc:/config/ldn_mitm/chinese_db.json";
static const char *kRelayCfgPath    = "sdmc:/config/ldn_mitm/relay.cfg";
static const char *kLogPath         = "sdmc:/config/ldn_mitm/monitor.log";

/* 诊断日志:记录各阶段,便于定位 ovlloader 堆不足时的爆点。
 * 每次 MonitorInit 截断重写。 */
static bool g_sdMounted = false;
static void EnsureSd() {
    /* tesla 覆盖了 __appInit,libnx 默认实现里的 fsdevMountSdmc() 不会执行,
     * 必须自行挂载,否则 sdmc: 的 fopen 全部失败(曾导致 fclose(NULL) 崩溃)。 */
    if (!g_sdMounted) {
        fsdevMountSdmc();
        g_sdMounted = true;
    }
}

static void MonLog(const char *msg) {
    EnsureSd();
    FILE *f = fopen(kLogPath, "a");
    if (!f)
        return;
    fputs(msg, f);
    fputc('\n', f);
    fclose(f);
}

/* bsd socket 初始化:必须在 sm 会话内拿到 bsd:u,且尽量安排在帧缓冲
 * 分配(initScreen)之前——bsd 缓冲是 tmem 堆分配,帧缓冲之后堆很紧。
 * 失败自动用缩容缓冲重试;AlreadyInitialized 视为成功。 */
static bool g_sockInited = false;
static void EnsureSockets() {
    if (g_sockInited)
        return;
    bool ownSm = R_SUCCEEDED(smInitialize());  /* 外层已开 sm 时返回错误,不影响 */
    Result rc = socketInitializeDefault();
    if (R_FAILED(rc)) {
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.tcp_tx_buf_size = 0x4000;
        cfg.tcp_rx_buf_size = 0x8000;
        cfg.tcp_tx_buf_max_size = 0x10000;
        cfg.tcp_rx_buf_max_size = 0x10000;
        cfg.udp_tx_buf_size = 0x1400;
        cfg.udp_rx_buf_size = 0x4000;
        cfg.sb_efficiency = 2;
        cfg.num_bsd_sessions = 1;
        rc = socketInitialize(&cfg);
    }
    if (R_FAILED(rc)) {
        /* 最后一试:bsd:s(bsd:u 会话被同进程先前组件占满时可能需要) */
        SocketInitConfig cfg = *socketGetDefaultInitConfig();
        cfg.tcp_tx_buf_size = 0x4000;
        cfg.tcp_rx_buf_size = 0x8000;
        cfg.tcp_tx_buf_max_size = 0x10000;
        cfg.tcp_rx_buf_max_size = 0x10000;
        cfg.udp_tx_buf_size = 0x1400;
        cfg.udp_rx_buf_size = 0x4000;
        cfg.sb_efficiency = 2;
        cfg.num_bsd_sessions = 1;
        cfg.bsd_service_type = BsdServiceType_System;
        rc = socketInitialize(&cfg);
    }
    if (ownSm)
        smExit();
    if (R_SUCCEEDED(rc) ||
        rc == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized))
        g_sockInited = true;
    char msg[48];
    snprintf(msg, sizeof(msg), "sockets-init %s rc=%08x",
             g_sockInited ? "ok" : "FAIL", (unsigned)rc);
    MonLog(msg);
}

void MonitorInitSockets() {
    /* 日志截断放在这里(最早的监视器调用):MonitorInit 里截断会把
     * sockets-init 这行吃掉,曾导致无法判断网络栈死活。 */
    EnsureSd();
    FILE *lf = fopen(kLogPath, "w");
    if (lf)
        fclose(lf);
    EnsureSockets();
    if (g_sockInited) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        char m[24];
        snprintf(m, sizeof(m), "soc-probe fd=%d", fd);
        MonLog(m);
        if (fd >= 0)
            close(fd);
    }
}

/* 堆余量探针:试分配与帧缓冲同量级的块并立即释放。
 * 崩溃时日志里的 probe 结果直接告诉我们堆还剩多少余量。 */
static void ProbeHeap() {
    static const size_t kProbes[] = { 256u * 1024, 700u * 1024, 1400u * 1024 };
    char msg[48];
    for (size_t sz : kProbes) {
        void *p = malloc(sz);
        snprintf(msg, sizeof(msg), "probe-%uK %s",
                 (unsigned)(sz / 1024), p ? "ok" : "FAIL");
        MonLog(msg);
        if (p)
            free(p);
    }
}

#ifdef __SWITCH__
/* 帧缓冲降级兜底(-Wl,--wrap=framebufferCreate):
 * libtesla 固定双缓冲(448×720 RGBA4444×2 ≈ 1.34MB),在小堆 loader 上
 * 分配失败即 ASSERT_FATAL → 0x559 直接崩。这里失败时自动改单缓冲
 * (≈672KB,少一半),代价是重绘时可能轻微撕裂,插件仍可正常使用。 */
extern "C" Result __real_framebufferCreate(Framebuffer *fb, NWindow *win,
                                           u32 width, u32 height,
                                           u32 format, u32 num_fbs);
extern "C" Result __wrap_framebufferCreate(Framebuffer *fb, NWindow *win,
                                           u32 width, u32 height,
                                           u32 format, u32 num_fbs) {
    Result rc = __real_framebufferCreate(fb, win, width, height, format, num_fbs);
    if (R_FAILED(rc) && num_fbs > 1) {
        MonLog("fb: fallback to 1 buffer");
        rc = __real_framebufferCreate(fb, win, width, height, format, 1);
    }
    if (R_SUCCEEDED(rc))
        MonLog("fb: ready");
    return rc;
}
#endif

void MonitorLog(const char *msg) {
    MonLog(msg);
}

static constexpr size_t kMaxDownload  = 3u * 1024 * 1024;  /* 订阅文件上限 */
static constexpr size_t kMaxHttpBody  = 512u * 1024;       /* 查询响应上限 */
static constexpr u32   kTitleHashSize = 16384;             /* 标题映射哈希桶(2 的幂;须 ≥ 条目数,10521 条) */
static constexpr size_t kTitleArena   = 288u * 1024;       /* 标题名字符串池(实测 10521 条约 216KB) */
static constexpr u32   kMaxScanRooms  = 24;

/* ------------------------------------------------------------------ */
/* 全局状态                                                            */
/* ------------------------------------------------------------------ */

static std::mutex g_mutex;
static MonitorSnapshot g_snap;

static std::atomic<u32>  g_reqGen{0};
/* Request epoch used to reject superseded in-flight responses. */
static MonitorTargetGeneration::Gate g_targetGen;
static std::atomic<bool> g_exit{false};
static std::atomic<bool> g_traditional{false};
static u32   g_doneGen = 0;
static Thread g_thread;
static bool  g_inited = false;
static char  g_pendingName[48];      /* 切换服务器时指定;空 = 读 relay.cfg */
static bool  g_pendingSubs = false;  /* 下一次刷新是否重新拉订阅缓存 */

#define T_STR(hans, hant) (g_traditional.load() ? (hant) : (hans))

/* 服务器列表:servers.json 已彻底移除,只读 relay.cfg(见 MonitorListRelayServers)。 */

/* 标题映射(chinese_db.json 订阅):开放寻址哈希 + 字符串池 */
static u64  *g_titleKey   = nullptr;    /* 存 key+1,0 = 空槽 */
static u32  *g_titleOff   = nullptr;
static u16  *g_titleLen   = nullptr;
static char *g_titleArena = nullptr;
static u32  g_titleArenaUsed = 0;
static void EnsureTitleTables();

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static bool StrIsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static const char *JsWs(const char *p) {
    while (p && *p && StrIsSpace(*p))
        p++;
    return p;
}

/* 跳过 UTF-8 BOM */
static const char *SkipBom(const char *p) {
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB &&
        (unsigned char)p[2] == 0xBF)
        return p + 3;
    return p;
}

static int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ------------------------------------------------------------------ */
/* 迷你 JSON 解析(容错、有界,覆盖本场景所需)                        */
/* ------------------------------------------------------------------ */

static void JsonPut(char *out, size_t n, size_t *o, char c) {
    if (out && *o + 1 < n)
        out[*o] = c;
    (*o)++;
}

/* 解析 JSON 字符串(支持转义与 \uXXXX,含代理对)。*pp 指向起始引号。 */
static bool JsonString(const char **pp, char *out, size_t n) {
    const char *p = *pp;
    if (!p || *p != '"')
        return false;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        unsigned char c = (unsigned char)*p++;
        if (c == '\\') {
            char e = *p++;
            switch (e) {
                case 'n': JsonPut(out, n, &o, '\n'); break;
                case 'r': JsonPut(out, n, &o, '\r'); break;
                case 't': JsonPut(out, n, &o, '\t'); break;
                case 'b': JsonPut(out, n, &o, '\b'); break;
                case 'f': JsonPut(out, n, &o, '\f'); break;
                case '/': JsonPut(out, n, &o, '/');  break;
                case '"': JsonPut(out, n, &o, '"');  break;
                case '\\': JsonPut(out, n, &o, '\\'); break;
                case 'u': {
                    if (HexVal(p[0]) < 0 || HexVal(p[1]) < 0 || HexVal(p[2]) < 0 || HexVal(p[3]) < 0)
                        return false;
                    u32 cp = (u32)((HexVal(p[0]) << 12) | (HexVal(p[1]) << 8) |
                                   (HexVal(p[2]) << 4) | HexVal(p[3]));
                    p += 4;
                    if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u' &&
                        HexVal(p[2]) >= 0 && HexVal(p[3]) >= 0 &&
                        HexVal(p[4]) >= 0 && HexVal(p[5]) >= 0) {
                        u32 lo = (u32)((HexVal(p[2]) << 12) | (HexVal(p[3]) << 8) |
                                       (HexVal(p[4]) << 4) | HexVal(p[5]));
                        if (lo >= 0xDC00 && lo <= 0xDFFF) {
                            cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                            p += 6;
                        }
                    }
                    if (cp < 0x80) {
                        JsonPut(out, n, &o, (char)cp);
                    } else if (cp < 0x800) {
                        JsonPut(out, n, &o, (char)(0xC0 | (cp >> 6)));
                        JsonPut(out, n, &o, (char)(0x80 | (cp & 0x3F)));
                    } else if (cp < 0x10000) {
                        JsonPut(out, n, &o, (char)(0xE0 | (cp >> 12)));
                        JsonPut(out, n, &o, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        JsonPut(out, n, &o, (char)(0x80 | (cp & 0x3F)));
                    } else {
                        JsonPut(out, n, &o, (char)(0xF0 | (cp >> 18)));
                        JsonPut(out, n, &o, (char)(0x80 | ((cp >> 12) & 0x3F)));
                        JsonPut(out, n, &o, (char)(0x80 | ((cp >> 6) & 0x3F)));
                        JsonPut(out, n, &o, (char)(0x80 | (cp & 0x3F)));
                    }
                    break;
                }
                default: JsonPut(out, n, &o, e); break;
            }
        } else {
            JsonPut(out, n, &o, (char)c);
        }
    }
    if (*p != '"')
        return false;
    p++;
    if (out && n > 0)
        out[o < n ? o : n - 1] = 0;
    *pp = p;
    return true;
}

/* 跳过一个完整 JSON 值,返回其后位置;失败返回 nullptr。 */
static const char *JsonSkip(const char *p, int depth) {
    if (!p || depth > 24)
        return nullptr;
    p = JsWs(p);
    switch (*p) {
        case '"':
            return JsonString(&p, nullptr, 0) ? p : nullptr;
        case '{':
        case '[': {
            char close = (*p == '{') ? '}' : ']';
            p = JsWs(p + 1);
            if (*p == close)
                return p + 1;
            for (;;) {
                if (close == '}') {
                    p = JsonSkip(p, depth + 1);          /* key */
                    if (!p) return nullptr;
                    p = JsWs(p);
                    if (*p != ':') return nullptr;
                    p = JsonSkip(p + 1, depth + 1);      /* value */
                } else {
                    p = JsonSkip(p, depth + 1);
                }
                if (!p) return nullptr;
                p = JsWs(p);
                if (*p == ',') { p = JsWs(p + 1); continue; }
                if (*p == close) return p + 1;
                return nullptr;
            }
        }
        case 't': return (strncmp(p, "true", 4) == 0) ? p + 4 : nullptr;
        case 'f': return (strncmp(p, "false", 5) == 0) ? p + 5 : nullptr;
        case 'n': return (strncmp(p, "null", 4) == 0) ? p + 4 : nullptr;
        default: {
            const char *s = p;
            if (*p == '-' || *p == '+') p++;
            bool any = false;
            while ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' ||
                   *p == 'E' || *p == '-' || *p == '+') {
                p++;
                any = true;
            }
            return (any && p != s) ? p : nullptr;
        }
    }
}

/* 在 obj(指向 '{')中取 key 对应值的起始位置;没有则 nullptr。 */
static const char *JsonGet(const char *obj, const char *key) {
    if (!obj || *obj != '{')
        return nullptr;
    const char *p = JsWs(obj + 1);
    if (*p == '}')
        return nullptr;
    for (;;) {
        char k[64];
        if (*p != '"' || !JsonString(&p, k, sizeof(k)))
            return nullptr;
        p = JsWs(p);
        if (*p != ':')
            return nullptr;
        p = JsWs(p + 1);
        if (strcmp(k, key) == 0)
            return p;
        p = JsonSkip(p, 1);
        if (!p)
            return nullptr;
        p = JsWs(p);
        if (*p == ',') { p = JsWs(p + 1); continue; }
        return nullptr;
    }
}

static bool JsonGetStr(const char *obj, const char *key, char *out, size_t n) {
    const char *v = JsonGet(obj, key);
    return v && *v == '"' && JsonString(&v, out, n);
}

static bool JsonGetU32(const char *obj, const char *key, u32 *out) {
    const char *v = JsonGet(obj, key);
    if (!v)
        return false;
    v = JsWs(v);
    if (*v < '0' || *v > '9')
        return false;
    u32 acc = 0;
    while (*v >= '0' && *v <= '9') {
        if (acc < 0x0CCCCCCC)
            acc = acc * 10 + (u32)(*v - '0');
        v++;
    }
    *out = acc;
    return true;
}

/* 同义 key 依次尝试(如 contentId/content_id) */
static bool JsonGetStrAny(const char *obj, const char *k1, const char *k2,
                          char *out, size_t n) {
    if (JsonGetStr(obj, k1, out, n))
        return true;
    return k2 && JsonGetStr(obj, k2, out, n);
}

/* ------------------------------------------------------------------ */
/* HTTP 客户端(纯 HTTP;Content-Length / chunked / 连接关闭三种定界)   */
/* ------------------------------------------------------------------ */

struct HttpUrl {
    char host[96];
    u16  port;
    char path[320];
};

static bool HttpParseUrl(const char *url, HttpUrl *out) {
    if (strncmp(url, "http://", 7) != 0)
        return false;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    const char *hostEnd = slash ? slash : p + strlen(p);
    const char *colon = (const char *)memchr(p, ':', (size_t)(hostEnd - p));
    size_t hostLen = (size_t)((colon ? colon : hostEnd) - p);
    if (hostLen == 0 || hostLen >= sizeof(out->host))
        return false;
    memcpy(out->host, p, hostLen);
    out->host[hostLen] = 0;
    out->port = 80;
    if (colon) {
        u32 port = 0;
        const char *q = colon + 1;
        while (q < hostEnd && *q >= '0' && *q <= '9')
            port = port * 10 + (u32)(*q++ - '0');
        if (port == 0 || port > 65535)
            return false;
        out->port = (u16)port;
    }
    if (!slash) {
        out->path[0] = '/';
        out->path[1] = 0;
    } else {
        size_t plen = strlen(slash);
        if (plen >= sizeof(out->path))
            return false;
        memcpy(out->path, slash, plen + 1);
    }
    return true;
}

/* libnx/newlib 的 netinet/in.h 不定义 INADDR_NONE,补一个 */
#ifndef INADDR_NONE
#define INADDR_NONE ((in_addr_t)0xFFFFFFFFu)
#endif

/* ------------------------------------------------------------------ */
/* 原始 UDP DNS(与 sysmodule relay_client 同款)。
 * 真机诊断:覆盖层进程里 getaddrinfo 全部失败(refresh 里所有 e=1),
 * 而 sysmodule 的延迟测量一直正常——它从来不用 getaddrinfo,而是手工
 * 构造 DNS 报文发给路由器通告的 DNS(nifmGetCurrentIpConfigInfo)。
 * 这里照搬该实现;nifm 拿不到 dns 时退公网 DNS(国内 223.5.5.5/
 * 114.114.114.114,海外 8.8.8.8/1.1.1.1)。 */
/* ------------------------------------------------------------------ */

static u32 DnsBe32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* 构造 A 查询报文;成功返回长度,失败 0。 */
static size_t DnsBuildQuery(const char *host, u8 *q, size_t cap, u8 txid0, u8 txid1) {
    if (host == nullptr || host[0] == '\0' || cap < 18)
        return 0;
    q[0] = txid0; q[1] = txid1;
    q[2] = 0x01; q[3] = 0x00;                 /* RD */
    q[4] = 0x00; q[5] = 0x01;                 /* qdcount=1 */
    q[6] = q[7] = q[8] = q[9] = q[10] = q[11] = 0;
    size_t p = 12;
    const char *s = host;
    while (*s != '\0') {
        const char *dot = strchr(s, '.');
        size_t len = dot ? (size_t)(dot - s) : strlen(s);
        if (len == 0 || len > 63 || p + len + 6 > cap)
            return 0;
        q[p++] = (u8)len;
        memcpy(q + p, s, len);
        p += len;
        s += len;
        if (*s == '.')
            s++;
    }
    q[p++] = 0x00;
    q[p++] = 0x00; q[p++] = 0x01;             /* A    */
    q[p++] = 0x00; q[p++] = 0x01;             /* IN   */
    return p;
}

/* 跳过一个域名(标签串或 0xC0 压缩指针)。 */
static bool DnsSkipName(const u8 *r, ssize_t n, size_t *off) {
    for (;;) {
        if (*off >= (size_t)n)
            return false;
        u8 c = r[*off];
        if ((c & 0xc0) == 0xc0) {
            *off += 2;
            return true;
        }
        if (c == 0) {
            *off += 1;
            return true;
        }
        *off += (size_t)c + 1;
    }
}

/* 解析应答,取第一个 A 记录;*ipNet 为网络字序。 */
static bool DnsParseA(const u8 *r, ssize_t n, u8 txid0, u8 txid1, u32 *ipNet) {
    if (n < 12)
        return false;
    if (r[0] != txid0 || r[1] != txid1)
        return false;
    if ((r[2] & 0x80) == 0)                   /* QR */
        return false;
    if ((r[3] & 0x0f) != 0)                   /* RCODE */
        return false;
    int ancount = (r[6] << 8) | r[7];
    if (ancount < 1)
        return false;

    size_t off = 12;
    if (!DnsSkipName(r, n, &off))
        return false;
    off += 4;                                 /* qtype + qclass */
    if (off > (size_t)n)
        return false;

    for (int i = 0; i < ancount && off + 12 <= (size_t)n; i++) {
        if (!DnsSkipName(r, n, &off))
            return false;
        if (off + 10 > (size_t)n)
            break;
        int type  = (r[off] << 8) | r[off + 1];
        int rdlen = (r[off + 8] << 8) | r[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= (size_t)n) {
            *ipNet = htonl(DnsBe32(r + off));
            return true;
        }
        off += rdlen;
    }
    return false;
}

/* 向单个 DNS 服务器查 A;返回主机字序 IP,0=失败。 */
static u32 DnsQueryServer(u32 srvHost, const char *host, u8 txid0, u8 txid1) {
    u8 q[300];
    size_t qlen = DnsBuildQuery(host, q, sizeof(q), txid0, txid1);
    if (qlen == 0)
        return 0;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(srvHost);
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    u32 ip = 0;
    if (sendto(fd, q, (int)qlen, 0, (struct sockaddr *)&sa, sizeof(sa)) >= 0) {
        u8 r[512];
        ssize_t n = recvfrom(fd, r, sizeof(r), 0, nullptr, nullptr);
        if (n > 0) {
            u32 net = 0;
            if (DnsParseA(r, n, txid0, txid1, &net))
                ip = ntohl(net);
        }
    }
    close(fd);
    return ip;
}

static u32 g_dnsSrv[6];
static int g_dnsSrvN = 0;
static bool g_dnsSrvReady = false;
static u32 g_dnsNifmRc = 1;                   /* 0=ok,其它=失败码 */

static void DnsInitServers() {
    if (g_dnsSrvReady)
        return;
    g_dnsSrvReady = true;
    u32 a = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    Result rc = nifmInitialize(NifmServiceType_User);
    if (R_FAILED(rc))
        rc = nifmInitialize(NifmServiceType_System);
    if (R_SUCCEEDED(rc)) {
        g_dnsNifmRc = 0;
        if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&a, &mask, &gw, &dns1, &dns2))) {
            if (dns1 != 0)
                g_dnsSrv[g_dnsSrvN++] = ntohl(dns1);
            if (dns2 != 0)
                g_dnsSrv[g_dnsSrvN++] = ntohl(dns2);
        }
        nifmExit();
    } else {
        g_dnsNifmRc = (u32)rc;
    }
    /* 公网兜底:国内两台 + 海外两台 */
    g_dnsSrv[g_dnsSrvN++] = 0xDF050505u;      /* 223.5.5.5 */
    g_dnsSrv[g_dnsSrvN++] = 0x72727272u;      /* 114.114.114.114 */
    g_dnsSrv[g_dnsSrvN++] = 0x08080808u;      /* 8.8.8.8 */
    g_dnsSrv[g_dnsSrvN++] = 0x01010101u;      /* 1.1.1.1 */
    char m[96];
    snprintf(m, sizeof(m), "dns srv nifm=%08x n=%d",
             (unsigned)g_dnsNifmRc, g_dnsSrvN);
    MonLog(m);
}

struct DnsEnt {
    char host[48];
    u32 ip;                                   /* 主机字序 */
};
static DnsEnt g_dnsCache[8];
static int g_dnsCacheN = 0;

static bool DnsResolve(const char *host, u32 *ipNet) {
    DnsInitServers();
    for (int i = 0; i < g_dnsCacheN; i++) {
        if (strcasecmp(g_dnsCache[i].host, host) == 0 && g_dnsCache[i].ip != 0) {
            *ipNet = htonl(g_dnsCache[i].ip);
            return true;
        }
    }
    for (int s = 0; s < g_dnsSrvN; s++) {
        u32 ip = DnsQueryServer(g_dnsSrv[s], host, 0x13, 0x37);
        if (ip != 0) {
            if (g_dnsCacheN < (int)(sizeof(g_dnsCache) / sizeof(g_dnsCache[0]))) {
                snprintf(g_dnsCache[g_dnsCacheN].host,
                         sizeof(g_dnsCache[0].host), "%s", host);
                g_dnsCache[g_dnsCacheN].ip = ip;
                g_dnsCacheN++;
            }
            *ipNet = htonl(ip);
            char m[96];
            snprintf(m, sizeof(m), "dns ok %s %08x", host, (unsigned)ip);
            MonLog(m);
            return true;
        }
    }
    char m[64];
    snprintf(m, sizeof(m), "dns raw fail %s", host);
    MonLog(m);
    return false;
}

static bool HttpResolve(const char *host, struct sockaddr_in *sa) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    u32 ip = inet_addr(host);
    if (ip != (u32)INADDR_NONE && ip != 0) {
        sa->sin_addr.s_addr = ip;
        return true;
    }
    /* 1) 原始 UDP DNS(sysmodule 同款,真机可用) */
    if (DnsResolve(host, &sa->sin_addr.s_addr))
        return true;
    /* 2) getaddrinfo 兜底(真机通常失败,保留无害) */
    /* libnx/newlib 没有 gethostbyname,域名解析用 getaddrinfo */
    struct addrinfo hints, *res = nullptr;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res)
        return false;
    memcpy(&sa->sin_addr, &((struct sockaddr_in *)res->ai_addr)->sin_addr,
           sizeof(sa->sin_addr));
    freeaddrinfo(res);
    return true;
}

static void HttpSetNonBlock(int fd, bool nb) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl < 0)
        return;
    fcntl(fd, F_SETFL, nb ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
}

using Deadline = std::chrono::steady_clock::time_point;

static bool HttpSendAll(int fd, const char *buf, size_t len, const Deadline &deadline) {
    size_t off = 0;
    while (off < len) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        ssize_t n = send(fd, buf + off, len - off, 0);
        if (n < 0)
            return false;
        off += (size_t)n;
    }
    return true;
}

static bool HttpIsChunked(const std::string &headersLower) {
    size_t te = headersLower.find("transfer-encoding:");
    if (te == std::string::npos)
        return false;
    size_t lineEnd = headersLower.find("\r\n", te);
    size_t ck = headersLower.find("chunked", te);
    return ck != std::string::npos && (lineEnd == std::string::npos || ck < lineEnd);
}

/* 发起请求,响应(头+体)收进 raw。 */
/* HTTP 传输错误码(诊断用):0=ok 1=resolve 2=socket 3=connect 4=timeout
 * 5=io;≥100 表示 HTTP 状态码(如 131=301 跳转)。 */
static int g_httpErr = 0;

static bool HttpExchange(const char *host, u16 port, const char *req, size_t reqLen,
                         std::string &raw, size_t maxBody, int timeoutMs) {
    g_httpErr = 0;
    struct sockaddr_in sa;
    if (!HttpResolve(host, &sa)) {
        g_httpErr = 1;
        return false;
    }
    sa.sin_port = htons(port);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        g_httpErr = 2;
        return false;
    }

    Deadline deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    /* 非阻塞 connect + select,避免不可达主机长时间挂起 */
    HttpSetNonBlock(fd, true);
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
    if (rc != 0) {
        fd_set wset;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        long left = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { g_httpErr = 4; close(fd); return false; }
        struct timeval tv;
        tv.tv_sec = left / 1000;
        tv.tv_usec = (left % 1000) * 1000;
        rc = select(fd + 1, nullptr, &wset, nullptr, &tv);
        if (rc <= 0) { g_httpErr = 4; close(fd); return false; }
        int err = 0;
        socklen_t elen = sizeof(err);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0) {
            g_httpErr = 3;
            close(fd);
            return false;
        }
    }
    HttpSetNonBlock(fd, false);

    struct timeval rtv;
    rtv.tv_sec = timeoutMs / 1000;
    rtv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));

    if (!HttpSendAll(fd, req, reqLen, deadline)) {
        g_httpErr = 5;
        close(fd);
        return false;
    }

    raw.clear();
    char buf[2048];
    bool haveHeaders = false;
    bool chunked = false;
    long contentLength = -1;
    size_t headerEnd = 0;

    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline) {
            /* 超时:已有完整响应体则接受,否则失败 */
            if (haveHeaders)
                break;
            close(fd);
            return false;
        }
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n == 0)
            break;                                   /* 对端关闭:EOF 定界 */
        if (n < 0) {
            if (haveHeaders)
                break;
            close(fd);
            return false;
        }
        if (raw.size() + (size_t)n > maxBody + 8192) {
            close(fd);
            return false;
        }
        raw.append(buf, (size_t)n);

        if (!haveHeaders) {
            size_t pos = raw.find("\r\n\r\n");
            if (pos == std::string::npos)
                continue;
            headerEnd = pos + 4;
            haveHeaders = true;

            std::string headersLower = raw.substr(0, pos);
            for (auto &ch : headersLower)
                ch = (char)tolower((unsigned char)ch);
            chunked = HttpIsChunked(headersLower);
            size_t cl = headersLower.find("content-length:");
            if (cl != std::string::npos) {
                contentLength = atol(headersLower.c_str() + cl + 15);
                if (contentLength < 0)
                    contentLength = 0;
            }
            /* 按 Content-Length 精确预留,避免 std::string 几何增长
             * 产生约 2 倍的瞬时容量(挤爆 ovlloader 的小堆)。 */
            if (!chunked && contentLength > 0 &&
                (size_t)contentLength <= maxBody)
                raw.reserve(headerEnd + (size_t)contentLength);
        }
        if (haveHeaders && contentLength >= 0 && !chunked &&
            raw.size() >= headerEnd + (size_t)contentLength)
            break;
    }
    close(fd);
    return haveHeaders;
}

/* 解析响应:状态码 + body(chunked 已解码;Content-Length 已截断)。 */
static bool HttpParseResponse(std::string &raw, int *outStatus, std::string *outBody) {
    size_t pos = raw.find("\r\n\r\n");
    if (pos == std::string::npos)
        return false;
    const char *sp = strchr(raw.c_str(), ' ');
    if (!sp)
        return false;
    *outStatus = atoi(sp + 1);

    std::string headersLower = raw.substr(0, pos);
    for (auto &ch : headersLower)
        ch = (char)tolower((unsigned char)ch);

    std::string body;
    size_t p = pos + 4;
    if (HttpIsChunked(headersLower)) {
        for (;;) {
            size_t eol = raw.find("\r\n", p);
            if (eol == std::string::npos)
                return false;
            unsigned long chunk = strtoul(raw.c_str() + p, nullptr, 16);
            if (chunk == 0)
                break;                    /* 尾块:忽略 trailer */
            p = eol + 2;
            if (chunk > raw.size() || p + chunk + 2 > raw.size())
                return false;
            body.append(raw, p, chunk);
            p += chunk + 2;
        }
    } else {
        /* 原地截掉响应头并搬走,避免 body 再复制一份
         * (overlay 堆只有几 MB,少一次订阅文件大小的峰值拷贝)。 */
        raw.erase(0, p);
        size_t cl = headersLower.find("content-length:");
        if (cl != std::string::npos) {
            long n = atol(headersLower.c_str() + cl + 15);
            if (n >= 0 && (size_t)n < raw.size())
                raw.resize((size_t)n);
        }
        *outBody = std::move(raw);
        return true;
    }
    *outBody = std::move(body);
    return true;
}

static bool HttpGet(const char *url, std::string *outBody, int timeoutMs, size_t maxBody) {
    HttpUrl u;
    if (!HttpParseUrl(url, &u))
        return false;
    char req[768];
    int n = snprintf(req, sizeof(req),
                     "GET %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: ldn_mitm-overlay\r\n"
                     "Accept: */*\r\nConnection: close\r\n\r\n",
                     u.path, u.host, (unsigned)u.port);
    if (n <= 0 || n >= (int)sizeof(req))
        return false;
    std::string raw;
    if (!HttpExchange(u.host, u.port, req, (size_t)n, raw, maxBody, timeoutMs))
        return false;
    int status = 0;
    if (!HttpParseResponse(raw, &status, outBody))
        return false;
    if (status >= 200 && status < 300)
        return true;
    g_httpErr = 100 + status;
    return false;
}

static bool HttpPostJson(const char *url, const char *json,
                         std::string *outBody, int timeoutMs, size_t maxBody) {
    HttpUrl u;
    if (!HttpParseUrl(url, &u))
        return false;
    size_t jlen = strlen(json);
    char req[512];
    int n = snprintf(req, sizeof(req),
                     "POST %s HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: ldn_mitm-overlay\r\n"
                     "Content-Type: application/json\r\nAccept: application/json\r\n"
                     "Connection: close\r\nContent-Length: %u\r\n\r\n",
                     u.path, u.host, (unsigned)u.port, (unsigned)jlen);
    if (n <= 0 || n >= (int)sizeof(req))
        return false;
    std::string full;
    full.assign(req, (size_t)n);
    full.append(json, jlen);
    std::string raw;
    if (!HttpExchange(u.host, u.port, full.c_str(), full.size(), raw, maxBody, timeoutMs))
        return false;
    int status = 0;
    if (!HttpParseResponse(raw, &status, outBody))
        return false;
    if (status >= 200 && status < 300)
        return true;
    g_httpErr = 100 + status;
    return false;
}

/* HTTPS 通道(switch-curl,libnx SSL 后端):overlay 是纯 HTTP 客户端,
 * GitHub 全站强制 HTTPS,只有走 TLS 才能把订阅真正拉下来(v6.gh-proxy.org
 * 加速镜像就是 https)。DNS 用本文件的原始解析(覆盖层进程里 getaddrinfo
 * 不可用),以 CURLOPT_RESOLVE 钉住 IP,SNI/Host 仍是域名。 */
static bool s_curlInited = false;

static size_t CurlWrite(char *ptr, size_t size, size_t nmemb, void *userdata) {
    std::string *out = (std::string *)userdata;
    size_t n = size * nmemb;
    if (out->size() + n > kMaxDownload)
        return 0;
    out->append(ptr, n);
    return n;
}

static bool HttpsGet(const char *url, std::string *outBody, int timeoutMs) {
    if (strncmp(url, "https://", 8) != 0)
        return false;
    EnsureSockets();
    if (!s_curlInited) {
        /* libnx SSL 后端经 sm 拿 ssl 服务;外层已开 sm 时失败也无妨 */
        bool ownSm = R_SUCCEEDED(smInitialize());
        curl_global_init(CURL_GLOBAL_DEFAULT);
        if (ownSm)
            smExit();
        s_curlInited = true;
    }
    /* 预解析 host:curl 默认 getaddrinfo 在 overlay 里全灭,必须钉 IP */
    const char *hostBeg = url + 8;
    const char *hostEnd = strchr(hostBeg, '/');
    size_t hl = hostEnd ? (size_t)(hostEnd - hostBeg) : strlen(hostBeg);
    char host[128];
    if (hl == 0 || hl >= sizeof(host))
        return false;
    memcpy(host, hostBeg, hl);
    host[hl] = 0;
    struct sockaddr_in sa = {};
    if (!HttpResolve(host, &sa))
        return false;
    char resolve[192];
    snprintf(resolve, sizeof(resolve), "%s:443:%u.%u.%u.%u", host,
             (unsigned)(sa.sin_addr.s_addr & 0xFF),
             (unsigned)((sa.sin_addr.s_addr >> 8) & 0xFF),
             (unsigned)((sa.sin_addr.s_addr >> 16) & 0xFF),
             (unsigned)((sa.sin_addr.s_addr >> 24) & 0xFF));

    outBody->clear();
    CURL *curl = curl_easy_init();
    if (!curl)
        return false;
    curl_easy_setopt(curl, CURLOPT_URL, url);
    /* CURLOPT_RESOLVE 收的是 struct curl_slist *,不是 char *(curl 7.69 同)。
     * 传字符串会把地址当链表头解引用,首次 perform 即崩(2168-0002)——
     * 10-19 设备报错正死在这一窗口。 */
    struct curl_slist *lst = curl_slist_append(nullptr, resolve);
    if (!lst) {
        curl_easy_cleanup(curl);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_RESOLVE, lst);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, outBody);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXFILESIZE, (long)kMaxDownload);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)timeoutMs);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "ldn-mitm-monitor");
    MonLog("curl-try");
    CURLcode rc = curl_easy_perform(curl);
    {
        char m[48];
        snprintf(m, sizeof(m), "curl-done rc=%d", (int)rc);
        MonLog(m);
    }
    curl_slist_free_all(lst);          /* perform 结束后才可释放 */
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (rc == CURLE_OK && status >= 200 && status < 300)
        return true;
    /* 错误码:700+curlcode(curl 层),否则 100+HTTP 状态 */
    g_httpErr = (rc != CURLE_OK) ? (700 + (int)rc) : (100 + (int)status);
    outBody->clear();
    return false;
}

/* 按镜像链下载订阅文件(https 镜像走 curl,http 兜底走内置轻客户端) */
static bool DownloadSubscription(const char *relPath, std::string *out) {
    for (size_t i = 0; i < sizeof(kUrlPrefixes) / sizeof(kUrlPrefixes[0]); i++) {
        char url[512];
        snprintf(url, sizeof(url), "%s%s", kUrlPrefixes[i], relPath);
        bool ok = (strncmp(url, "https://", 8) == 0)
                      ? HttpsGet(url, out, 5000)
                      : HttpGet(url, out, 5000, kMaxDownload);
        if (ok) {
            MonLog("download ok");
            return true;
        }
        char msg[40];
        snprintf(msg, sizeof(msg), "download try fail e=%d", g_httpErr);
        MonLog(msg);
    }
    return false;
}

static void SaveFile(const char *path, const std::string &data) {
    EnsureSd();
    mkdir("sdmc:/config", 0777);
    mkdir(kSaveDir, 0777);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
}

static bool LoadFile(const char *path, std::string *out) {
    EnsureSd();
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char buf[4096];
    out->clear();
    for (;;) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
            break;
        if (out->size() + n > kMaxDownload) {
            fclose(f);
            return false;
        }
        out->append(buf, n);
    }
    fclose(f);
    return !out->empty();
}

/* ------------------------------------------------------------------ */
/* 订阅解析:chinese_db.json(servers.json 已移除) */
/* ------------------------------------------------------------------ */

static u64 TitleKeyPack(const char *hex16) {
    u64 key = 0;
    for (int i = 0; i < 16; i++) {
        int v = HexVal(hex16[i]);
        if (v < 0)
            return 0;
        key = (key << 4) | (u64)v;
    }
    return key;
}

static u32 TitleHash(u64 key) {
    key ^= key >> 33;
    key *= 0xFF51AFD7ED558CCDull;
    key ^= key >> 33;
    return (u32)key & (kTitleHashSize - 1);
}

static void TitleInsert(u64 key, const char *name, size_t nameLen) {
    if (!g_titleKey || key == 0)
        return;
    u32 idx = TitleHash(key);
    for (u32 probe = 0; probe < kTitleHashSize; probe++) {
        u32 i = (idx + probe) & (kTitleHashSize - 1);
        if (g_titleKey[i] == 0) {
            if (g_titleArenaUsed + nameLen + 1 > kTitleArena)
                return;
            u32 off = g_titleArenaUsed;
            memcpy(g_titleArena + off, name, nameLen);
            g_titleArena[off + nameLen] = 0;
            g_titleArenaUsed = off + (u32)nameLen + 1;
            g_titleOff[i] = off;
            g_titleLen[i] = (u16)nameLen;
            g_titleKey[i] = key + 1;
            return;
        }
        if (g_titleKey[i] == key + 1)
            return;                      /* 重复 key:保留先到的 */
    }
}

static const char *TitleLookup(u64 key, size_t *outLen) {
    if (!g_titleKey || key == 0)
        return nullptr;
    u32 idx = TitleHash(key);
    for (u32 probe = 0; probe < kTitleHashSize; probe++) {
        u32 i = (idx + probe) & (kTitleHashSize - 1);
        if (g_titleKey[i] == 0)
            return nullptr;
        if (g_titleKey[i] == key + 1) {
            if (outLen)
                *outLen = g_titleLen[i];
            return g_titleArena + g_titleOff[i];
        }
    }
    return nullptr;
}

/* (整读 JSON 的旧解析器已由 ParseChineseDbFile 流式版取代) */

/* 从文件流式解析 {"KEY":"NAME",...} 到标题表。
 * 不把 480KB 的订阅 JSON 整个读进 std::string——窗口只有 1KB,
 * 峰值内存 = 标题表(约 512KB) + 窗口,避免与订阅字符串叠加把
 * ovlloader 仅几 MB 的堆挤爆(0x559 OutOfMemory)。 */
static bool ParseChineseDbFile(const char *path) {
    EnsureSd();
    EnsureTitleTables();          /* 幂等:不依赖调用方先建表 */
    if (!g_titleKey)
        return false;
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    char win[1280];
    size_t winLen = 0;
    bool eof = false, atStart = true;

    while (!eof || winLen > 0) {
        if (!eof && winLen < sizeof(win) - 1) {
            size_t n = fread(win + winLen, 1, sizeof(win) - 1 - winLen, f);
            if (n == 0)
                eof = true;
            winLen += n;
        }
        win[winLen] = '\0';

        const char *p = win;
        if (atStart) {
            p = JsWs(SkipBom(p));
            if (*p == '{')
                p = JsWs(p + 1);
            else if (*p == '\0' && !eof)
                continue;                 /* 数据未到 */
            atStart = false;
        }

        bool advanced = true;
        while (advanced) {
            advanced = false;
            p = JsWs(p);
            if (*p == '\0')
                break;                    /* 窗口尾:等更多数据 */
            if (*p == '}')
                { fclose(f); return true; }
            if (*p == ',')
                { p++; advanced = true; continue; }
            if (*p != '"')
                { p++; advanced = true; continue; }   /* 容错跳过 */

            const char *q = p;
            char key[24], name[128];
            if (!JsonString(&q, key, sizeof(key)))
                break;                    /* 不完整(跨块)或坏转义 */
            q = JsWs(q);
            if (*q != ':')
                break;
            q = JsWs(q + 1);
            if (!JsonString(&q, name, sizeof(name)))
                break;

            u64 k = TitleKeyPack(key);
            if (k != 0 && name[0])
                TitleInsert(k, name, strlen(name));
            p = q;
            advanced = true;
        }

        size_t used = (size_t)(p - win);
        if (used > 0) {
            memmove(win, win + used, winLen - used);
            winLen -= used;
        }
        if (!advanced && !eof && winLen == sizeof(win) - 1) {
            /* 病态长 token 塞满窗口:丢一字节防止死锁 */
            memmove(win, win + 1, --winLen);
        }
        if (eof && !advanced)
            break;
    }
    fclose(f);
    return true;
}

/* ------------------------------------------------------------------ */
/* relay.cfg 行解析:与 sysmodule(relay_client.cpp LoadConfig)逐条对齐  */
/* ------------------------------------------------------------------ */

/* 指令值:LoadConfig 的 DirectiveValue 语义(name 后可接 =/空白,值再跳过)。 */
static const char *RelayDirective(const char *l, const char *name) {
    size_t n = strlen(name);
    if (strncmp(l, name, n) != 0)
        return nullptr;
    char c = l[n];
    if (c != 0 && c != '=' && c != ' ' && c != '\t')
        return nullptr;
    const char *v = l + n;
    while (*v == '=' || *v == ' ' || *v == '\t')
        v++;
    return v;
}

/* 解析 relay.cfg 的一行(服务器条目)。与 LoadConfig 完全同语义:
 *   sscanf("%31s %63s") —— 任意空白(含 \v \f)切 token;旧版用 " \t"
 *   切分,遇到 \v/\f 分隔的行就匹配不到名字(resolve noname 之谜);
 *   单 token 行 name=addr=token(随后 addr 再去 :port);
 *   enabled=/broadcast=/id=/selected= 指令行与 #/空行跳过;
 *   addr 可带 :port(默认 11451;port 为 0 或 >65535 整行丢弃)。
 * 返回 true = 是服务器行。 */
static bool RelayParseLine(const char *lineIn,
                           char *name, size_t nameN,
                           char *host, size_t hostN, u16 *port) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", lineIn);
    char *line = buf;
    while (*line == ' ' || *line == '\t')
        line++;
    if (*line == '#' || *line == 0)
        return false;
    if (RelayDirective(line, "enabled") || RelayDirective(line, "broadcast") ||
        RelayDirective(line, "selected") || RelayDirective(line, "id"))
        return false;

    char t1[32] = {}, t2[64] = {};
    if (sscanf(line, "%31s %63s", t1, t2) < 2) {
        if (sscanf(line, "%63s", t2) < 1)
            return false;
        snprintf(t1, sizeof(t1), "%.31s", t2);   /* 单 token:name=addr=token(同 %31s 截断) */
    }
    unsigned p = 11451;
    char *colon = strchr(t2, ':');
    if (colon) {
        *colon = 0;
        p = (unsigned)atoi(colon + 1);
    }
    if (t2[0] == 0 || p == 0 || p > 65535)
        return false;
    snprintf(name, nameN, "%s", t1);
    snprintf(host, hostN, "%s", t2);
    *port = (u16)p;
    return true;
}

/* 匹配 relay.cfg 一行里的服务器名;isEntry 标记该行是服务器行(指令/注释不算)。 */
static bool MatchRelayLine(const std::string &line, const char *want,
                           char *nameOut, size_t nameN,
                           char *hostOut, size_t hostN, u16 *portOut,
                           bool *isEntry = nullptr) {
    if (isEntry)
        *isEntry = false;
    char name[32] = {}, host[64] = {};
    u16 port = 0;
    if (!RelayParseLine(line.c_str(), name, sizeof(name), host, sizeof(host), &port))
        return false;
    if (isEntry)
        *isEntry = true;
    if (strcmp(name, want) != 0)
        return false;
    snprintf(nameOut, nameN, "%s", name);
    snprintf(hostOut, hostN, "%s", host);
    *portOut = port;
    return true;
}

/* 服务器列表:直接读 relay.cfg(与 sysmodule LoadConfig 同一套解析,
 * 第 i 条与 sysmodule 的 g_servers[i] 一一对应)。 */
u32 MonitorListRelayServers(MonitorServer *out, u32 max) {
    std::string cfg;
    if (!out || !LoadFile(kRelayCfgPath, &cfg))
        return 0;
    u32 n = 0;
    size_t i = 0;
    while (i < cfg.size() && n < max) {
        size_t e = cfg.find_first_of("\r\n", i);
        std::string line = cfg.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = (e == std::string::npos) ? cfg.size() : e + 1;
        char host[64] = {};
        u16 port = 0;
        if (RelayParseLine(line.c_str(), out[n].name, sizeof(out[n].name),
                           host, sizeof(host), &port)) {
            snprintf(out[n].addr, sizeof(out[n].addr), "%.55s:%u", host, (unsigned)port);
            n++;
        }
    }
    return n;
}

static bool ResolveSelectedServer(const char *nameOverride,
                                  char *nameOut, size_t nameN,
                                  char *hostOut, size_t hostN, u16 *portOut) {
    std::string cfg;
    if (!LoadFile(kRelayCfgPath, &cfg)) {
        MonLog("resolve FAIL cfg-missing");
        return false;
    }

    char want[64] = {};
    if (nameOverride && nameOverride[0])
        snprintf(want, sizeof(want), "%s", nameOverride);
    /* 名字去尾部空白(IPC/配置可能带填充;sysmodule 同样修剪) */
    for (int pass = 0; pass < 2; pass++) {
        size_t wl = strlen(want);
        while (wl > 0 && (want[wl - 1] == ' ' || want[wl - 1] == '\t'))
            want[--wl] = 0;
        if (pass == 0 && want[0])
            break;
        if (pass == 0 && !want[0]) {
            size_t pos = cfg.find("selected=");
            if (pos != std::string::npos) {
                size_t e = cfg.find_first_of("\r\n", pos);
                std::string line = cfg.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
                if (line.size() > 9)
                    snprintf(want, sizeof(want), "%s", line.c_str() + 9);
            }
        }
    }
    if (!want[0]) {
        MonLog("resolve FAIL no-name");
        return false;
    }

    int entryN = 0;
    char entries[80] = {};
    size_t i = 0;
    while (i < cfg.size()) {
        size_t e = cfg.find_first_of("\r\n", i);
        std::string line = cfg.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = (e == std::string::npos) ? cfg.size() : e + 1;

        bool isEntry = false;
        if (MatchRelayLine(line, want, nameOut, nameN, hostOut, hostN, portOut, &isEntry))
            return true;
        if (isEntry) {
            entryN++;
            char ename[32] = {}, ehost[64] = {};
            u16 eport = 0;
            if (RelayParseLine(line.c_str(), ename, sizeof(ename),
                               ehost, sizeof(ehost), &eport) &&
                strlen(entries) + strlen(ename) + 2 < sizeof(entries)) {
                strcat(entries, ename);
                strcat(entries, "|");
            }
        }
    }
    {
        char m[200];
        snprintf(m, sizeof(m), "resolve noname %s n=%d [%s]", want, entryN, entries);
        MonLog(m);
    }
    snprintf(nameOut, nameN, "%s", want);
    return false;
}

/* ------------------------------------------------------------------ */
/* 服务器查询:graphql / rest                                          */
/* ------------------------------------------------------------------ */

struct QueryResult {
    s32 online = -1;
    u32 roomN = 0;
    MonitorRoom rooms[MONITOR_MAX_ROOMS];
};

/* Room-specific advertiseData resolves all-F communication-ID sentinels.
 * Never substitute the local foreground ProgramId for a remote room: a server
 * can list several games, and that fallback would mislabel every such room. */
static void FormatGameName(const char *contentId, const char *advertiseData,
                           const char *hostPlayerName, char *out, size_t n) {
    char up[20] = {};
    size_t len = 0;
    for (const char *p = contentId; p && *p && len < 16; p++) {
        char c = *p;
        if (c >= 'a' && c <= 'f') c = (char)(c - 'a' + 'A');
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))
            up[len++] = c;
    }
    up[len] = 0;
    if (len != 16) {
        snprintf(out, n, "%s", T_STR("未知游戏", "未知遊戲"));
        return;
    }

    bool allOnes = true;
    for (size_t i = 0; i < 16; i++)
        allOnes &= (up[i] == 'F');
    if (allOnes) {
        char resolved[20] = {};
        if (!MonitorTitleResolver::ResolveAdvertisedTitleId(
                advertiseData, hostPlayerName, resolved, sizeof(resolved))) {
            snprintf(out, n, "%s", T_STR("未知游戏", "未知遊戲"));
            return;
        }
        /* Violet also advertises Scarlet's communication ID, so the room
         * payload identifies the shared Scarlet/Violet family, not a version. */
        if (strcmp(resolved, "0100A3D008C5C000") == 0) {
            snprintf(out, n, "%s", T_STR("宝可梦 朱/紫", "寶可夢 朱/紫"));
            return;
        }
        snprintf(up, sizeof(up), "%s", resolved);
    }

    u64 key = TitleKeyPack(up);
    size_t nameLen = 0;
    const char *name = TitleLookup(key, &nameLen);
    if (name && name[0])
        snprintf(out, n, "%s", name);
    else
        snprintf(out, n, "%s", up);      /* 未映射:直接显示标题 ID */
}

/* room 对象数组 -> rooms[](graphql 与 rest 共用字段口径) */
static void ParseRoomArray(const char *arr, QueryResult *qr) {
    const char *p = JsWs(arr);
    if (*p != '[')
        return;
    p = JsWs(p + 1);
    while (qr->roomN < MONITOR_MAX_ROOMS && *p == '{') {
        MonitorRoom &r = qr->rooms[qr->roomN];
        memset(&r, 0, sizeof(r));
        char cid[24] = {};
        char advertiseData[MonitorTitleResolver::kAdvertiseDataMaxHex + 1] = {};
        char hostName[MONITOR_PLAYER_LEN + 1] = {};
        JsonGetStrAny(p, "contentId", "content_id", cid, sizeof(cid));
        JsonGetStrAny(p, "advertiseData", "advertise_data",
                      advertiseData, sizeof(advertiseData));
        JsonGetStrAny(p, "hostPlayerName", "host_player_name",
                      hostName, sizeof(hostName));
        FormatGameName(cid, advertiseData, hostName, r.game, sizeof(r.game));
        JsonGetStrAny(p, "sessionId", "session_id", r.sessionId, sizeof(r.sessionId));

        u32 v = 0;
        const bool hasPlayerCount =
            JsonGetU32(p, "nodeCount", &v) || JsonGetU32(p, "node_count", &v);
        if (hasPlayerCount)
            r.players = v;
        if (JsonGetU32(p, "nodeCountMax", &v) || JsonGetU32(p, "node_count_max", &v))
            r.playersMax = v;

        /* 玩家名单:hostPlayerName 排最前(UI 异色),再收节点名 */
        const char *nodes = JsonGet(p, "nodes");
        if (!nodes)
            nodes = JsonGet(p, "players");   /* rest 服务器的同义字段 */
        if (nodes && *nodes == '[') {
            const char *q = JsWs(nodes + 1);
            while (*q && *q != ']' && r.playerN < MONITOR_MAX_PLAYERS) {
                const char *next = JsonSkip(q, 1);
                if (!next)
                    break;
                if (*q == '{') {
                    const char *connected = JsonGet(q, "isConnected");
                    if (!connected)
                        connected = JsonGet(q, "is_connected");
                    if (MonitorPlayerFilter::IncludeNode(connected != nullptr, connected)) {
                        char nm[MONITOR_PLAYER_LEN] = {};
                        if (JsonGetStrAny(q, "playerName", "player_name", nm, sizeof(nm)) ||
                            JsonGetStr(q, "name", nm, sizeof(nm))) {
                            bool dup = false;
                            for (u8 i = 0; i < r.playerN; i++) {
                                if (strcmp(r.player[i], nm) == 0) {
                                    dup = true;
                                    break;
                                }
                            }
                            if (nm[0] && !dup)
                                snprintf(r.player[r.playerN++], MONITOR_PLAYER_LEN, "%s", nm);
                        }
                    }
                } else if (*q == '"') {
                    char nm[MONITOR_PLAYER_LEN] = {};
                    const char *s2 = q;
                    if (JsonString(&s2, nm, sizeof(nm)) && nm[0])
                        snprintf(r.player[r.playerN++], MONITOR_PLAYER_LEN, "%s", nm);
                }
                q = JsWs(next);
                if (*q == ',') q = JsWs(q + 1);
            }
        }
        /* 房主最前且不重复 */
        if (hostName[0]) {
            bool dup = false;
            for (u8 i = 0; i < r.playerN; i++) {
                if (strcmp(r.player[i], hostName) == 0) {
                    dup = true;
                    break;
                }
            }
            if (!dup && r.playerN < MONITOR_MAX_PLAYERS) {
                memmove(r.player[1], r.player[0], (size_t)r.playerN * MONITOR_PLAYER_LEN);
                snprintf(r.player[0], MONITOR_PLAYER_LEN, "%s", hostName);
                r.playerN++;
            }
        }
        /* The server's count is authoritative. A short-lived/stale nodes array
         * must not display more names than nodeCount says are connected. */
        if (hasPlayerCount && r.players > 0 && r.playerN > r.players) {
            const u8 keep = static_cast<u8>(r.players);
            for (u8 i = keep; i < r.playerN; i++)
                memset(r.player[i], 0, MONITOR_PLAYER_LEN);
            r.playerN = keep;
        }
        /* 人数兜底放到房主处理之后:房主若不在 nodes 里也计入人数 */
        if (r.players == 0)
            r.players = r.playerN ? r.playerN : 1;
        if (r.playersMax == 0)
            r.playersMax = r.players;
        if (r.game[0])
            qr->roomN++;

        const char *next = JsonSkip(p, 0);
        if (!next)
            break;
        next = JsWs(next);
        if (*next != ',')
            break;
        p = JsWs(next + 1);
    }
}

static bool QueryGraphqlRequest(const char *host, u16 port, const char *query,
                                QueryResult *qr, bool *retryLegacy) {
    if (retryLegacy)
        *retryLegacy = false;
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/", host, (unsigned)port);
    std::string payload = "{\"query\":\"";
    payload += query;                    /* 查询串无引号/反斜杠,可直接拼接 */
    payload += "\"}";
    std::string resp;
    if (!HttpPostJson(url, payload.c_str(), &resp, 6000, kMaxHttpBody)) {
        /* g_httpErr >= 500 && < 600 表示 HTTP 4xx (错误码 = 100 + status);
         * 这种响应可能是旧 schema 的字段校验错误,允许旧查询再试一次。 */
        if (retryLegacy && g_httpErr >= 500 && g_httpErr < 600)
            *retryLegacy = true;
        return false;
    }

    /* GraphQL 通常以 HTTP 200 返回字段校验错误;网络超时/断连则直接
     * 交给 REST 回退,避免额外等待一个超时周期。 */
    const char *errors = JsonGet(resp.c_str(), "errors");
    if (errors && *errors == '[' && *JsWs(errors + 1) != ']') {
        if (retryLegacy)
            *retryLegacy = true;
        return false;
    }
    const char *data = JsonGet(resp.c_str(), "data");
    if (!data) {
        if (retryLegacy)
            *retryLegacy = true;
        return false;
    }
    u32 v = 0;
    const char *info = JsonGet(data, "serverInfo");
    if (!info || !JsonGetU32(info, "online", &v)) {
        if (retryLegacy)
            *retryLegacy = true;
        return false;
    }
    qr->online = (s32)v;
    const char *rooms = JsonGet(data, "room");
    if (rooms)
        ParseRoomArray(rooms, qr);
    return true;
}

static bool QueryGraphql(const char *host, u16 port, QueryResult *qr) {
    /* Prefer title data + connected-node flags. On schema validation errors,
     * progressively drop optional fields; transport errors still go straight
     * to REST rather than paying for another timeout. */
    bool retry = false;
    if (QueryGraphqlRequest(host, port, kGraphqlQuery, qr, &retry))
        return true;
    if (!retry)
        return false;

    const char *fallback[] = { kGraphqlQueryNoAdvert,
                               kGraphqlQueryNoConnection,
                               kGraphqlQueryOldest };
    for (const char *query : fallback) {
        bool retryNext = false;
        if (QueryGraphqlRequest(host, port, query, qr, &retryNext))
            return true;
        if (!retryNext)
            return false;
    }
    return false;
}

static bool QueryRest(const char *host, u16 port, QueryResult *qr) {
    char url[160];
    snprintf(url, sizeof(url), "http://%s:%u/info", host, (unsigned)port);
    std::string resp;
    if (!HttpGet(url, &resp, 6000, kMaxHttpBody))
        return false;
    const char *root = JsWs(SkipBom(resp.c_str()));
    u32 v = 0;
    if (JsonGetU32(root, "online", &v) || JsonGetU32(root, "clientCount", &v))
        qr->online = (s32)v;
    const char *rooms = JsonGet(root, "rooms");
    if (rooms)
        ParseRoomArray(rooms, qr);
    return qr->online >= 0;
}

/* ------------------------------------------------------------------ */
/* UDP 主动房间扫描(lan-play 中继协议的 LDN 扫描)                     */
/* ------------------------------------------------------------------ */

/* LanPlayMonitor 的 LDN_SCAN_FRAME 预生成常量:
 *   0x01                              lan-play 原始包 chunk
 *   IPv4(10.13.37.0 -> 10.13.255.255, UDP 11452)
 *   LDN 头 {00 14 45 11} + type=0     扫描请求
 * 校验和已预计算,勿手改。 */
static const u8 kScanFrame[] = {
    0x01, 0x45, 0x00, 0x00, 0x28, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0x01,
    0xAC, 0x0A, 0x0D, 0x25, 0x00, 0x0A, 0x0D, 0xFF, 0xFF, 0x2C, 0xBC, 0x2C,
    0xBC, 0x00, 0x14, 0x28, 0x0F, 0x00, 0x14, 0x45, 0x11, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00,
};

/* ldn_mitm RLE 解压:字面字节;0x00 + count = count 个 0 */
static bool LdnDecompress(const u8 *in, size_t inLen, u8 *out, size_t expect) {
    if (expect == 0 || expect > 8192 || inLen > 8192)
        return false;
    size_t o = 0, i = 0;
    while (i < inLen && o < expect) {
        u8 v = in[i++];
        out[o++] = v;
        if (v == 0) {
            if (i >= inLen)
                return false;
            u8 rep = in[i++];
            if (o + rep > expect)
                return false;
            memset(out + o, 0, rep);
            o += rep;
        }
    }
    return i == inLen && o == expect;
}

/* NetworkInfo(≥0x480)-> 房间条目 + 会话标识(去重用) */
static bool ParseNetworkInfo(const u8 *p, size_t len, MonitorRoom *out,
                             char *sessionTag, size_t tagN) {
    if (len < 0x480)
        return false;
    memset(out, 0, sizeof(*out));
    char cid[20];
    for (int i = 0; i < 8; i++)
        snprintf(cid + i * 2, 3, "%02X", p[7 - i]);   /* 小端 8 字节 -> 标题ID */

    /* NetworkInfo: advertiseDataSize @ 0x26a, data @ 0x26c; the first
     * NodeInfo's playerName (host) starts at 0x74. */
    char advertiseHex[MonitorTitleResolver::kAdvertiseDataMaxHex + 1] = {};
    size_t advertiseSize = (size_t)p[0x26A] | ((size_t)p[0x26B] << 8);
    if (advertiseSize > MonitorTitleResolver::kAdvertiseDataMaxBytes)
        advertiseSize = MonitorTitleResolver::kAdvertiseDataMaxBytes;
    if (0x26C + advertiseSize > len)
        advertiseSize = 0;
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < advertiseSize; i++) {
        const u8 byte = p[0x26C + i];
        advertiseHex[i * 2] = kHex[byte >> 4];
        advertiseHex[i * 2 + 1] = kHex[byte & 0x0F];
    }
    char hostName[MONITOR_PLAYER_LEN + 1] = {};
    memcpy(hostName, p + 0x74, MONITOR_PLAYER_LEN);
    hostName[MONITOR_PLAYER_LEN] = 0;
    FormatGameName(cid, advertiseHex, hostName, out->game, sizeof(out->game));

    u32 maxN = p[0x66];
    u32 cnt = p[0x67];
    if (maxN > 8) maxN = 8;
    if (cnt > 8) cnt = 8;
    out->players = cnt;
    out->playersMax = maxN ? maxN : cnt;
    if (sessionTag && tagN > 0) {
        /* 与 lpm_main.py 一致:sessionId = payload[16:32] 的 hex;
           全零时退回标题ID 做标识。 */
        bool allZero = true;
        for (int i = 0; i < 16; i++) {
            if (p[16 + i]) {
                allZero = false;
                break;
            }
        }
        if (allZero) {
            snprintf(sessionTag, tagN, "%s", cid);
        } else {
            char tag[33];
            for (int i = 0; i < 16; i++)
                snprintf(tag + i * 2, 3, "%02x", p[16 + i]);
            snprintf(sessionTag, tagN, "%s", tag);
        }
        snprintf(out->sessionId, sizeof(out->sessionId), "%s", sessionTag);
    }
    /* 玩家名单:NetworkInfo 的 nodes[8] 带每个节点的用户名(起始 0x68、
     * 步长 0x40:isConnected @+11、userName[33] @+12)。只读取 nodeCount
     * 范围内的槽位;固定数组后面的槽可能保留上一个房间的旧昵称/连接位,
     * 是切换房间或服务器后偶发串名的来源。主机(AP, nodes[0])排最前。 */
    if (len >= 0x68 + (size_t)cnt * 0x40) {
        u8 pn = 0;
        for (u32 i = 0; i < cnt && pn < MONITOR_MAX_PLAYERS; i++) {
            const u8 *nd = p + 0x68 + (size_t)i * 0x40;
            if (!MonitorPlayerFilter::IncludeConnectedSlot(i, cnt, nd[11] == 1))
                continue;
            char nm[MONITOR_PLAYER_LEN];
            memcpy(nm, nd + 12, MONITOR_PLAYER_LEN - 1);
            nm[MONITOR_PLAYER_LEN - 1] = 0;
            if (nm[0]) {
                bool dup = false;
                for (u8 j = 0; j < pn; j++) {
                    if (strcmp(out->player[j], nm) == 0) {
                        dup = true;
                        break;
                    }
                }
                if (!dup)
                    snprintf(out->player[pn++], MONITOR_PLAYER_LEN, "%s", nm);
            }
        }
        out->playerN = pn;
    }
    return out->game[0] != 0;
}

/* IPv4 + UDP(11452) + LDN 响应 */
static bool ParseLdnResponse(const u8 *pkt, size_t len, MonitorRoom *room,
                             char *sessionTag, size_t tagN) {
    if (len < 20 || (pkt[0] >> 4) != 4)
        return false;
    size_t ihl = (size_t)(pkt[0] & 0x0F) * 4;
    if (ihl < 20 || len < ihl + 8)
        return false;
    if (pkt[9] != 17)                      /* UDP */
        return false;
    const u8 *udp = pkt + ihl;
    u32 sport = ((u32)udp[0] << 8) | udp[1];
    u32 dport = ((u32)udp[2] << 8) | udp[3];
    if (sport != 11452 || dport != 11452)
        return false;
    size_t udpLen = ((size_t)udp[4] << 8) | udp[5];
    if (udpLen < 8)
        return false;
    size_t body = udpLen - 8;
    if (body > len - ihl - 8)
        body = len - ihl - 8;
    const u8 *ldn = udp + 8;
    if (body < 12)
        return false;
    if (ldn[0] != 0x00 || ldn[1] != 0x14 || ldn[2] != 0x45 || ldn[3] != 0x11)
        return false;
    if (ldn[4] != 1)                       /* type 1 = NetworkInfo 响应 */
        return false;
    size_t bodyLen   = (size_t)ldn[6] | ((size_t)ldn[7] << 8);
    size_t decompLen = (size_t)ldn[8] | ((size_t)ldn[9] << 8);
    if (bodyLen > body - 12)
        return false;

    /* This parser is only called by the single monitor worker. Keep the
     * decompression scratch off its stack; older ovlloader builds provide a
     * smaller/less forgiving thread stack than current builds. */
    static u8 tmp[8192];
    const u8 *info = ldn + 12;
    size_t infoLen = bodyLen;
    if (ldn[5] == 1) {                     /* 压缩 */
        if (!LdnDecompress(info, bodyLen, tmp, decompLen))
            return false;
        info = tmp;
        infoLen = decompLen;
    }
    return ParseNetworkInfo(info, infoLen, room, sessionTag, tagN);
}

/* type=3 分片重组:同 src+ident 的分片按 pmtu 偏移拼接 */
struct FragSlot {
    bool used;
    u32 srcIp;
    u16 ident;
    u8  total;
    u16 pmtu;
    u16 dataLen;
    u64 partsMask;                  /* 已收分片位图(total ≤ 64) */
    u8  data[8192];
};
static FragSlot g_frags[4];

static bool FragAdd(const u8 *frame, size_t len, const u8 **outPkt, size_t *outLen) {
    if (len < 16)
        return false;
    u32 srcIp;
    memcpy(&srcIp, frame, 4);
    u16 ident = (u16)(((u32)frame[8] << 8) | frame[9]);
    u8 part = frame[10];
    u8 total = frame[11];
    u16 partLen = (u16)((u32)frame[12] | ((u32)frame[13] << 8));
    u16 pmtu = (u16)(((u32)frame[14] << 8) | frame[15]);
    if (total == 0 || total > 64 || part >= total || pmtu == 0)
        return false;
    if ((size_t)partLen > len - 16)
        return false;
    size_t end = (size_t)part * pmtu + partLen;
    if (end == 0 || end > 8192)
        return false;

    FragSlot *slot = nullptr;
    for (auto &f : g_frags) {
        if (f.used && f.srcIp == srcIp && f.ident == ident) {
            slot = &f;
            break;
        }
    }
    if (!slot) {
        for (auto &f : g_frags) {
            if (!f.used) {
                slot = &f;
                break;
            }
        }
        if (!slot)
            return false;
        memset(slot, 0, sizeof(*slot));
        slot->used = true;
        slot->srcIp = srcIp;
        slot->ident = ident;
        slot->total = total;
        slot->pmtu = pmtu;
    }
    if (slot->total != total || slot->pmtu != pmtu) {
        slot->used = false;
        return false;
    }
    memcpy(slot->data + (size_t)part * pmtu, frame + 16, partLen);
    if ((size_t)part * pmtu + partLen > slot->dataLen)
        slot->dataLen = (u16)end;
    slot->partsMask |= 1ull << part;

    u64 need = (total == 64) ? ~0ull : ((1ull << total) - 1);
    if (slot->partsMask != need)
        return false;
    *outPkt = slot->data;
    *outLen = slot->dataLen;
    slot->used = false;
    return true;
}

static void UdpRoomScan(const char *host, u16 port, QueryResult *qr) {
    struct sockaddr_in sa;
    if (!HttpResolve(host, &sa))
        return;
    sa.sin_port = htons(port);

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return;
    }

    /* 排空积压 */
    HttpSetNonBlock(fd, true);
    u8 drain[2048];
    while (recv(fd, drain, sizeof(drain), 0) > 0) {}
    HttpSetNonBlock(fd, false);

    struct timeval rtv;
    rtv.tv_sec = 0;
    rtv.tv_usec = 200 * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

    memset(g_frags, 0, sizeof(g_frags));

    Deadline start = std::chrono::steady_clock::now();
    Deadline deadline = start + std::chrono::milliseconds(1500);
    Deadline nextSend = start;

    char seen[kMaxScanRooms][36];
    u32 seenN = 0;

    u8 frame[2048];
    while (std::chrono::steady_clock::now() < deadline && qr->roomN < MONITOR_MAX_ROOMS) {
        Deadline now = std::chrono::steady_clock::now();
        if (now >= nextSend) {
            send(fd, kScanFrame, sizeof(kScanFrame), 0);
            nextSend = now + std::chrono::milliseconds(700);
        }
        ssize_t n = recv(fd, frame, sizeof(frame), 0);
        if (n <= 0)
            continue;
        const u8 *pkt = nullptr;
        size_t pktLen = 0;
        if (frame[0] == 1) {
            pkt = frame + 1;
            pktLen = (size_t)n - 1;
        } else if (frame[0] == 3) {
            if (!FragAdd(frame + 1, (size_t)n - 1, &pkt, &pktLen))
                continue;
        } else {
            continue;
        }

        MonitorRoom room;
        memset(&room, 0, sizeof(room));
        char tag[36] = {};
        if (!ParseLdnResponse(pkt, pktLen, &room, tag, sizeof(tag)))
            continue;

        bool dup = false;
        for (u32 i = 0; i < seenN; i++) {
            if (strcmp(seen[i], tag) == 0) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (seenN < kMaxScanRooms)
            snprintf(seen[seenN++], sizeof(seen[0]), "%s", tag);

        qr->rooms[qr->roomN++] = room;
    }
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 房间 5 秒 TTL 缓存                                                   */
/* ------------------------------------------------------------------ */

/* 查询/UDP 扫描都有偶发丢包与瞬间空列表(日志里 scan rooms=0/1 交替),
 * 直接把当次结果写进快照会让房间列表每秒闪一下。
 * 缓存以 sessionId 为键:命中即刷新时间戳并更新房间数据;当次没出现
 * 的条目再保留 5 秒才删除;服务器身份变化时整体清空。
 * 在线人数/房间数量同口径:查询失败时沿用最近一次成功值,超过 5 秒
 * 仍失败才退回「未知」。 */
static constexpr auto kRoomTtl = std::chrono::seconds(5);

struct RoomCacheEnt {
    bool used = false;
    char key[40] = {};
    Deadline lastSeen{};
    MonitorRoom room;
};
static RoomCacheEnt g_roomCache[MONITOR_MAX_ROOMS];
static char g_cacheServer[128] = {};    /* 缓存归属的服务器 host:port */
static Deadline g_lastOk{};             /* 最近一次查询成功时刻 */
static bool  g_haveLastOk = false;
static s32   g_lastOnline = -1;
static s32   g_lastRoomCount = -1;

static void RoomCacheKey(const MonitorRoom &r, char *out, size_t n) {
    if (r.sessionId[0])
        snprintf(out, n, "%s", r.sessionId);
    else
        snprintf(out, n, "g:%s", r.game);
}

static void RoomCacheReset() {
    for (auto &e : g_roomCache)
        e.used = false;
    g_cacheServer[0] = 0;
    g_haveLastOk = false;
    g_lastOnline = -1;
    g_lastRoomCount = -1;
}

static void RoomCacheUpsert(const MonitorRoom &r, const Deadline &now) {
    char key[40];
    RoomCacheKey(r, key, sizeof(key));
    int freeSlot = -1, oldest = -1;
    for (u32 i = 0; i < MONITOR_MAX_ROOMS; i++) {
        RoomCacheEnt &e = g_roomCache[i];
        if (!e.used) {
            if (freeSlot < 0)
                freeSlot = (int)i;
            continue;
        }
        if (strcmp(e.key, key) == 0) {
            e.room = r;
            e.lastSeen = now;
            return;
        }
        if (oldest < 0 || e.lastSeen < g_roomCache[oldest].lastSeen)
            oldest = (int)i;
    }
    int slot = (freeSlot >= 0) ? freeSlot : oldest;
    if (slot < 0)
        return;
    RoomCacheEnt &e = g_roomCache[slot];
    e.used = true;
    snprintf(e.key, sizeof(e.key), "%s", key);
    e.lastSeen = now;
    e.room = r;
}

static void RoomCacheExpire(const Deadline &now) {
    for (auto &e : g_roomCache) {
        if (e.used && (now - e.lastSeen) > kRoomTtl)
            e.used = false;
    }
}

/* ------------------------------------------------------------------ */
/* 刷新流程                                                            */
/* ------------------------------------------------------------------ */

/* TLS/curl and JSON parsing can nest more deeply on older runtimes. Reserve
 * 256 KiB instead of 64 KiB; this is static overlay memory, not heap. */
alignas(0x1000) static u8 g_workerStack[0x40000];

static void LogWorkerStack(const char *phase) {
    volatile u8 probe = 0;
    const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(g_workerStack);
    const std::uintptr_t end = base + sizeof(g_workerStack);
    const std::uintptr_t current = reinterpret_cast<std::uintptr_t>(&probe);
    char msg[128];
    if (current >= base && current < end) {
        snprintf(msg, sizeof(msg), "%s worker-stack used=%uK free=%uK", phase,
                 static_cast<unsigned>((end - current) / 1024),
                 static_cast<unsigned>((current - base) / 1024));
    } else {
        /* Never print a wrapped/negative "free stack" number. This also
         * diagnoses a runtime that did not use the supplied stack buffer. */
        snprintf(msg, sizeof(msg), "%s worker-stack range-mismatch sp=%p range=%p..%p",
                 phase, reinterpret_cast<void *>(current),
                 reinterpret_cast<void *>(base), reinterpret_cast<void *>(end));
    }
    MonLog(msg);
}

static void DoRefresh(const char *nameOverride, bool reloadSubs,
                      MonitorTargetGeneration::Value targetGen) {
    bool staleTarget = false;
    {
        std::scoped_lock lk(g_mutex);
        staleTarget = !g_targetGen.IsCurrent(targetGen);
        if (!staleTarget) {
            g_snap.busy = true;
            /* 同一服务器的轮询继续展示上一结果;切服时由请求入口先清空。 */
            snprintf(g_snap.status, sizeof(g_snap.status), "%s",
                     T_STR("正在查询…", "正在查詢…"));
        }
    }
    if (staleTarget) {
        MonLog("refresh-discard stale-target-before-start");
        return;
    }

    LogWorkerStack("refresh-start");
    EnsureSockets();

    static QueryResult qr;            /* 约 8.7KB:static,不占工作线程栈 */
    qr = QueryResult{};

    /* 1. 当前选择的服务器 */
    char name[48] = {}, host[64] = {};
    u16 port = 11451;
    bool haveServer = ResolveSelectedServer(nameOverride, name, sizeof(name),
                                            host, sizeof(host), &port);
    {
        char m[160];
        if (haveServer)
            snprintf(m, sizeof(m), "resolve ok gen=%u %s %s:%u",
                     static_cast<unsigned>(targetGen), name, host, (unsigned)port);
        else
            snprintf(m, sizeof(m), "resolve FAIL gen=%u %s",
                     static_cast<unsigned>(targetGen),
                     nameOverride ? nameOverride : "(selected)");
        MonLog(m);
    }

    /* Publish/clear only if this request still targets the selected server. */
    {
        char ident[128];
        snprintf(ident, sizeof(ident), "%s:%u", host, (unsigned)port);
        std::scoped_lock lk(g_mutex);
        staleTarget = !g_targetGen.IsCurrent(targetGen);
        if (!staleTarget) {
            snprintf(g_snap.server, sizeof(g_snap.server), "%.47s",
                     name[0] ? name : (host[0] ? host : "?"));
            if (strcmp(ident, g_cacheServer) != 0) {
                RoomCacheReset();
                g_snap.online = -1;
                g_snap.roomCount = -1;
                g_snap.roomN = 0;
                memset(g_snap.rooms, 0, sizeof(g_snap.rooms));
                g_snap.version++;
            }
            snprintf(g_cacheServer, sizeof(g_cacheServer), "%s", ident);
        }
    }
    if (staleTarget) {
        MonLog("refresh-discard stale-target-after-resolve");
        return;
    }

    /* 2. 订阅(chinese_db.json):开盖/手动刷新时拉取(周期轮询跳过),
     *    失败退回本地缓存;servers.json 已彻底移除,服务器列表只读 relay.cfg。
     *    下载走 HTTPS 加速镜像(https://v6.gh-proxy.org/),删了缓存也能重拉 */
    if (reloadSubs) {
        MonLog("subs-start");
        std::string chineseJson;
        bool gotChinese = DownloadSubscription(kSubChinesePath, &chineseJson);
        if (gotChinese)
            SaveFile(kSaveChinesePath, chineseJson);
        else {
            gotChinese = LoadFile(kSaveChinesePath, &chineseJson);
            MonLog(gotChinese ? "chinese: use cache" : "chinese: no cache");
        }
        if (gotChinese) {
            EnsureTitleTables();
            if (g_titleKey) {
                g_titleArenaUsed = 0;
                memset(g_titleKey, 0, kTitleHashSize * sizeof(u64));
                /* 先释放 JSON 字符串(约 0.5MB),再从落盘文件流式解析,
                 * 避免字符串与标题表同驻把 ovlloader 的堆挤爆。 */
                std::string().swap(chineseJson);
                if (!ParseChineseDbFile(kSaveChinesePath))
                    MonLog("chinese_db: open file failed");
            }
        }
    }

    if (!haveServer) {
        bool stale = false;
        {
            std::scoped_lock lk(g_mutex);
            stale = !g_targetGen.IsCurrent(targetGen);
            if (!stale) {
                snprintf(g_snap.status, sizeof(g_snap.status), "%s",
                         T_STR("找不到选中的服务器", "找不到選中的伺服器"));
                g_snap.busy = false;
                g_snap.version++;
            }
        }
        if (stale)
            MonLog("refresh-discard stale-target-no-server");
        return;
    }

    if (!g_targetGen.IsCurrent(targetGen)) {
        MonLog("refresh-discard stale-target-before-query");
        return;
    }

    /* 4. 查询:自动识别 —— graphql 优先(含房间),失败再 rest
     *    (servers.json 已移除,不再按 host:port 查类型表) */
    bool ok = QueryGraphql(host, port, &qr);
    if (!ok)
        ok = QueryRest(host, port, &qr);
    {
        char m[56];
        snprintf(m, sizeof(m), "query auto e=%d on=%d rooms=%u",
                 g_httpErr, qr.online, (unsigned)qr.roomN);
        MonLog(m);
    }

    /* 6. 接口不给房间时,用 UDP 主动扫描补房间(rest 1.0 服务器) */
    if (ok && qr.roomN == 0) {
        UdpRoomScan(host, port, &qr);
        char m[24];
        snprintf(m, sizeof(m), "scan rooms=%u", (unsigned)qr.roomN);
        MonLog(m);
    }

    /* 7. 并入房间 5 秒 TTL 缓存后写快照:当次查到的房间刷新时间戳,
     *    没查到的旧房间再保留 5 秒,列表不再随单次丢包闪烁。 */
    u32 mlogN = 0;
    s32 mlogOn = -1;
    bool staleAfterQuery = false;
    {
        const Deadline now = std::chrono::steady_clock::now();
        std::scoped_lock lk(g_mutex);
        staleAfterQuery = !g_targetGen.IsCurrent(targetGen);
        if (!staleAfterQuery) {
            if (ok) {
                for (u32 i = 0; i < qr.roomN; i++)
                    RoomCacheUpsert(qr.rooms[i], now);
                g_lastOk = now;
                g_haveLastOk = true;
            }
            RoomCacheExpire(now);

            u32 n = 0;
            for (u32 i = 0; i < MONITOR_MAX_ROOMS; i++) {
                if (g_roomCache[i].used)
                    g_snap.rooms[n++] = g_roomCache[i].room;
            }
            g_snap.roomN = n;
            const bool fresh = g_haveLastOk && (now - g_lastOk) <= kRoomTtl;
            if (ok) {
                g_snap.online = qr.online;
                g_snap.roomCount = (s32)n;
                g_lastOnline = qr.online;
                g_lastRoomCount = (s32)n;
            } else if (fresh) {
                /* 瞬间失败:5 秒内沿用最近一次成功的在线/房间数 */
                g_snap.online = g_lastOnline;
                g_snap.roomCount = g_lastRoomCount;
            } else {
                g_snap.online = -1;
                g_snap.roomCount = -1;
            }
            if (!ok) {
                if (!g_sockInited)
                    snprintf(g_snap.status, sizeof(g_snap.status), "%s",
                             T_STR("网络初始化失败", "網絡初始化失敗"));
                else
                    snprintf(g_snap.status, sizeof(g_snap.status), "%s e=%d",
                             T_STR("查询失败", "查詢失敗"), g_httpErr);
            } else if (n == 0) {
                snprintf(g_snap.status, sizeof(g_snap.status), "%s",
                         T_STR("暂无房间", "暫無房間"));
            } else {
                g_snap.status[0] = 0;
            }
            g_snap.busy = false;
            g_snap.version++;
            mlogN = n;
            mlogOn = g_snap.online;
        }
    }
    if (staleAfterQuery) {
        MonLog("refresh-discard stale-target-after-query");
        return;
    }
    {
        char m[48];
        snprintf(m, sizeof(m), "cache rooms=%u on=%d", (unsigned)mlogN, (int)mlogOn);
        MonLog(m);
    }
}

/* ------------------------------------------------------------------ */
/* 后台线程(libnx 线程,风格同 libtesla)                               */
/* ------------------------------------------------------------------ */


static void MonitorWorker(void *) {
    while (!g_exit.load()) {
        u32 want = g_reqGen.load();
        if (want != g_doneGen) {
            g_doneGen = want;
            char name[48];
            bool subs;
            MonitorTargetGeneration::Value targetGen;
            {
                std::scoped_lock lk(g_mutex);
                memcpy(name, g_pendingName, sizeof(name));
                subs = g_pendingSubs;
                g_pendingSubs = false;
                targetGen = g_targetGen.Capture();
            }
            DoRefresh(name[0] ? name : nullptr, subs, targetGen);
            MonLog("refresh-done");
        } else {
            svcSleepThread(50'000'000);      /* 50ms */
        }
    }
}

void MonitorSetTraditional(bool traditional) {
    g_traditional.store(traditional);
}

/* 标题表延迟到首次解析时才分配(约 0.5MB),避免在 overlay 刚打开、
 * 渲染器 frameBuffer(约 1.3MB)分配之前就把 ovlloader 仅几 MB 的堆挤爆。 */
static void EnsureTitleTables() {
    if (g_titleKey)
        return;
    g_titleKey   = (u64 *)calloc(kTitleHashSize, sizeof(u64));
    g_titleOff   = (u32 *)calloc(kTitleHashSize, sizeof(u32));
    g_titleLen   = (u16 *)calloc(kTitleHashSize, sizeof(u16));
    g_titleArena = (char *)calloc(kTitleArena, 1);
    if (!g_titleKey || !g_titleOff || !g_titleLen || !g_titleArena) {
        free(g_titleKey); free(g_titleOff); free(g_titleLen); free(g_titleArena);
        g_titleKey = nullptr; g_titleOff = nullptr;
        g_titleLen = nullptr; g_titleArena = nullptr;
    }
}

void MonitorInit() {
    if (g_inited)
        return;
    g_inited = true;
    g_exit.store(false);
    g_pendingName[0] = 0;
    EnsureSd();
    mkdir("sdmc:/config", 0777);
    mkdir(kSaveDir, 0777);
    MonLog("worker-init");
    ProbeHeap();

    Result rc = threadCreate(&g_thread, MonitorWorker, nullptr, g_workerStack,
                             sizeof(g_workerStack), 0x2C, -2);
    if (R_SUCCEEDED(rc))
        threadStart(&g_thread);
}

void MonitorExit() {
    if (!g_inited)
        return;
    g_exit.store(true);
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    if (g_sockInited) {
        socketExit();
        g_sockInited = false;
    }
    g_inited = false;
}

/* Must be called with g_mutex held. Detach the published room list from the
 * previous target immediately, before the blocking worker starts its query. */
static void ResetSnapshotForRequestLocked(const char *serverName,
                                          const char *status) {
    snprintf(g_snap.server, sizeof(g_snap.server), "%.47s",
             serverName ? serverName : "");
    g_snap.online = -1;
    g_snap.roomCount = -1;
    g_snap.roomN = 0;
    memset(g_snap.rooms, 0, sizeof(g_snap.rooms));
    snprintf(g_snap.status, sizeof(g_snap.status), "%s", status);
    g_snap.busy = true;
    g_snap.version++;
}

void MonitorRequestRefresh(const char *serverName) {
    std::scoped_lock lk(g_mutex);
    snprintf(g_pendingName, sizeof(g_pendingName), "%s", serverName ? serverName : "");
    g_pendingSubs = true;      /* 开盖/手动刷新:同时重新拉订阅缓存 */
    g_targetGen.Advance();
    ResetSnapshotForRequestLocked(
        serverName, T_STR("正在刷新服务器…", "正在刷新伺服器…"));
    g_reqGen.fetch_add(1);
}

void MonitorRequestSwitch(const char *serverName) {
    std::scoped_lock lk(g_mutex);
    snprintf(g_pendingName, sizeof(g_pendingName), "%s", serverName ? serverName : "");
    g_targetGen.Advance();
    ResetSnapshotForRequestLocked(
        serverName, T_STR("正在切换服务器…", "正在切換伺服器…"));
    g_reqGen.fetch_add(1);
}

void MonitorRequestPoll() {
    /* 周期刷新(与延迟 ping 同节奏):不动目标、不拉订阅 */
    g_reqGen.fetch_add(1);
}

void MonitorCopySnapshot(MonitorSnapshot *out) {
    std::scoped_lock lk(g_mutex);
    *out = g_snap;
}
