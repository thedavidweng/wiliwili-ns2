// Host tests for Multi-CDN candidate classification, per-video host speed
// modeling, and end-to-end loopback HTTP Range proxy streaming & integrity.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <curl/curl.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using TestSock = SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using TestSock = int;
#endif

#include "test_check.hpp"
#include "utils/multi_cdn_candidates.hpp"
#include "utils/multi_cdn_host_model.hpp"
#include "utils/multi_cdn_proxy.hpp"

namespace {

using namespace wili::multicdn;

class MockHttpServer {
public:
    struct Config {
        int statusCode      = 206;
        int delayMs         = 0;
        bool corruptByte    = false;
        size_t corruptAt    = 0;
        bool dropConnection = false;
        size_t pauseAfter   = 0;
        int pauseMs         = 0;
        std::vector<uint8_t> payload;
    };

    explicit MockHttpServer(Config cfg) : cfg_(std::move(cfg)) {}
    ~MockHttpServer() { stop(); }

    bool start() {
        listenSock_ = ::socket(AF_INET6, SOCK_STREAM, 0);
        if (listenSock_ >= 0) {
            int opt   = 1;
            int v6off = 0;
            ::setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
            ::setsockopt(listenSock_, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6off), sizeof(v6off));
            sockaddr_in6 addr6{};
            addr6.sin6_family = AF_INET6;
            addr6.sin6_addr   = in6addr_any;
            addr6.sin6_port   = 0;
            if (::bind(listenSock_, reinterpret_cast<const sockaddr*>(&addr6), sizeof(addr6)) == 0 &&
                ::listen(listenSock_, 16) == 0) {
                sockaddr_in6 bound{};
                socklen_t len = sizeof(bound);
                if (::getsockname(listenSock_, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
                    port_    = ntohs(bound.sin6_port);
                    running_ = true;
                    thread_  = std::thread(&MockHttpServer::run, this);
                    return true;
                }
            }
            ::close(listenSock_);
            listenSock_ = -1;
        }

        // Fallback to IPv4 loopback if IPv6 is unavailable.
        listenSock_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenSock_ < 0) return false;
        int opt = 1;
        ::setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;
        if (::bind(listenSock_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(listenSock_, 16) != 0) return false;
        sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (::getsockname(listenSock_, reinterpret_cast<sockaddr*>(&bound), &len) != 0) return false;
        port_    = ntohs(bound.sin_port);
        running_ = true;
        thread_  = std::thread(&MockHttpServer::run, this);
        return true;
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (listenSock_ >= 0) {
            ::close(listenSock_);
            listenSock_ = -1;
        }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t port() const { return port_; }
    int requestCount() const { return requests_.load(); }

private:
    void run() {
        while (running_.load()) {
            struct pollfd pfd{listenSock_, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0) continue;
            sockaddr_storage peer{};
            socklen_t plen = sizeof(peer);
            TestSock c     = ::accept(listenSock_, reinterpret_cast<sockaddr*>(&peer), &plen);
            if (c < 0) continue;
            requests_.fetch_add(1);
            std::thread([this, c]() { serveOne(c); }).detach();
        }
    }

    void serveOne(TestSock c) {
        std::string req;
        char buf[2048];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
            auto n = ::recv(c, buf, sizeof(buf), 0);
            if (n <= 0) {
                ::close(c);
                return;
            }
            req.append(buf, static_cast<size_t>(n));
        }

        if (cfg_.delayMs > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.delayMs));
        }
        if (cfg_.dropConnection) {
            ::close(c);
            return;
        }

        if (cfg_.statusCode != 206) {
            std::ostringstream oss;
            oss << "HTTP/1.1 " << cfg_.statusCode << " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            std::string h = oss.str();
            ::send(c, h.data(), h.size(), 0);
            ::close(c);
            return;
        }

        uint64_t rStart = 0;
        uint64_t rEnd   = cfg_.payload.empty() ? 0 : (cfg_.payload.size() - 1);
        auto pos        = req.find("Range: bytes=");
        if (pos == std::string::npos) pos = req.find("range: bytes=");
        if (pos != std::string::npos) {
            auto eol  = req.find("\r\n", pos);
            auto spec = req.substr(pos + 13, eol - (pos + 13));
            auto dash = spec.find('-');
            if (dash != std::string::npos) {
                rStart    = std::stoull(spec.substr(0, dash));
                auto tail = spec.substr(dash + 1);
                if (!tail.empty()) rEnd = std::min<uint64_t>(std::stoull(tail), cfg_.payload.size() - 1);
            }
        }

        uint64_t total = cfg_.payload.size();
        uint64_t len   = (rEnd >= rStart && rStart < total) ? (rEnd - rStart + 1) : 0;

        std::ostringstream hdr;
        hdr << "HTTP/1.1 206 Partial Content\r\n"
            << "Content-Range: bytes " << rStart << "-" << rEnd << "/" << total << "\r\n"
            << "Content-Length: " << len << "\r\n"
            << "Connection: close\r\n\r\n";
        std::string hs = hdr.str();
        ::send(c, hs.data(), hs.size(), 0);

        if (len > 0) {
            std::vector<uint8_t> body(cfg_.payload.begin() + rStart, cfg_.payload.begin() + rStart + len);
            if (cfg_.corruptByte && cfg_.corruptAt >= rStart && cfg_.corruptAt <= rEnd) {
                body[cfg_.corruptAt - rStart] ^= 0xFF;
            }
            size_t sent = 0;
            bool paused = false;
            while (sent < body.size()) {
                size_t toSend = body.size() - sent;
                if (!paused && cfg_.pauseAfter > 0 && sent < cfg_.pauseAfter) {
                    toSend = std::min(toSend, cfg_.pauseAfter - sent);
                }
                auto n = ::send(c, reinterpret_cast<const char*>(body.data() + sent), toSend, 0);
                if (n <= 0) break;
                sent += static_cast<size_t>(n);
                if (!paused && cfg_.pauseAfter > 0 && sent >= cfg_.pauseAfter) {
                    paused = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(cfg_.pauseMs));
                }
            }
        }
        ::close(c);
    }

    Config cfg_;
    TestSock listenSock_ = -1;
    uint16_t port_       = 0;
    std::atomic<bool> running_{false};
    std::atomic<int> requests_{0};
    std::thread thread_;
};

struct HttpFetchResult {
    long status = 0;
    std::vector<uint8_t> body;
    std::string headers;
};

size_t captureBody(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* out = static_cast<std::vector<uint8_t>*>(ud);
    out->insert(out->end(), reinterpret_cast<uint8_t*>(ptr), reinterpret_cast<uint8_t*>(ptr) + size * nmemb);
    return size * nmemb;
}

size_t captureHeader(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* out = static_cast<std::string*>(ud);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

HttpFetchResult fetchViaCurl(const std::string& url, const std::string& range = {}, bool headOnly = false) {
    HttpFetchResult res;
    CURL* curl = curl_easy_init();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (!range.empty()) curl_easy_setopt(curl, CURLOPT_RANGE, range.c_str());
    if (headOnly) curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, captureHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);
    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    curl_easy_cleanup(curl);
    return res;
}

void testUrlClassificationAndCandidates() {
    auto uMirror = parseUrl(
        "https://upos-sz-mirrorcos.bilivideo.com/upgcxcode/12/34/56.m4s?upsig=abc&deadline=2000000000");
    CHECK(uMirror.valid);
    CHECK(classifyCdnUrl(uMirror) == CdnUrlClass::Mirror);
    CHECK(signatureFamilyOf(uMirror) == SignatureFamily::Upos);
    CHECK(signatureDeadlineOf(uMirror) == 2000000000ULL);
    CHECK(!isP2pCdnDomain(uMirror.host));

    auto uAkam = parseUrl(
        "https://upos-hz-mirrorakam.akamaized.net/upgcxcode/12/34/56.m4s?hdnts=exp=1800000000~hmac=zz&deadline=1900000000");
    CHECK(classifyCdnUrl(uAkam) == CdnUrlClass::Mirror);
    CHECK(signatureFamilyOf(uAkam) == SignatureFamily::Akamai);
    CHECK(signatureDeadlineOf(uAkam) == 1800000000ULL);

    auto uMcdn = parseUrl("https://xy123x45x67x89xy.mcdn.bilivideo.cn:4483/v1/resource/abc.m4s");
    CHECK(classifyCdnUrl(uMcdn) == CdnUrlClass::McdnTf);
    CHECK(isP2pCdnDomain(uMcdn.host));

    auto uBcache = parseUrl("https://cn-hk-eq-01-01.bilivideo.com/upgcxcode/12/34/56.m4s?upsig=abc");
    CHECK(classifyCdnUrl(uBcache) == CdnUrlClass::Bcache);
    CHECK(isOverseasHost(uBcache.host));

    // Upos donor must NOT be moved onto Akamai, and synthetic URLs drop non-standard ports.
    auto candsOverseas = buildCandidates(
        {"https://upos-sz-mirrorali.bilivideo.com/upgcxcode/1/2/3.m4s?upsig=xyz"},
        {"cn-sccd-cu-01-01.bilivideo.com"},
        CdnMode::PreferOverseas);
    CHECK(!candsOverseas.empty());
    bool hasAkamai            = false;
    bool hasOverseasSynthetic = false;
    for (const auto& c : candsOverseas) {
        if (isAkamaiHost(c.host)) hasAkamai = true;
        if (c.host == "upos-sz-mirrorcosov.bilivideo.com" && !c.listed) hasOverseasSynthetic = true;
    }
    CHECK(!hasAkamai);
    CHECK(hasOverseasSynthetic);

    // Overseas Akamai-only fallback: allows swapping onto ordinary mirrors when no Upos donor exists.
    auto akamOnly = buildCandidates(
        {"https://upos-hz-mirrorakam.akamaized.net/upgcxcode/1/2/3.m4s?hdnts=exp=2000000000~hmac=zz"},
        {}, CdnMode::Auto);
    CHECK(akamOnly.size() > 1);
    CHECK(akamOnly[0].host == "upos-hz-mirrorakam.akamaized.net");
    CHECK(akamOnly[1].tier == CandidateTier::Foreign);
}

void testHostModel() {
    HostModel hm;
    const std::string vidA = "av100";
    const std::string vidB = "av200";
    const std::string file = "f1";

    hm.recordSpeed(vidA, "fast.bilivideo.com", 20.0, 4096.0, false, 1000);
    hm.recordSpeed(vidA, "slow.bilivideo.com", 200.0, 256.0, false, 1000);

    auto estFast = hm.estimate(vidA, "fast.bilivideo.com", 512 * 1024, 1500);
    auto estSlow = hm.estimate(vidA, "slow.bilivideo.com", 512 * 1024, 1500);
    CHECK(estFast.measured && estSlow.measured);
    CHECK(estFast.etaMs < estSlow.etaMs);

    // Per-video isolation: vidB must not inherit vidA's speed stats.
    auto estB = hm.estimate(vidB, "fast.bilivideo.com", 512 * 1024, 1500);
    CHECK(!estB.measured);

    // Distinct-host racer selection & cooldown.
    MediaCandidate c1{"https://fast.bilivideo.com/upgcxcode/a.m4s", "fast.bilivideo.com", "/upgcxcode/a.m4s",
                      SignatureFamily::Upos, 0, CandidateTier::ListedMirror, false, true, false};
    MediaCandidate c2{"https://slow.bilivideo.com/upgcxcode/a.m4s", "slow.bilivideo.com", "/upgcxcode/a.m4s",
                      SignatureFamily::Upos, 0, CandidateTier::Mirror, false, false, false};
    std::vector<MediaCandidate> cands = {c1, c2};
    std::unordered_map<std::string, int> flight;
    std::unordered_set<std::string> excl;
    CHECK_EQ(hm.pickBestCandidate(cands, vidA, file, 512 * 1024, excl, flight, 3, CdnMode::Auto, 1500, 1000), 0);
    excl.insert("fast.bilivideo.com");
    CHECK_EQ(hm.pickBestCandidate(cands, vidA, file, 512 * 1024, excl, flight, 3, CdnMode::Auto, 1500, 1000), 1);

    // Cooldown excludes slow host after Throttled.
    hm.recordOutcome(vidA, file, "slow.bilivideo.com", SignatureFamily::Upos, FetchOutcome::Throttled, 1500);
    CHECK(!hm.isEligible(c2, file, 1600, 1000));
}

void testLoopbackProxyStreamingAndIntegrity() {
    std::vector<uint8_t> payload(1600 * 1024);
    for (size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<uint8_t>((i * 131 + 17) & 0xFF);
    }

    // 1. Multi-host streaming (closed range, open-ended range, HEAD, and slow-host rescue).
    MockHttpServer fastSrv(MockHttpServer::Config{206, 0, false, 0, false, 0, 0, payload});
    MockHttpServer slowSrv(MockHttpServer::Config{206, 2500, false, 0, false, 0, 0, payload});
    MockHttpServer errSrv(MockHttpServer::Config{503, 0, false, 0, false, 0, 0, payload});
    CHECK(fastSrv.start());
    CHECK(slowSrv.start());
    CHECK(errSrv.start());

    auto& proxy = MultiCdnProxy::instance();
    proxy.resetStateForTests();
    ProxyBudget budget = defaultProxyBudget();
    budget.pieceBytes  = 256 * 1024;
    budget.hedgeIdleMs = 200;
    proxy.setBudget(budget);
    CHECK(proxy.ensureStarted());

    std::string fastUrl = "http://127.0.0.1:" + std::to_string(fastSrv.port()) + "/media/test.m4s";
    std::string slowUrl = "http://localhost:" + std::to_string(slowSrv.port()) + "/media/test.m4s";
    std::string errUrl  = "http://localhost:" + std::to_string(errSrv.port()) + "/media/test.m4s";

    // slowUrl comes first on localhost; fastUrl on 127.0.0.1 rescues it immediately via race.
    std::string localUrl = proxy.registerTrack("vid1", "file1", {slowUrl, fastUrl}, {}, CdnMode::Auto);
    CHECK(MultiCdnProxy::isLoopbackProxyUrl(localUrl));

    // Bounded range request across multiple 256 KiB pieces.
    auto r1 = fetchViaCurl(localUrl, "0-1048575");
    CHECK_EQ(r1.status, 206);
    CHECK_EQ(r1.body.size(), size_t(1048576));
    CHECK(std::memcmp(r1.body.data(), payload.data(), r1.body.size()) == 0);

    // Open-ended range request (mpv DASH streaming pattern: Range: bytes=524288-).
    auto r2 = fetchViaCurl(localUrl, "524288-");
    CHECK_EQ(r2.status, 206);
    CHECK_EQ(r2.body.size(), payload.size() - 524288);
    CHECK(std::memcmp(r2.body.data(), payload.data() + 524288, r2.body.size()) == 0);

    // HEAD request (should return cached Content-Length immediately).
    auto rHead = fetchViaCurl(localUrl, "", true);
    CHECK_EQ(rHead.status, 200);
    CHECK(rHead.headers.find("Content-Length: " + std::to_string(payload.size())) != std::string::npos);

    // 2. Failing primary (503 on localhost) skipped cleanly in favor of healthy secondary (127.0.0.1).
    proxy.resetStateForTests();
    std::string fallbackOkLocal = proxy.registerTrack("vidFailover", "fileFailover", {errUrl, fastUrl}, {}, CdnMode::Auto);
    auto rOk = fetchViaCurl(fallbackOkLocal, "0-262143");
    CHECK_EQ(rOk.status, 206);
    CHECK_EQ(rOk.body.size(), size_t(262144));
    CHECK(std::memcmp(rOk.body.data(), payload.data(), rOk.body.size()) == 0);

    // 3. Integrity mismatch detection (two servers return different bytes -> aborts & trips circuit).
    proxy.resetStateForTests();
    MockHttpServer corruptSrv(MockHttpServer::Config{206, 0, true, 128, false, 4096, 80, payload});
    CHECK(corruptSrv.start());
    std::string corruptUrl = "http://localhost:" + std::to_string(corruptSrv.port()) + "/media/corrupt.m4s";
    std::string integrityLocal =
        proxy.registerTrack("vid2", "file2", {corruptUrl, fastUrl}, {}, CdnMode::Auto);
    auto rBad = fetchViaCurl(integrityLocal, "0-65535");
    CHECK(rBad.status == 307 || rBad.body.size() < 65536);
    // Circuit breaker must remain open immediately after integrity mismatch -> next request returns 307.
    auto rCircuit = fetchViaCurl(integrityLocal, "0-65535");
    CHECK_EQ(rCircuit.status, 307);

    // 4. All hosts failing -> 307 Temporary Redirect to primary URL.
    proxy.resetStateForTests();
    std::string allFailLocal = proxy.registerTrack("vid3", "file3", {errUrl}, {}, CdnMode::Auto);
    auto r307 = fetchViaCurl(allFailLocal, "0-65535");
    CHECK_EQ(r307.status, 307);
    CHECK(r307.headers.find("Location: " + errUrl) != std::string::npos);

    proxy.shutdown();
}

}  // namespace

int main() {
    curl_global_init(CURL_GLOBAL_ALL);
    testUrlClassificationAndCandidates();
    testHostModel();
    testLoopbackProxyStreamingAndIntegrity();
    curl_global_cleanup();
    return test_check::report("all multi_cdn checks passed");
}
