#include "utils/multi_cdn_proxy.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <map>
#include <sstream>

#include <curl/curl.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using SockHandle = SOCKET;
static constexpr SockHandle kInvalidSock = INVALID_SOCKET;
static inline void closeSock(SockHandle s) {
    if (s != INVALID_SOCKET) closesocket(s);
}
static inline int lastSockErr() { return WSAGetLastError(); }
static inline bool isWouldBlock(int err) { return err == WSAEWOULDBLOCK; }
static inline bool setNonBlocking(SockHandle s) {
    u_long mode = 1;
    return ioctlsocket(s, FIONBIO, &mode) == 0;
}
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
using SockHandle = int;
static constexpr SockHandle kInvalidSock = -1;
static inline void closeSock(SockHandle s) {
    if (s >= 0) ::close(s);
}
static inline int lastSockErr() { return errno; }
static inline bool isWouldBlock(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
static inline bool setNonBlocking(SockHandle s) {
    int flags = fcntl(s, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
#endif

namespace wili::multicdn {

namespace {

int64_t steadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t unixNowSec() {
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch())
                    .count();
    return secs > 0 ? static_cast<uint64_t>(secs) : 0;
}

struct ParsedHttpRequest {
    std::string method;
    std::string path;
    bool hasRange    = false;
    bool openEnded   = true;
    uint64_t start   = 0;
    uint64_t end     = 0;
    bool valid       = false;
};

std::string trimAscii(std::string s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) {
        s.erase(s.begin());
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

bool parseRangeHeader(const std::string& val, bool& openEnded, uint64_t& start, uint64_t& end) {
    std::string lower = detail::toLowerAscii(trimAscii(val));
    if (!detail::startsWith(lower, "bytes=")) return false;
    std::string spec = trimAscii(lower.substr(6));
    size_t comma     = spec.find(',');
    if (comma != std::string::npos) return false;  // multi-range unsupported -> 307 fallback
    size_t dash = spec.find('-');
    if (dash == std::string::npos || dash == 0) return false;

    std::string sStr = spec.substr(0, dash);
    std::string eStr = spec.substr(dash + 1);
    for (char c : sStr) {
        if (!detail::isDigit(c)) return false;
    }
    start = detail::parsePositiveUint64(sStr);
    if (sStr != "0" && start == 0) return false;

    if (eStr.empty()) {
        openEnded = true;
        end       = 0;
        return true;
    }
    for (char c : eStr) {
        if (!detail::isDigit(c)) return false;
    }
    end       = detail::parsePositiveUint64(eStr);
    openEnded = false;
    return end >= start;
}

ParsedHttpRequest parseHttpRequest(const std::string& raw) {
    ParsedHttpRequest req;
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos) return req;
    std::string firstLine = raw.substr(0, lineEnd);
    std::istringstream iss(firstLine);
    std::string target, proto;
    if (!(iss >> req.method >> target >> proto)) return req;
    req.method = detail::toLowerAscii(req.method);
    if (req.method == "get") {
        req.method = "GET";
    } else if (req.method == "head") {
        req.method = "HEAD";
    } else {
        return req;
    }
    size_t qPos = target.find('?');
    req.path    = (qPos == std::string::npos) ? target : target.substr(0, qPos);
    req.valid   = true;

    size_t pos = lineEnd + 2;
    while (pos < raw.size()) {
        size_t next = raw.find("\r\n", pos);
        if (next == std::string::npos || next == pos) break;
        std::string line = raw.substr(pos, next - pos);
        size_t colon     = line.find(':');
        if (colon != std::string::npos) {
            std::string name = detail::toLowerAscii(trimAscii(line.substr(0, colon)));
            std::string val  = trimAscii(line.substr(colon + 1));
            if (name == "range") {
                bool oe = true;
                uint64_t s = 0, e = 0;
                if (parseRangeHeader(val, oe, s, e)) {
                    req.hasRange  = true;
                    req.openEnded = oe;
                    req.start     = s;
                    req.end       = e;
                } else {
                    req.valid = false;
                    return req;
                }
            }
        }
        pos = next + 2;
    }
    return req;
}

bool parseContentRangeHeader(const std::string& val, uint64_t& start, uint64_t& end, uint64_t& total) {
    std::string s = detail::toLowerAscii(trimAscii(val));
    if (!detail::startsWith(s, "bytes ")) return false;
    s = trimAscii(s.substr(6));
    size_t slash = s.find('/');
    size_t dash  = s.find('-');
    if (slash == std::string::npos || dash == std::string::npos || dash > slash) return false;
    std::string sStr = s.substr(0, dash);
    std::string eStr = s.substr(dash + 1, slash - (dash + 1));
    std::string tStr = s.substr(slash + 1);
    if (sStr.empty() || eStr.empty() || tStr.empty() || tStr == "*") return false;
    for (char c : sStr) {
        if (!detail::isDigit(c)) return false;
    }
    for (char c : eStr) {
        if (!detail::isDigit(c)) return false;
    }
    for (char c : tStr) {
        if (!detail::isDigit(c)) return false;
    }
    start = detail::parsePositiveUint64(sStr);
    end   = detail::parsePositiveUint64(eStr);
    total = detail::parsePositiveUint64(tStr);
    if (end < start || total <= end) return false;
    return true;
}

bool sendAllBlocking(SockHandle sock, const char* data, size_t len, const std::atomic<bool>& running) {
    size_t sent = 0;
    while (sent < len && running.load()) {
#ifdef MSG_NOSIGNAL
        int flags = MSG_NOSIGNAL;
#else
        int flags = 0;
#endif
        int chunk = static_cast<int>(std::min<size_t>(len - sent, 64 * 1024));
        auto n    = ::send(sock, data + sent, chunk, flags);
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && isWouldBlock(lastSockErr())) {
#ifdef _WIN32
            WSAPOLLFD pfd{sock, POLLOUT, 0};
            int pr = WSAPoll(&pfd, 1, 50);
#else
            struct pollfd pfd{sock, POLLOUT, 0};
            int pr = ::poll(&pfd, 1, 50);
#endif
            if (pr < 0 && lastSockErr() != EINTR) return false;
        } else {
            return false;
        }
    }
    return sent == len;
}

void sendRedirect307(SockHandle sock, const std::string& location, const std::atomic<bool>& running) {
    std::string resp = "HTTP/1.1 307 Temporary Redirect\r\nLocation: " + location +
                       "\r\nCache-Control: no-store\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sendAllBlocking(sock, resp.data(), resp.size(), running);
}

void sendSimpleStatus(SockHandle sock, const char* statusLine, const std::atomic<bool>& running) {
    std::string resp = std::string("HTTP/1.1 ") + statusLine + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    sendAllBlocking(sock, resp.data(), resp.size(), running);
}

std::string buildSuccessHeaders(bool is206, uint64_t start, uint64_t end, uint64_t totalSize) {
    uint64_t len = (totalSize == 0 || end < start) ? 0 : (end - start + 1);
    std::ostringstream oss;
    if (is206) {
        oss << "HTTP/1.1 206 Partial Content\r\n"
            << "Content-Range: bytes " << start << "-" << end << "/" << totalSize << "\r\n";
    } else {
        oss << "HTTP/1.1 200 OK\r\n";
    }
    oss << "Content-Length: " << len << "\r\n"
        << "Accept-Ranges: bytes\r\n"
        << "Content-Type: application/octet-stream\r\n"
        << "Cache-Control: no-store\r\n"
        << "Connection: close\r\n\r\n";
    return oss.str();
}

FetchOutcome classifyHttpStatus(long status, uint64_t deadlineSec, uint64_t nowUnix) {
    if (status == 200) return FetchOutcome::NoRange;
    if (status >= 300 && status < 400) return FetchOutcome::Redirect;
    if (status == 401 || status == 403) {
        if (deadlineSec > 0 && nowUnix > 0 && nowUnix >= deadlineSec) return FetchOutcome::Expired;
        return FetchOutcome::Refused;
    }
    if (status == 404 || status == 410) return FetchOutcome::Missing;
    if (status == 416) return FetchOutcome::BadRange;
    if (status == 412 || status == 429 || status == 503) return FetchOutcome::Throttled;
    if (status >= 500) return FetchOutcome::ServerError;
    return FetchOutcome::BadRange;
}

FetchOutcome classifyCurlError(CURLcode code) {
    switch (code) {
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_CONNECT:
        case CURLE_SSL_CONNECT_ERROR:
            return FetchOutcome::ConnectFail;
        case CURLE_OPERATION_TIMEDOUT:
            return FetchOutcome::Stall;
        case CURLE_PARTIAL_FILE:
            return FetchOutcome::Truncated;
        default:
            return FetchOutcome::Network;
    }
}

struct PieceState;

struct AttemptState {
    CURL* easy            = nullptr;
    PieceState* piece     = nullptr;
    MediaCandidate cand;
    int64_t startedAtMs   = 0;
    int64_t firstByteAtMs = 0;
    int64_t lastByteAtMs  = 0;
    double estimatedEtaMs = 1500.0;

    long httpStatus       = 0;
    bool hasContentRange  = false;
    uint64_t crStart      = 0;
    uint64_t crEnd        = 0;
    uint64_t crTotal      = 0;
    bool headersChecked   = false;
    bool headersValid     = false;
    bool canceled         = false;
    bool headOnlyProbe    = false;

    uint64_t receivedBytes = 0;
    FetchOutcome errorOutcome = FetchOutcome::Ok;
};

struct PieceState {
    uint64_t index      = 0;
    uint64_t start      = 0;
    uint64_t end        = 0;
    size_t sentLen      = 0;
    int tries           = 0;
    int extraHedges     = 0;
    bool completed      = false;
    bool integrityError = false;
    bool staleObject    = false;
    std::vector<uint8_t> data;
    std::vector<AttemptState*> attempts;
    std::unordered_set<std::string> triedHosts;

    uint64_t expectedLen() const { return (end >= start) ? (end - start + 1) : 0; }
};

struct SessionContext {
    SockHandle clientSock   = kInvalidSock;
    bool isHead             = false;
    bool hasRange           = false;
    bool openEnded          = true;
    uint64_t reqStart       = 0;
    uint64_t reqEnd         = 0;
    uint64_t respEnd        = 0;
    uint64_t totalSize      = 0;
    bool headersCommitted   = false;
    bool clientClosed       = false;
    bool integrityMismatch  = false;
    bool fatalError         = false;

    uint64_t writePos       = 0;
    uint64_t nextPieceStart = 0;
    uint64_t nextPieceIndex = 0;
    int64_t startedAtMs     = 0;
    int64_t lastProgressMs  = 0;
    bool headProbeDone      = false;

    std::vector<uint8_t> pendingHeaderBytes;
    size_t pendingHeaderSent = 0;

    std::map<uint64_t, PieceState*> pieces;
    std::unordered_map<std::string, int> inFlightPerHost;
    int totalInFlight = 0;
};

size_t curlHeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
    size_t totalBytes = size * nitems;
    auto* att         = static_cast<AttemptState*>(userdata);
    if (!att || att->canceled) return 0;

    std::string line(buffer, totalBytes);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.empty()) return totalBytes;

    if (detail::startsWith(detail::toLowerAscii(line), "http/")) {
        att->hasContentRange = false;
        att->crStart = att->crEnd = att->crTotal = 0;
        std::istringstream iss(line);
        std::string proto;
        long code = 0;
        if (iss >> proto >> code) att->httpStatus = code;
        return totalBytes;
    }

    size_t colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = detail::toLowerAscii(trimAscii(line.substr(0, colon)));
        std::string val  = trimAscii(line.substr(colon + 1));
        if (name == "content-range") {
            uint64_t s = 0, e = 0, t = 0;
            if (parseContentRangeHeader(val, s, e, t)) {
                att->hasContentRange = true;
                att->crStart         = s;
                att->crEnd           = e;
                att->crTotal         = t;
            }
        }
    }
    return totalBytes;
}

struct WriteCallbackEnv {
    AttemptState* attempt   = nullptr;
    SessionContext* session = nullptr;
    HostModel* hostModel    = nullptr;
    std::string fileKey;
};

bool validateAttemptHeaders(WriteCallbackEnv& env, uint64_t nowUnix) {
    AttemptState* att   = env.attempt;
    PieceState* piece   = att->piece;
    SessionContext* ses = env.session;

    if (att->headersChecked) return att->headersValid;
    att->headersChecked = true;

    if (att->httpStatus == 0) {
        curl_easy_getinfo(att->easy, CURLINFO_RESPONSE_CODE, &att->httpStatus);
    }

    if (att->httpStatus != 206) {
        att->errorOutcome = classifyHttpStatus(att->httpStatus, att->cand.deadline, nowUnix);
        att->headersValid = false;
        return false;
    }
    if (!att->hasContentRange || att->crStart != piece->start || att->crEnd > piece->end) {
        att->errorOutcome = att->hasContentRange ? FetchOutcome::BadRange : FetchOutcome::Unverifiable;
        att->headersValid = false;
        return false;
    }
    if (ses->totalSize > 0 && att->crTotal != ses->totalSize) {
        att->errorOutcome   = FetchOutcome::StaleObject;
        piece->staleObject  = true;
        att->headersValid   = false;
        return false;
    }

    if (ses->totalSize == 0) {
        ses->totalSize = att->crTotal;
        if (ses->openEnded) {
            ses->respEnd = (ses->totalSize > 0) ? (ses->totalSize - 1) : 0;
        } else {
            ses->respEnd = (ses->totalSize > 0) ? std::min<uint64_t>(ses->reqEnd, ses->totalSize - 1) : 0;
        }
    }

    // Clamp piece end if EOF came earlier than requested (e.g., last chunk of open-ended stream).
    if (att->crEnd < piece->end) {
        piece->end = att->crEnd;
    }
    att->headersValid = true;
    return true;
}

size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    size_t totalBytes     = size * nmemb;
    auto* env             = static_cast<WriteCallbackEnv*>(userdata);
    AttemptState* att     = env->attempt;
    PieceState* piece     = att->piece;
    SessionContext* ses   = env->session;

    if (att->canceled || ses->clientClosed || ses->integrityMismatch) return 0;

    if (!validateAttemptHeaders(*env, unixNowSec())) {
        return 0;
    }

    if (att->headOnlyProbe) {
        ses->headProbeDone = true;
        att->canceled      = true;
        return 0;
    }

    const int64_t nowMs = steadyNowMs();
    if (att->firstByteAtMs == 0) att->firstByteAtMs = nowMs;
    att->lastByteAtMs = nowMs;
    ses->lastProgressMs = nowMs;
    env->hostModel->noteBytes(nowMs);

    if (totalBytes == 0) return 0;

    const uint64_t expectedLen = piece->expectedLen();
    if (att->receivedBytes + totalBytes > expectedLen) {
        att->errorOutcome = FetchOutcome::BadRange;
        return 0;
    }

    const size_t offset  = static_cast<size_t>(att->receivedBytes);
    const size_t overlap = (offset < piece->data.size())
                               ? std::min(totalBytes, piece->data.size() - offset)
                               : 0;
    if (overlap > 0) {
        if (std::memcmp(piece->data.data() + offset, ptr, overlap) != 0) {
            att->errorOutcome       = FetchOutcome::Integrity;
            piece->integrityError   = true;
            ses->integrityMismatch  = true;
            return 0;
        }
    }

    if (offset + totalBytes > piece->data.size()) {
        size_t appendFrom = piece->data.size() - offset;
        piece->data.insert(piece->data.end(), reinterpret_cast<const uint8_t*>(ptr) + appendFrom,
                           reinterpret_cast<const uint8_t*>(ptr) + totalBytes);
    }

    att->receivedBytes += totalBytes;
    if (piece->data.size() >= expectedLen) {
        piece->completed = true;
    }
    return totalBytes;
}

bool isSocketDisconnected(SockHandle sock) {
    char tmp = 0;
    auto n   = ::recv(sock, &tmp, 1, MSG_PEEK);
    if (n == 0) return true;
    if (n < 0) {
        int err = lastSockErr();
        if (!isWouldBlock(err)
#ifndef _WIN32
            && err != EINTR
#endif
        ) {
            return true;
        }
    }
    return false;
}

}  // namespace

MultiCdnProxy& MultiCdnProxy::instance() {
    static MultiCdnProxy inst;
    return inst;
}

MultiCdnProxy::MultiCdnProxy() : budget_(defaultProxyBudget()), rangeCache_(defaultProxyBudget().cacheLimitBytes) {}

MultiCdnProxy::~MultiCdnProxy() { shutdown(); }

bool MultiCdnProxy::ensureStarted() {
    if (running_.load()) return true;

#ifdef _WIN32
    if (!wsaInitialized_) {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            return false;
        }
        wsaInitialized_ = true;
    }
#endif

    SockHandle sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == kInvalidSock) return false;

    int opt = 1;
    ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;

    if (::bind(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
        closeSock(sock);
        return false;
    }
    if (::listen(sock, 32) != 0) {
        closeSock(sock);
        return false;
    }

    sockaddr_in bound{};
    socklen_t boundLen = sizeof(bound);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&bound), &boundLen) != 0) {
        closeSock(sock);
        return false;
    }

    listenSock_ = static_cast<intptr_t>(sock);
    port_.store(ntohs(bound.sin_port));
    running_.store(true);
    acceptThread_ = std::thread(&MultiCdnProxy::acceptLoop, this);
    return true;
}

void MultiCdnProxy::shutdown() {
    if (!running_.exchange(false)) return;

    SockHandle ls = static_cast<SockHandle>(listenSock_);
    listenSock_   = -1;
    if (ls != kInvalidSock) closeSock(ls);

    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }

    // Wait briefly for detached worker threads to exit cleanly.
    for (int i = 0; i < 60 && activeWorkers_.load() > 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    port_.store(0);
#ifdef _WIN32
    if (wsaInitialized_) {
        WSACleanup();
        wsaInitialized_ = false;
    }
#endif
}

void MultiCdnProxy::setNetworkOptions(const ProxyNetworkOptions& options) {
    std::lock_guard<std::mutex> lock(netMutex_);
    netOptions_ = options;
}

void MultiCdnProxy::setBudget(const ProxyBudget& budget) {
    {
        std::lock_guard<std::mutex> lock(netMutex_);
        budget_ = budget;
    }
    rangeCache_.setLimit(budget.cacheLimitBytes);
}

std::string MultiCdnProxy::registerTrack(const std::string& videoKey, const std::string& fileKey,
                                         const std::vector<std::string>& listedUrls,
                                         const std::vector<std::string>& learnedHosts, CdnMode mode,
                                         uint64_t bitrateBps) {
    if (!running_.load() || port_.load() == 0 || listedUrls.empty()) return {};

    std::vector<MediaCandidate> candidates = buildCandidates(listedUrls, learnedHosts, mode);
    if (candidates.empty()) return {};

    std::string token;
    {
        std::lock_guard<std::mutex> lock(tracksMutex_);
        token = std::to_string(nextTokenId_++);

        TrackEntry entry;
        entry.token      = token;
        entry.videoKey   = videoKey;
        entry.fileKey    = fileKey;
        entry.primaryUrl = candidates.front().url;
        entry.candidates = std::move(candidates);
        entry.mode       = mode;
        entry.bitrateBps = bitrateBps;

        while (trackOrder_.size() >= kMaxTracks) {
            tracks_.erase(trackOrder_.front());
            trackOrder_.pop_front();
        }
        trackOrder_.push_back(token);
        tracks_.emplace(token, std::move(entry));
    }

    return "http://127.0.0.1:" + std::to_string(port_.load()) + "/media/" + token;
}

void MultiCdnProxy::resetStateForTests() {
    hostModel_.reset();
    rangeCache_.clear();
    {
        std::lock_guard<std::mutex> lock(tracksMutex_);
        tracks_.clear();
        trackOrder_.clear();
    }
    {
        std::lock_guard<std::mutex> lock(circuitMutex_);
        recentFailureTimesMs_.clear();
        circuitOpenUntilMs_ = 0;
    }
}

bool MultiCdnProxy::getTrack(const std::string& token, TrackEntry& out) const {
    std::lock_guard<std::mutex> lock(tracksMutex_);
    auto it = tracks_.find(token);
    if (it == tracks_.end()) return false;
    out = it->second;
    return true;
}

bool MultiCdnProxy::isCircuitOpen(int64_t nowMs) const {
    std::lock_guard<std::mutex> lock(circuitMutex_);
    return circuitOpenUntilMs_ > nowMs;
}

void MultiCdnProxy::noteJobFailure(bool integrityMismatch, int64_t nowMs) {
    std::lock_guard<std::mutex> lock(circuitMutex_);
    if (integrityMismatch) {
        circuitOpenUntilMs_ = nowMs + kCircuitOpenDurationMs;
        return;
    }
    recentFailureTimesMs_.push_back(nowMs);
    while (!recentFailureTimesMs_.empty() && (nowMs - recentFailureTimesMs_.front()) > kFailureWindowMs) {
        recentFailureTimesMs_.pop_front();
    }
    if (static_cast<int>(recentFailureTimesMs_.size()) >= kFailuresToTrip) {
        circuitOpenUntilMs_ = nowMs + kCircuitOpenDurationMs;
        recentFailureTimesMs_.clear();
    }
}

void MultiCdnProxy::noteJobSuccess() {
    std::lock_guard<std::mutex> lock(circuitMutex_);
    recentFailureTimesMs_.clear();
}

void MultiCdnProxy::acceptLoop() {
    SockHandle ls = static_cast<SockHandle>(listenSock_);
    while (running_.load()) {
#ifdef _WIN32
        WSAPOLLFD pfd{ls, POLLIN, 0};
        int ready = WSAPoll(&pfd, 1, 200);
#else
        struct pollfd pfd{ls, POLLIN, 0};
        int ready = ::poll(&pfd, 1, 200);
#endif
        if (ready <= 0) continue;

        sockaddr_in peer{};
        socklen_t peerLen = sizeof(peer);
        SockHandle client = ::accept(ls, reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == kInvalidSock) continue;

        activeWorkers_.fetch_add(1);
        std::thread([this, cs = static_cast<intptr_t>(client)]() {
            this->handleClient(cs);
            this->activeWorkers_.fetch_sub(1);
        }).detach();
    }
}

void MultiCdnProxy::handleClient(intptr_t rawSock) {
    SockHandle clientSock = static_cast<SockHandle>(rawSock);

    int nodelay = 1;
    ::setsockopt(clientSock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#ifdef SO_NOSIGPIPE
    int nosig = 1;
    ::setsockopt(clientSock, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif

    // Read HTTP request header (up to 16 KiB).
    std::string rawReq;
    rawReq.reserve(1024);
    char buf[2048];
    const int64_t readDeadline = steadyNowMs() + 3000;
    while (rawReq.find("\r\n\r\n") == std::string::npos && rawReq.size() < 16384 && running_.load()) {
#ifdef _WIN32
        WSAPOLLFD pfd{clientSock, POLLIN, 0};
        int pr = WSAPoll(&pfd, 1, 100);
#else
        struct pollfd pfd{clientSock, POLLIN, 0};
        int pr = ::poll(&pfd, 1, 100);
#endif
        if (pr > 0) {
            auto n = ::recv(clientSock, buf, sizeof(buf), 0);
            if (n <= 0) {
                closeSock(clientSock);
                return;
            }
            rawReq.append(buf, static_cast<size_t>(n));
        } else if (steadyNowMs() > readDeadline) {
            closeSock(clientSock);
            return;
        }
    }

    ParsedHttpRequest req = parseHttpRequest(rawReq);
    if (!req.valid || !detail::startsWith(req.path, "/media/")) {
        sendSimpleStatus(clientSock, "400 Bad Request", running_);
        closeSock(clientSock);
        return;
    }

    std::string token = req.path.substr(7);
    TrackEntry track;
    if (!getTrack(token, track)) {
        sendSimpleStatus(clientSock, "404 Not Found", running_);
        closeSock(clientSock);
        return;
    }

    const int64_t startMs = steadyNowMs();
    if (isCircuitOpen(startMs)) {
        sendRedirect307(clientSock, track.primaryUrl, running_);
        closeSock(clientSock);
        return;
    }

    // Serve cached HEAD or bounded GET immediately if available.
    if (req.method == "HEAD") {
        uint64_t cachedTotal = rangeCache_.getTotalSize(track.fileKey);
        if (cachedTotal > 0) {
            uint64_t end = req.hasRange && !req.openEnded ? std::min<uint64_t>(req.end, cachedTotal - 1)
                                                          : (cachedTotal - 1);
            std::string hdr = buildSuccessHeaders(req.hasRange, req.start, end, cachedTotal);
            sendAllBlocking(clientSock, hdr.data(), hdr.size(), running_);
            closeSock(clientSock);
            return;
        }
    } else if (req.hasRange && !req.openEnded) {
        std::vector<uint8_t> cachedBytes;
        uint64_t cachedTotal = 0;
        if (rangeCache_.get(track.fileKey, req.start, req.end, cachedBytes, cachedTotal)) {
            uint64_t respEnd = req.start + cachedBytes.size() - 1;
            std::string hdr  = buildSuccessHeaders(true, req.start, respEnd, cachedTotal);
            if (sendAllBlocking(clientSock, hdr.data(), hdr.size(), running_)) {
                sendAllBlocking(clientSock, reinterpret_cast<const char*>(cachedBytes.data()),
                                cachedBytes.size(), running_);
            }
            closeSock(clientSock);
            return;
        }
    }

    ProxyNetworkOptions netOpts;
    ProxyBudget budget;
    {
        std::lock_guard<std::mutex> lock(netMutex_);
        netOpts = netOptions_;
        budget  = budget_;
    }

    setNonBlocking(clientSock);

    SessionContext ses;
    ses.clientSock     = clientSock;
    ses.isHead         = (req.method == "HEAD");
    ses.hasRange       = req.hasRange;
    ses.openEnded      = req.openEnded;
    ses.reqStart       = req.start;
    ses.reqEnd         = req.end;
    ses.writePos       = req.start;
    ses.nextPieceStart = req.start;
    ses.startedAtMs    = startMs;
    ses.lastProgressMs = startMs;

    CURLM* curlm = curl_multi_init();
    if (!curlm) {
        sendRedirect307(clientSock, track.primaryUrl, running_);
        closeSock(clientSock);
        return;
    }

    std::unordered_map<CURL*, WriteCallbackEnv*> activeEnvs;

    auto launchAttempt = [&](PieceState* piece, const std::unordered_set<std::string>& excludeHosts,
                             bool headProbe) -> bool {
        uint64_t len   = piece->expectedLen();
        int64_t nowMs  = steadyNowMs();
        uint64_t nowUx = unixNowSec();
        int idx        = hostModel_.pickBestCandidate(track.candidates, track.videoKey, track.fileKey, len,
                                               excludeHosts, ses.inFlightPerHost, budget.maxPerHost,
                                               track.mode, nowMs, nowUx);
        if (idx < 0) return false;

        const MediaCandidate& cand = track.candidates[static_cast<size_t>(idx)];
        CURL* easy                 = curl_easy_init();
        if (!easy) return false;

        auto* att           = new AttemptState();
        att->easy           = easy;
        att->piece          = piece;
        att->cand           = cand;
        att->startedAtMs    = nowMs;
        att->lastByteAtMs   = nowMs;
        att->headOnlyProbe  = headProbe;
        att->estimatedEtaMs = hostModel_.estimate(track.videoKey, cand.host, len, nowMs).etaMs;

        auto* env      = new WriteCallbackEnv();
        env->attempt   = att;
        env->session   = &ses;
        env->hostModel = &hostModel_;
        env->fileKey   = track.fileKey;

        std::string rangeSpec = std::to_string(piece->start) + "-" + std::to_string(piece->end);

        curl_easy_setopt(easy, CURLOPT_URL, cand.url.c_str());
        curl_easy_setopt(easy, CURLOPT_RANGE, rangeSpec.c_str());
        curl_easy_setopt(easy, CURLOPT_USERAGENT, netOpts.userAgent.c_str());
        curl_easy_setopt(easy, CURLOPT_REFERER, netOpts.referer.c_str());
        curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, 4000L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 4096L);
        curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, 6L);
        curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYPEER, netOpts.verifySsl ? 1L : 0L);
        curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, netOpts.verifySsl ? 2L : 0L);
        if (!netOpts.proxy.empty()) {
            curl_easy_setopt(easy, CURLOPT_PROXY, netOpts.proxy.c_str());
        }
        curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, curlHeaderCallback);
        curl_easy_setopt(easy, CURLOPT_HEADERDATA, att);
        curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, curlWriteCallback);
        curl_easy_setopt(easy, CURLOPT_WRITEDATA, env);
        curl_easy_setopt(easy, CURLOPT_PRIVATE, env);

        if (curl_multi_add_handle(curlm, easy) != CURLM_OK) {
            curl_easy_cleanup(easy);
            delete env;
            delete att;
            return false;
        }

        piece->attempts.push_back(att);
        ses.inFlightPerHost[cand.host]++;
        ses.totalInFlight++;
        activeEnvs[easy] = env;
        return true;
    };

    auto cancelAttempt = [&](AttemptState* att) {
        if (!att || att->canceled) return;
        att->canceled = true;
    };

    auto finalizeEasy = [&](CURL* easy, CURLcode result) {
        auto it = activeEnvs.find(easy);
        if (it == activeEnvs.end()) return;
        WriteCallbackEnv* env = it->second;
        activeEnvs.erase(it);

        AttemptState* att = env->attempt;
        PieceState* piece = att->piece;

        curl_multi_remove_handle(curlm, easy);

        auto flightIt = ses.inFlightPerHost.find(att->cand.host);
        if (flightIt != ses.inFlightPerHost.end()) {
            flightIt->second--;
            if (flightIt->second <= 0) ses.inFlightPerHost.erase(flightIt);
        }
        ses.totalInFlight = std::max(0, ses.totalInFlight - 1);

        const int64_t nowMs = steadyNowMs();
        FetchOutcome outcome = FetchOutcome::Ok;
        if (att->canceled) {
            outcome = FetchOutcome::Canceled;
        } else if (att->errorOutcome != FetchOutcome::Ok) {
            outcome = att->errorOutcome;
        } else if (result != CURLE_OK) {
            outcome = classifyCurlError(result);
        } else if (!att->headersChecked) {
            // 0-byte response without write callback: check headers explicitly.
            if (!validateAttemptHeaders(*env, unixNowSec())) {
                outcome = att->errorOutcome;
            } else if (att->receivedBytes < piece->expectedLen()) {
                outcome = FetchOutcome::Truncated;
            }
        } else if (att->receivedBytes < piece->expectedLen()) {
            outcome = FetchOutcome::Truncated;
        }

        if (outcome == FetchOutcome::Ok) {
            double ttfbMs = (att->firstByteAtMs > att->startedAtMs)
                                ? static_cast<double>(att->firstByteAtMs - att->startedAtMs)
                                : 1.0;
            double xferMs = (nowMs > att->firstByteAtMs) ? static_cast<double>(nowMs - att->firstByteAtMs) : 1.0;
            if (att->receivedBytes >= 16 * 1024) {
                double rate = static_cast<double>(att->receivedBytes) / std::max(xferMs, 1.0);
                hostModel_.recordSpeed(track.videoKey, att->cand.host, ttfbMs, rate,
                                       att->receivedBytes < 48 * 1024, nowMs);
            }
            hostModel_.recordOutcome(track.videoKey, track.fileKey, att->cand.host, att->cand.family,
                                     FetchOutcome::Ok, nowMs);
        } else if (outcome != FetchOutcome::Canceled) {
            bool anotherAlive = false;
            for (const auto& kv : activeEnvs) {
                if (kv.second->attempt->cand.host == att->cand.host &&
                    (nowMs - kv.second->attempt->lastByteAtMs) < 2000) {
                    anotherAlive = true;
                    break;
                }
            }
            hostModel_.recordOutcome(track.videoKey, track.fileKey, att->cand.host, att->cand.family, outcome, nowMs,
                                     anotherAlive);
            if (!piece->completed) {
                piece->tries++;
                piece->triedHosts.insert(att->cand.host);
            }
        }

        auto pIt = std::find(piece->attempts.begin(), piece->attempts.end(), att);
        if (pIt != piece->attempts.end()) piece->attempts.erase(pIt);

        curl_easy_cleanup(easy);
        delete env;
        delete att;
    };

    // Create initial piece(s). Piece 0 uses smaller initial chunk on open-ended streams for fast TTFF.
    auto createNextPiece = [&]() -> PieceState* {
        if (ses.totalSize > 0 && ses.nextPieceStart > ses.respEnd) return nullptr;
        if (!ses.openEnded && ses.nextPieceStart > ses.reqEnd) return nullptr;

        uint64_t chunkSz = budget.pieceBytes;
        if (ses.nextPieceIndex == 0 && ses.openEnded) {
            chunkSz = std::min<uint64_t>(budget.pieceBytes, 256 * 1024);
        }
        uint64_t end = ses.nextPieceStart + chunkSz - 1;
        if (!ses.openEnded) end = std::min(end, ses.reqEnd);
        if (ses.totalSize > 0) end = std::min(end, ses.respEnd);
        if (end < ses.nextPieceStart) return nullptr;

        auto* p           = new PieceState();
        p->index          = ses.nextPieceIndex++;
        p->start          = ses.nextPieceStart;
        p->end            = end;
        ses.nextPieceStart = end + 1;
        ses.pieces[p->index] = p;
        return p;
    };

    if (PieceState* p0 = createNextPiece()) {
        std::unordered_set<std::string> used;
        int initialRace = ses.isHead ? 1 : 2;
        for (int r = 0; r < initialRace; ++r) {
            if (!launchAttempt(p0, used, ses.isHead)) break;
            used.insert(p0->attempts.back()->cand.host);
        }
    }

    // Track bounded response bytes for caching when small enough.
    const bool shouldCacheResponse = (!ses.isHead && !ses.openEnded && (ses.reqEnd - ses.reqStart + 1) <= 1024 * 1024);
    std::vector<uint8_t> cachedBuffer;

    while (running_.load() && !ses.clientClosed && !ses.integrityMismatch && !ses.fatalError) {
        // 1. Schedule pieces within sliding window if headers are committed (or 1 piece before commit).
        if (ses.headersCommitted && !ses.isHead) {
            while (ses.totalInFlight < budget.maxConcurrency) {
                // Find any piece needing an attempt first, else create a new piece within window.
                PieceState* needPiece = nullptr;
                for (auto& kv : ses.pieces) {
                    PieceState* p = kv.second;
                    if (!p->completed && p->attempts.empty()) {
                        needPiece = p;
                        break;
                    }
                }
                if (!needPiece) {
                    if (ses.nextPieceStart <= ses.writePos + budget.windowBytes) {
                        needPiece = createNextPiece();
                    }
                }
                if (!needPiece) break;
                if (!launchAttempt(needPiece, needPiece->triedHosts, false)) break;
            }
        }

        // 2. Drive curl_multi transfers.
        int stillRunning = 0;
        curl_multi_perform(curlm, &stillRunning);

        CURLMsg* msg = nullptr;
        int msgsLeft = 0;
        while ((msg = curl_multi_info_read(curlm, &msgsLeft)) != nullptr) {
            if (msg->msg == CURLMSG_DONE) {
                finalizeEasy(msg->easy_handle, msg->data.result);
            }
        }

        if (ses.isHead && ses.headProbeDone && ses.totalSize > 0) {
            uint64_t hdrEnd = ses.hasRange && !ses.openEnded
                                  ? std::min<uint64_t>(ses.reqEnd, ses.totalSize - 1)
                                  : (ses.totalSize - 1);
            std::string hdr = buildSuccessHeaders(ses.hasRange, ses.reqStart, hdrEnd, ses.totalSize);
            sendAllBlocking(clientSock, hdr.data(), hdr.size(), running_);
            rangeCache_.setTotalSize(track.fileKey, ses.totalSize);
            ses.headersCommitted = true;
            break;
        }

        const int64_t nowMs = steadyNowMs();

        // 3. Cancel losing racers on completed pieces & check for retry/hedging.
        for (auto& kv : ses.pieces) {
            PieceState* p = kv.second;
            if (p->completed) {
                for (AttemptState* a : p->attempts) cancelAttempt(a);
                continue;
            }
            if (p->attempts.empty()) {
                if (p->tries >= 6) {
                    ses.fatalError = true;
                    break;
                }
                // Retry with fresh attempt.
                if (ses.totalInFlight < budget.maxConcurrency) {
                    if (!launchAttempt(p, p->triedHosts, ses.isHead)) {
                        // Try again without excluding triedHosts if all candidates failed once.
                        std::unordered_set<std::string> emptyExcl;
                        if (!launchAttempt(p, emptyExcl, ses.isHead)) {
                            ses.fatalError = true;
                            break;
                        }
                    }
                }
            } else if (p->attempts.size() == 1 && !ses.isHead && p->extraHedges < 2 &&
                       ses.totalInFlight < budget.maxConcurrency) {
                AttemptState* sole = p->attempts.front();
                int64_t idleMs     = nowMs - sole->lastByteAtMs;
                int64_t elapsedMs  = nowMs - sole->startedAtMs;
                if (idleMs > budget.hedgeIdleMs ||
                    elapsedMs > static_cast<int64_t>(std::max(1200.0, sole->estimatedEtaMs * 1.6))) {
                    std::unordered_set<std::string> excl = p->triedHosts;
                    excl.insert(sole->cand.host);
                    if (launchAttempt(p, excl, false)) {
                        p->extraHedges++;
                    }
                }
            }
        }

        // Remove any attempts marked canceled above.
        std::vector<CURL*> toCancel;
        for (const auto& kv : activeEnvs) {
            if (kv.second->attempt->canceled) toCancel.push_back(kv.first);
        }
        for (CURL* e : toCancel) finalizeEasy(e, CURLE_ABORTED_BY_CALLBACK);

        // 4. Commit response headers once totalSize and leading bytes are validated.
        if (!ses.headersCommitted && !ses.isHead && !ses.pieces.empty()) {
            PieceState* p0 = ses.pieces.begin()->second;
            if (ses.totalSize > 0 && (!p0->data.empty() || p0->completed)) {
                if (ses.reqStart >= ses.totalSize) {
                    sendSimpleStatus(clientSock, "416 Range Not Satisfiable", running_);
                    ses.clientClosed = true;
                    break;
                }
                std::string hdr = buildSuccessHeaders(ses.hasRange, ses.reqStart, ses.respEnd, ses.totalSize);
                ses.pendingHeaderBytes.assign(hdr.begin(), hdr.end());
                ses.pendingHeaderSent = 0;
                ses.headersCommitted  = true;
                rangeCache_.setTotalSize(track.fileKey, ses.totalSize);
            }
        }

        // 5. Stream contiguous headers + verified bytes to loopback client (non-blocking).
        bool wantPollOut = false;
        if (ses.headersCommitted) {
            if (ses.pendingHeaderSent < ses.pendingHeaderBytes.size()) {
#ifdef MSG_NOSIGNAL
                int flags = MSG_NOSIGNAL;
#else
                int flags = 0;
#endif
                auto n = ::send(clientSock,
                                reinterpret_cast<const char*>(ses.pendingHeaderBytes.data() + ses.pendingHeaderSent),
                                static_cast<int>(ses.pendingHeaderBytes.size() - ses.pendingHeaderSent), flags);
                if (n > 0) {
                    ses.pendingHeaderSent += static_cast<size_t>(n);
                } else if (n < 0 && isWouldBlock(lastSockErr())) {
                    wantPollOut = true;
                } else {
                    ses.clientClosed = true;
                    break;
                }
            }

            if (ses.pendingHeaderSent == ses.pendingHeaderBytes.size()) {
                while (!ses.pieces.empty()) {
                    PieceState* head = ses.pieces.begin()->second;
                    if (head->start + head->sentLen != ses.writePos) break;

                    if (head->sentLen < head->data.size()) {
                        size_t avail = head->data.size() - head->sentLen;
                        int chunk    = static_cast<int>(std::min<size_t>(avail, 64 * 1024));
#ifdef MSG_NOSIGNAL
                        int flags = MSG_NOSIGNAL;
#else
                        int flags = 0;
#endif
                        auto n = ::send(clientSock,
                                        reinterpret_cast<const char*>(head->data.data() + head->sentLen), chunk,
                                        flags);
                        if (n > 0) {
                            if (shouldCacheResponse) {
                                cachedBuffer.insert(cachedBuffer.end(), head->data.data() + head->sentLen,
                                                    head->data.data() + head->sentLen + n);
                            }
                            head->sentLen += static_cast<size_t>(n);
                            ses.writePos += static_cast<uint64_t>(n);
                        } else if (n < 0 && isWouldBlock(lastSockErr())) {
                            wantPollOut = true;
                            break;
                        } else {
                            ses.clientClosed = true;
                            break;
                        }
                    }

                    if (head->completed && head->sentLen >= head->expectedLen()) {
                        if (head->attempts.empty()) {
                            ses.pieces.erase(ses.pieces.begin());
                            delete head;
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
            }
        }

        if (ses.clientClosed || ses.integrityMismatch || ses.fatalError) break;

        // Finished streaming entire range?
        if (ses.headersCommitted && ses.pendingHeaderSent == ses.pendingHeaderBytes.size() && ses.totalSize > 0 &&
            ses.writePos > ses.respEnd) {
            break;
        }

        // Check timeouts.
        if (!ses.headersCommitted && (nowMs - ses.startedAtMs) >= budget.commitTimeoutMs) {
            ses.fatalError = true;
            break;
        }
        if (ses.headersCommitted && !wantPollOut && (nowMs - ses.lastProgressMs) >= budget.stallTimeoutMs) {
            ses.fatalError = true;
            break;
        }

        // Detect client disconnect while idle/backpressured.
        if (isSocketDisconnected(clientSock)) {
            ses.clientClosed = true;
            break;
        }

        struct curl_waitfd extraFd{};
        extraFd.fd     = static_cast<curl_socket_t>(clientSock);
        extraFd.events = CURL_WAIT_POLLIN | (wantPollOut ? CURL_WAIT_POLLOUT : 0);
        int numFds     = 0;
        curl_multi_wait(curlm, &extraFd, 1, 40, &numFds);
    }

    // Drain any remaining curl handles cleanly.
    for (const auto& kv : activeEnvs) {
        curl_multi_remove_handle(curlm, kv.first);
        curl_easy_cleanup(kv.first);
        delete kv.second->attempt;
        delete kv.second;
    }
    activeEnvs.clear();
    curl_multi_cleanup(curlm);

    for (auto& kv : ses.pieces) {
        delete kv.second;
    }
    ses.pieces.clear();

    if (!ses.headersCommitted && !ses.clientClosed) {
        noteJobFailure(ses.integrityMismatch, steadyNowMs());
        sendRedirect307(clientSock, track.primaryUrl, running_);
    } else if (ses.integrityMismatch || ses.fatalError) {
        noteJobFailure(ses.integrityMismatch, steadyNowMs());
    } else if (ses.headersCommitted && !ses.isHead && ses.writePos > ses.respEnd) {
        noteJobSuccess();
        if (shouldCacheResponse && !cachedBuffer.empty()) {
            rangeCache_.put(track.fileKey, ses.reqStart, ses.totalSize, cachedBuffer.data(), cachedBuffer.size());
        }
    }

    closeSock(clientSock);
}

}  // namespace wili::multicdn
