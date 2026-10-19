#define TESLA_INIT_IMPL // If you have more than one file using the tesla header, only define this in the main one
#include <tesla.hpp>    // The Tesla Header
#include "ldn.h"
#include <vector>

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

            char addr[96];
            snprintf(addr, sizeof(addr), "%s:%u", info.host, static_cast<unsigned>(info.port));
            auto *item = new tsl::elm::ListItem(info.name);
            item->setValue(present ? T("已添加", "已新增") : addr, present);

            auto added = std::make_shared<bool>(present);
            auto count = std::make_shared<u32>(current);
            item->setClickListener([item, info, added, count](u64 keys) {
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


/* Everything below the frame header. Kept out of createUI() so it can be
   called again after the relay list changes: libtesla cannot refresh a
   single row, but it can delete and re-add the whole list on the next
   frame (List::clear() + addItem() are both deferred). */
static void buildList(tsl::elm::List *list) {
    g_serverItems.clear();
    g_restoreItem = nullptr;
    g_addServerItem = nullptr;
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
