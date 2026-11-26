#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

namespace wili::multicdn {

enum class CdnMode {
    Auto           = 0,
    PreferMainland = 1,
    PreferOverseas = 2,
};

enum class CdnUrlClass {
    Mirror,
    Bcache,
    McdnUpgcxcode,
    McdnTf,
    Szbdyd,
    Unknown,
};

enum class SignatureFamily {
    Upos,
    Akamai,
};

enum class CandidateTier {
    ListedMirror = 0,
    ListedBcache = 1,
    Mirror       = 2,
    Bcache       = 3,
    Proxy        = 4,
    P2P          = 5,
    Foreign      = 6,
};

struct ParsedUrl {
    std::string scheme;
    std::string host;
    int port = 0;
    std::string pathname;
    std::string query;
    std::string pathAndQuery;
    std::string href;
    bool valid = false;

    std::string queryParam(const std::string& key) const {
        if (query.empty() || key.empty()) return {};
        size_t pos = 0;
        while (pos < query.size()) {
            size_t amp = query.find('&', pos);
            if (amp == std::string::npos) amp = query.size();
            size_t eq = query.find('=', pos);
            if (eq != std::string::npos && eq < amp) {
                if (query.compare(pos, eq - pos, key) == 0) {
                    return query.substr(eq + 1, amp - (eq + 1));
                }
            } else if (query.compare(pos, amp - pos, key) == 0) {
                return {};
            }
            pos = amp + 1;
        }
        return {};
    }

    bool hasQueryParam(const std::string& key) const {
        if (query.empty() || key.empty()) return false;
        size_t pos = 0;
        while (pos < query.size()) {
            size_t amp = query.find('&', pos);
            if (amp == std::string::npos) amp = query.size();
            size_t eq = query.find('=', pos);
            size_t klen = (eq != std::string::npos && eq < amp) ? (eq - pos) : (amp - pos);
            if (klen == key.size() && query.compare(pos, klen, key) == 0) {
                return true;
            }
            pos = amp + 1;
        }
        return false;
    }
};

namespace detail {

inline std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

inline bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool endsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool contains(const std::string& s, const std::string& sub) { return s.find(sub) != std::string::npos; }

inline bool isAlphaNum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; }

inline bool isDigit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

inline bool isIpv4Host(const std::string& host) {
    int dots   = 0;
    int segLen = 0;
    int val    = 0;
    for (char c : host) {
        if (c == '.') {
            if (segLen == 0 || val > 255) return false;
            dots++;
            segLen = 0;
            val    = 0;
        } else if (isDigit(c)) {
            val = val * 10 + (c - '0');
            segLen++;
            if (segLen > 3 || val > 255) return false;
        } else {
            return false;
        }
    }
    return dots == 3 && segLen > 0 && val <= 255;
}

inline uint64_t parsePositiveUint64(const std::string& s) {
    if (s.empty()) return 0;
    uint64_t val = 0;
    for (char c : s) {
        if (!isDigit(c)) return 0;
        val = val * 10 + static_cast<uint64_t>(c - '0');
    }
    return val;
}

}  // namespace detail

inline ParsedUrl parseUrl(const std::string& raw) {
    ParsedUrl out;
    size_t schemeEnd = raw.find("://");
    if (schemeEnd == std::string::npos || schemeEnd == 0) return out;

    out.scheme = detail::toLowerAscii(raw.substr(0, schemeEnd));
    if (out.scheme != "http" && out.scheme != "https") return out;

    size_t authStart = schemeEnd + 3;
    size_t hashPos   = raw.find('#', authStart);
    size_t endPos    = (hashPos == std::string::npos) ? raw.size() : hashPos;

    size_t pathStart = raw.find('/', authStart);
    size_t qStart    = raw.find('?', authStart);
    size_t authEnd   = endPos;
    if (pathStart != std::string::npos && pathStart < authEnd) authEnd = pathStart;
    if (qStart != std::string::npos && qStart < authEnd) authEnd = qStart;

    std::string authority = raw.substr(authStart, authEnd - authStart);
    if (authority.empty()) return out;

    // Strip optional userinfo
    size_t atPos = authority.rfind('@');
    if (atPos != std::string::npos) {
        authority = authority.substr(atPos + 1);
    }
    if (authority.empty()) return out;

    if (authority.front() == '[') {
        size_t closeBracket = authority.find(']');
        if (closeBracket == std::string::npos) return out;
        out.host = detail::toLowerAscii(authority.substr(0, closeBracket + 1));
        if (closeBracket + 1 < authority.size() && authority[closeBracket + 1] == ':') {
            out.port = static_cast<int>(detail::parsePositiveUint64(authority.substr(closeBracket + 2)));
        }
    } else {
        size_t colonPos = authority.rfind(':');
        if (colonPos != std::string::npos) {
            out.host = detail::toLowerAscii(authority.substr(0, colonPos));
            out.port = static_cast<int>(detail::parsePositiveUint64(authority.substr(colonPos + 1)));
        } else {
            out.host = detail::toLowerAscii(authority);
        }
    }

    if (out.host.empty()) return out;

    if (authEnd < endPos) {
        out.pathAndQuery = raw.substr(authEnd, endPos - authEnd);
        if (out.pathAndQuery.front() == '?') {
            out.pathAndQuery = "/" + out.pathAndQuery;
        }
    } else {
        out.pathAndQuery = "/";
    }

    size_t pqQuery = out.pathAndQuery.find('?');
    if (pqQuery == std::string::npos) {
        out.pathname = out.pathAndQuery;
    } else {
        out.pathname = out.pathAndQuery.substr(0, pqQuery);
        out.query    = out.pathAndQuery.substr(pqQuery + 1);
    }

    out.href  = out.scheme + "://" + out.host + (out.port > 0 ? (":" + std::to_string(out.port)) : "") + out.pathAndQuery;
    out.valid = true;
    return out;
}

inline std::string buildUrlWithHost(const ParsedUrl& base, const std::string& newHost) {
    if (!base.valid || newHost.empty()) return {};
    const std::string host = detail::toLowerAscii(newHost);
    // Clear any non-standard port (e.g., PCDN :4483) when swapping onto an ordinary CDN host.
    return base.scheme + "://" + host + base.pathAndQuery;
}

inline bool isP2pCdnDomain(const std::string& host) {
    const std::string h = detail::toLowerAscii(host);
    if (h.empty()) return false;
    static constexpr const char* kPatterns[] = {
        "302ppio",
        "302kodo",
        ".mcdn.bilivideo",
        "szbdyd.com",
        ".nexusedgeio.com",
        ".ahdohpiechei.com",
        "upos-sz-mirror14b.bilivideo.com",
    };
    for (const char* p : kPatterns) {
        if (detail::contains(h, p)) return true;
    }
    size_t dot        = h.find('.');
    std::string label = (dot == std::string::npos) ? h : h.substr(0, dot);
    return detail::contains(label, "302");
}

inline bool isAkamaiHost(const std::string& host) {
    return detail::endsWith(detail::toLowerAscii(host), ".akamaized.net");
}

inline bool isProxyHost(const std::string& host) {
    const std::string h = detail::toLowerAscii(host);
    return detail::startsWith(h, "upos-tf-") || detail::startsWith(h, "proxy-tf-");
}

inline bool isMirrorHost(const std::string& host) {
    const std::string h = detail::toLowerAscii(host);
    if (!detail::endsWith(h, ".bilivideo.com") && !detail::endsWith(h, ".bilivideo.net") &&
        !detail::endsWith(h, ".akamaized.net") && !detail::endsWith(h, ".akamaized.com")) {
        return false;
    }
    if (isProxyHost(h)) return true;
    if (!detail::startsWith(h, "upos-")) return false;

    size_t firstDash = 4;  // index of '-' in "upos-"
    size_t secDash   = h.find('-', firstDash + 1);
    size_t firstDot  = h.find('.');
    if (secDash == std::string::npos || firstDot == std::string::npos || secDash >= firstDot) return false;
    if (secDash == firstDash + 1) return false;
    for (size_t i = firstDash + 1; i < secDash; ++i) {
        if (!detail::isAlphaNum(h[i]) && h[i] != '_') return false;
    }
    std::string tag = h.substr(secDash + 1, firstDot - (secDash + 1));
    if (tag.empty() || detail::startsWith(tag, "302")) return false;
    for (char c : tag) {
        if (!detail::isAlphaNum(c) && c != '_') return false;
    }
    return true;
}

inline bool isOverseasHost(const std::string& host) {
    const std::string h = detail::toLowerAscii(host);
    if (isAkamaiHost(h)) return true;
    if (detail::startsWith(h, "cn-hk-")) return true;
    size_t dot = h.find('.');
    if (dot != std::string::npos) {
        std::string label = h.substr(0, dot);
        if (detail::endsWith(label, "ov")) return true;
    }
    return false;
}

inline bool isMainlandMirrorHost(const std::string& host) {
    return isMirrorHost(host) && !isOverseasHost(host) && !isP2pCdnDomain(host) && !isProxyHost(host);
}

inline CdnUrlClass classifyCdnUrl(const ParsedUrl& url) {
    if (!url.valid) return CdnUrlClass::Unknown;
    if (detail::contains(url.pathname, "/upgcxcode/") || detail::contains(url.href, "/upgcxcode/")) {
        if (isMirrorHost(url.host)) {
            if (detail::toLowerAscii(url.queryParam("os")) != "mcdn" && !isP2pCdnDomain(url.host)) {
                return CdnUrlClass::Mirror;
            }
            return CdnUrlClass::McdnUpgcxcode;
        }
        return isP2pCdnDomain(url.host) ? CdnUrlClass::McdnUpgcxcode : CdnUrlClass::Bcache;
    }
    if ((detail::isIpv4Host(url.host) || detail::contains(url.host, ".mcdn.bilivideo.")) &&
        url.pathname.size() >= 12 && url.pathname[0] == '/' && url.pathname[1] == 'v' &&
        detail::isDigit(url.pathname[2]) && url.pathname.compare(3, 9, "/resource") == 0) {
        return CdnUrlClass::McdnTf;
    }
    if (detail::contains(url.host, "szbdyd.com")) {
        return CdnUrlClass::Szbdyd;
    }
    return CdnUrlClass::Unknown;
}

inline SignatureFamily signatureFamilyOf(const ParsedUrl& url) {
    if (url.hasQueryParam("hdnts")) return SignatureFamily::Akamai;
    if (!url.hasQueryParam("upsig") && isAkamaiHost(url.host)) return SignatureFamily::Akamai;
    return SignatureFamily::Upos;
}

inline uint64_t signatureDeadlineOf(const ParsedUrl& url) {
    uint64_t deadline = detail::parsePositiveUint64(url.queryParam("deadline"));
    std::string hdnts = url.queryParam("hdnts");
    uint64_t expVal   = 0;
    if (!hdnts.empty()) {
        size_t pos = 0;
        while (pos < hdnts.size()) {
            size_t tilde = hdnts.find('~', pos);
            if (tilde == std::string::npos) tilde = hdnts.size();
            if (hdnts.compare(pos, 4, "exp=") == 0) {
                expVal = detail::parsePositiveUint64(hdnts.substr(pos + 4, tilde - (pos + 4)));
                break;
            }
            pos = tilde + 1;
        }
    }
    if (deadline == 0 || expVal == 0) return deadline != 0 ? deadline : expVal;
    return std::min(deadline, expVal);
}

inline const std::vector<std::string>& mainlandSeedHosts() {
    static const std::vector<std::string> kHosts = {
        "upos-sz-mirrorali.bilivideo.com",
        "upos-sz-mirrorhw.bilivideo.com",
        "upos-sz-mirrorbos.bilivideo.com",
        "upos-sz-mirror08c.bilivideo.com",
        "upos-sz-mirrorbd.bilivideo.com",
        "upos-sz-mirrorcos.bilivideo.com",
        "upos-sz-mirrorcoso1.bilivideo.com",
        "upos-sz-mirrorcosb.bilivideo.com",
        "upos-sz-estgoss.bilivideo.com",
    };
    return kHosts;
}

inline const std::vector<std::string>& overseasSeedHosts() {
    static const std::vector<std::string> kHosts = {
        "upos-sz-mirrorcosov.bilivideo.com",
        "upos-sz-mirroraliov.bilivideo.com",
        "cn-hk-eq-01-01.bilivideo.com",
        "cn-hk-eq-01-03.bilivideo.com",
    };
    return kHosts;
}

inline std::vector<std::string> extractUpgcxcodeHosts(const std::vector<std::string>& urls) {
    std::vector<std::string> hosts;
    std::unordered_set<std::string> seen;
    for (const auto& raw : urls) {
        ParsedUrl u = parseUrl(raw);
        if (!u.valid) continue;
        CdnUrlClass cls = classifyCdnUrl(u);
        if ((cls == CdnUrlClass::Mirror || cls == CdnUrlClass::Bcache) && !isAkamaiHost(u.host) &&
            !isProxyHost(u.host) && !isP2pCdnDomain(u.host)) {
            if (seen.insert(u.host).second) {
                hosts.push_back(u.host);
            }
        }
    }
    return hosts;
}

struct MediaCandidate {
    std::string url;
    std::string host;
    std::string addressKey;  // pathname + query
    SignatureFamily family = SignatureFamily::Upos;
    uint64_t deadline      = 0;
    CandidateTier tier     = CandidateTier::ListedMirror;
    bool p2p               = false;
    bool listed            = false;
    bool overseas          = false;
};

inline std::vector<MediaCandidate> buildCandidates(const std::vector<std::string>& listedUrls,
                                                   const std::vector<std::string>& learnedHosts,
                                                   CdnMode mode = CdnMode::Auto) {
    std::vector<MediaCandidate> out;
    std::unordered_set<std::string> seenUrls;

    std::vector<ParsedUrl> parsedListed;
    parsedListed.reserve(listedUrls.size());
    for (const auto& raw : listedUrls) {
        ParsedUrl u = parseUrl(raw);
        if (!u.valid) continue;
        parsedListed.push_back(u);
        if (seenUrls.insert(u.href).second) {
            CdnUrlClass cls = classifyCdnUrl(u);
            CandidateTier tier = CandidateTier::ListedMirror;
            bool p2p           = false;
            if (cls == CdnUrlClass::Bcache) {
                tier = CandidateTier::ListedBcache;
            } else if (cls == CdnUrlClass::McdnUpgcxcode || cls == CdnUrlClass::McdnTf || cls == CdnUrlClass::Szbdyd) {
                tier = CandidateTier::P2P;
                p2p  = true;
            } else if (isProxyHost(u.host)) {
                tier = CandidateTier::Proxy;
            }
            MediaCandidate c;
            c.url        = u.href;
            c.host       = u.host;
            c.addressKey = u.pathAndQuery;
            c.family     = signatureFamilyOf(u);
            c.deadline   = signatureDeadlineOf(u);
            c.tier       = tier;
            c.p2p        = p2p;
            c.listed     = true;
            c.overseas   = isOverseasHost(u.host);
            out.push_back(std::move(c));
        }
    }

    // Build unified target host pool ordered by CdnMode preference for initial seeding.
    std::vector<std::string> targetHosts;
    std::unordered_set<std::string> seenHosts;
    auto appendHost = [&](const std::string& h) {
        const std::string lower = detail::toLowerAscii(h);
        if (lower.empty() || isP2pCdnDomain(lower) || isProxyHost(lower) || isAkamaiHost(lower)) return;
        if (seenHosts.insert(lower).second) {
            targetHosts.push_back(lower);
        }
    };

    if (mode == CdnMode::PreferOverseas) {
        for (const auto& h : overseasSeedHosts()) appendHost(h);
        for (const auto& h : learnedHosts) {
            if (isOverseasHost(h)) appendHost(h);
        }
        for (const auto& h : learnedHosts) appendHost(h);
        for (const auto& h : mainlandSeedHosts()) appendHost(h);
    } else if (mode == CdnMode::PreferMainland) {
        for (const auto& h : mainlandSeedHosts()) appendHost(h);
        for (const auto& h : learnedHosts) {
            if (!isOverseasHost(h)) appendHost(h);
        }
        for (const auto& h : learnedHosts) appendHost(h);
        for (const auto& h : overseasSeedHosts()) appendHost(h);
    } else {
        for (const auto& h : learnedHosts) appendHost(h);
        for (const auto& h : mainlandSeedHosts()) appendHost(h);
        for (const auto& h : overseasSeedHosts()) appendHost(h);
    }

    // Collect /upgcxcode/ donors. Prefer non-Akamai Upos donors; fall back to Akamai donors
    // onto ordinary mirrors only when no non-Akamai ordinary donor exists (BTR overseas safeguard).
    std::vector<ParsedUrl> uposDonors;
    std::vector<ParsedUrl> akamDonors;
    for (const auto& u : parsedListed) {
        CdnUrlClass cls = classifyCdnUrl(u);
        if (cls == CdnUrlClass::Mirror || cls == CdnUrlClass::Bcache || cls == CdnUrlClass::McdnUpgcxcode) {
            if (signatureFamilyOf(u) == SignatureFamily::Upos && !isAkamaiHost(u.host)) {
                uposDonors.push_back(u);
            } else {
                akamDonors.push_back(u);
            }
        }
    }

    const std::vector<ParsedUrl>& activeDonors = !uposDonors.empty() ? uposDonors : akamDonors;
    const bool usingAkamFallback               = uposDonors.empty() && !akamDonors.empty();

    std::unordered_set<std::string> movedAddresses;
    for (const auto& donor : activeDonors) {
        if (!movedAddresses.insert(donor.pathAndQuery).second) continue;
        for (const auto& targetHost : targetHosts) {
            std::string swapped = buildUrlWithHost(donor, targetHost);
            if (swapped.empty() || !seenUrls.insert(swapped).second) continue;

            CandidateTier tier = CandidateTier::Mirror;
            if (usingAkamFallback) {
                tier = CandidateTier::Foreign;
            } else if (!isMirrorHost(targetHost)) {
                tier = CandidateTier::Bcache;
            }

            MediaCandidate c;
            c.url        = std::move(swapped);
            c.host       = targetHost;
            c.addressKey = donor.pathAndQuery;
            c.family     = signatureFamilyOf(donor);
            c.deadline   = signatureDeadlineOf(donor);
            c.tier       = tier;
            c.p2p        = false;
            c.listed     = false;
            c.overseas   = isOverseasHost(targetHost);
            out.push_back(std::move(c));
        }
    }

    return out;
}

}  // namespace wili::multicdn
