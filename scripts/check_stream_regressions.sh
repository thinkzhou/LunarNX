#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

fail() {
  printf 'FAIL: %s\n' "$1" >&2
  exit 1
}

if rg -n 'std::thread\(\[this, alive\]' src/ui/main_activity.cpp >/dev/null; then
  fail "MainActivity console fetch workers must not capture this in detached threads"
fi

if rg -n 'setViewports\(.*frame_w|setScissors\(.*frame_w' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch renderer viewport/scissor must be based on framebuffer dimensions, not stream frame dimensions"
fi

if ! rg -n 'enum class PostProcessMode' src/stream/media_pipeline.h >/dev/null; then
  fail "Stream media pipeline must expose a session-scoped PostProcessMode enum"
fi

if ! rg -n 'post_process_mode' src/stream/media_pipeline.h src/stream/media_pipeline.cpp src/app/stream_controller.* src/ui/main_activity.* >/dev/null; then
  fail "Stream media pipeline must propagate a session-scoped post-process mode"
fi

if ! rg -n 'setPostProcessMode' src/stream/video_renderer.* src/stream/media_pipeline.cpp >/dev/null; then
  fail "VideoRenderer must expose a post-process mode switch"
fi

if ! rg -n 'setPostProcessEnabled' src/stream/video_renderer.* >/dev/null; then
  fail "VideoRenderer must keep the compatibility post-process enable switch"
fi

if ! rg -n 'post_process_btn_|post_process_mode_' src/ui/main_activity.* >/dev/null; then
  fail "Switch UI must expose a default-off post-process mode control"
fi

for label in '"off": "Off"' '"upscale": "Upscale"' '"upscale_rcas": "Upscale \+ RCAS"'; do
  if ! rg -n "$label" romfs/i18n/en-US/lunarnx.json >/dev/null; then
    fail "Switch UI must expose localized post-process label: $label"
  fi
done

if ! rg -n 'PostProcessMode::Off|PostProcessMode::Upscale|PostProcessMode::UpscaleRcas' \
  src/ui/stream_settings_activity.cpp >/dev/null; then
  fail "Stream settings must map every post-process choice"
fi

if ! rg -n 'UpscaleRcas' src/stream src/ui >/dev/null; then
  fail "Post-process modes must include an UpscaleRcas setting"
fi

if ! rg -n 'RenderTarget|ensurePostProcessTargets|releasePostProcessTargets' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch renderer must own post-process render targets and lifecycle helpers"
fi

for symbol in EasuConstants RcasConstants populateEasuConstants populateRcasConstants recordUpscalePass recordRcasPass bindUniformBuffer upscale_target rcas_target; do
  if ! rg -n "$symbol" src/stream/video_renderer.cpp >/dev/null; then
    fail "Switch renderer must include EASU/RCAS implementation symbol: $symbol"
  fi
done

if [[ ! -f shaders/upscaling_pass_fsh.glsl ]]; then
  fail "Switch post-process blit shader source must exist"
fi

if [[ ! -f shaders/upscaling_fsh.glsl ]]; then
  fail "Switch EASU shader source must exist"
fi

if [[ ! -f shaders/rcas_fsh.glsl ]]; then
  fail "Switch RCAS shader source must exist"
fi

for shader in upscaling_pass_fsh upscaling_fsh rcas_fsh; do
  if ! rg -n "${shader}\.dksh" Makefile.switch scripts/compile_shaders.sh >/dev/null; then
    fail "Switch post-process shader must be generated into RomFS: ${shader}.dksh"
  fi
done

for symbol in getFrameColorInfo AVCOL_SPC_BT2020_NCL bt2020 AVCOL_RANGE_JPEG; do
  if ! rg -n "$symbol" src/stream/video_renderer.cpp >/dev/null; then
    fail "Switch renderer must use AVFrame color metadata and BT.2020/full-range aware transforms: $symbol"
  fi
done

if ! rg -n 'lo == luma_offset' src/stream/video_renderer.cpp >/dev/null ||
   ! rg -n 'co == chroma_offset' src/stream/video_renderer.cpp >/dev/null ||
   ! rg -n 'w == width && hgt == height' src/stream/video_renderer.cpp >/dev/null ||
   ! rg -n 'linear == is_linear' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch NVTEGRA frame mapping cache must compare both plane offsets, dimensions, and layout"
fi

if ! rg -n 'mapping\.ll|mapping\.cll' src/stream/video_renderer.cpp >/dev/null ||
   ! rg -n 'DkTileSize_TwoGobs|setPitchStride' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch renderer must retain per-mapping layouts for real NVTEGRA surface geometry"
fi

if ! rg -n 'canUpscaleFrame' src/stream/video_renderer.cpp >/dev/null ||
   ! rg -n 'target_width > frame_width \|\| target_height > frame_height' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch EASU path must only run when the presentation target is larger than the decoded frame"
fi

if ! rg -n 'use_rcas = usesRcas' src/stream/video_renderer.cpp >/dev/null ||
   ! rg -n 'use_intermediate = use_upscale \|\| use_rcas \|\| can_dither' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch RCAS/dithering must be able to use a post-process target without requiring EASU"
fi

for symbol in DitheringConstants setDitheringEnabled dithering_enabled dithering_strength PostProcessSettings; do
  if ! rg -n "$symbol" src/stream src/ui shaders/upscaling_pass_fsh.glsl >/dev/null; then
    fail "Switch renderer/UI must expose default-off dithering support: $symbol"
  fi
done

for symbol in recordRenderSubmit recordPostProcess recordUpscaling recordSharpening recordDithering render_submit_total_us post_process_total_us upscaling_total_us sharpening_total_us dithering_total_us; do
  if ! rg -n "$symbol" src/stream src/ui >/dev/null; then
    fail "PerfStats must track render/post-process timings: $symbol"
  fi
done

for symbol in audio_latency_ms audio_buffer_ms audio_overflow_ms; do
  if ! rg -n "$symbol" src/stream src/ui >/dev/null; then
    fail "PerfStats/UI must expose audio queue latency tuning stat: $symbol"
  fi
done

if ! rg -n 'AUDREN_SAMPLES_PER_FRAME_48KHZ' src/stream/audio_player.* >/dev/null; then
  fail "Switch Audren buffer sizing should be based on AUDREN_SAMPLES_PER_FRAME_48KHZ"
fi

if ! rg -n 'kAudioOverflowMs' src/stream/audio_player.* >/dev/null; then
  fail "Switch Audren queue overflow threshold should be named and visible for tuning"
fi

audio_wavebuf_count="$(awk '/BUFFER_COUNT =/ { gsub(/[^0-9]/, "", $0); print $0; exit }' src/stream/audio_player.h)"
if [[ -z "${audio_wavebuf_count}" || "${audio_wavebuf_count}" -lt 5 ]]; then
  fail "Switch Audren audio output should keep a Moonlight-style wavebuf queue"
fi

if ! rg -n 'audrenInitialize|audrvCreate|AudioDriverWaveBuf|audrvVoiceAddWaveBuf' src/stream/audio_player.* >/dev/null; then
  fail "Switch audio output should use Audren wave buffers, not audout buffers"
fi

if rg -n 'audoutInitialize|AudioOutBuffer|audoutAppendAudioOutBuffer' src/stream/audio_player.* >/dev/null; then
  fail "Switch audio output should not use the older audout queue path"
fi

if ! rg -n 'sendZero|zero_sent|needs_zero' src/input/rumble_controller.cpp >/dev/null; then
  fail "Rumble controller must explicitly send a zero vibration packet on natural expiry"
fi

if rg -n 'find_package\(SDL2 REQUIRED\)' CMakeLists.txt >/dev/null; then
  fail "Desktop CMake must not use generic find_package(SDL2) while DEVKITPRO can be present"
fi

if rg -n 'DkImageDescriptor\*|dkImageDescriptorInitialize|dkCmdBufBindImageDescriptorSet|dkCmdBufBindSamplerDescriptorSet|img_descs|smp_descs' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch renderer must use Borealis image slots/updateImageDescriptor instead of rewriting descriptor memory"
fi

if ! rg -n 'CCmdMemRing<brls::FRAMEBUFFERS_COUNT>' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch renderer must use per-frame CCmdMemRing command memory for dynamic Deko3D submissions"
fi

if ! rg -n 'updateImageDescriptor' src/stream/video_renderer.cpp >/dev/null; then
  fail "Switch renderer must update image descriptors through SwitchVideoContext"
fi

if rg -n 'std::lock_guard<std::mutex> lifecycle_lock\(stream_lifecycle_mutex_\)' src/app/stream_controller.cpp >/dev/null; then
  fail "startStream must not hold stream_lifecycle_mutex_ across blocking network/ICE setup"
fi

if ! rg -n 'stream_generation_|cancel_requested_|isStreamCancelled' src/app/stream_controller.* >/dev/null; then
  fail "Stream lifecycle must expose a cancellation token/generation"
fi

if ! rg -n 'auth_operation_mutex_' src/app/stream_controller.* >/dev/null; then
  fail "Console fetch and sign-out must share an auth/session operation mutex"
fi

if ! awk '/bool StreamController::fetchConsoles\(\)/ { inside=1 } inside && /std::lock_guard<std::mutex> auth_lock\(auth_operation_mutex_\)/ { found=1 } inside && /^}/ { inside=0 } END { exit found ? 0 : 1 }' src/app/stream_controller.cpp; then
  fail "StreamController::fetchConsoles must hold the auth/session operation mutex"
fi

if ! awk '/void StreamController::signOut\(\)/ { inside=1 } inside && /std::lock_guard<std::mutex> auth_lock\(auth_operation_mutex_\)/ { found=1 } inside && /^}/ { inside=0 } END { exit found ? 0 : 1 }' src/app/stream_controller.cpp; then
  fail "StreamController::signOut must hold the auth/session operation mutex before clearing tokens"
fi

if ! rg -n 'getConsoles\(.*CancelCallback' src/api/xbox_api_client.* >/dev/null ||
   ! rg -n 'api->getConsoles\(.*signout_requested' src/app/stream_controller.cpp >/dev/null; then
  fail "Console discovery must be cancellable when sign-out is requested"
fi

if ! rg -n 'setStateCallback' src/ui/main_activity.cpp >/dev/null; then
  fail "Switch main UI must surface StreamController connection status callbacks"
fi

if rg -n 'elapsed_s\+\+' src/ui/stream_view.cpp >/dev/null; then
  fail "Stream elapsed timer must be computed from steady_clock/perf start time, not incremented per loop"
fi

if rg -n 'registerAction\("Stop Stream", brls::ControllerButton::BUTTON_BACK' src/ui/stream_view.cpp >/dev/null; then
  fail "Stop stream must not be bound to single Minus because Minus is Xbox View"
fi

if ! rg -n 'getStr\("lunarnx/stream/stop_action_plus"\)' src/ui/stream_view.cpp >/dev/null ||
   ! rg -n 'ControllerButton::BUTTON_START, stop_handler' src/ui/stream_view.cpp >/dev/null ||
   ! rg -n 'getStr\("lunarnx/stream/stop_action_minus"\)' src/ui/stream_view.cpp >/dev/null ||
   ! rg -n 'ControllerButton::BUTTON_BACK, stop_handler' src/ui/stream_view.cpp >/dev/null; then
  fail "Stop stream combo must listen on both Plus and Minus while ignoring single-button presses"
fi

if ! rg -n 'setInputSuppressed|input_suppressed_' src/app/stream_controller.* src/ui/stream_view.cpp >/dev/null; then
  fail "Exit overlay/combo must suppress reserved View/Menu input frames sent to Xbox"
fi

if ! rg -n 'audio_drops|recordAudioDrop|audio_queued' src/stream src/ui >/dev/null; then
  fail "Audio output drops/queue depth must be visible in PerfStats/UI"
fi

if ! rg -n 'HandshakeAck' src/app src/webrtc >/dev/null; then
  fail "Xbox message channel should wait for HandshakeAck before starting control/input"
fi

if ! rg -n 'encodeMetadata|ClientMetadata' src/input src/app >/dev/null; then
  fail "Xbox input channel should send the initial ClientMetadata packet"
fi

for symbol in src/app/ice_candidate_processor.h src/app/ice_candidate_processor.cpp parseRemotePayload toApiJson toLibPeerLines candidateIsInvalid stream_tests; do
  if ! rg -n "$symbol" src/app tests Makefile.desktop scripts/check_stream_regressions.sh >/dev/null; then
    fail "Xbox ICE candidate processor regression check missing symbol: $symbol"
  fi
done

for symbol in WebRtcTransport gatherLocalCandidates waitDataChannels; do
  if ! rg -n "$symbol" src/app tests scripts/check_stream_regressions.sh >/dev/null; then
    fail "Xbox WebRTC transport regression check missing symbol: $symbol"
  fi
done

for symbol in h264_rtp_depacketizer_test h264_rtp_tests; do
  if ! rg -n "$symbol" tests Makefile.desktop scripts/check_stream_regressions.sh >/dev/null; then
    fail "H264 RTP depacketizer regression check missing symbol: $symbol"
  fi
done

for symbol in XboxChannelManager sendMessageHandshake authorizationRequest gamepadChanged videoKeyframeRequested; do
  if ! rg -n "$symbol" src/app tests scripts/check_stream_regressions.sh >/dev/null; then
    fail "Xbox channel manager regression check missing symbol: $symbol"
  fi
done

if ! rg -n 'makeHomeStreamProfile' src/app/stream_controller.cpp >/dev/null; then
  fail "StreamController::startStream must build a StreamProfile via makeHomeStreamProfile"
fi

if ! rg -n 'XboxStreamSession' src/app/stream_controller.cpp >/dev/null; then
  fail "StreamController::startStream must delegate startup to XboxStreamSession"
fi

if rg -n 'sendSdpOffer|getSdpAnswer|sendIceCandidates|getIceCandidates' src/app/stream_controller.cpp >/dev/null; then
  fail "StreamController must not own raw SDP/ICE API exchange after session refactor"
fi

if ! rg -n 'SessionType::Cloud' src/app >/dev/null; then
  fail "Xbox session layer must expose Cloud profile support"
fi

if ! rg -n 'ReadyToConnect|sendConnect' src/app src/api >/dev/null; then
  fail "xCloud session path must handle ReadyToConnect via sendConnect"
fi

if ! rg -n 'startCloudStream|makeCloudStreamProfile' src/app >/dev/null; then
  fail "StreamController must expose startCloudStream / makeCloudStreamProfile"
fi

if ! rg -n 'OFFERING_XGPUWEB|gssv_cloud_token|getCloudStreamingToken' src/auth >/dev/null; then
  fail "Auth layer must acquire and store xCloud streaming tokens"
fi

if rg -n 'len >= off \+ 10' src/webrtc/peer_manager.cpp >/dev/null; then
  fail "Rumble parser boundary check must match the bytes actually read through repeat"
fi

for symbol in 'void StreamController::signOut' 'void AuthManager::clearTokens'; do
  if ! rg -n "$symbol" src/app src/auth >/dev/null; then
    fail "Sign-out must clear in-memory streaming/auth state: $symbol"
  fi
done

if ! rg -n 'std::chrono::system_clock::now' src/auth/auth_manager.cpp >/dev/null ||
   ! rg -n 'expires_at_ms <= now_ms \+ kRefreshLeadMs' src/auth/auth_manager.cpp >/dev/null; then
  fail "Cold-start token refresh must compare persisted system-clock expiry"
fi

if ! rg -n 'hasSavedCredentials' src/auth src/app src/ui >/dev/null; then
  fail "Saved refresh tokens must be treated as recoverable credentials for cold-start UI resume"
fi

if rg -n 'return auth_->isAuthenticated\(\);' src/app/stream_controller.cpp >/dev/null; then
  fail "StreamController::loadTokens/hasCredentials must not require already-derived streaming tokens"
fi

if awk '/bool AuthManager::refreshTokensIfNeeded\(\)/ { inside=1 } inside && /if \(msal_refresh_token_\.empty\(\)\) return false;/ { found=1 } inside && /^}/ { inside=0 } END { exit found ? 0 : 1 }' src/auth/auth_manager.cpp; then
  fail "AuthManager::refreshTokensIfNeeded must allow already usable streaming tokens without a refresh token"
fi

if rg -n 'Applet Mode is unsupported|appletGetAppletType|AppletType_Application|hasApplicationMemory|showAppletModeUnsupportedScreen' src/main.cpp >/dev/null; then
  fail "Switch startup must stay light and must not restore an Applet Mode blocking screen"
fi

if ! rg -n 'sdmc:/switch/LunarNX/lunarnx\.log' src/diagnostics.h src/platform/switch_wrapper.c >/dev/null ||
   ! rg -n 'diagnosticLog\("main"' src/main.cpp >/dev/null; then
  fail "Switch startup must write an SD-card diagnostic log for hbmenu-launched failures"
fi

if ! rg -n 'brls::Logger::setLogLevel\(brls::LogLevel::LOG_DEBUG\)' src/main.cpp >/dev/null; then
  fail "Switch startup must keep Borealis logging enabled for real-device diagnosis"
fi

if ! rg -n 'nxlinkStdio\(\)' src/platform/switch_wrapper.c >/dev/null ||
   ! rg -n '#ifdef DEBUG' src/platform/switch_wrapper.c >/dev/null; then
  fail "Switch startup must keep nxlink stdio limited to debug wrapper builds"
fi

if ! rg -n 'static int nxlink_sock = -1' src/platform/switch_wrapper.c >/dev/null ||
   ! rg -n 'close\(nxlink_sock\)' src/platform/switch_wrapper.c >/dev/null; then
  fail "Switch startup must close nxlink stdio socket when nxlink is enabled"
fi

if ! rg -n 'src/platform/switch_wrapper\.c' Makefile.switch >/dev/null; then
  fail "Switch build must link LunarNX switch_wrapper.c so romfs and Switch services are initialized before Borealis startup"
fi

if rg -n 'socketInitializeDefault\(\)|socketExit\(\)' src/main.cpp >/dev/null; then
  fail "Switch main must not duplicate socket init/exit; switch_wrapper.c owns libnx socket lifecycle"
fi

if ! rg -n 'ROMFS_STAGE' Makefile.switch >/dev/null ||
   ! rg -n 'lib/borealis/resources' Makefile.switch >/dev/null ||
   ! rg -n -- '--romfsdir=\$\(ROMFS_STAGE\)' Makefile.switch >/dev/null; then
  fail "Switch NRO must stage Borealis resources into romfs alongside LunarNX assets"
fi

if ! awk '
  /int main\(int argc, char\* argv\[\]\)/ { inside=1 }
  inside && /brls::Application::init\(\)/ && !init { init=NR }
  inside && /brls::Application::createWindow\("LunarNX"\)/ && !window { window=NR }
  inside && /brls::Application::pushActivity/ && !push { push=NR }
  END { exit (init && window && push && init < window && window < push) ? 0 : 1 }
' src/main.cpp; then
  fail "Switch Borealis startup must create a window before pushing the first activity"
fi

if ! rg -n 'Connection lost\. Reconnect attempts failed' src/app/xbox_stream_session.cpp >/dev/null; then
  fail "Stream session must leave Streaming state after reconnect exhaustion"
fi

if ! rg -n 'getStr\("lunarnx/stream/menu_hint"\)' src/ui/stream_view.cpp >/dev/null ||
   ! rg -n '"menu_hint": "Swipe left from the right edge to open\. Swipe right or press B to close\."' romfs/i18n/en-US/lunarnx.json >/dev/null; then
  fail "Stream quick menu must explain how to open and close it"
fi

if ! rg -n 'getStr\("lunarnx/main/sign_out_again"\)' src/ui/main_activity.cpp >/dev/null ||
   ! rg -n '"sign_out_again": "Press B again to sign out"' romfs/i18n/en-US/lunarnx.json >/dev/null ||
   ! rg -n 'signout_pending_' src/ui/main_activity.* >/dev/null; then
  fail "Main UI must require a second B press before signing out"
fi

if rg -n -F 'brls::Application::clear(' src/ui >/dev/null; then
  fail "Switch UI must not call private Borealis Application::clear"
fi

if ! rg -n 'void AuthActivity::onResume' src/ui/auth_activity.cpp >/dev/null ||
   ! rg -n 'resetSignedOutUi' src/ui/auth_activity.* >/dev/null; then
  fail "Auth UI must reset stale signed-in state after sign-out navigation"
fi

if ! rg -n 'requestStreamStop' src/app/stream_controller.* >/dev/null ||
   ! rg -n 'stream_operation_mutex_' src/app/stream_controller.cpp >/dev/null; then
  fail "StreamController stop/sign-out paths must request cancellation before serialized cleanup"
fi

if ! rg -n 'state_callback_mutex_' src/app/stream_controller.* >/dev/null; then
  fail "StreamController state callback access must be synchronized across UI/background threads"
fi

if ! rg -n -F 'std::vector<api::XboxConsole> getConsoles() const' src/app/stream_controller.h >/dev/null; then
  fail "StreamController::getConsoles must return a snapshot, not a mutable cross-thread reference"
fi

if ! rg -n 'last_console_error_' src/app/stream_controller.* >/dev/null ||
   ! rg -n 'getConsoleFetchError' src/app/stream_controller.* >/dev/null; then
  fail "Console fetch failures must expose a UI-readable error and clear stale console rows"
fi

if ! rg -n 'enum class DeviceCodePollResult' src/auth/auth_manager.h >/dev/null ||
   ! rg -n 'pollAuthStatus' src/app/stream_controller.* src/ui/auth_activity.cpp >/dev/null; then
  fail "Device-code polling must distinguish pending/success/error outcomes"
fi

for symbol in authorization_declined expired_token slow_down getPollIntervalSeconds getDeviceCodeExpiresInSeconds; do
  if ! rg -n "$symbol" src/auth src/app src/ui >/dev/null; then
    fail "Device-code UI must handle auth polling detail: $symbol"
  fi
done

if ! rg -n -F 'return hasUsableStreamingTokens()' src/auth/auth_manager.cpp >/dev/null; then
  fail "Auth token derivation must use the same complete-token definition as isAuthenticated"
fi

if ! rg -n 'brls::ScrollingFrame' src/ui/main_activity.cpp >/dev/null; then
  fail "MainActivity must use a scrolling container for console/settings overflow"
fi

if ! rg -n 'poll_thread_done_' src/ui/auth_activity.* >/dev/null; then
  fail "AuthActivity must avoid blocking UI while a device-code poll request is still unwinding"
fi

if rg -n 'if \(!ctrl_->startAuth\(\)\)' src/ui/auth_activity.cpp >/dev/null; then
  fail "AuthActivity must not request the device code on the UI thread"
fi

if ! rg -n 'auth_request_thread_|auth_request_done_|auth_requesting_' src/ui/auth_activity.* >/dev/null; then
  fail "AuthActivity must request the device code on a cancellable background path"
fi

if rg -n 'if \(poll_thread_\.joinable\(\)\) poll_thread_\.join\(\);' src/ui/auth_activity.cpp >/dev/null; then
  fail "AuthActivity must not synchronously join an in-flight auth polling thread on the UI thread"
fi

if rg -n 'if \(connecting_->load\(\)\) ctrl_->stopStream\(false\);' src/ui/main_activity.cpp >/dev/null; then
  fail "MainActivity destructor must not synchronously stop an in-flight connection on the UI thread"
fi

if ! rg -n 'network_error|error_message' src/api/http_client.* >/dev/null; then
  fail "HttpResponse must expose curl/network failure details for UI and diagnostics"
fi

if ! rg -n 'last_error_|getLastError' src/auth/auth_manager.* >/dev/null; then
  fail "AuthManager must expose the last user-readable auth/network error"
fi

if ! rg -n 'last_stream_error_|getLastStreamError' src/app/stream_controller.* >/dev/null; then
  fail "StreamController must expose the last stream failure reason to the UI"
fi

if ! rg -n 'getLastStreamError' src/ui/stream_loading_activity.cpp >/dev/null ||
   ! rg -n 'StreamLaunchResult::Failed' src/ui/stream_loading_activity.cpp src/ui/main_activity.cpp >/dev/null; then
  fail "Stream launch UI must display the specific stream failure reason instead of a generic connection message"
fi

for file in src/api/http_client.cpp src/auth/auth_manager.cpp src/api/xbox_api_client.cpp src/app/stream_controller.cpp src/app/xbox_session_client.cpp src/app/xbox_stream_session.cpp; do
  if ! rg -n 'diagnosticLog' "$file" >/dev/null; then
    fail "Network/session failures must write diagnostic logs: $file"
  fi
done

if ! rg -n 'src/platform/switch_wrapper\.c' Makefile.switch >/dev/null; then
  fail "Switch build must use LunarNX-owned switch_wrapper.c so startup diagnostics are tracked"
fi

if ! [[ -f src/platform/switch_wrapper.c ]] ||
   ! rg -n 'switchEarlyLog' src/platform/switch_wrapper.c >/dev/null ||
   ! rg -n 'userAppInit begin' src/platform/switch_wrapper.c >/dev/null ||
   ! rg -n 'romfsInit' src/platform/switch_wrapper.c >/dev/null ||
   ! rg -n 'nifmInitialize' src/platform/switch_wrapper.c >/dev/null; then
  fail "Switch userAppInit must write pre-main diagnostics so startup crashes produce sdmc logs"
fi

if rg -n 'fsdevMountSdmc' src/platform/switch_wrapper.c >/dev/null; then
  fail "Switch userAppInit must not remount sdmc; libnx mounts it before userAppInit"
fi

printf 'stream regression checks passed\n'
