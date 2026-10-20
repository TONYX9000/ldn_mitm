#include "../source/monitor_title_resolver.hpp"

#include <assert.h>
#include <string.h>

using MonitorTitleResolver::kAdvertiseDataMaxHex;
using MonitorTitleResolver::ResolveAdvertisedTitleId;

static void ExpectResolved(const char *advertiseData, const char *hostName,
                          const char *expectedTitleId) {
    char titleId[17] = {};
    assert(ResolveAdvertisedTitleId(advertiseData, hostName,
                                    titleId, sizeof(titleId)));
    assert(strcmp(titleId, expectedTitleId) == 0);
}

static void ExpectUnresolved(const char *advertiseData, const char *hostName) {
    char titleId[17] = "not-a-title-id";
    assert(!ResolveAdvertisedTitleId(advertiseData, hostName,
                                     titleId, sizeof(titleId)));
    assert(titleId[0] == 0);
}

static void SetByte(char *hex, size_t byteOffset, unsigned char value) {
    static const char digits[] = "0123456789abcdef";
    hex[byteOffset * 2] = digits[value >> 4];
    hex[byteOffset * 2 + 1] = digits[value & 0x0f];
}

/* Build the documented 132-byte Scarlet/Violet application-data shape:
 * 0x5c-byte Pia header plus the game's 40 bytes. mode 0 = no code, mode 1 =
 * changing trade-search value, mode 2 = Link Code, mode 3 = Union Circle,
 * mode 4 = aa 88 f4, mode 5 = aa e8 8c, mode 6 = aa 88 72 (latest live). */
static void BuildScarletVioletAdvertiseData(char *hex, size_t hexSize, int mode) {
    static const size_t kBytes = 132;
    assert(hexSize >= kBytes * 2 + 1);
    memset(hex, '0', kBytes * 2);
    hex[kBytes * 2] = 0;

    SetByte(hex, 0, 0x00);
    SetByte(hex, 1, 0x5c);
    SetByte(hex, 2, 0x15); // system communication version
    SetByte(hex, 3, 0x00);
    SetByte(hex, 4, 0x15); // application communication version
    SetByte(hex, 0x15, 1); // player limit enabled
    SetByte(hex, 0x16, 1); // one participant
    SetByte(hex, 0x17, 0);
    SetByte(hex, 0x18, 0);
    SetByte(hex, 0x19, 0);
    SetByte(hex, 0x1a, 1); // one-byte nickname
    SetByte(hex, 0x1b, 1); // UTF-8
    SetByte(hex, 0x1c, 0x20); // the published nickname is one space

    const size_t game = 0x5c;
    if (mode == 1) {
        SetByte(hex, game + 0x21, 0xfb);
        SetByte(hex, game + 0x22, 0x14);
        SetByte(hex, game + 0x23, 0x97);
        SetByte(hex, game + 0x24, 0x00);
    } else if (mode >= 2 && mode <= 6) {
        SetByte(hex, game + 0, '1');
        SetByte(hex, game + 1, '2');
        SetByte(hex, game + 2, '3');
        SetByte(hex, game + 3, '4');
        if (mode >= 3) {
            unsigned char markerMode = 0x08;
            unsigned char markerTail = 0x3e;
            if (mode == 4) {
                markerMode = 0x88;
                markerTail = 0xf4;
            } else if (mode == 5) {
                markerMode = 0xe8;
                markerTail = 0x8c;
            } else if (mode == 6) {
                markerMode = 0x88;
                markerTail = 0x72;
            }
            SetByte(hex, game + 0x21, 0xaa);
            SetByte(hex, game + 0x22, markerMode);
            SetByte(hex, game + 0x23, markerTail);
        }
        SetByte(hex, game + 0x24, 4); // little-endian code length
    }
}

int main() {
    ExpectResolved("ffff0000", nullptr, "0100A250097F0000");
    ExpectResolved("7b7d", nullptr, "01005EE0036EC000");
    ExpectResolved("00000000081000008b9000"
                   "00000004"
                   "1234"
                   "00000004"
                   "5678",
                   nullptr, "0100B04011742000");
    ExpectResolved("00000000081000000a0000"
                   "00000004"
                   "1234",
                   nullptr, "0100E21011446000");
    ExpectResolved("00112233445566778899aabbccddeeff4d6961", "Mia",
                   "0100D71004694000");

    /* Scarlet and Violet use Scarlet's local-communication ID on both builds. */
    char sv[kAdvertiseDataMaxHex + 1] = {};
    for (int mode = 0; mode < 7; ++mode) {
        BuildScarletVioletAdvertiseData(sv, sizeof(sv), mode);
        ExpectResolved(sv, nullptr, "0100A3D008C5C000");
    }

    /* Case/spacing normalization is harmless, while partial signatures are
     * not promoted to a title. Unknown sentinels must remain unknown. */
    ExpectResolved("FF FF 00 00", nullptr, "0100A250097F0000");
    ExpectUnresolved("00000000081000008b9000"
                     "00000004"
                     "1234",
                     nullptr); // Rise signature is missing its second delimiter.
    ExpectUnresolved("00112233445566778899aabbccddeeff4d6961", "Not-Mia");
    ExpectUnresolved("11223344", nullptr);

    BuildScarletVioletAdvertiseData(sv, sizeof(sv), 0);
    SetByte(sv, 2, 0x14); // wrong Pia system version
    ExpectUnresolved(sv, nullptr);
    BuildScarletVioletAdvertiseData(sv, sizeof(sv), 0);
    SetByte(sv, 0x1c, 'A'); // unexpected nickname
    ExpectUnresolved(sv, nullptr);
    BuildScarletVioletAdvertiseData(sv, sizeof(sv), 0);
    SetByte(sv, 0x5c + 10, 0xaa); // unknown game-data shape
    ExpectUnresolved(sv, nullptr);
    BuildScarletVioletAdvertiseData(sv, sizeof(sv), 2);
    SetByte(sv, 0x5c + 0x21, 0xaa);
    SetByte(sv, 0x5c + 0x22, 0x12); // unrecognized marker remains unknown
    SetByte(sv, 0x5c + 0x23, 0xef);
    ExpectUnresolved(sv, nullptr);
    BuildScarletVioletAdvertiseData(sv, sizeof(sv), 0);
    strcat(sv, "00"); // app-data size is not 132 bytes
    ExpectUnresolved(sv, nullptr);
    return 0;
}
