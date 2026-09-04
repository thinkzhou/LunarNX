#!/usr/bin/env python3

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    patch = (ROOT / "tools/chiaki_switch/lunarnx-chiaki-route-preference.patch").read_text()
    connector = (ROOT / "src/ps/ps_remote_connector.cpp").read_text()
    session = (ROOT / "src/ps/ps_stream_session.cpp").read_text()
    controller = (ROOT / "src/ps/ps_stream_controller.cpp").read_text()

    require("preferred_index" in patch and
            "switch_stun_servers[i].host" in patch and
            "preferred_stun_host" in patch,
            "remembered STUN must only reorder the current Switch server list")
    require("memcpy(candidates, candidates_received" in patch and
            "preferred_remote_address" in patch and
            "candidates[0] = preferred" in patch,
            "remembered PS routes must only reorder candidates from the fresh offer")
    require("selected_remote_port = selected_candidate.port" in patch and
            "successful_stun_host" in patch,
            "Chiaki must expose the route that actually succeeded")
    require("PsRoutePreferenceStore().load" in connector and
            "set_preferred_stun_server" in connector and
            "set_preferred_remote_candidate" in connector,
            "remote startup must apply persisted soft preferences")
    require("CHIAKI_EVENT_CONNECTED" in session and
            "get_selected_remote_candidate" in session and
            "get_successful_stun_server" in session,
            "route success must be captured only after the complete stream connects")
    require("PsRoutePreferenceStore().save" in controller and
            "callbacks.on_streaming" in controller,
            "a complete remote connection must persist its successful route")

    with tempfile.TemporaryDirectory(prefix="lunarnx-ps-route-") as temp:
        temp_path = Path(temp)
        binary = temp_path / "ps_route_preferences_test"
        cjson_object = temp_path / "cJSON.o"
        subprocess.run([
            "clang", "-std=c11", "-Wno-deprecated-declarations",
            f"-I{ROOT / 'lib'}", "-c", str(ROOT / "lib/cJSON.c"),
            "-o", str(cjson_object),
        ], check=True)
        subprocess.run([
            "clang++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-Wno-deprecated-declarations", "-isystem",
            "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1",
            f"-I{ROOT / 'src'}", f"-I{ROOT / 'lib'}",
            str(ROOT / "tests/ps_route_preferences_test.cpp"),
            str(ROOT / "src/ps/ps_route_preferences.cpp"),
            str(cjson_object), "-o", str(binary),
        ], check=True)
        subprocess.run([str(binary), str(temp_path / "routes.json")], check=True)

    print("PS route preference tests passed")


if __name__ == "__main__":
    main()
