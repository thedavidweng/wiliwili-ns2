# Bilibili API Compatibility Inventory (`wiliwili-NS2`)

This document tracks the Bilibili endpoints exercised by `wiliwili-NS2`, their signing/authentication requirements, deterministic test coverage (`tests/bilibili_api_test.cpp`), and repaired or known defects.

All host tests use sanitized representative fixtures with zero live `Session` cookies, `CSRF token` values, or personal identifiers (`docs/adr/0003-no-independent-bilibili-stack.md`).

## Endpoint Inventory

| Area | Endpoint (`api.h`) | Auth / Signing | Status | Notes |
|---|---|---|---|---|
| WBI keys | `Api::Nav` (`/x/web-interface/nav`) | Guest / Cookie | Verified in `bilibili_api` | Extracts `img_key` & `sub_key` from `wbi_img` URLs; 1-hour cache in `wbi.cpp` |
| Home Recommend | `Api::Recommend` (`/x/web-interface/wbi/index/top/feed/rcmd`) | WBI signature | Verified model + WBI | `RecommendVideoListResultWrapper` parsed & covered by fixture test |
| Search | `Api::Search` (`/x/web-interface/wbi/search/type`) | WBI signature | Verified model + WBI | `SearchResult` handles `video`, `media_bangumi`, `media_ft`, skips `ketang` |
| Video Detail | `Api::DetailAll` (`/x/web-interface/view/detail`) | Guest / Cookie | Verified model | `VideoDetailAllResult` parses `View`, `Card`, `Related`, `pages`, `ugc_season` |
| DASH PlayURL | `Api::PlayUrl2` (`/x/player/wbi/playurl`) | WBI + Cookie (for >360p/VIP) | Verified model + selector | `selectDashTracks` clamps QN (`landscapeQualityMax`/`portraitQualityMax`), selects codec (AVC/HEVC/AV1), and falls back Dolby -> FLAC -> Standard audio |
| PGC Season Detail / PlayURL | `Api::SeasonDetail` (`/pgc/view/pc/season`), `Api::SeasonUrl2` (`/pgc/player/web/v2/playurl`) | Cookie (entitlements) | Verified model | `SeasonUrlResult` / `VideoUrlResult`; unentitled/regional errors surface honest dialog messages |
| Video Comments | `Api::Comment` (`/x/v2/reply/main`) | Guest / Cookie | Verified model | `VideoCommentResultWrapper` + `VideoSingleCommentDetail` |
| Triple Action | `Api::TripleWeb` (`/x/web-interface/archive/like/triple`) | Cookie + `CSRF token` (`bili_jct`) | **Repaired** | Fixed missing leading `/` (`_apiBase + "x/..."` -> `_apiBase + "/x/..."`); verified in `bilibili_api` |
| QR Login V2 | `Api::QrCodeV2`, `Api::QrCodeCheckV2` | Guest | Verified poll states | `QrLoginInfoResultV2` maps `0`, `86101`, `86090`, `86038` cleanly; session persisted in `/config/wiliwili-ns2/wiliwili_config.json` |
| Watch History | `Api::HistoryVideo` (`/x/web-interface/history/cursor`), `Api::ProgressReport` | Cookie + `CSRF token` | Verified model | `HistoryVideoResultWrapper` cursor pagination & resume progress |
| Creator Space / Dynamics | `Api::DynamicVideo`, `Api::UserUploadedVideos` | Cookie / WBI | Integrated upstream (`2f583df`) | `DynamicUpResult::mid` string conversion from `dragonflylee/wiliwili` |
| Live Room PlayInfo | `Api::LiveRoomPlayInfo` | Guest / Cookie | Verified model | `LiveRoomPlayInfo` parses `playurl_info.playurl.stream` & `is_locked` |

## Limitations & Security Guarantees

- **No bypasses:** Membership (`10403`), charging-exclusive (`87007`/`87008`), and regional restrictions are surfaced as clear status dialogs and never bypassed.
- **TLS Verification:** `SettingItem::TLS_VERIFY` defaults to `1` (`true`) on Switch 2 (`wili::tlsVerifyDefaultForPlatform("switch2")`).
- **Session Isolation:** Cookies and `refresh_token` reside exclusively in `/config/wiliwili-ns2/wiliwili_config.json` on Switch 2, isolated from Switch 1 `/config/wiliwili/`.
