// See utils/platform_api.h: the calls the Switch 2 platform half makes into the
// app. Only built for the console, where the platform backend is the caller.

#include "utils/platform_api.h"

#if defined(__SWITCH2__)
#include "api/bilibili/util/http.hpp"

void wiliwili_network_reset(void) {
    bilibili::HTTP::resetDnsCache();
}
#endif
