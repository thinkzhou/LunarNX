#ifdef __SWITCH__
#include "auth_activity.h"
#include "../api/http_client.h"
#include "../common.h"
#include "../diagnostics.h"
#include <cJSON.h>
#include "../ui/main_activity.h"
#include "stream_settings_activity.h"
#include "qr_code.h"
#include "qr_code_view.h"
#include "ui_style.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <utility>
#include <vector>

namespace lunar::ui {

namespace {

void openXboxHome(const std::shared_ptr<app::StreamController>& ctrl) {
    auto open = [ctrl]() {
        brls::Application::pushActivity(
            new MainActivity(ctrl), brls::TransitionAnimation::NONE);
    };
    if (!brls::Application::popActivity(brls::TransitionAnimation::NONE, open)) {
        open();
    }
}

} // namespace

#ifndef LUNARNX_NETWORK_DIAG
#define LUNARNX_NETWORK_DIAG 0
#endif

#if LUNARNX_NETWORK_DIAG
namespace {
#endif

#if LUNARNX_NETWORK_DIAG

struct NetworkProbe {
    const char* name;
    const char* url;
};

static std::string runNetworkDiagnostics() {
    lunar::diagnosticLog("netdiag", "begin");

    api::HttpClient http;
    const NetworkProbe probes[] = {
        {"baidu-http", "http://www.baidu.com/"},
        {"baidu-https", "https://www.baidu.com/"},
        {"qq-https", "https://www.qq.com/"},
        {"microsoft-oidc", "https://login.microsoftonline.com/consumers/v2.0/.well-known/openid-configuration"},
    };

    int ok = 0;
    for (const auto& probe : probes) {
        lunar::diagnosticLog("netdiag", "probe %s begin url=%s", probe.name, probe.url);
        auto resp = http.get(probe.url, {});
        bool success = !resp.network_error && resp.status_code > 0 && resp.status_code < 500;
        if (success) ok++;
        lunar::diagnosticLog("netdiag",
                             "probe %s done success=%s status=%d network=%s error=%s body_len=%zu",
                             probe.name,
                             success ? "true" : "false",
                             resp.status_code,
                             resp.network_error ? "true" : "false",
                             resp.error_message.c_str(),
                             resp.body.size());
    }

    lunar::diagnosticLog("netdiag", "done ok=%d total=%zu", ok, sizeof(probes) / sizeof(probes[0]));
    return brls::getStr("lunarnx/auth/network_diag_done", ok,
                        sizeof(probes) / sizeof(probes[0]));
}

} // namespace
#endif

static stream::VideoBackend parseVideoBackendConfig(const char* value) {
    if (value && std::strcmp(value, "hardware_zero_copy") == 0) {
        return stream::VideoBackend::HardwareZeroCopy;
    }
    if (value && std::strcmp(value, "hardware_copy_out") == 0) {
        return stream::VideoBackend::HardwareCopyOut;
    }
    if (value && std::strcmp(value, "software") == 0) {
        return stream::VideoBackend::Software;
    }
    if (value && std::strcmp(value, "hardware") == 0) {
        return stream::VideoBackend::HardwareCopyOut;
    }
    return stream::VideoBackend::HardwareZeroCopy;
}

AuthActivity::AuthActivity(std::shared_ptr<app::StreamController> ctrl)
    : ctrl_(std::move(ctrl)) {}

brls::View* AuthActivity::createContentView() {
    const auto& p = uiPalette();
    auto* root = new brls::Box(brls::Axis::ROW);
    root->setWidth(brls::Application::ORIGINAL_WINDOW_WIDTH);
    root->setHeight(brls::Application::ORIGINAL_WINDOW_HEIGHT);
    root->setPadding(48, 56, 48, 56);
    root->setBackgroundColor(p.background);
    root->setAlignItems(brls::AlignItems::CENTER);
    root->registerAction(brls::getStr("lunarnx/auth/back_action"),
        brls::ControllerButton::BUTTON_B,
        [this](brls::View*) -> bool {
            if (brls::Application::popActivity(brls::TransitionAnimation::NONE)) {
                alive_->store(false);
                polling_->store(false);
                poll_generation_->fetch_add(1);
            } else {
                brls::Application::quit();
            }
            return true;
        });

    auto* story = new brls::Box(brls::Axis::COLUMN);
    story->setWidth(480);
    story->setHeight(624);
    story->setPadding(38, 38, 38, 38);
    story->setBackgroundColor(p.surface);
    story->setBorderThickness(0);
    story->setCornerRadius(0);

    auto* brand = new brls::Label();
    brand->setText("LUNARNX");
    brand->setFontSize(24);
    brand->setTextColor(p.accent);
    story->addView(brand);

    auto* headline = new brls::Label();
    headline->setHeight(136);
    headline->setText(brls::getStr("lunarnx/auth/tagline"));
    headline->setFontSize(43);
    headline->setTextColor(p.text);
    headline->setVerticalAlign(brls::VerticalAlign::CENTER);
    story->addView(headline);

    auto* intro = makeMutedLabel(brls::getStr("lunarnx/auth/intro"), 16);
    intro->setHeight(92);
    story->addView(intro);

    auto add_feature = [story, &p](const std::string& number,
                                   const std::string& title,
                                   const std::string& detail) {
        auto* row = new brls::Box(brls::Axis::ROW);
        row->setHeight(68);
        row->setAlignItems(brls::AlignItems::CENTER);

        auto* marker = new brls::Label();
        marker->setWidth(46);
        marker->setText(number);
        marker->setFontSize(13);
        marker->setTextColor(p.accent);
        row->addView(marker);

        auto* copy = new brls::Box(brls::Axis::COLUMN);
        copy->setGrow(1.0f);
        auto* feature_title = new brls::Label();
        feature_title->setText(title);
        feature_title->setFontSize(17);
        feature_title->setTextColor(p.text);
        copy->addView(feature_title);
        copy->addView(makeMutedLabel(detail, 13));
        row->addView(copy);
        story->addView(row);
    };
    add_feature("01", brls::getStr("lunarnx/auth/feature_remote_title"),
                brls::getStr("lunarnx/auth/feature_remote_detail"));
    add_feature("02", brls::getStr("lunarnx/auth/feature_cloud_title"),
                brls::getStr("lunarnx/auth/feature_cloud_detail"));
    add_feature("03", brls::getStr("lunarnx/auth/feature_switch_title"),
                brls::getStr("lunarnx/auth/feature_switch_detail"));

    auto* privacy = makeMutedLabel(brls::getStr("lunarnx/auth/privacy"), 12);
    privacy->setGrow(1.0f);
    privacy->setVerticalAlign(brls::VerticalAlign::BOTTOM);
    story->addView(privacy);
    root->addView(story);

    auto* sign_in = new brls::Box(brls::Axis::COLUMN);
    sign_in->setWidth(660);
    sign_in->setHeight(624);
    sign_in->setMarginLeft(28);
    sign_in->setPadding(30, 36, 26, 36);
    sign_in->setBackgroundColor(p.background);
    sign_in->setBorderThickness(1);
    sign_in->setBorderColor(p.border);
    sign_in->setCornerRadius(0);

    auto* eyebrow = new brls::Label();
    eyebrow->setText(brls::getStr("lunarnx/auth/sign_in"));
    eyebrow->setFontSize(13);
    eyebrow->setTextColor(p.accent);
    sign_in->addView(eyebrow);

    auto* title = new brls::Label();
    title->setHeight(48);
    title->setText(brls::getStr("lunarnx/auth/connect_account"));
    title->setFontSize(28);
    title->setTextColor(p.text);
    title->setVerticalAlign(brls::VerticalAlign::CENTER);
    sign_in->addView(title);

    auto* hint = makeMutedLabel(brls::getStr("lunarnx/auth/authorize_hint"), 14);
    hint->setHeight(46);
    sign_in->addView(hint);

    auto* auth_body = new brls::Box(brls::Axis::ROW);
    auth_body->setHeight(270);
    auth_body->setAlignItems(brls::AlignItems::CENTER);

    qr_view_ = new QrCodeView();
    qr_view_->setWidth(224);
    qr_view_->setHeight(224);
    qr_view_->clearQrCode();
    auth_body->addView(qr_view_);

    auto* device = new brls::Box(brls::Axis::COLUMN);
    device->setGrow(1.0f);
    device->setPadding(14, 0, 14, 24);

    auto* step = new brls::Label();
    step->setText(brls::getStr("lunarnx/auth/open_other_device"));
    step->setFontSize(11);
    step->setTextColor(p.text_muted);
    device->addView(step);

    url_label_ = new brls::Label();
    url_label_->setText("microsoft.com/link");
    url_label_->setHeight(52);
    url_label_->setFontSize(21);
    url_label_->setTextColor(p.accent);
    url_label_->setVerticalAlign(brls::VerticalAlign::CENTER);
    device->addView(url_label_);

    auto* code_hint = new brls::Label();
    code_hint->setText(brls::getStr("lunarnx/auth/enter_code"));
    code_hint->setFontSize(11);
    code_hint->setTextColor(p.text_muted);
    device->addView(code_hint);

    code_label_ = new brls::Label();
    code_label_->setText("--------");
    code_label_->setHeight(78);
    code_label_->setFontSize(41);
    code_label_->setTextColor(p.text);
    code_label_->setVerticalAlign(brls::VerticalAlign::CENTER);
    device->addView(code_label_);
    device->addView(makeMutedLabel(brls::getStr("lunarnx/auth/code_appears"), 12));
    auth_body->addView(device);
    sign_in->addView(auth_body);

    auto* status_card = new brls::Box(brls::Axis::ROW);
    status_card->setHeight(58);
    status_card->setPadding(10, 16, 10, 16);
    status_card->setBackgroundColor(p.card_muted);
    status_card->setBorderThickness(1);
    status_card->setBorderColor(p.border);
    status_card->setCornerRadius(0);
    auto* status_mark = new brls::Label();
    status_mark->setWidth(72);
    status_mark->setText(brls::getStr("lunarnx/common/status"));
    status_mark->setFontSize(11);
    status_mark->setTextColor(p.accent);
    status_card->addView(status_mark);
    status_label_ = new brls::Label();
    status_label_->setText(brls::getStr("lunarnx/auth/select_start"));
    status_label_->setFontSize(14);
    status_label_->setTextColor(p.text_muted);
    status_label_->setGrow(1.0f);
    status_card->addView(status_label_);
    sign_in->addView(status_card);

    auto* btn_row = new brls::Box(brls::Axis::ROW);
    btn_row->setHeight(58);
    btn_row->setMarginTop(16);

    start_btn_ = new brls::Button();
    start_btn_->setText(brls::getStr("lunarnx/auth/start_sign_in"));
    start_btn_->setGrow(1.0f);
    stylePrimaryButton(start_btn_);
    start_btn_->registerClickAction([this](brls::View*) -> bool {
        lunar::diagnosticLog("auth-ui", "Start sign-in clicked");
        if (resumeSavedSessionIfPresent()) {
            return true;
        }
        beginAuthRequest();
        return true;
    });
    btn_row->addView(start_btn_);

    cancel_btn_ = new brls::Button();
    cancel_btn_->setText(brls::getStr("lunarnx/common/cancel"));
    cancel_btn_->setWidth(160);
    cancel_btn_->setMarginLeft(12);
    styleSecondaryButton(cancel_btn_);
    cancel_btn_->setState(brls::ButtonState::DISABLED);
    cancel_btn_->registerClickAction([this](brls::View*) -> bool {
        polling_->store(false);
        poll_generation_->fetch_add(1);
        if (start_btn_) start_btn_->setState(brls::ButtonState::ENABLED);
        if (cancel_btn_) cancel_btn_->setState(brls::ButtonState::DISABLED);
        status_label_->setText(brls::getStr("lunarnx/auth/cancelled"));
        code_label_->setText("--------");
        if (qr_view_) qr_view_->clearQrCode();
        return true;
    });
    btn_row->addView(cancel_btn_);
    sign_in->addView(btn_row);

    auto* footer = makeMutedLabel(brls::getStr("lunarnx/auth/footer_exit"), 12);
    footer->setGrow(1.0f);
    footer->setHorizontalAlign(brls::HorizontalAlign::RIGHT);
    footer->setVerticalAlign(brls::VerticalAlign::BOTTOM);
    sign_in->addView(footer);
    root->addView(sign_in);

    return makeAppFrame("Xbox", root);
}

std::shared_ptr<app::StreamController> AuthActivity::createConfiguredController() {
    auto ctrl = std::make_shared<app::StreamController>();
    const auto stream_settings = loadStreamSettings();
    ctrl->setDefaultVideoBackend(stream_settings.video_backend);
    ctrl->setRumbleEnabled(stream_settings.vibration_enabled);
    ctrl->setRumbleStrengthPercent(stream_settings.rumble_strength_percent);

    // Check config.json for mock Xbox base URL (for local testing).
    std::string config_path = lunar::get_config_path();
    FILE* f = fopen(config_path.c_str(), "rb");
    if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            std::string buf(sz, '\0');
            fread(&buf[0], 1, sz, f);
            fclose(f);

            cJSON* root = cJSON_Parse(buf.c_str());
            if (root) {
                cJSON* backend = cJSON_GetObjectItem(root, "video_backend");
                if (backend && cJSON_IsString(backend) && backend->valuestring) {
                    auto video_backend = parseVideoBackendConfig(backend->valuestring);
                    lunar::diagnosticLog("auth-ui", "config video_backend=%s",
                                         stream::videoBackendName(video_backend));
                    ctrl->setDefaultVideoBackend(video_backend);
                }

                cJSON* vibration = cJSON_GetObjectItem(root, "vibration");
                if (vibration && cJSON_IsBool(vibration)) {
                    ctrl->setRumbleEnabled(cJSON_IsTrue(vibration));
                }
                cJSON* rumble_strength =
                    cJSON_GetObjectItem(root, "rumble_strength_percent");
                if (rumble_strength && cJSON_IsNumber(rumble_strength)) {
                    ctrl->setRumbleStrengthPercent(rumble_strength->valueint);
                }

                cJSON* base = cJSON_GetObjectItem(root, "base_url");
                if (base && cJSON_IsString(base) && base->valuestring &&
                    strlen(base->valuestring) > 0) {
                    std::string url(base->valuestring);
                    lunar::diagnosticLog("auth-ui", "config base_url=%s", url.c_str());
                    const bool mock_ok = ctrl->bypassAuthForMock(url);
                    lunar::diagnosticLog("auth-ui", "bypassAuthForMock ok=%s mock=%s",
                                         mock_ok ? "true" : "false",
                                         ctrl->isMockMode() ? "true" : "false");
                }

                cJSON* region_ip = cJSON_GetObjectItem(root, "force_region_ip");
                if (region_ip && cJSON_IsString(region_ip) && region_ip->valuestring && region_ip->valuestring[0]) {
                    ctrl->setForceRegionIp(region_ip->valuestring);
                    lunar::diagnosticLog("auth-ui", "config force_region_ip=%s", region_ip->valuestring);
                } else if (region_ip && cJSON_IsString(region_ip) && region_ip->valuestring && !region_ip->valuestring[0]) {
                    // Explicit empty = Auto/Native
                    ctrl->setForceRegionIp("");
                    lunar::diagnosticLog("auth-ui", "config force_region_ip=(auto/native)");
                } else {
                    ctrl->setForceRegionIp("4.2.2.2");
                    lunar::diagnosticLog("auth-ui", "config force_region_ip default US1 4.2.2.2");
                }
                cJSON_Delete(root);
            }
    }
    return ctrl;
}

std::shared_ptr<app::StreamController> AuthActivity::controller() {
    if (!ctrl_) ctrl_ = createConfiguredController();
    return ctrl_;
}

bool AuthActivity::resumeSavedSessionIfPresent() {
    auto ctrl = controller();

    // Mock mode must be checked before the one-shot flag. bypassAuthForMock()
    // already populates a mock console locally and does not need network.
    if (ctrl->isMockMode()) {
        lunar::diagnosticLog("auth-ui", "Mock mode detected via isMockMode(), skipping auth");
        if (status_label_) {
            status_label_->setText(brls::getStr("lunarnx/auth/mock_opening"));
        }
        auto alive = alive_;
        brls::sync([alive, ctrl]() {
            if (!alive->load()) return;
            openXboxHome(ctrl);
        });
        return true;
    }

    // Fallback for older builds / partial mock setup.
    {
        auto consoles = ctrl->getConsoles();
        if (!consoles.empty() && consoles[0].id == "MOCKXBOX001") {
            lunar::diagnosticLog("auth-ui", "Mock console list detected, skipping auth");
            auto alive = alive_;
            brls::sync([alive, ctrl]() {
                if (!alive->load()) return;
                openXboxHome(ctrl);
            });
            return true;
        }
    }

    if (saved_session_resume_attempted_) return false;
    saved_session_resume_attempted_ = true;

    if (!ctrl->loadTokens(lunar::get_token_path()) || !ctrl->hasCredentials()) {
        lunar::diagnosticLog("auth-ui", "No saved sign-in session found");
        return false;
    }

    lunar::diagnosticLog("auth-ui", "Saved sign-in session loaded");
    if (start_btn_) start_btn_->setState(brls::ButtonState::DISABLED);
    if (cancel_btn_) cancel_btn_->setState(brls::ButtonState::DISABLED);
    if (code_label_) code_label_->setText("--------");
    if (url_label_) url_label_->setText(brls::getStr("lunarnx/auth/saved_sign_in"));
    if (qr_view_) qr_view_->clearQrCode();
    if (status_label_) {
        status_label_->setText(brls::getStr("lunarnx/auth/signed_in_opening"));
    }

    auto alive = alive_;
    brls::sync([alive, ctrl]() {
        if (!alive->load()) return;
        openXboxHome(ctrl);
    });
    return true;
}

void AuthActivity::beginAuthRequest() {
    if (polling_->load() || auth_requesting_->load()) return;
    if (auth_request_thread_.joinable()) {
        if (!auth_request_done_->load()) {
            status_label_->setText(brls::getStr("lunarnx/auth/finishing_previous"));
            return;
        }
        auth_request_thread_.join();
    }
    if (poll_thread_.joinable()) {
        if (!poll_thread_done_->load()) {
            status_label_->setText(brls::getStr("lunarnx/auth/finishing_previous"));
            return;
        }
        poll_thread_.join();
    }

    auth_requesting_->store(true);
    auth_request_done_->store(false);
    uint32_t generation = auth_request_generation_->fetch_add(1) + 1;
    if (start_btn_) start_btn_->setState(brls::ButtonState::DISABLED);
    if (cancel_btn_) cancel_btn_->setState(brls::ButtonState::DISABLED);
    if (code_label_) code_label_->setText("--------");
    if (status_label_) {
        status_label_->setText(brls::getStr("lunarnx/auth/requesting_code"));
    }

    auto alive = alive_;
    auto requesting = auth_requesting_;
    auto request_done = auth_request_done_;
    auto request_generation = auth_request_generation_;
    auto ctrl = controller();
    auto* start_btn = start_btn_;
    auto* cancel_btn = cancel_btn_;
    auto* status_label = status_label_;
    auto* code_label = code_label_;
    auto* url_label = url_label_;
    auto* qr_view = qr_view_;
    auth_request_thread_ = std::thread([this, alive, requesting, request_done,
                                        request_generation, ctrl, generation,
                                        start_btn, cancel_btn, status_label,
                                        code_label, url_label, qr_view]() {
        struct DoneGuard {
            std::shared_ptr<std::atomic<bool>> done;
            ~DoneGuard() { done->store(true); }
        } done_guard{request_done};

#if LUNARNX_NETWORK_DIAG
        std::string diag_summary = runNetworkDiagnostics();
        brls::sync([alive, requesting, request_generation, generation,
                    diag_summary, start_btn, cancel_btn, status_label,
                    code_label, url_label, qr_view]() {
            requesting->store(false);
            if (!alive->load() || request_generation->load() != generation) return;
            if (start_btn) start_btn->setState(brls::ButtonState::ENABLED);
            if (cancel_btn) cancel_btn->setState(brls::ButtonState::DISABLED);
            if (url_label) url_label->setText("sdmc:/switch/LunarNX/lunarnx.log");
            if (code_label) code_label->setText("NETDIAG");
            if (status_label) status_label->setText(diag_summary);
        });
        return;
#endif

        bool ok = ctrl->startAuth();
        std::string code = ctrl->getAuthCode();
        std::string url = ctrl->getAuthUrl();
        std::string error = ctrl->getAuthError();

        brls::sync([this, alive, requesting, request_generation, generation,
                    ok, code, url, error, start_btn, cancel_btn, status_label,
                    code_label, url_label, qr_view]() {
            requesting->store(false);
            if (!alive->load() || request_generation->load() != generation) return;
            if (!ok) {
                if (start_btn) start_btn->setState(brls::ButtonState::ENABLED);
                if (cancel_btn) cancel_btn->setState(brls::ButtonState::DISABLED);
                if (code_label) code_label->setText("--------");
                if (status_label) {
                    status_label->setText(error.empty()
                        ? brls::getStr("lunarnx/auth/code_request_failed")
                        : error);
                }
                return;
            }

            if (url_label && !url.empty()) url_label->setText(url);
            if (qr_view) qr_view->setQrCode(makeQrCode(url));
            if (code_label) code_label->setText(code);
            if (status_label) {
                status_label->setText(brls::getStr("lunarnx/auth/enter_at_website"));
            }
            startPollingThread();
        });
    });
}

void AuthActivity::startPollingThread() {
    if (polling_->load()) return;
    if (poll_thread_.joinable()) {
        if (!poll_thread_done_->load()) {
            if (status_label_) {
                status_label_->setText(brls::getStr("lunarnx/auth/finishing_previous"));
            }
            return;
        }
        poll_thread_.join();
    }

    polling_->store(true);
    uint32_t generation = poll_generation_->fetch_add(1) + 1;
    if (cancel_btn_) cancel_btn_->setState(brls::ButtonState::ENABLED);
    if (start_btn_) start_btn_->setState(brls::ButtonState::DISABLED);

    auto alive = alive_;
    auto polling = polling_;
    auto poll_thread_done = poll_thread_done_;
    auto poll_generation = poll_generation_;
    auto ctrl = ctrl_;
    auto* start_btn = start_btn_;
    auto* cancel_btn = cancel_btn_;
    auto* status_label = status_label_;
    auto* code_label = code_label_;
    auto* qr_view = qr_view_;
    poll_thread_done_->store(false);
    poll_thread_ = std::thread([alive, polling, poll_generation, ctrl, generation,
                                poll_thread_done,
                                start_btn, cancel_btn, status_label, code_label, qr_view]() {
        struct DoneGuard {
            std::shared_ptr<std::atomic<bool>> done;
            ~DoneGuard() { done->store(true); }
        } done_guard{poll_thread_done};
        auto started_at = std::chrono::steady_clock::now();
        int poll_tries = 0;
        std::string progress_dots;
        while (alive->load() && polling->load() && poll_generation->load() == generation) {
            int interval = ctrl->getAuthPollIntervalSeconds();
            if (interval < 1) interval = 5;
            for (int i = 0; i < interval * 10; i++) {
                if (!alive->load() || !polling->load() || poll_generation->load() != generation) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            poll_tries += interval;
            if (!alive->load() || !polling->load() || poll_generation->load() != generation) return;

            int expires_in = ctrl->getDeviceCodeExpiresInSeconds();
            if (expires_in <= 0) expires_in = 900;
            int elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started_at).count());
            if (elapsed >= expires_in) {
                polling->store(false);
                brls::sync([alive, start_btn, cancel_btn, status_label, code_label, qr_view]() {
                    if (!alive->load()) return;
                    if (start_btn) start_btn->setState(brls::ButtonState::ENABLED);
                    if (cancel_btn) cancel_btn->setState(brls::ButtonState::DISABLED);
                    status_label->setText(brls::getStr("lunarnx/auth/code_expired"));
                    code_label->setText("--------");
                    if (qr_view) qr_view->clearQrCode();
                });
                return;
            }

            auto result = ctrl->pollAuthStatus();
            if (!alive->load() || !polling->load() || poll_generation->load() != generation) return;

            if (result == auth::DeviceCodePollResult::Authenticated) {
                polling->store(false);
                brls::sync([alive, ctrl]() {
                    if (!alive->load()) return;
                    openXboxHome(ctrl);
                });
                return;
            }

            if (result == auth::DeviceCodePollResult::Declined ||
                result == auth::DeviceCodePollResult::Expired ||
                result == auth::DeviceCodePollResult::Error) {
                polling->store(false);
                std::string error = ctrl->getAuthError();
                brls::sync([alive, start_btn, cancel_btn, status_label, code_label, qr_view, result, error]() {
                    if (!alive->load()) return;
                    if (start_btn) start_btn->setState(brls::ButtonState::ENABLED);
                    if (cancel_btn) cancel_btn->setState(brls::ButtonState::DISABLED);
                    if (code_label) code_label->setText("--------");
                    if (qr_view) qr_view->clearQrCode();
                    if (!status_label) return;
                    if (!error.empty()) {
                        status_label->setText(error);
                    } else if (result == auth::DeviceCodePollResult::Declined) {
                        status_label->setText(brls::getStr("lunarnx/auth/declined"));
                    } else if (result == auth::DeviceCodePollResult::Expired) {
                        status_label->setText(brls::getStr("lunarnx/auth/code_expired"));
                    } else {
                        status_label->setText(brls::getStr("lunarnx/auth/failed"));
                    }
                });
                return;
            }

            // Animated dots for visual progress
            progress_dots += ".";
            if (progress_dots.size() > 3) progress_dots = ".";

            brls::sync([alive, status_label, progress_dots, poll_tries, result]() {
                if (!alive->load()) return;
                status_label->setText(result == auth::DeviceCodePollResult::SlowDown
                    ? brls::getStr("lunarnx/auth/waiting_slow", progress_dots, poll_tries)
                    : brls::getStr("lunarnx/auth/waiting", progress_dots, poll_tries));
            });
        }
    });
}

void AuthActivity::onResume() {
    alive_->store(true);
    resetSignedOutUi();
}

void AuthActivity::resetSignedOutUi() {
    polling_->store(false);
    poll_generation_->fetch_add(1);
    if (start_btn_) start_btn_->setState(brls::ButtonState::ENABLED);
    if (cancel_btn_) cancel_btn_->setState(brls::ButtonState::DISABLED);
    if (code_label_) code_label_->setText("--------");
    if (url_label_) url_label_->setText("microsoft.com/link");
    if (qr_view_) qr_view_->clearQrCode();
    if (status_label_) {
        status_label_->setText(brls::getStr("lunarnx/auth/select_start"));
    }
}

AuthActivity::~AuthActivity() {
    alive_->store(false);
    auth_requesting_->store(false);
    auth_request_generation_->fetch_add(1);
    polling_->store(false);
    poll_generation_->fetch_add(1);
    if (auth_request_thread_.joinable()) {
        if (auth_request_done_->load()) auth_request_thread_.join();
        else auth_request_thread_.detach();
    }
    if (poll_thread_.joinable()) {
        if (poll_thread_done_->load()) poll_thread_.join();
        else poll_thread_.detach();
    }
}

} // namespace lunar::ui
#endif
