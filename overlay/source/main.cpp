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

std::vector<tsl::elm::ListItem*> g_serverItems;

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

class MainGui : public tsl::Gui {
public:
    MainGui() { }

    virtual tsl::elm::Element* createUI() override {
        // This overlay ships inside a Chinese integration package and the
        // bundled font has CJK glyphs, so the UI is localised in place.
        auto frame = new tsl::elm::OverlayFrame("联机插件", g_version);

        auto list = new tsl::elm::List();

        if (g_state == State::Error) {
            list->addItem(new tsl::elm::ListItem("插件没有加载"));
        } else if (g_state == State::Uninit) {
            list->addItem(new tsl::elm::ListItem("错误的状态"));
        } else {
            list->addItem(new ConfigToggleListItem("启用", ldnMitmGetEnabled, ldnMitmSetEnabled));
            list->addItem(new ConfigToggleListItem("广播转发", ldnMitmGetBroadcastRelay, ldnMitmSetBroadcastRelay));
            list->addItem(new ConfigToggleListItem("日志", ldnMitmGetLogging, ldnMitmSetLogging));

            list->addItem(new tsl::elm::CategoryHeader("互联网中继（跨网联机）"));
            list->addItem(new ConfigToggleListItem("互联网中继", ldnMitmGetInternetRelay, ldnMitmSetInternetRelay));

            g_serverItems.clear();
            u32 count = 0;
            if (R_FAILED(ldnMitmGetRelayServerCount(&g_ldnConfig, &count))) {
                list->addItem(new tsl::elm::ListItem("读取服务器列表失败"));
            } else if (count == 0) {
                list->addItem(new tsl::elm::ListItem("没有服务器，请编辑 relay.cfg"));
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
                                g_serverItems[i]->setValue("错误");
                                return true;
                            }
                            for (size_t k = 0; k < g_serverItems.size(); k++) {
                                g_serverItems[k]->setValue(k == i ? "*" : "");
                            }
                            return true;
                        }
                        return false;
                    });
                    g_serverItems.push_back(item);
                    list->addItem(item);
                }
            }

            // Show this console's relay identity. relay.cfg only stores the
            // low 16 bits (`id=`), and the plugin assigns one on first use if
            // the line is absent, so this is the only place to read it back.
            list->addItem(new tsl::elm::CategoryHeader("本机身份"));
            u32 vaddr = 0;
            const bool addr_ok = R_SUCCEEDED(ldnMitmGetRelayVirtualAddress(&g_ldnConfig, &vaddr));
            if (!addr_ok) {
                list->addItem(new tsl::elm::ListItem("虚拟地址", "错误"));
            } else if (vaddr == 0) {
                list->addItem(new tsl::elm::ListItem("虚拟地址", "尚未分配"));
            } else {
                char buf[48];
                snprintf(buf, sizeof(buf), "%u.%u.%u.%u  (id=%u)",
                         (vaddr >> 24) & 0xff, (vaddr >> 16) & 0xff,
                         (vaddr >> 8) & 0xff, vaddr & 0xff, vaddr & 0xffff);
                list->addItem(new tsl::elm::ListItem("虚拟地址", buf));
            }

            // Tell the user whether the address is stable. "硬件派生" is
            // derived from this console's LAN address, so it is the same on
            // every boot even when the SD card is write-protected. "启动随机"
            // still gets written to relay.cfg, so check it again after a
            // reboot -- if the number changes, the write is failing.
            if (addr_ok && vaddr != 0) {
                u32 src = 0;
                if (R_FAILED(ldnMitmGetRelayIdSource(&g_ldnConfig, &src))) {
                    src = 0;
                }
                if (src == 1) {
                    list->addItem(new tsl::elm::ListItem("地址固定", "硬件派生"));
                } else if (src == 2) {
                    list->addItem(new tsl::elm::ListItem("地址固定", "启动随机"));
                } else {
                    list->addItem(new tsl::elm::ListItem("地址固定", "未知"));
                }
            }
            list->addItem(new tsl::elm::ListItem("两台机器的地址必须不同"));
            list->addItem(new tsl::elm::ListItem("重启后地址不变即正常"));
        }

        frame->setContent(list);

        return frame;
    }

    // Called once every frame to update values
    virtual void update() override {
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
        static u32 shown = 0xFFFFFFFFu;
        if (ms == shown) {
            return;
        }
        shown = ms;

        // ListItem::setValue(value, faint): faint=false renders in the
        // highlight colour, faint=true in the dim description colour. That
        // gives a green / grey traffic light without a custom element.
        char buf[48];
        bool faint = false;
        if (ms == 0) {
            snprintf(buf, sizeof(buf), "* 延迟 --");
            faint = true;
        } else if (ms > 999) {
            snprintf(buf, sizeof(buf), "* 延迟 >999ms");
            faint = true;
        } else {
            snprintf(buf, sizeof(buf), "* 延迟 %ums", ms);
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

class Overlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        g_state = State::Uninit;
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
                strcpy(g_version, "错误");
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
