#include "test_check.hpp"

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

#include "bilibili.h"
#include "bilibili/api.h"
#include "bilibili/result/dynamic_video.h"
#include "bilibili/result/home_live_result.h"
#include "bilibili/result/home_pgc_season_result.h"
#include "bilibili/result/home_result.h"
#include "bilibili/result/live_danmaku_result.h"
#include "bilibili/result/mine_collection_result.h"
#include "bilibili/result/mine_history_result.h"
#include "bilibili/result/mine_later_result.h"
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

void check_session_hygiene_and_extended_models() {
    // 1. Cookie / session hygiene (#12): init resets prior cookies and clearCookies strips header state.
    bilibili::Cookies demoCookies = {{"DedeUserID", "123456"}, {"demo_session", "placeholder"}};
    bilibili::BilibiliClient::init(demoCookies, nullptr);
    const std::string encodedOnce = bilibili::HTTP::getEncodedCookie(bilibili::HTTP::COOKIES);
    CHECK(bilibili::HTTP::HEADERS.count("cookie") == size_t(1));
    CHECK(encodedOnce.find("DedeUserID=123456") != std::string::npos);

    bilibili::BilibiliClient::init(demoCookies, nullptr);
    CHECK_EQ(bilibili::HTTP::getEncodedCookie(bilibili::HTTP::COOKIES), encodedOnce);

    bilibili::BilibiliClient::clearCookies();
    CHECK_EQ(bilibili::HTTP::HEADERS.count("cookie"), size_t(0));
    CHECK(bilibili::HTTP::getEncodedCookie(bilibili::HTTP::COOKIES).empty());

    bilibili::BilibiliClient::init(demoCookies, nullptr);
    bilibili::Cookies emptyCookies;
    bilibili::BilibiliClient::init(emptyCookies, nullptr);
    CHECK_EQ(bilibili::HTTP::HEADERS.count("cookie"), size_t(0));
    CHECK(bilibili::HTTP::getEncodedCookie(bilibili::HTTP::COOKIES).empty());

    // 2. Unauthenticated -101 envelope maps cleanly to error callback (#12, #17, #19).
    bool userOk   = false;
    int errCode   = 0;
    std::string errMsg;
    bilibili::HTTP::parseJson<bilibili::UserResult>(
        makeJsonResponse(R"({"code":-101,"message":"not logged in"})"),
        [&](const auto&) { userOk = true; },
        [&](const std::string& msg, int code) {
            errMsg  = msg;
            errCode = code;
        });
    CHECK(!userOk);
    CHECK_EQ(errCode, -101);
    CHECK_EQ(errMsg, std::string("not logged in"));

    // 3. Collection list (#17, #19).
    const std::string collectionJson = R"({
        "code": 0,
        "message": "0",
        "data": {
            "count": 1,
            "has_more": true,
            "list": [{
                "id": 1001,
                "fid": 1001,
                "type": 11,
                "cover": "https://i0.hdslb.com/bfs/archive/coll.jpg",
                "title": "Saved Clips",
                "media_count": 3,
                "upper": {"mid": 999, "name": "DemoCreator", "face": "https://i0.hdslb.com/bfs/face/demo.jpg"}
            }]
        }
    })";
    bilibili::CollectionListResultWrapper collections;
    bool ok = false;
    bilibili::HTTP::parseJson<bilibili::CollectionListResultWrapper>(
        makeJsonResponse(collectionJson), [&](const auto& res) {
            collections = res;
            ok          = true;
        });
    CHECK(ok);
    CHECK(collections.has_more);
    CHECK_EQ(collections.list.size(), size_t(1));
    if (collections.list.size() == 1) {
        CHECK_EQ(collections.list[0].title, std::string("Saved Clips"));
        CHECK_EQ(collections.list[0].upper.name, std::string("DemoCreator"));
    }

    // 4. Watch later empty wrapper (#17, #19).
    bilibili::WatchLaterListWrapper later;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::WatchLaterListWrapper>(
        makeJsonResponse(R"({"code":0,"message":"0","data":{"count":0,"list":[]}})"),
        [&](const auto& res) {
            later = res;
            ok    = true;
        });
    CHECK(ok);
    CHECK_EQ(later.count, 0);
    CHECK(later.list.empty());

    // 5. Creator uploaded videos (#18).
    const std::string uploadedJson = R"({
        "code": 0,
        "message": "0",
        "data": {
            "page": {"pn": 1, "ps": 20, "count": 1},
            "list": {
                "vlist": [{
                    "comment": 12,
                    "play": 4567,
                    "pic": "https://i0.hdslb.com/bfs/archive/up.jpg",
                    "description": "Demo desc",
                    "copyright": "1",
                    "title": "Creator Upload",
                    "video_review": 8,
                    "author": "DemoCreator",
                    "mid": 999,
                    "created": 1700000100,
                    "length": "04:20",
                    "aid": 555666,
                    "bvid": "BV1xx411c7mD",
                    "is_charging_arc": false
                }]
            }
        }
    })";
    bilibili::UserUploadedVideoResultWrapper uploaded;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::UserUploadedVideoResultWrapper>(
        makeJsonResponse(uploadedJson), [&](const auto& res) {
            uploaded = res;
            ok       = true;
        });
    CHECK(ok);
    CHECK_EQ(uploaded.page.count, 1u);
    CHECK_EQ(uploaded.list.size(), size_t(1));
    if (uploaded.list.size() == 1) {
        CHECK_EQ(uploaded.list[0].bvid, std::string("BV1xx411c7mD"));
        CHECK(!uploaded.list[0].is_charging_arc);
    }

    // 6. Video comments & relation (#18, #19).
    const std::string commentsJson = R"({
        "code": 0,
        "message": "0",
        "data": {
            "upper": {"mid": 999},
            "cursor": {"all_count": 2, "mode": 3, "next": 2, "prev": 0, "is_end": false, "is_begin": true},
            "top_replies": [{
                "ctime": 1700000200,
                "rpid": 101,
                "parent": 0,
                "root": 0,
                "oid": 555666,
                "rcount": 1,
                "like": 15,
                "action": 0,
                "replies": null,
                "member": {
                    "mid": "999",
                    "uname": "DemoCreator",
                    "avatar": "https://i0.hdslb.com/bfs/face/demo.jpg",
                    "is_senior_member": 1,
                    "level_info": {"current_level": 6},
                    "vip": {"nickname_color": ""}
                },
                "content": {"message": "Pinned creator note"},
                "reply_control": {
                    "sub_reply_entry_text": "1 reply",
                    "sub_reply_title_text": "1 reply",
                    "time_desc": "today",
                    "up_like": true
                }
            }],
            "replies": [{
                "ctime": 1700000250,
                "rpid": 102,
                "parent": 0,
                "root": 0,
                "oid": 555666,
                "rcount": 0,
                "like": 2,
                "action": 0,
                "replies": [],
                "member": {
                    "mid": "888",
                    "uname": "Viewer",
                    "avatar": "https://i0.hdslb.com/bfs/face/viewer.jpg",
                    "is_senior_member": 0,
                    "level_info": {"current_level": 4},
                    "vip": {"nickname_color": ""}
                },
                "content": {"message": "Nice test"},
                "reply_control": {
                    "sub_reply_entry_text": "",
                    "sub_reply_title_text": "",
                    "time_desc": "today",
                    "up_like": false
                }
            }]
        }
    })";
    bilibili::VideoCommentResultWrapper comments;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::VideoCommentResultWrapper>(
        makeJsonResponse(commentsJson), [&](const auto& res) {
            comments = res;
            ok       = true;
        });
    CHECK(ok);
    CHECK_EQ(comments.top_replies.size(), size_t(1));
    if (comments.top_replies.size() == 1) {
        CHECK(comments.top_replies[0].top);
        CHECK(comments.top_replies[0].member.is_uploader);
    }
    CHECK_EQ(comments.replies.size(), size_t(1));
    if (comments.replies.size() == 1) {
        CHECK(!comments.replies[0].member.is_uploader);
    }

    bilibili::VideoRelation relation;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::VideoRelation>(
        makeJsonResponse(
            R"({"code":0,"message":"0","data":{"attention":true,"favorite":false,"season_fav":false,"like":true,"dislike":false,"coin":2}})"),
        [&](const auto& res) {
            relation = res;
            ok       = true;
        });
    CHECK(ok);
    CHECK(relation.attention);
    CHECK(relation.like);
    CHECK_EQ(relation.coin, 2);

    // 7. PGC season detail wrapper & episode indexing (#18, #19).
    const std::string seasonJson = R"({
        "code": 0,
        "message": "0",
        "result": {
            "season_id": 28223043,
            "season_title": "Demo Season",
            "evaluate": "Fixture synopsis",
            "cover": "https://i0.hdslb.com/bfs/bangumi/cover.jpg",
            "type": 1,
            "rating": {"score": 9.8, "count": 1200},
            "up_info": {"mid": 777, "uname": "Studio", "avatar": "", "follower": 100, "is_follow": 1},
            "stat": {"views": 50000, "danmakus": 300, "favorite": 400, "favorites": 400, "coins": 50, "share": 10, "likes": 800, "reply": 20},
            "publish": {"is_finish": 1, "is_started": 1, "pub_time_show": "2024"},
            "user_status": {"follow": 1, "progress": {"last_ep_id": 323435, "last_time": 90}},
            "episodes": [
                {"aid": 1, "cid": 11, "id": 323434, "title": "1", "pub_time": 1700000000, "link": "https://example.invalid/ep1"},
                {"aid": 2, "cid": 12, "id": 323435, "title": "2", "pub_time": 1700000100, "link": "https://example.invalid/ep2"}
            ]
        }
    })";
    bilibili::SeasonResultWrapper season;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::SeasonResultWrapper>(
        makeJsonResponse(seasonJson), [&](const auto& res) {
            season = res;
            ok     = true;
        });
    CHECK(ok);
    CHECK_EQ(season.episodes.size(), size_t(2));
    if (season.episodes.size() == 2) {
        CHECK_EQ(season.episodes[0].index, size_t(0));
        CHECK_EQ(season.episodes[1].index, size_t(1));
    }
    CHECK_EQ(season.user_status.last_ep_id, size_t(323435));

    // 8. Live danmaku host & pay info (#18, #19).
    bilibili::LiveDanmakuinfo danmakuInfo;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::LiveDanmakuinfo>(
        makeJsonResponse(
            R"({"code":0,"message":"0","data":{"token":"demo-token","host_list":[{"host":"broadcastlv.chat.bilibili.com","ws_port":2244}]}})"),
        [&](const auto& res) {
            danmakuInfo = res;
            ok          = true;
        });
    CHECK(ok);
    CHECK_EQ(danmakuInfo.token, std::string("demo-token"));
    CHECK_EQ(danmakuInfo.host_list.size(), size_t(1));
    if (danmakuInfo.host_list.size() == 1) {
        CHECK_EQ(danmakuInfo.host_list[0].ws_port, 2244);
    }

    bilibili::LivePayInfo payInfo;
    ok = false;
    bilibili::HTTP::parseJson<bilibili::LivePayInfo>(
        makeJsonResponse(R"({"code":0,"message":"0","data":{"permission":0,"pic":"https://i0.hdslb.com/bfs/live/pay.png"}})"),
        [&](const auto& res) {
            payInfo = res;
            ok      = true;
        });
    CHECK(ok);
    CHECK_EQ(payInfo.permission, 0);
}

}  // namespace

int main() {
    check_api_endpoints_and_wbi();
    check_guest_discovery_models();
    check_auth_history_and_interactions();
    check_session_hygiene_and_extended_models();

    return test_check::report("all Bilibili API fixture checks passed");
}
