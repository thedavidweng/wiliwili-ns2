# Bilibili API Compatibility Inventory (`wiliwili-plus`)

This document tracks the Bilibili endpoints exercised by `wiliwili-plus`, their signing/authentication requirements, deterministic test coverage (`tests/bilibili_api_test.cpp`), and repaired or known defects.

All host tests use sanitized representative fixtures with zero live `Session` cookies, `CSRF token` values, or personal identifiers (`docs/adr/0003-no-independent-bilibili-stack.md`).

## Endpoint Inventory

| Area | Endpoint (`api.h`) | Auth / Signing | Status | Notes |
|---|---|---|---|---|
| WBI keys | `Api::Nav` (`/x/web-interface/nav`) | Guest / Cookie | Verified in `bilibili_api` | Extracts `img_key` & `sub_key` from `wbi_img` URLs; 1-hour cache in `wbi.cpp` |
| Home Recommend | `Api::Recommend` (`/x/web-interface/wbi/index/top/feed/rcmd`) | WBI signature | Verified model + WBI | `RecommendVideoListResultWrapper` parsed & covered by fixture test |
| Search | `Api::Search` (`/x/web-interface/wbi/search/type`) | WBI signature | Verified model + WBI | `SearchResult` handles `video`, `media_bangumi`, `media_ft`, skips `ketang` |
| Video Detail & Relation | `Api::DetailAll` (`/x/web-interface/view/detail`), `Api::VideoRelation` | Guest / Cookie | Verified models | `VideoDetailAllResult` & `VideoRelation` covered by fixture tests |
| DASH PlayURL | `Api::PlayUrl2` (`/x/player/wbi/playurl`) | WBI + Cookie (for >360p/VIP) | Verified model + selector | `selectDashTracks` clamps QN (`landscapeQualityMax`/`portraitQualityMax`), selects codec (AVC/HEVC/AV1), and falls back Dolby -> FLAC -> Standard audio |
| PGC Season Detail / PlayURL | `Api::SeasonDetail` (`/pgc/view/pc/season`), `Api::SeasonUrl2` (`/pgc/player/web/v2/playurl`) | Cookie (entitlements) | Verified models | `SeasonResultWrapper`, `SeasonUrlResult`, `VideoUrlResult`; unentitled/regional errors surface honest dialog messages |
| Video Comments | `Api::Comment` (`/x/v2/reply/main`) | Guest / Cookie | Verified model | `VideoCommentResultWrapper` verified with creator & pinned flags |
| Triple Action | `Api::TripleWeb` (`/x/web-interface/archive/like/triple`) | Cookie + `CSRF token` (`bili_jct`) | **Repaired** | Fixed missing leading `/` (`_apiBase + "x/..."` -> `_apiBase + "/x/..."`); verified in `bilibili_api` |
| QR Login V2 & Logout | `Api::QrCodeV2`, `Api::QrCodeCheckV2` | Guest | Verified poll states & cookie hygiene | `QrLoginInfoResultV2` maps `0`, `86101`, `86090`, `86038`; `BilibiliClient::clearCookies()` + `ProgramConfig::clearSession()` clear in-memory cookies, `-101` clears stale session, raw auth payloads are never logged |
| Watch History & Watch Later | `Api::HistoryVideo` (`/x/web-interface/history/cursor`), `Api::ProgressReport`, `Api::WatchLaterList` | Cookie + `CSRF token` | Verified models | `HistoryVideoResultWrapper` & presenter `ASYNC_RETAIN` / `-101` unauthenticated handling |
| Collections & Subscriptions | `Api::CollectionList` (`/x/v3/fav/folder/created/list-all`), `Api::CollectionVideoList` | Cookie | Verified models | `CollectionListResultWrapper`; `MineCollectionRequest` releases `requesting` lock cleanly on index mismatch |
| Creator Space / Dynamics | `Api::DynamicVideo`, `Api::DynamicUp`, `Api::UserUploadedVideos` (`/x/space/wbi/arc/search`) | Cookie / WBI | Verified models | `DynamicUpResult::mid` string conversion (`2f583df`), `UserUploadedVideoResultWrapper`, `DynamicTabRequest` `ASYNC_RETAIN` & `-101`/`-6` handling |
| Live Room PlayInfo & Danmaku | `Api::LiveRoomPlayInfo`, `Api::LiveDanmakuInfo`, `Api::LiveReportHistory` | Guest / Cookie + `CSRF` | Verified models | `LiveRoomPlayInfo` & `LiveDanmakuinfo` fixtures verified; `reportHistory` guards empty `csrf` |

## Limitations & Security Guarantees

- **No bypasses:** Membership (`10403`), charging-exclusive (`87007`/`87008`), and regional restrictions are surfaced as clear status dialogs and never bypassed.
- **TLS Verification:** `SettingItem::TLS_VERIFY` defaults to `1` (`true`) on Switch 2 (`wili::tlsVerifyDefaultForPlatform("switch2")`).
- **Session Isolation:** Cookies and `refresh_token` reside exclusively in `/config/wiliwili-ns2/wiliwili_config.json` on Switch 2, isolated from Switch 1 `/config/wiliwili/`.
- **Manual Authenticated Console Checklist (`#12` / `#18`, requires hardware after G1/G2):**
  1. Launch on console -> scan QR code in Mine tab -> verify profile & avatar load and `wiliwili_config.json` saves session.
  2. Restart app -> verify persistent signed-in state across History / Collections / Dynamic / Watch Later.
  3. Trigger Logout -> verify `HTTP::COOKIES` and `wiliwili_config.json` are cleared and re-launch returns cleanly to guest mode.
  4. Re-login and validate Watch Later / Favorites / Coin / Triple action against a real account (no credentials committed to git).
