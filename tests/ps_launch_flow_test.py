#!/usr/bin/env python3
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main():
    ui = Path("src/ui/ps_activity.cpp").read_text()
    controller = Path("src/ps/ps_stream_controller.h").read_text()
    session = Path("src/ps/ps_stream_session.cpp").read_text()

    require("PsConsoleSource::Local" in ui and
            "PsConnectionPreference::LocalOnly" in ui,
            "local tab must force a local route")
    require("PsConsoleSource::Remote" in ui and
            "PsConnectionPreference::RemoteOnly" in ui,
            "remote tab must force a PSN route")
    require("LaunchCallback" in controller and "setLaunchCallback" in controller,
            "PS controller must expose launch progress")
    controller_source = Path("src/ps/ps_stream_controller.cpp").read_text()
    media_bridge = Path("src/ps/ps_media_bridge.cpp").read_text()
    media_pipeline = Path("src/stream/media_pipeline.cpp").read_text()
    require("setVideoReadyCallback" in controller_source and
            "Waiting for video" in controller_source and
            "video_renderer_->render(frame)" in media_pipeline and
            "video_ready_notified_.exchange(true)" in media_pipeline,
            "PS loading must wait for the first rendered video frame")
    require("std::shared_mutex stream_operation_mutex_" in controller and
            "std::unique_lock<std::shared_mutex> operation_lock(stream_operation_mutex_)" in
                controller_source and
            "std::shared_lock<std::shared_mutex> operation_lock(stream_operation_mutex_)" in
                controller_source,
            "PS lifecycle mutations must exclude concurrent input/presentation without "
            "serializing their hot paths")
    require("chiaki_opus_decoder_get_sink" in media_bridge and
            "opus_sink_.frame_cb(buf, buf_size" in media_bridge,
            "PS encoded audio must pass through Chiaki's Opus decoder")
    require("frame.pcm_data.assign(bytes" in media_bridge and
            "media_.playDecodedAudio(frame)" in media_bridge,
            "PS decoded PCM must enter the asynchronous media pipeline")
    require("requestLoginPin" in ui and "submitLoginPin" in ui,
            "loading UI must collect the console login PIN")
    cancel_body = ui.split("void cancel()", 1)[1].split(
        "void scheduleCancelCleanup()", 1)[0]
    require("requestCancel()" in cancel_body and
            "stopStream(false)" not in cancel_body,
            "loading UI cancellation must not destroy an active connector")
    require("PsConnectWorkerFinalizer" in ui and
            "context->connect_worker_done = true" in ui and
            "runPsConnectCleanup" in ui,
            "loading UI must finalize cancellation even when connection code throws")
    worker_failure = ui.split("if (!worker", 1)[1].split("});", 1)[0]
    require("connect_worker_done" in worker_failure,
            "a failed connect-worker launch must still enable loading-page cancellation")
    require("connect_worker_started" in ui and
            "if (context_->cancel_requested.load())" in ui,
            "a failed cleanup-worker launch must leave B cancellation retryable")
    connect_worker = ui.split('startNetworkWorker("ps-connect"', 1)[1].split(
        '}, 8 * 1024 * 1024)', 1)[0]
    require("[context, controller, manager, remote, status]" in connect_worker and
            "[this" not in connect_worker,
            "detached PS connect worker must not retain the loading activity")
    require("ensureValidToken(" in connect_worker and
            "context->cancel_requested.load()" in connect_worker and
            "token_refreshed &&" in connect_worker and
            "manager->psnAuth().saveToken(" in connect_worker and
            "controller->setPsnCredentials(" in connect_worker and
            "controller->startStream()" in connect_worker,
            "remote loading must validate and persist PSN credentials before streaming")
    require("setPsnRefreshCallback" in connect_worker and
            "psnAuth().refreshToken(" in connect_worker,
            "remote loading must provide a one-shot refresh for server-rejected tokens")
    require("PsnAuthErrorKind::SessionExpired" in connect_worker and
            "manager->psnAuth().signOut()" in connect_worker and
            "new PsnLoginActivity(manager)" in connect_worker,
            "rejected refresh credentials must be cleared and return to PSN login")
    require("setLaunchCallback({})" in ui and "setLoginPinCallback({})" in ui,
            "PS loading callbacks must be cleared before stop or destruction")
    connect_destructor = ui.split("~PsConnectActivity()", 1)[1].split(
        "brls::View* createContentView", 1)[0]
    require("setPsnRefreshCallback({})" not in connect_destructor,
            "PSN refresh ownership must survive the loading activity for foreground rebuilds")
    require("void requestCancel()" in controller and
            "PsStreamController::requestCancel()" in controller_source,
            "PS controller must expose non-destructive connection cancellation")
    require("on_login_pin_requested" in session and "setLoginPin" in session,
            "console login PIN requests must remain interactive")
    connect_body = ui.split("void PsActivity::connectToConsole", 1)[1].split(
        "void PsActivity::showRemotePlayHelp", 1)[0]
    require("new PsConnectActivity" in connect_body and
            "new StreamView(controller)" not in connect_body,
            "PS page must route startup through a loading activity")
    require("loadPsSettings()" in connect_body and
            "settings.width, settings.height, 60, settings.bitrate_kbps" in connect_body and
            "1280, 720, 60, 10000" not in connect_body,
            "PS launch must use the shared saved resolution and bitrate settings")
    require("hasStoredPsnSession()" in connect_body and
            "hasPsnToken()" not in connect_body,
            "an expired but refreshable PSN session must reach the loading-page preflight")

    print("PS launch flow tests passed")


if __name__ == "__main__":
    main()
