#include "test_check.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "bilibili/api.h"
#include "bilibili/result/home_live_result.h"
#include "bilibili/result/home_pgc_season_result.h"
#include "bilibili/result/home_result.h"
#include "bilibili/result/mine_history_result.h"
#include "bilibili/result/mine_result.h"
#include "bilibili/result/search_result.h"
#include "bilibili/result/video_detail_result.h"
#include "bilibili/util/http.hpp"

namespace bilibili::wbi {
std::string extractKeyFromUrl(const std::string& url);
std::string getMixinKey(const std::string& img_key, const std::string& sub_key);
}  // namespace bilibili::wbi

namespace {

cpr::Response makeJsonResponse(const std::string& body, long statusCode = 200) {
    cpr::Response r;
    r.status_code = statusCode;
    r.text        = body;
    return r;
}

void check_api_endpoints_and_wbi() {
    // Regression guard for #18: TripleWeb must include a leading slash after _apiBase.
    CHECK_EQ(bilibili::Api::TripleWeb, std::string("//api.bilibili.com/x/web-interface/archive/like/triple"));
    CHECK_EQ(bilibili::parseLink(bilibili::Api::TripleWeb),
             std::string("https://api.bilibili.com/x/web-interface/archive/like/triple"));
    CHECK_EQ(bilibili::parseLink(bilibili::Api::Recommend),
             std::string("https://api.bilibili.com/x/web-interface/wbi/index/top/feed/rcmd"));
    CHECK_EQ(bilibili::parseLink(bilibili::Api::Search),
             std::string("https://api.bilibili.com/x/web-interface/wbi/search/type"));
    CHECK_EQ(bilibili::parseLink(bilibili::Api::PlayUrl2),
             std::string("https://api.bilibili.com/x/player/wbi/playurl"));

    // Known Bilibili WBI key-extraction & mixin vector (from bilibili-API-collect docs).
    const std::string imgKey =
        bilibili::wbi::extractKeyFromUrl("https://i0.hdslb.com/bfs/wbi/7cd084941338484aae1ad9425b84077c.png");
    const std::string subKey =
        bilibili::wbi::extractKeyFromUrl("https://i0.hdslb.com/bfs/wbi/4932caff0ff746eab6f01bf08b70ac45.png");
    CHECK_EQ(imgKey, std::string("7cd084941338484aae1ad9425b84077c"));
    CHECK_EQ(subKey, std::string("4932caff0ff746eab6f01bf08b70ac45"));
    CHECK_EQ(bilibili::wbi::getMixinKey(imgKey, subKey), std::string("ea1db124af3c7062474693fa704f4ff8"));
}

void check_guest_discovery_models() {
    // Home recommendation payload (#11)
    const std::string rcmdJson = R"({
      "code": 0,
      "data": {
        "item": [
          {
            "id": 1000001,
            "bvid": "BV1xx411c7mD",
            "cid": 2000001,
            "pic": "https://i0.hdslb.com/bfs/archive/sample.jpg",
            "title": "Switch 2 Native Test Video",
            "duration": 185,
            "pubdate": 1700000000,
            "owner": { "mid": 12345, "name": "TestCreator", "face": "https://i0.hdslb.com/face.jpg" },
            "stat": { "view": 4200, "danmaku": 88 },
            "is_followed": 0,
            "rcmd_reason": { "reason_type": 3, "content": "1万点赞" }
          }
        ]
      }
    })";

    bilibili::RecommendVideoListResultWrapper rcmd;
    bool ok = false;
    bilibili::HTTP::parseJson<bilibili::RecommendVideoListResultWrapper>(
        makeJsonResponse(rcmdJson), [&](const auto& res) {
            rcmd = res;
            ok   = true;
        });
    CHECK(ok);
    CHECK_EQ(rcmd.item.size(), size_t(1));
    CHECK_EQ(rcmd.item[0].bvid, std::string("BV1xx411c7mD"));
    CHECK_EQ(rcmd.item[0].rcmd_reason.content, std::string("1万点赞"));

    // Search payload (#11): video + media_bangumi + ketang (marked unsupported)
    const std::string searchJson = R"({
      "code": 0,
      "data": {
        "page": 1,
        "pagesize": 20,
        "numResults": 3,
        "numPages": 1,
        "result": [
          {
            "type": "video",
            "aid": 111,
            "bvid": "BV1aa411b7cc",
            "author": "CreatorA",
            "duration": "03:05",
            "pic": "//i0.hdslb.com/bfs/archive/v.jpg",
            "play": 1234,
            "danmaku": 56,
            "like": 78,
            "pubdate": 1700000000,
            "title": "中文搜索结果"
          },
          {
            "type": "ketang",
            "title": "Unsupported Course"
          },
          {
            "type": "media_bangumi",
            "season_id": 9999,
            "pubtime": 1700000000,
            "cover": "https://i0.hdslb.com/bfs/bangumi/c.jpg",
            "index_show": "全12话",
            "media_score": { "score": 9.8, "user_count": 5000 },
            "styles": "奇幻/校园",
            "areas": "日本",
            "desc": "Sample Bangumi",
            "cv": "ActorA\nActorB",
            "staff": "DirectorA\nStudioB",
            "season_type_name": "番剧",
            "title": "Sample Season"
          }
        ]
      }
    })";

    bilibili::SearchResult search;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::SearchResult>(makeJsonResponse(searchJson), [&](const auto& res) {
        search = res;
        ok     = true;
    });
    CHECK(ok);
    CHECK_EQ(search.result.size(), size_t(3));
    CHECK_EQ(search.result[0].cover, std::string("http://i0.hdslb.com/bfs/archive/v.jpg"));
    CHECK(search.result[1].notSupported);
    CHECK_EQ(search.result[2].season_id, uint64_t(9999));
    CHECK_EQ(search.result[2].cv, std::string("ActorA ActorB"));

    // Error code mapping (#11)
    int errCode = 0;
    std::string errMsg;
    bilibili::HTTP::parseJson<bilibili::SearchResult>(
        makeJsonResponse(R"({"code": -412, "message": "请求被拦截"})"), nullptr,
        [&](const std::string& msg, int code) {
            errMsg  = msg;
            errCode = code;
        });
    CHECK_EQ(errCode, -412);
    CHECK_EQ(errMsg, std::string("请求被拦截"));
}

void check_auth_history_and_interactions() {
    // QR Login V2 poll transitions (#12)
    auto parsePoll = [](int code, const char* token) {
        nlohmann::json j = {{"code", code}, {"refresh_token", token}};
        return j.get<bilibili::QrLoginInfoResultV2>();
    };
    CHECK(parsePoll(86101, "").data == bilibili::LoginInfo::NEED_SCAN);
    CHECK(parsePoll(86090, "").data == bilibili::LoginInfo::NEED_CONFIRM);
    CHECK(parsePoll(86038, "").data == bilibili::LoginInfo::OAUTH_KEY_TIMEOUT);
    const auto success = parsePoll(0, "sanitized_refresh_token");
    CHECK(success.status);
    CHECK(success.data == bilibili::LoginInfo::SUCCESS);
    CHECK_EQ(success.refresh_token, std::string("sanitized_refresh_token"));

    // Watch History cursor (#12)
    const std::string historyJson = R"({
      "code": 0,
      "data": {
        "cursor": { "max": 1000, "view_at": 1700001234, "business": "archive", "ps": 20 },
        "list": [
          {
            "title": "History Item",
            "show_title": "",
            "author_name": "Author",
            "cover": "https://i0.hdslb.com/h.jpg",
            "covers": [],
            "badge": "",
            "progress": 84,
            "duration": 300,
            "view_at": 1700001234,
            "live_status": 0,
            "history": { "oid": 123, "epid": 0, "cid": 456, "business": "archive", "dt": 2, "bvid": "BV1xx411c7mD" }
          }
        ]
      }
    })";
    bilibili::HistoryVideoResultWrapper history;
    bool ok = false;
    bilibili::HTTP::parseJson<bilibili::HistoryVideoResultWrapper>(
        makeJsonResponse(historyJson), [&](const auto& res) {
            history = res;
            ok      = true;
        });
    CHECK(ok);
    CHECK_EQ(history.cursor.max, 1000);
    CHECK_EQ(history.list.size(), size_t(1));
    CHECK_EQ(history.list[0].progress, 84);

    // Triple & Comments (#18)
    const std::string tripleJson = R"({"code": 0, "data": {"like": true, "coin": true, "fav": true, "multiply": 2}})";
    bilibili::VideoTriple triple{};
    ok = false;
    bilibili::HTTP::parseJson<bilibili::VideoTriple>(makeJsonResponse(tripleJson), [&](const auto& res) {
        triple = res;
        ok     = true;
    });
    CHECK(ok);
    CHECK(triple.like && triple.coin && triple.fav);
    CHECK_EQ(triple.multiply, 2);

    // Live room playinfo (#19)
    const std::string liveJson = R"({
      "code": 0,
      "data": {
        "room_id": 1942240,
        "uid": 999,
        "live_status": 1,
        "live_time": 1700000000,
        "is_locked": false,
        "lock_till": 0,
        "playurl_info": {
          "playurl": {
            "g_qn_desc": [{"qn": 10000, "desc": "原画"}],
            "stream": [{
              "protocol_name": "http_stream",
              "format": [{
                "format_name": "flv",
                "codec": [{
                  "codec_name": "avc",
                  "current_qn": 10000,
                  "accept_qn": [10000, 400],
                  "base_url": "/live-bvc/sample.flv",
                  "url_info": [{"host": "https://live.example.com", "extra": "?token=sample", "stream_ttl": 3600}]
                }]
              }]
            }]
          }
        }
      }
    })";
    bilibili::LiveRoomPlayInfo live{};
    ok = false;
    bilibili::HTTP::parseJson<bilibili::LiveRoomPlayInfo>(makeJsonResponse(liveJson), [&](const auto& res) {
        live = res;
        ok   = true;
    });
    CHECK(ok);
    CHECK_EQ(live.room_id, 1942240);
    CHECK_EQ(live.live_status, 1);
    CHECK_EQ(live.playurl_info.playurl.stream.size(), size_t(1));
}

}  // namespace

int main() {
    check_api_endpoints_and_wbi();
    check_guest_discovery_models();
    check_auth_history_and_interactions();

    return test_check::report("all Bilibili API fixture checks passed");
}
