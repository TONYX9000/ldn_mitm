#include "ldn.h"
#include <string.h>

Result ldnMitmGetConfig(LdnMitmConfigService *out) {
    Handle handle;
    Result rc = svcConnectToNamedPort(&handle, "ldnmitm");
    if (R_SUCCEEDED(rc)) {
        serviceCreate(&out->s, handle);
    }
    return rc;
}

Result ldnMitmGetConfigFromService(Service* s, LdnMitmConfigService *out) {
    return serviceDispatch(s, 65000,
        .out_num_objects = 1,
        .out_objects = &out->s
    );
}

Result ldnMitmGetLogging(LdnMitmConfigService *s, u32 *enabled) {
    return serviceDispatchOut(&s->s, 65002, *enabled);
}

Result ldnMitmSetLogging(LdnMitmConfigService *s, u32 enabled) {
    return serviceDispatchIn(&s->s, 65003, enabled);
}

Result ldnMitmGetEnabled(LdnMitmConfigService *s, u32 *enabled) {
    return serviceDispatchOut(&s->s, 65004, *enabled);
}

Result ldnMitmSetEnabled(LdnMitmConfigService *s, u32 enabled) {
    return serviceDispatchIn(&s->s, 65005, enabled);
}

Result ldnMitmGetVersion(LdnMitmConfigService *s, char *version) {
    /* 以 0 填满:即使服务端漏了结尾 NUL,strnlen 也不会越界读。 */
    char version_s[32] = {};
    Result rc = serviceDispatchOut(&s->s, 65001, version_s);
    if (R_SUCCEEDED(rc)) {
        size_t n = 0;
        while (n < sizeof(version_s) && version_s[n] != 0)
            n++;
        memcpy(version, version_s, n);
        version[n] = 0;
    }
    return rc;
}

Result ldnMitmSaveLogToFile(LdnMitmConfigService *s) {
    return serviceDispatch(&s->s, 65000);
}