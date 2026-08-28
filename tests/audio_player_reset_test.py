#!/usr/bin/env python3
"""Guard the Switch Audren reset path against destructive voice flushes."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/stream/audio_player.cpp").read_text()


def function_body(name: str) -> str:
    marker = f"void AudioPlayer::{name}()"
    start = SOURCE.index(marker)
    brace = SOURCE.index("{", start)
    depth = 0
    for index in range(brace, len(SOURCE)):
        if SOURCE[index] == "{":
            depth += 1
        elif SOURCE[index] == "}":
            depth -= 1
            if depth == 0:
                return SOURCE[brace:index + 1]
    raise AssertionError(f"unterminated {name}()")


def main() -> None:
    flush = function_body("flush")
    assert "audrvVoiceStop" in flush
    assert "audrvVoiceDrop" not in flush
    assert "wavebuf = {}" not in flush
    assert "wavebuf.state = AudioDriverWaveBufState_Free" in flush
    assert "wavebuf.next = nullptr" in flush

    append_start = SOURCE.index("size_t AudioPlayer::appendAudio")
    append_end = SOURCE.index("bool AudioPlayer::writeAudio", append_start)
    append = SOURCE[append_start:append_end]
    assert "if (!audrvVoiceAddWaveBuf" in append
    assert "wave-buffer enqueue failed" in append

    free_start = SOURCE.index("int AudioPlayer::freeWavebufIndex() const")
    free_end = SOURCE.index("uint32_t AudioPlayer::queuedWavebufCount() const",
                            free_start)
    free_wavebuf = SOURCE[free_start:free_end]
    assert "requested_latency_mode_" in free_wavebuf
    assert "std::min(active_buffer_count_," in free_wavebuf

    write_start = SOURCE.index("bool AudioPlayer::writeAudio")
    write_end = SOURCE.index("void AudioPlayer::setVolume", write_start)
    write = SOURCE[write_start:write_end]
    assert ("audioBufferConfig(requested_latency_mode_)"
            ".max_writer_wait_frames" in write)

    print("audio player reset regression checks passed")


if __name__ == "__main__":
    main()
