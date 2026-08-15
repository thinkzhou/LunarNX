#!/usr/bin/env python3
import json
from pathlib import Path


def require(condition, message):
    if not condition:
        raise SystemExit(f"FAIL: {message}")


header = Path("src/input/button_mapping.h").read_text()
mapping = Path("src/input/button_mapping.cpp").read_text()
reader = Path("src/input/gamepad_reader.cpp").read_text()
activity = Path("src/ui/button_mapping_activity.cpp").read_text()
settings = Path("src/ui/stream_settings_activity.cpp").read_text()
ps_settings = Path("src/ui/ps_settings_activity.cpp").read_text()
controller = Path("src/app/stream_controller.cpp").read_text()
ps_controller = Path("src/ps/ps_stream_controller.cpp").read_text()
ps_mapper = Path("src/ps/ps_input_mapper.cpp").read_text()
switch_makefile = Path("Makefile.switch").read_text()
desktop_makefile = Path("Makefile.desktop").read_text()

for target in ("A", "B", "X", "Y", "DpadUp", "DpadDown", "DpadLeft",
               "DpadRight", "Lb", "Rb", "Lt", "Rt", "L3", "R3", "View",
               "Menu", "Guide", "Touchpad"):
    require(target in header, f"missing remappable target: {target}")

require('"xbox_button_mapping"' in mapping and '"ps_button_mapping"' in mapping and
        "loadButtonMapping" in mapping and
        "saveButtonMapping" in mapping,
        "Xbox and PlayStation mappings must persist separately")
require('"button_mapping"' in mapping and
        "profile == ButtonMappingProfile::Xbox" in mapping,
        "the former shared mapping may only migrate to Xbox")
ps_defaults = mapping[mapping.index("ButtonMapping defaultButtonMapping"):mapping.index("const char* remoteButtonConfigKey")]
ps_profile_defaults = ps_defaults[ps_defaults.index(
    "if (profile == ButtonMappingProfile::PlayStation"):]
require("RemoteButton::A)] = HidNpadButton_B" in ps_profile_defaults and
        "RemoteButton::B)] = HidNpadButton_A" in ps_profile_defaults,
        "PlayStation defaults must map Switch B to Cross and Switch A to Circle")
require("reloadButtonMapping();" in reader and
        reader.index("reloadButtonMapping();") < reader.index("padInitializeDefault"),
        "each stream input reader must load the saved mapping")
require("consumed |= mapping" in reader and "auto mapped" in reader,
        "combination mappings must take priority over component mappings")
require("ButtonMappingProfile::Xbox" in settings and
        "ButtonMappingProfile::PlayStation" in ps_settings,
        "each platform settings page must open its own mapping profile")
require("ButtonMappingProfile::Xbox" in controller and
        "ButtonMappingProfile::PlayStation" in ps_controller,
        "each streaming controller must load only its platform mapping")
require("state.touchpad" in ps_mapper and
        "CHIAKI_CONTROLLER_BUTTON_TOUCHPAD" in ps_mapper,
        "PlayStation mapping must support an independent touchpad click target")
require("waiting_for_release_ = true" in activity and
        "peak_buttons_ |= buttons" in activity and
        "kCaptureReleaseFrames" in activity,
        "capture must wait for release and retain the peak button combination")
require("hasConflict" in activity and '"lunarnx/button_mapping/conflict"' in activity,
        "duplicate mappings must be visibly marked")
require("HidNpadButton_Minus | HidNpadButton_Plus" in activity and
        '"lunarnx/button_mapping/reserved_chord"' in activity,
        "the reserved quick-menu chord must not be saved as a mapping")
require("kButtonMappingCapture" in header and
        "isCaptureButtonPressed()" in activity and
        "acquireCaptureButtonInput()" in activity and
        "releaseCaptureButtonInput()" in activity,
        "the mapping screen must capture and release the Switch screenshot button")
require("mappingUsesCaptureButton" in reader and
        "btns |= kButtonMappingCapture" in reader and
        "releaseCaptureButton()" in reader,
        "configured screenshot mappings must feed streams and restore system input")
require("defaultButtonMapping(profile_)" in activity and "reset_all" in activity,
        "the mapping screen must reset all mappings to defaults")
require("src/input/button_mapping.cpp" in switch_makefile and
        "src/ui/button_mapping_activity.cpp" in switch_makefile,
        "Switch build must include mapping input and UI sources")
require("button_mapping.cpp" not in desktop_makefile and
        "button_mapping_activity.cpp" not in desktop_makefile,
        "Switch HID mapping must not be added to the desktop build")

for locale in ("en-US", "zh-Hans", "zh-Hant"):
    data = json.loads(Path(f"romfs/i18n/{locale}/lunarnx.json").read_text())
    require("button_mapping" in data, f"missing mapping translations: {locale}")
    require(len(data["button_mapping"]) >= 25,
            f"incomplete mapping translations: {locale}")

print("button mapping tests passed")
