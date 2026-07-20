#pragma once

#include <string>

namespace lunar::api::constants {

// Xbox Live endpoints
inline constexpr const char* DEVICE_AUTH_URL   = "https://device.auth.xboxlive.com/device/authenticate";
inline constexpr const char* SISU_AUTH_URL     = "https://sisu.xboxlive.com/authenticate";
inline constexpr const char* SISU_AUTHORIZE_URL = "https://sisu.xboxlive.com/authorize";
inline constexpr const char* XSTS_AUTH_URL     = "https://xsts.auth.xboxlive.com/xsts/authorize";
inline constexpr const char* LOGIN_URL         = "https://login.live.com/oauth20_token.srf";

// GameStream endpoints
inline constexpr const char* GSSV_HOME_BASE    = "https://uks.core.gssv-play-prodxhome.xboxlive.com";
inline constexpr const char* GSSV_CLOUD_BASE   = "https://uks.core.gssv-play-prodxcloud.xboxlive.com";
inline constexpr const char* GSSV_XHOME_LOGIN  = "https://xhome.gssv-play-prod.xboxlive.com/v2/login/user";
inline constexpr const char* GSSV_XGPUWEB_LOGIN = "https://xgpuweb.gssv-play-prod.xboxlive.com/v2/login/user";
inline constexpr const char* GSSV_XGPUWEBF2P_LOGIN = "https://xgpuwebf2p.gssv-play-prod.xboxlive.com/v2/login/user";
inline constexpr const char* OFFERING_XHOME    = "xhome";
inline constexpr const char* OFFERING_XGPUWEB  = "xgpuweb";
inline constexpr const char* OFFERING_XGPUWEBF2P = "xgpuwebf2p";
inline constexpr const char* GAMEPASS_NEW_TITLES_URL =
    "https://catalog.gamepass.com/sigls/v2?id=f13cf6b4-57e6-4459-89df-6aec18cf0538&market=US&language=en-US";
inline constexpr const char* XSTREAMING_TITLES_JSON_URL =
    "https://cdn.jsdelivr.net/gh/Geocld/XStreaming@main/titles.json";

// Fixed app identifiers
inline constexpr const char* APP_ID            = "000000004c20a908";
inline constexpr const char* TITLE_ID          = "328178078";
inline constexpr const char* REDIRECT_URI      = "ms-xal-000000004c20a908://auth";
inline constexpr const char* SANDBOX           = "RETAIL";

// Token scopes
inline constexpr const char* SCOPE_AUTH        = "service::user.auth.xboxlive.com::MBI_SSL";
inline constexpr const char* RELYING_PARTY_WEB = "http://xboxlive.com";
inline constexpr const char* RELYING_PARTY_GSSV = "http://gssv.xboxlive.com/";

} // namespace lunar::api::constants
