#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    decoder = (ROOT / "src/stream/audio_decoder.cpp").read_text()
    switch_makefile = (ROOT / "Makefile.switch").read_text()

    assert "#include <opus/opus_multistream.h>" in decoder
    assert "opus_multistream_decoder_create" in decoder
    assert "opus_multistream_decode" in decoder
    assert "avcodec_send_packet" not in decoder
    assert "AV_CODEC_ID_OPUS" not in decoder
    assert "-lopus" in switch_makefile

    print("Audio decoder config tests passed")


if __name__ == "__main__":
    main()
