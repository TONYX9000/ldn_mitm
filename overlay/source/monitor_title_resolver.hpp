#pragma once

#include <stddef.h>
#include <string.h>

/* Resolve title IDs for games whose LDN contentId is the all-F sentinel.
 * advertiseData is the room's own hexadecimal LDN payload; no local process
 * state is used, so rooms from different games cannot inherit the foreground
 * application's title by accident. */
namespace MonitorTitleResolver {

static constexpr size_t kAdvertiseDataMaxBytes = 384;
static constexpr size_t kAdvertiseDataMaxHex = kAdvertiseDataMaxBytes * 2;

inline int HexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

inline bool NormalizeAdvertiseHex(const char *src, char *out, size_t outN,
                                  size_t *outLen) {
    if (outLen)
        *outLen = 0;
    if (!src || !out || outN == 0)
        return false;

    size_t len = 0;
    for (const char *p = src; *p; ++p) {
        const int digit = HexDigit(*p);
        if (digit >= 0) {
            if (len + 1 >= outN) {
                out[0] = 0;
                return false;
            }
            out[len++] = "0123456789abcdef"[digit];
        } else if (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' ||
                   *p == ':' || *p == '-') {
            continue;
        } else {
            out[0] = 0;
            return false;
        }
    }
    out[len] = 0;
    if (outLen)
        *outLen = len;
    return len != 0 && (len & 1) == 0;
}

inline bool HexSpanContains(const char *begin, const char *end,
                            const char *needle) {
    if (!begin || !end || !needle || end < begin)
        return false;
    const size_t needleLen = strlen(needle);
    const size_t spanLen = (size_t)(end - begin);
    if (needleLen == 0)
        return true;
    if (needleLen > spanLen)
        return false;
    for (size_t i = 0; i + needleLen <= spanLen; ++i) {
        if (memcmp(begin + i, needle, needleLen) == 0)
            return true;
    }
    return false;
}

inline bool CopyTitleId(const char *titleId, char *out, size_t outN) {
    if (!titleId || !out || outN < 17)
        return false;
    memcpy(out, titleId, 17);
    return true;
}

inline unsigned char AdvertiseByte(const char *hex, size_t byteOffset) {
    const size_t i = byteOffset * 2;
    return (unsigned char)((HexDigit(hex[i]) << 4) | HexDigit(hex[i + 1]));
}

inline bool AdvertiseBytesAreZero(const char *hex, size_t startByte,
                                  size_t byteCount) {
    for (size_t i = 0; i < byteCount; ++i) {
        if (AdvertiseByte(hex, startByte + i) != 0)
            return false;
    }
    return true;
}

/* S/V carries 40 game bytes after its 0x5c-byte Pia property header.
 * Retail Link Trade search beacons have all-zero game bytes or a changing
 * four-byte field at +0x21. Link-Code sessions store a printable code at
 * +0x00 and its little-endian length at +0x24. Live rooms showed markers
 * aa 88 f4, aa e8 8c and aa 88 72: the final byte varies across sessions,
 * while the first byte and marker family remain stable. */
inline bool IsPokemonScarletVioletGameData(const char *hex) {
    static constexpr size_t kGameStart = 0x5c;
    static constexpr size_t kGameSize = 40;

    if (AdvertiseBytesAreZero(hex, kGameStart, kGameSize))
        return true;

    /* Searching Link Trade beacon: changing bytes at game-data +0x21..+0x24. */
    if (AdvertiseBytesAreZero(hex, kGameStart, 0x21) &&
        !AdvertiseBytesAreZero(hex, kGameStart + 0x21, 4) &&
        AdvertiseBytesAreZero(hex, kGameStart + 0x25, 3))
        return true;

    /* Link Code sessions use a printable code and a little-endian u32 length. */
    const unsigned int codeLen =
        (unsigned int)AdvertiseByte(hex, kGameStart + 0x24) |
        ((unsigned int)AdvertiseByte(hex, kGameStart + 0x25) << 8) |
        ((unsigned int)AdvertiseByte(hex, kGameStart + 0x26) << 16) |
        ((unsigned int)AdvertiseByte(hex, kGameStart + 0x27) << 24);
    if (codeLen == 0) {
        /* Also allow an open Union Circle with its observed marker. */
        return AdvertiseBytesAreZero(hex, kGameStart, 0x21) &&
               AdvertiseByte(hex, kGameStart + 0x21) == 0xaa &&
               AdvertiseByte(hex, kGameStart + 0x22) == 0x08 &&
               AdvertiseByte(hex, kGameStart + 0x23) == 0x3e &&
               AdvertiseBytesAreZero(hex, kGameStart + 0x24, 4);
    }
    if (codeLen > 16)
        return false;

    for (size_t i = 0; i < codeLen; ++i) {
        const unsigned char c = AdvertiseByte(hex, kGameStart + i);
        if (c < 0x21 || c > 0x7e)
            return false;
    }
    if (!AdvertiseBytesAreZero(hex, kGameStart + codeLen, 0x21 - codeLen))
        return false;

    const bool noModeMarker =
        AdvertiseBytesAreZero(hex, kGameStart + 0x21, 3);
    const bool unionCircleMarker =
        AdvertiseByte(hex, kGameStart + 0x21) == 0xaa &&
        AdvertiseByte(hex, kGameStart + 0x22) == 0x08 &&
        AdvertiseByte(hex, kGameStart + 0x23) == 0x3e;
    const unsigned char observedMarkerPrefix =
        AdvertiseByte(hex, kGameStart + 0x21);
    const unsigned char observedMarkerMode =
        AdvertiseByte(hex, kGameStart + 0x22);
    const bool observedScarletVioletMarker =
        observedMarkerPrefix == 0xaa &&
        (observedMarkerMode == 0x08 || observedMarkerMode == 0x88 ||
         observedMarkerMode == 0xe8);
    return (noModeMarker || unionCircleMarker || observedScarletVioletMarker) &&
           AdvertiseByte(hex, kGameStart + 0x24) == codeLen &&
           AdvertiseBytesAreZero(hex, kGameStart + 0x25, 3);
}

/* Scarlet and Violet share Scarlet's local-communication ID on the air.
 * Their 132-byte app data is a 0x5c Pia block plus 40 game-specific bytes. */
inline bool IsPokemonScarletVioletAdvertiseHex(const char *hex, size_t hexLen) {
    static constexpr size_t kAdvertiseBytes = 132;
    if (!hex || hexLen != kAdvertiseBytes * 2 ||
        memcmp(hex, "005c150015", 10) != 0)
        return false;

    /* Pia header: player limit enabled, 1-4 players, UTF-8 single-space name. */
    if (AdvertiseByte(hex, 0x15) != 1 ||
        AdvertiseByte(hex, 0x16) < 1 || AdvertiseByte(hex, 0x16) > 4 ||
        AdvertiseByte(hex, 0x17) != 0 || AdvertiseByte(hex, 0x18) != 0 ||
        AdvertiseByte(hex, 0x19) != 0 || AdvertiseByte(hex, 0x1a) != 1 ||
        AdvertiseByte(hex, 0x1b) != 1 || AdvertiseByte(hex, 0x1c) != 0x20 ||
        !AdvertiseBytesAreZero(hex, 0x1d, 0x5c - 0x1d))
        return false;

    return IsPokemonScarletVioletGameData(hex);
}

/* Returns false when the advertisement is absent or does not match a known
 * signature. The caller must then show the safe unknown-game fallback. */
inline bool ResolveAdvertisedTitleId(const char *advertiseData,
                                     const char *hostPlayerName,
                                     char *outTitleId, size_t outN) {
    if (outTitleId && outN > 0)
        outTitleId[0] = 0;
    if (!outTitleId || outN < 17)
        return false;

    char hex[kAdvertiseDataMaxHex + 1] = {};
    size_t hexLen = 0;
    if (!NormalizeAdvertiseHex(advertiseData, hex, sizeof(hex), &hexLen))
        return false;

    /* DRAGON BALL FighterZ: advertiseData == ffff0000. */
    if (hexLen == 8 && strcmp(hex, "ffff0000") == 0)
        return CopyTitleId("0100A250097F0000", outTitleId, outN);

    /* Rocket League advertises a JSON object (7b = '{', 7d = '}'). */
    if (hexLen >= 4 && hex[0] == '7' && hex[1] == 'b' &&
        hex[hexLen - 2] == '7' && hex[hexLen - 1] == 'd')
        return CopyTitleId("01005EE0036EC000", outTitleId, outN);

    if (IsPokemonScarletVioletAdvertiseHex(hex, hexLen))
        return CopyTitleId("0100A3D008C5C000", outTitleId, outN);

    static const char kDelimiter[] = "00000004";
    const size_t delimiterLen = sizeof(kDelimiter) - 1;
    size_t delimiterCount = 0;
    for (const char *p = hex; (p = strstr(p, kDelimiter)) != nullptr;
         p += delimiterLen)
        ++delimiterCount;
    const char *firstDelimiter = strstr(hex, kDelimiter);
    const char *firstPartEnd = firstDelimiter ? firstDelimiter : hex + hexLen;

    /* MONSTER HUNTER RISE has player/rank sections after two delimiters. */
    if (delimiterCount >= 2 &&
        HexSpanContains(hex, firstPartEnd, "0000000008100000") &&
        HexSpanContains(hex, firstPartEnd, "8b9000"))
        return CopyTitleId("0100B04011742000", outTitleId, outN);

    /* Monster Hunter Stories 2 shares the common prefix, but has 0a0000. */
    if (HexSpanContains(hex, firstPartEnd, "0000000008100000") &&
        HexSpanContains(hex, firstPartEnd, "0a0000"))
        return CopyTitleId("0100E21011446000", outTitleId, outN);

    /* Minecraft stores the host name as UTF-8 starting at hex offset 32. */
    if (hostPlayerName && hostPlayerName[0] && hexLen > 32 &&
        ((hexLen - 32) & 1) == 0) {
        char decoded[96] = {};
        size_t decodedLen = 0;
        for (size_t i = 32; i + 1 < hexLen; i += 2) {
            const int hi = HexDigit(hex[i]);
            const int lo = HexDigit(hex[i + 1]);
            if (hi < 0 || lo < 0)
                return false;
            const unsigned char byte = (unsigned char)((hi << 4) | lo);
            if (byte == 0)
                break;
            if (decodedLen + 1 >= sizeof(decoded))
                return false;
            decoded[decodedLen++] = (char)byte;
        }
        decoded[decodedLen] = 0;
        if (decodedLen != 0 && strcmp(decoded, hostPlayerName) == 0)
            return CopyTitleId("0100D71004694000", outTitleId, outN);
    }

    return false;
}

} // namespace MonitorTitleResolver
