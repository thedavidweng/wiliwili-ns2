// See utils/platform_api.h: the calls that cross between the two halves. The
// proxy check is pure and compiled everywhere (the settings screen that opens
// the keyboard for it is not console specific); the network reset only exists on
// the console, which is the only caller.

#include "utils/platform_api.h"

#include <cstring>

#if defined(__SWITCH2__)
#include "api/bilibili/util/http.hpp"
#include "utils/event_helper.hpp"
#include "utils/perf_diagnostics.hpp"
#endif

/* scheme://user:pass@host. The credentials sit between the scheme separator and
 * the first '@' in a URL that is not a bare host: that is the only part of the
 * string a keyboard should mask. */
int wiliwili_proxy_is_sensitive(const char* url) {
    if (url == nullptr) return 0;

    const char* scheme    = std::strstr(url, "://");
    const char* authority = scheme != nullptr ? scheme + 3 : url;
    const char* at        = std::strchr(authority, '@');
    if (at == nullptr) return 0;

    const char* colon = std::strchr(authority, ':');
    return colon != nullptr && colon < at;
}

#if defined(__SWITCH2__)
void wiliwili_network_reset(void) {
    bilibili::HTTP::resetDnsCache();
    APP_E->fire("SWITCH2_NETWORK_RECOVERED", nullptr);
}

void wiliwili_report_display_metrics(int refreshHz, int vrr, int outputW, int outputH, int renderW, int renderH,
                                     int docked) {
    wili::PerfDiagnostics::instance().setDisplayMetrics(refreshHz, vrr != 0, outputW, outputH, renderW, renderH,
                                                        docked != 0);
}
#else
void wiliwili_report_display_metrics(int, int, int, int, int, int, int) {}
#endif
