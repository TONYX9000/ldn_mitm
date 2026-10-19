#define TESLA_INIT_IMPL // If you have more than one file using the tesla header, only define this in the main one
#include <tesla.hpp>    // The Tesla Header
#include "ldn.h"
#include "monitor.hpp"  // 房间/在线人数监控（Zero, GPL-2.0）
#include <vector>
#include <string>

enum class State {
    Uninit,
    Error,
    Loaded,
};

Service g_ldnSrv;
LdnMitmConfigService g_ldnConfig;
State g_state;
char g_version[32];

/* Brand string shown in the frame header. Must outlive createUI(), because
   OverlayFrame keeps the pointer instead of copying it. */
char g_brand[64] = "阿东版";   // overwritten by detectLanguage() at startup

std::vector<tsl::elm::ListItem*> g_serverItems;

/* Latency value currently painted on the server rows. Lives at file scope
   rather than as a function-local static so createUI() can invalidate it:
   rebuilt rows start with a bare "*", and a stale cache made update() skip
   painting them entirely. */
u32 g_shown_latency = 0xFFFFFFFFu;

/* Traditional / Simplified Chinese switch. The overlay used to hardcode
   Simplified strings, which left Traditional Chinese consoles with the
   wrong glyphs. libtesla already loads both CJK shared fonts, so all we
   need is the console's system language. */
static bool g_isTraditional = false;

#define T(hans, hant) (g_isTraditional ? (hant) : (hans))

/* The list currently on screen, plus the "restore defaults" row. Both are
   needed after a server is added/removed: libtesla has no per-item refresh,
   so the whole list is rebuilt in place (see rebuildUi()). */
class RebuildList : public tsl::elm::List {
public:
    /* List::clear() resets the scroll position to the top on the frame it
       rebuilds, which made the menu visibly bounce back to the first entry
       before re-focusing the deleted row. Snap straight to the row that is
       about to be focused instead, so the rebuilt list appears in place. */
    void snapToFocus() {
        this->m_offset = this->m_nextOffset;
        this->invalidate();
    }
};

static RebuildList *g_list = nullptr;
static tsl::elm::ListItem *g_restoreItem = nullptr;
static bool g_restoreArmed = false;

/* Rows only enter the List on the next draw, so a rebuilt list cannot take
   focus immediately. update() re-applies this once the target is really in
   the list, which keeps the cursor on the row the user was working on
   instead of snapping back to the first entry. */
static tsl::elm::Element *g_focus_after_rebuild = nullptr;
static int g_focus_retries = 0;

/* Set by the add page. The main list is not the top Gui while that page is
   open, so it rebuilds on the first frame it becomes visible again. */
static bool g_main_list_dirty = false;
static tsl::elm::ListItem *g_addServerItem = nullptr;

/* --- 房间监控 ---------------------------------------------------------
   数据抓取在 monitor.cpp 的后台线程里完成，UI 只拷贝快照。
   GPL-2.0, 来自 Zero（jieluojun/ldn_mitm）的 patch 0010；保留署名与鸣谢。 */
static bool g_monitor_inited = false;
static tsl::elm::ListItem *g_roomSummaryItem = nullptr;

static void EnsureMonitor() {
    if (g_monitor_inited) {
        return;
    }
    g_monitor_inited = true;
    MonitorInit();
    MonitorInitSockets();
    MonitorSetTraditional(g_isTraditional);
}

static void GetSelectedServerName(char *out, size_t n) {
    out[0] = '\0';
    u32 sel = 0;
    if (R_SUCCEEDED(ldnMitmGetSelectedRelayServer(&g_ldnConfig, &sel))) {
        char name[32] = {};
        ldnMitmGetRelayServerName(&g_ldnConfig, sel, name);
        snprintf(out, n, "%s", name);
    }
}

/* 房间功能额外要 ~0.5MB 游戏名库；19.0.1 及以下的旧系统/旧 ovlloader
   堆太小，打开插件就会崩。旧系统直接隐藏房间入口，其余功能照常。
   hosversionGet()==0 表示读不到版本，按新系统处理（不误伤）。 */
static bool RoomsSupported() {
    const u32 v = hosversionGet();
    if (v != 0) {
        return !hosversionBefore(20, 0, 0);
    }
    /* 版本读不到时用堆探测兜底：连 600KB 都拿不出来，肯定放不下游戏名库。 */
    void *probe = malloc(600 * 1024);
    if (probe == nullptr) {
        return false;
    }
    free(probe);
    return true;
}

static void buildList(tsl::elm::List *list);
static void rebuildUi();

static void focusAfterRebuild(tsl::elm::Element *element) {
    g_focus_after_rebuild = element;
    g_focus_retries = 10;
}

class ConfigToggleListItem : public tsl::elm::ToggleListItem {
public:
    ConfigToggleListItem(const char *label,
                         Result (*get)(LdnMitmConfigService *, u32 *),
                         Result (*set)(LdnMitmConfigService *, u32))
        : ToggleListItem(label, false) {
        u32 enabled = 0;
        if (R_FAILED(get(&g_ldnConfig, &enabled))) {
            g_state = State::Error;
        }
        this->setState(enabled);

        this->setStateChangedListener([set](bool enabled) {
            if (R_FAILED(set(&g_ldnConfig, enabled))) {
                g_state = State::Error;
            }
        });
    }
};

static void detectLanguage()
{
    /* setGetSystemLanguage returns the language as an 8-byte ASCII code
       (e.g. "zh-Hans" / "zh-Hant" / "en-US") packed into a u64. */
    u64 langCode = 0;
    if (R_SUCCEEDED(setInitialize())) {
        setGetSystemLanguage(&langCode);
        setExit();
    }
    char code[9] = {};
    memcpy(code, &langCode, 8);
    g_isTraditional = (strncmp(code, "zh-Hant", 7) == 0);

    strcpy(g_brand, g_isTraditional ? "阿東版" : "阿东版");
}

class MainGui : public tsl::Gui {
public:
    MainGui() { }

    virtual tsl::elm::Element* createUI() override {
        // This overlay ships inside a Chinese integration package and the
        // bundled font has CJK glyphs, so the UI is localised in place.
        // The header subtitle carries the release brand; the full version
        // string is repeated in the T("关于", "關於") block at the bottom.
        auto frame = new tsl::elm::OverlayFrame(T("联机插件", "連線插件"), g_brand);

        g_list = new RebuildList();
        buildList(g_list);

        // 房间监控改为进入房间页时才启动：旧系统（19.0.1 及以下）的
        // overlay 堆太小，一开主菜单就加载 0.5MB 游戏名库会直接崩。
        frame->setContent(g_list);

        // Every item was just created with a bare "*". Force the first
        // update() to paint the latency text even if the reading has not
        // changed since the last time the overlay was open -- otherwise the
        // cached value made update() return early and the label stayed at
        // just "*" with no "--" and no number.
        g_shown_latency = 0xFFFFFFFFu;

        return frame;
    }

    // Called once every frame to update values
    virtual void update() override {
        // Coming back from the add page: the relay list changed while this
        // Gui sat underneath it, so rebuild now that it is on top again.
        if (g_main_list_dirty) {
            g_main_list_dirty = false;
            rebuildUi();
            focusAfterRebuild(g_addServerItem);
        }

        // Put the cursor back on the row the user was on before the list was
        // rebuilt (delete / restore). Runs before draw, so the target is only
        // found once List::draw() has inserted the freshly built rows.
        if (g_focus_after_rebuild != nullptr) {
            if (g_list != nullptr) {
                const s32 idx = g_list->getIndexInList(g_focus_after_rebuild);
                if (idx >= 0) {
                    g_list->setFocusedIndex(static_cast<u32>(idx));
                    g_list->snapToFocus();
                    this->requestFocus(g_focus_after_rebuild, tsl::FocusDirection::None);
                    g_focus_after_rebuild = nullptr;
                } else if (--g_focus_retries <= 0) {
                    g_focus_after_rebuild = nullptr;
                }
            } else {
                g_focus_after_rebuild = nullptr;
            }
        }

        // Cancel an armed "restore defaults" if the user moved the cursor
        // away, so the row never stays in a half-confirmed state.
        if (g_restoreArmed && g_restoreItem != nullptr &&
            this->getFocusedElement() != g_restoreItem) {
            g_restoreArmed = false;
            g_restoreItem->setValue("");
        }

        // Show the live relay latency next to the selected server.
        // Only the selected server is connected, so it is the only one
        // whose round-trip time we can actually know.
        if (g_state != State::Loaded || g_serverItems.empty()) {
            return;
        }
        u32 ms = 0;
        if (R_FAILED(ldnMitmGetRelayLatency(&g_ldnConfig, &ms))) {
            return;
        }
        // Rebuild the label only when the measurement actually changes.
        if (ms == g_shown_latency) {
            return;
        }
        g_shown_latency = ms;

        // ListItem::setValue(value, faint): faint=false renders in the
        // highlight colour, faint=true in the dim description colour. That
        // gives a green / grey traffic light without a custom element.
        char buf[48];
        bool faint = false;
        if (ms == 0) {
            snprintf(buf, sizeof(buf), T("* 延迟 --", "* 延遲 --"));
            faint = true;
        } else if (ms > 999) {
            snprintf(buf, sizeof(buf), T("* 延迟 >999ms", "* 延遲 >999ms"));
            faint = true;
        } else {
            snprintf(buf, sizeof(buf), T("* 延迟 %ums", "* 延遲 %ums"), ms);
        }
        for (size_t k = 0; k < g_serverItems.size(); k++) {
            if (!g_serverItems[k]->getValue().empty()) {
                g_serverItems[k]->setValue(buf, faint);
            }
        }
    }

    // Called once every frame to handle inputs not handled by other UI elements
    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos, HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) {
        return false;
    }
};

/* Preset picker. libtesla has no text input (Tesla cannot open swkbd from a
   LibraryApplet), so "add" means putting one of the factory relays back.
   Entries that are already configured say 已添加 and do nothing. */
class AddServerGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        u32 builtin = 0;
        if (R_FAILED(ldnMitmGetBuiltinRelayServerCount(&g_ldnConfig, &builtin))) {
            builtin = 0;
        }
        u32 current = 0;
        ldnMitmGetRelayServerCount(&g_ldnConfig, &current);

        // The subtitle carries the capacity state: the list is capped at
        // LDN_MAX_SERVERS, so backups can only be added after freeing a slot.
        char subtitle[96];
        if (current >= LDN_MAX_SERVERS) {
            snprintf(subtitle, sizeof(subtitle),
                     T("已满 %u/%u · 先删一台再用备选", "已滿 %u/%u · 先刪一台再用備選"),
                     static_cast<unsigned>(current), static_cast<unsigned>(LDN_MAX_SERVERS));
        } else {
            snprintf(subtitle, sizeof(subtitle),
                     T("内置 %u 台 · 当前 %u/%u", "內建 %u 台 · 目前 %u/%u"),
                     static_cast<unsigned>(builtin), static_cast<unsigned>(current),
                     static_cast<unsigned>(LDN_MAX_SERVERS));
        }
        auto frame = new tsl::elm::OverlayFrame(T("添加服务器", "新增伺服器"), subtitle);
        auto *list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(T("内置服务器（按 A 添加）", "內建伺服器（按 A 新增）")));

        /* 所有行共享同一个计数：添加成功后同步刷新副标题和容量判断，
           否则「当前 X/32」要退出重进才会更新。 */
        auto count = std::make_shared<u32>(current);

        for (u32 i = 0; i < builtin; i++) {
            LdnRelayServerInfo info = {};
            if (R_FAILED(ldnMitmGetBuiltinRelayServerInfo(&g_ldnConfig, i, &info))) {
                continue;
            }
            bool present = false;
            for (u32 k = 0; k < current; k++) {
                LdnRelayServerInfo cur = {};
                if (R_SUCCEEDED(ldnMitmGetRelayServerInfo(&g_ldnConfig, k, &cur)) &&
                    strcmp(cur.host, info.host) == 0 && cur.port == info.port) {
                    present = true;
                    break;
                }
            }

            /* Do not paint host:port here. libtesla's ListItem does not
               truncate the value, and a long address (e.g.
               muitxobem-lanplay.ddns.net:11451) is wider than the row, so it
               was drawn on top of the left-hand name. A short status word
               keeps the name readable; the addresses live in the README. */
            auto *item = new tsl::elm::ListItem(info.name);
            item->setValue(present ? T("已添加", "已新增") : T("可添加", "可新增"), present);

            auto added = std::make_shared<bool>(present);
            item->setClickListener([item, info, added, count, frame, builtin](u64 keys) {
                if (!(keys & HidNpadButton_A)) {
                    return false;
                }
                if (*added) {
                    return true;
                }
                if (*count >= LDN_MAX_SERVERS) {
                    item->setValue(T("先删一台", "先刪一台"), true);
                    return true;
                }
                if (R_SUCCEEDED(ldnMitmAddRelayServer(&g_ldnConfig, &info))) {
                    *added = true;
                    (*count)++;
                    item->setValue(T("已添加", "已新增"), true);
                    /* 实时刷新副标题里的「当前 X/32」。 */
                    char sub[96];
                    if (*count >= LDN_MAX_SERVERS) {
                        snprintf(sub, sizeof(sub),
                                 T("已满 %u/%u · 先删一台再用备选", "已滿 %u/%u · 先刪一台再用備選"),
                                 static_cast<unsigned>(*count),
                                 static_cast<unsigned>(LDN_MAX_SERVERS));
                    } else {
                        snprintf(sub, sizeof(sub),
                                 T("内置 %u 台 · 当前 %u/%u", "內建 %u 台 · 目前 %u/%u"),
                                 static_cast<unsigned>(builtin), static_cast<unsigned>(*count),
                                 static_cast<unsigned>(LDN_MAX_SERVERS));
                    }
                    frame->setSubtitle(sub);
                    g_main_list_dirty = true;
                } else {
                    item->setValue(T("添加失败", "新增失敗"), true);
                }
                return true;
            });
            list->addItem(item);
        }

        frame->setContent(list);
        return frame;
    }
};


/* 房间列表页：显示当前中继服务器的在线人数与游戏房间。
   数据来自 monitor 模块，每 5 秒轮询一次；列表在快照版本变化时重建。 */
class RoomGui : public tsl::Gui {
public:
    RoomGui() = default;
    explicit RoomGui(std::string target) : m_target(std::move(target)) {}

    virtual tsl::elm::Element* createUI() override {
        if (!RoomsSupported()) {
            auto frame = new tsl::elm::OverlayFrame(T("房间列表", "房間列表"), "");
            auto *list = new tsl::elm::List();
            list->addItem(new tsl::elm::ListItem(
                T("旧系统不支持房间查询", "舊系統不支援房間查詢")));
            frame->setContent(list);
            return frame;
        }
        EnsureMonitor();
        MonitorSetTraditional(g_isTraditional);
        char sel[32] = {};
        MonitorRequestRefresh(TargetName(sel, sizeof(sel)));

        auto frame = new tsl::elm::OverlayFrame(T("房间列表", "房間列表"),
                                                T("A 刷新 · B 返回", "A 重新整理 · B 返回"));
        m_list = new RebuildList();
        Rebuild();
        frame->setContent(m_list);
        return frame;
    }

    virtual void update() override {
        const u64 now = svcGetSystemTick();
        if (now - m_lastPoll >= 5ull * 19200000ull) {
            m_lastPoll = now;
            /* 定向刷新：从「全部服务器」点进来时只查那一台，不动主连接。 */
            char sel[32] = {};
            MonitorRequestSwitch(TargetName(sel, sizeof(sel)));
        }
        MonitorCopySnapshot(&m_snap);
        if (m_snap.version != m_version) {
            m_version = m_snap.version;
            Rebuild();
        }
    }

private:
    const char *TargetName(char *buf, size_t n) {
        if (!m_target.empty()) {
            snprintf(buf, n, "%s", m_target.c_str());
            return buf;
        }
        GetSelectedServerName(buf, n);
        return buf;
    }

    void Rebuild() {
        MonitorCopySnapshot(&m_snap);
        m_list->clear();

        char hdr[96];
        if (m_snap.online >= 0) {
            snprintf(hdr, sizeof(hdr), T("%s · 在线 %d · 房间 %d", "%s · 線上 %d · 房間 %d"),
                     m_snap.server[0] ? m_snap.server : "?", m_snap.online, m_snap.roomCount);
        } else {
            snprintf(hdr, sizeof(hdr), "%s", m_snap.server[0] ? m_snap.server : "?");
        }
        m_list->addItem(new tsl::elm::CategoryHeader(hdr));

        if (m_snap.roomN == 0) {
            m_list->addItem(new tsl::elm::ListItem(
                m_snap.busy ? T("查询中…", "查詢中…") : T("暂无房间", "暫無房間")));
        } else {
            for (u32 i = 0; i < m_snap.roomN; i++) {
                const MonitorRoom &r = m_snap.rooms[i];
                char val[24];
                if (r.playersMax > 0) {
                    snprintf(val, sizeof(val), "%u/%u",
                             static_cast<unsigned>(r.players), static_cast<unsigned>(r.playersMax));
                } else {
                    snprintf(val, sizeof(val), "%u", static_cast<unsigned>(r.players));
                }
                auto *item = new tsl::elm::ListItem(r.game);
                item->setValue(val);
                m_list->addItem(item);
            }
        }
        m_list->snapToFocus();
    }

    RebuildList *m_list = nullptr;
    std::string m_target;
    /* MonitorSnapshot 约 8.6 KB，放静态区，别压在 overlay 的栈上。 */
    static inline MonitorSnapshot m_snap = {};
    u32 m_version = 0xFFFFFFFFu;
    u64 m_lastPoll = 0;
};

/* 全部服务器总览：逐台查在线人数/房间数，按 A 进某台的房间页。
   只在打开本页时扫描；离开（析构）立即取消。 */
class AllServersGui : public tsl::Gui {
public:
    ~AllServersGui() override { MonitorAllCancel(); }

    virtual tsl::elm::Element* createUI() override {
        if (!RoomsSupported()) {
            auto frame = new tsl::elm::OverlayFrame(T("全部服务器", "全部伺服器"), "");
            auto *list = new tsl::elm::List();
            list->addItem(new tsl::elm::ListItem(
                T("旧系统不支持房间查询", "舊系統不支援房間查詢")));
            frame->setContent(list);
            return frame;
        }
        EnsureMonitor();
        MonitorSetTraditional(g_isTraditional);
        MonitorRequestAll(m_scope, false);
        MonitorCopyAllSnapshot(&m_snap);
        m_limit = m_snap.limit;

        auto frame = new tsl::elm::OverlayFrame(T("全部服务器", "全部伺服器"),
                                                T("Y 切范围 · X 刷新", "Y 切範圍 · X 重新整理"));
        m_list = new RebuildList();
        BuildRows();
        frame->setContent(m_list);
        return frame;
    }

    virtual void update() override {
        MonitorCopyAllSnapshot(&m_snap);
        if (m_snap.limit != m_limit || NamesChanged()) {
            m_limit = m_snap.limit;
            BuildRows();
        }
        UpdateValues();
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState &touchPos,
                             HidAnalogStickState joyStickPosLeft, HidAnalogStickState joyStickPosRight) override {
        (void)keysHeld; (void)touchPos; (void)joyStickPosLeft; (void)joyStickPosRight;
        if (keysDown & HidNpadButton_Y) {
            m_scope = (m_scope == 8) ? 0 : 8;
            MonitorRequestAll(m_scope, true);
            return true;
        }
        if (keysDown & HidNpadButton_X) {
            MonitorRequestAll(m_scope, true);
            return true;
        }
        return false;
    }

private:
    bool NamesChanged() const {
        if (m_builtNames.size() != m_limit) {
            return true;
        }
        for (u32 i = 0; i < m_limit && i < MONITOR_ALL_MAX; i++) {
            if (m_builtNames[i] != m_snap.servers[i].name) {
                return true;
            }
        }
        return false;
    }

    void BuildRows() {
        m_list->clear();
        m_items.clear();
        m_last.clear();
        m_builtNames.clear();

        char hdr[64];
        snprintf(hdr, sizeof(hdr), T("服务器（%u 台）", "伺服器（%u 台）"),
                 static_cast<unsigned>(m_limit));
        m_list->addItem(new tsl::elm::CategoryHeader(hdr));

        if (m_limit == 0) {
            m_list->addItem(new tsl::elm::ListItem(T("没有服务器", "沒有伺服器")));
            m_list->snapToFocus();
            return;
        }

        for (u32 i = 0; i < m_limit && i < MONITOR_ALL_MAX; i++) {
            const std::string name = m_snap.servers[i].name;
            auto *item = new tsl::elm::ListItem(name);
            item->setClickListener([name](u64 keys) {
                if (keys & HidNpadButton_A) {
                    tsl::changeTo<RoomGui>(name);
                    return true;
                }
                return false;
            });
            m_items.push_back(item);
            m_last.push_back(std::string());
            m_builtNames.push_back(name);
            m_list->addItem(item);
        }
        m_list->snapToFocus();
    }

    void UpdateValues() {
        for (u32 i = 0; i < m_items.size() && i < MONITOR_ALL_MAX; i++) {
            const MonitorAllServer &s = m_snap.servers[i];
            char v[48] = {};
            bool faint = false;
            switch (s.state) {
                case 0:
                    snprintf(v, sizeof(v), "%s", T("…", "…"));
                    faint = true;
                    break;
                case 1:
                    snprintf(v, sizeof(v), "%s", T("查询中…", "查詢中…"));
                    faint = true;
                    break;
                case 2:
                    if (s.online >= 0) {
                        snprintf(v, sizeof(v), T("在线 %d · %d 间", "線上 %d · %d 間"),
                                 static_cast<int>(s.online), static_cast<int>(s.roomCount));
                    } else {
                        snprintf(v, sizeof(v), T("%d 间", "%d 間"), static_cast<int>(s.roomCount));
                    }
                    break;
                case 3:
                    snprintf(v, sizeof(v), "%s", T("超时", "逾時"));
                    faint = true;
                    break;
                default:
                    snprintf(v, sizeof(v), "%s", T("跳过", "略過"));
                    faint = true;
                    break;
            }
            if (m_last[i] != v) {
                m_last[i] = v;
                m_items[i]->setValue(v, faint);
            }
        }
    }

    RebuildList *m_list = nullptr;
    std::vector<tsl::elm::ListItem *> m_items;
    std::vector<std::string> m_last;
    std::vector<std::string> m_builtNames;
    /* MonitorAllSnapshot 约 36 KB，放静态区。 */
    static inline MonitorAllSnapshot m_snap = {};
    u32 m_limit = 0;
    u32 m_scope = 8;   /* 8 = 前 8 台；0 = relay.cfg 全部 */
};

/* Everything below the frame header. Kept out of createUI() so it can be
   called again after the relay list changes: libtesla cannot refresh a
   single row, but it can delete and re-add the whole list on the next
   frame (List::clear() + addItem() are both deferred). */
static void buildList(tsl::elm::List *list) {
    g_serverItems.clear();
    g_restoreItem = nullptr;
    g_addServerItem = nullptr;
    g_roomSummaryItem = nullptr;
    g_restoreArmed = false;

    if (g_state == State::Error) {
        list->addItem(new tsl::elm::ListItem(T("插件没有加载", "插件沒有載入")));
    } else if (g_state == State::Uninit) {
        list->addItem(new tsl::elm::ListItem(T("错误的状态", "錯誤的狀態")));
    } else {
        list->addItem(new ConfigToggleListItem(T("启用", "啟用"), ldnMitmGetEnabled, ldnMitmSetEnabled));
        list->addItem(new ConfigToggleListItem(T("广播转发", "廣播轉發"), ldnMitmGetBroadcastRelay, ldnMitmSetBroadcastRelay));
        list->addItem(new ConfigToggleListItem(T("日志", "日誌"), ldnMitmGetLogging, ldnMitmSetLogging));

        list->addItem(new tsl::elm::CategoryHeader(T("互联网中继（按 X 删除）", "網際網路中繼（按 X 刪除）")));
        list->addItem(new ConfigToggleListItem(T("互联网中继", "網際網路中繼"), ldnMitmGetInternetRelay, ldnMitmSetInternetRelay));

        u32 count = 0;
        if (R_FAILED(ldnMitmGetRelayServerCount(&g_ldnConfig, &count))) {
            list->addItem(new tsl::elm::ListItem(T("读取服务器列表失败", "讀取伺服器清單失敗")));
        } else if (count == 0) {
            list->addItem(new tsl::elm::ListItem(T("没有服务器，请恢复默认", "沒有伺服器，請恢復預設")));
        } else {
            u32 selected = 0;
            if (R_FAILED(ldnMitmGetSelectedRelayServer(&g_ldnConfig, &selected))) {
                selected = count;
            }
            for (u32 i = 0; i < count; i++) {
                char name[32] = {};
                ldnMitmGetRelayServerName(&g_ldnConfig, i, name);
                auto *item = new tsl::elm::ListItem(name);
                item->setValue(i == selected ? "*" : "");
                item->setClickListener([i](u64 keys) {
                    if (keys & HidNpadButton_A) {
                        if (R_FAILED(ldnMitmSetSelectedRelayServer(&g_ldnConfig, i))) {
                            g_serverItems[i]->setValue(T("错误", "錯誤"));
                            return true;
                        }
                        for (size_t k = 0; k < g_serverItems.size(); k++) {
                            g_serverItems[k]->setValue(k == i ? "*" : "");
                        }
                        // The loop above just wiped every latency label back
                        // to a bare "*". Invalidate the cache so the next
                        // update() repaints, otherwise the rows would sit
                        // there with only a star until the overlay is
                        // reopened.
                        g_shown_latency = 0xFFFFFFFFu;
                        // 房间列表切换到新选中的服务器。
                        {
                            char sel[32] = {};
                            GetSelectedServerName(sel, sizeof(sel));
                            MonitorRequestSwitch(sel);
                        }
                        return true;
                    }
                    if (keys & HidNpadButton_X) {
                        // Deleting shifts every following index, so the UI is
                        // rebuilt from the sysmodule's list instead of trying
                        // to patch the rows in place.
                        if (R_SUCCEEDED(ldnMitmDeleteRelayServer(&g_ldnConfig, i))) {
                            rebuildUi();
                            // Keep the cursor on the same slot: the row that
                            // slid up into the deleted one, or the new last
                            // row. With no servers left, land on "restore
                            // defaults" -- that is the only useful action.
                            if (!g_serverItems.empty()) {
                                const size_t next = (i < g_serverItems.size()) ? i : (g_serverItems.size() - 1);
                                focusAfterRebuild(g_serverItems[next]);
                            } else {
                                focusAfterRebuild(g_restoreItem);
                            }
                        } else if (i < g_serverItems.size()) {
                            g_serverItems[i]->setValue(T("删除失败", "刪除失敗"), true);
                        }
                        return true;
                    }
                    return false;
                });
                g_serverItems.push_back(item);
                list->addItem(item);
            }
        }

        // 标准版：房间功能整体关闭（房间模块另行发布测试版）。
        // 这里不创建任何房间入口，monitor 也就不会被初始化。

        // Undo a mistake without editing relay.cfg on a PC. Two presses on
        // purpose: this rewrites the whole relay list.
        list->addItem(new tsl::elm::CategoryHeader(T("管理", "管理")));

        g_addServerItem = new tsl::elm::ListItem(T("添加服务器", "新增伺服器"));
        g_addServerItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A) {
                tsl::changeTo<AddServerGui>();
                return true;
            }
            return false;
        });
        list->addItem(g_addServerItem);

        g_restoreItem = new tsl::elm::ListItem(T("恢复默认服务器", "恢復預設伺服器"));
        g_restoreItem->setClickListener([](u64 keys) {
            if (!(keys & HidNpadButton_A)) {
                return false;
            }
            if (!g_restoreArmed) {
                g_restoreArmed = true;
                g_restoreItem->setValue(T("再按 A 确认", "再按 A 確認"), true);
                return true;
            }
            g_restoreArmed = false;
            if (R_SUCCEEDED(ldnMitmRestoreDefaultRelayServers(&g_ldnConfig))) {
                rebuildUi();
                focusAfterRebuild(g_restoreItem);
            } else if (g_restoreItem != nullptr) {
                g_restoreItem->setValue(T("恢复失败", "恢復失敗"), true);
            }
            return true;
        });
        list->addItem(g_restoreItem);

        // Show this console's relay identity. relay.cfg only stores the
        // low 16 bits (`id=`), and the plugin assigns one on first use if
        // the line is absent, so this is the only place to read it back.
        list->addItem(new tsl::elm::CategoryHeader(T("本机身份", "本機身分")));

        // The relay address is derived from this LAN address, so showing
        // it makes it obvious why the two consoles get different ids.
        // nifm only hands out an address once the LAN session exists, so
        // before the game creates or joins a room there is nothing to
        // read -- that is expected, not a failure.
        u32 local_ip = 0;
        if (R_SUCCEEDED(ldnMitmGetRelayLocalIp(&g_ldnConfig, &local_ip)) && local_ip != 0) {
            char ipbuf[32];
            snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u",
                     (local_ip >> 24) & 0xff, (local_ip >> 16) & 0xff,
                     (local_ip >> 8) & 0xff, local_ip & 0xff);
            list->addItem(new tsl::elm::ListItem(T("本机 IP", "本機 IP"), ipbuf));
        } else {
            list->addItem(new tsl::elm::ListItem(T("本机 IP", "本機 IP"), T("建立房间后显示", "建立房間後顯示")));
        }

        u32 vaddr = 0;
        const bool addr_ok = R_SUCCEEDED(ldnMitmGetRelayVirtualAddress(&g_ldnConfig, &vaddr));
        if (!addr_ok) {
            list->addItem(new tsl::elm::ListItem(T("虚拟地址", "虛擬位址"), T("读取失败", "讀取失敗")));
        } else if (vaddr == 0) {
            list->addItem(new tsl::elm::ListItem(T("虚拟地址", "虛擬位址"), T("建立房间后分配", "建立房間後分配")));
        } else {
            char buf[48];
            snprintf(buf, sizeof(buf), "%u.%u.%u.%u  (id=%u)",
                     (vaddr >> 24) & 0xff, (vaddr >> 16) & 0xff,
                     (vaddr >> 8) & 0xff, vaddr & 0xff, vaddr & 0xffff);
            list->addItem(new tsl::elm::ListItem(T("虚拟地址", "虛擬位址"), buf));
        }

        // Tell the user whether the address is stable. T("硬件派生", "硬體衍生") is
        // derived from this console's LAN address, so it is the same on
        // every boot even when the SD card is write-protected. T("启动随机", "啟動隨機")
        // still gets written to relay.cfg, so check it again after a
        // reboot -- if the number changes, the write is failing.
        if (addr_ok && vaddr != 0) {
            u32 src = 0;
            if (R_FAILED(ldnMitmGetRelayIdSource(&g_ldnConfig, &src))) {
                src = 0;
            }
            if (src == 1) {
                list->addItem(new tsl::elm::ListItem(T("地址固定", "位址固定"), T("硬件派生", "硬體衍生")));
            } else if (src == 2) {
                list->addItem(new tsl::elm::ListItem(T("地址固定", "位址固定"), T("启动随机", "啟動隨機")));
            } else if (src == 3) {
                list->addItem(new tsl::elm::ListItem(T("地址固定", "位址固定"), T("已自动更换", "已自動更換")));
            } else {
                // The id either predates hardware derivation or was cached
                // before this console's LAN address changed.
                list->addItem(new tsl::elm::ListItem(T("地址固定", "位址固定"), T("未知来源", "未知來源")));
            }
        }
        list->addItem(new tsl::elm::ListItem(T("两台机器的地址必须不同", "兩台機器的位址必須不同")));
        list->addItem(new tsl::elm::ListItem(T("重启后地址不变即正常", "重新啟動後位址不變即正常")));

        // Release signature. Kept at the very bottom so it never gets in
        // the way of the settings, but every user scrolling down sees it.
        list->addItem(new tsl::elm::CategoryHeader(T("关于", "關於")));
        list->addItem(new tsl::elm::ListItem(T("制作", "製作"), T("阿东 & 大肥鱼", "阿東 & 大肥魚")));
        list->addItem(new tsl::elm::ListItem(T("版本", "版本"), g_version));
        list->addItem(new tsl::elm::ListItem(T("声明", "聲明"), T("免费分享 禁止倒卖", "免費分享 禁止轉賣")));
    }

    // Rebuilt rows start with a bare "*"; force update() to paint them.
    g_shown_latency = 0xFFFFFFFFu;
}

/* Rebuild the visible list after the relay list changed. The old rows are
   deleted by List::draw() on the next frame, so focus is dropped first:
   leaving the Gui pointing at a deleted element would crash on the next
   input. restoreFocus() makes the following frame pick a fresh row. */
static void rebuildUi() {
    if (g_list == nullptr) {
        return;
    }
    auto &gui = tsl::Overlay::get()->getCurrentGui();
    if (gui == nullptr) {
        return;
    }
    gui->removeFocus();
    g_list->clear();
    buildList(g_list);
    gui->restoreFocus();
}

class Overlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        g_state = State::Uninit;
        detectLanguage();
        tsl::hlp::doWithSmSession([&] {
            Result rc;

            rc = smGetService(&g_ldnSrv, "ldn:u");
            if (R_FAILED(rc)) {
                g_state = State::Error;
                return;
            }

            rc = ldnMitmGetConfigFromService(&g_ldnSrv, &g_ldnConfig);
            if (R_FAILED(rc)) {
                g_state = State::Error;
                return;
            }

            rc = ldnMitmGetVersion(&g_ldnConfig, g_version);
            if (R_FAILED(rc)) {
                strcpy(g_version, T("错误", "錯誤"));
            }

            g_state = State::Loaded;
        });

    }
    virtual void exitServices() override {
        serviceClose(&g_ldnConfig.s);
        serviceClose(&g_ldnSrv);
    }

    virtual void onShow() override {}    // Called before overlay wants to change from invisible to visible state
    virtual void onHide() override {}    // Called before overlay wants to change from visible to invisible state

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        return initially<MainGui>();  // Initial Gui to load. It's possible to pass arguments to it's constructor like this
    }
};

int main(int argc, char **argv) {
    return tsl::loop<Overlay>(argc, argv);
}
