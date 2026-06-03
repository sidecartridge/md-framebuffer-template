#!/usr/bin/env python3
"""
Convert a WAV file to a C header containing pre-rectified 4-bit YM2149
channel-volume values, ready to drop into the cart audio buffer with no
runtime conversion on the RP side.

Pipeline:
  - Decode WAV (any sample width, mono or multi-channel).
  - Resample to --target-rate via linear interpolation.
  - Take absolute value of each sample (rectification): this turns the
    signed PCM into unsigned magnitude, using the YM volume register's
    full 0..15 range (PCM silence -> YM 0 = silent, PCM peak -> YM 15
    = max). Voice gets a 2x harmonic content from rectification but
    the silences are real silences and the dynamic range is unimpaired
    by the YM's logarithmic curve baked into linear-biased mapping.
  - Quantize the rectified magnitude to 4 bits (0..15) and write one
    byte per sample (low nibble = YM volume, high nibble = 0).

Example:
  python3 tools/wav_to_ym4.py path/to/sample.wav \
    --target-rate 7100 \
    --header-output rp/src/include/audio_sample.h
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import wave


def _decode_sample_to_float(data: bytes, sample_width: int) -> float:
    if sample_width == 1:
        return (data[0] - 128) / 128.0
    if sample_width == 2:
        value = int.from_bytes(data, byteorder="little", signed=True)
        return value / 32768.0
    if sample_width == 3:
        value = int.from_bytes(data, byteorder="little", signed=False)
        if value & 0x800000:
            value -= 1 << 24
        return value / 8388608.0
    if sample_width == 4:
        value = int.from_bytes(data, byteorder="little", signed=True)
        return value / 2147483648.0
    raise ValueError(f"Unsupported sample width: {sample_width} bytes")


def _read_sam_as_mono_float(
    sam_path: pathlib.Path, src_rate: int
) -> tuple[list[float], int]:
    """Atari ST sampler output: raw unsigned 8-bit PCM, silence = 0x80."""
    data = sam_path.read_bytes()
    mono = [(b - 128) / 128.0 for b in data]
    return mono, src_rate


def _read_wav_as_mono_float(wav_path: pathlib.Path) -> tuple[list[float], int]:
    with wave.open(str(wav_path), "rb") as wav_file:
        if wav_file.getcomptype() != "NONE":
            raise ValueError(
                f"Compressed WAV is not supported: {wav_file.getcomptype()}"
            )
        channels = wav_file.getnchannels()
        sample_width = wav_file.getsampwidth()
        sample_rate = wav_file.getframerate()
        frame_count = wav_file.getnframes()
        raw_frames = wav_file.readframes(frame_count)

    frame_size = channels * sample_width
    if frame_size <= 0 or (len(raw_frames) % frame_size) != 0:
        raise ValueError("Invalid WAV: inconsistent frame data size")

    mono: list[float] = []
    offset = 0
    total_frames = len(raw_frames) // frame_size
    for _ in range(total_frames):
        mixed = 0.0
        for ch in range(channels):
            start = offset + (ch * sample_width)
            end = start + sample_width
            mixed += _decode_sample_to_float(raw_frames[start:end], sample_width)
        mono.append(mixed / channels)
        offset += frame_size
    return mono, sample_rate


def _resample_linear(samples: list[float], src_rate: int, dst_rate: int) -> list[float]:
    if not samples or src_rate == dst_rate:
        return list(samples)

    out_len = max(1, int(round(len(samples) * (dst_rate / src_rate))))
    out: list[float] = []
    step = src_rate / dst_rate
    for i in range(out_len):
        pos = i * step
        left = int(pos)
        if left >= len(samples) - 1:
            out.append(samples[-1])
            continue
        frac = pos - left
        right = left + 1
        out.append(samples[left] + (samples[right] - samples[left]) * frac)
    return out


# Hand-crafted 64-entry (A_vol, B_vol) LUT lifted from the
# atarist-ghostbusters-demo (1988), label SAMPLE1 in GHOST.S. Each
# DC.L entry is $00BB00AA -> (A=lower-word, B=upper-word). Indexed
# by the upper 6 bits of an unsigned 8-bit PCM sample (sample >> 2).
# The pairs were tuned by hand for the YM's logarithmic volume curve;
# max sum is (A=10, B=12)/(A=9, B=13) ~= 0.604 -- deliberately less
# than the theoretical 2.0 max, trading peak loudness for finer
# quantization in the voice-amplitude band.
GHOSTBUSTERS_LUT = [
    (0,  0), (0,  2), (1,  2), (2,  2), (2,  3), (1,  4), (2,  4), (2,  5),
    (0,  6), (2,  6), (3,  6), (4,  6), (2,  7), (4,  7), (5,  7), (2,  8),
    (3,  8), (4,  8), (5,  8), (2,  9), (3,  9), (4,  9), (5,  9), (6,  9),
    (7,  9), (3, 10), (4, 10), (5, 10), (6, 10), (7, 10), (0, 11), (1, 11),
    (2, 11), (4, 11), (5, 11), (6, 11), (7, 11), (8, 11), (8, 11), (9, 11),
    (9, 11), (0, 12), (1, 12), (2, 12), (3, 12), (4, 12), (5, 12), (6, 12),
    (8, 12), (8, 12), (9, 12), (9, 12), (9, 12), (10, 12), (0, 13), (2, 13),
    (3, 13), (4, 13), (5, 13), (6, 13), (7, 13), (8, 13), (8, 13), (9, 13),
]
assert len(GHOSTBUSTERS_LUT) == 64


def _float_to_ym_ghostbusters(
    samples: list[float], pcm_gain: float = 1.0
) -> list[int]:
    """Ghostbusters demo's hand-crafted (A, B) lookup. PCM sample byte
    (0..255) is right-shifted by 2 to index a 64-entry table of
    (ch_A_vol, ch_B_vol) pairs hand-tuned for the YM's logarithmic
    curve.

    pcm_gain (>= 1.0) pre-amplifies the PCM signal so quiet content
    lands in higher LUT entries; peaks saturate at +-1.0 (compressor-
    style soft clip). Silence (PCM 0) still maps to the LUT midpoint
    -- no DC hum added."""
    out: list[int] = []
    for s in samples:
        boosted = s * pcm_gain
        if boosted < -1.0:
            boosted = -1.0
        elif boosted > 1.0:
            boosted = 1.0
        byte = int((boosted + 1.0) * 128.0)
        if byte < 0:
            byte = 0
        elif byte > 255:
            byte = 255
        va, vb = GHOSTBUSTERS_LUT[byte >> 2]
        out.extend(_pack_ym_bytes(va, vb))
    return out


def _float_to_ym_dual_ghost(
    samples: list[float], pcm_gain: float = 1.0
) -> list[int]:
    """Ghostbusters dual-channel mapping, 2 bytes per sample.

    Same 64-entry hand-crafted (vA, vB) LUT as the 4-byte
    `ghostbusters` mode, but the output layout drops the embedded
    YM register-select bytes ($08, $09) so each sample is just
    [vA, vB] -- ready for a m68k handler that does explicit
    YM_SELECT writes between the two volume writes instead of using
    MOVEP.L against the PSG mirror at $8804/$8806.

    Two-byte layout works on every ST/STE/MegaSTE regardless of
    address-decoding quirks (the $8804/$8806 mirror is incomplete-
    decoding behaviour that not all clones expose)."""
    out: list[int] = []
    for s in samples:
        boosted = s * pcm_gain
        if boosted < -1.0:
            boosted = -1.0
        elif boosted > 1.0:
            boosted = 1.0
        byte = int((boosted + 1.0) * 128.0)
        if byte < 0:
            byte = 0
        elif byte > 255:
            byte = 255
        va, vb = GHOSTBUSTERS_LUT[byte >> 2]
        out.append(va)
        out.append(vb)
    return out


def _float_to_ym_raw_byte(
    samples: list[float], pcm_gain: float = 1.0
) -> list[int]:
    """Pass-through mode: 1 byte per sample = the unsigned 8-bit PCM
    value (silence = 0x80). No LUT, no nibble split, no logarithmic
    remap -- the file's bytes are embedded byte-for-byte. The m68k
    Timer-B handler is expected to extract whatever nibble(s) it
    wants at runtime. Pair with --source-rate == --target-rate to
    skip resampling and keep the .SAM file truly verbatim."""
    out: list[int] = []
    for s in samples:
        boosted = s * pcm_gain
        if boosted < -1.0:
            boosted = -1.0
        elif boosted > 1.0:
            boosted = 1.0
        byte = int((boosted + 1.0) * 128.0)
        if byte < 0:
            byte = 0
        elif byte > 255:
            byte = 255
        out.append(byte)
    return out


def _pack_ym_bytes(va: int, vb: int) -> list[int]:
    """Pack one sample as 4 bytes for the m68k MOVEP.L emit. The cart
    bus byte-swaps within each 16-bit word, so to make m68k's
    `move.l (xxx).L, Dn` end up with d0 = $08_VA_09_VB (which MOVEP.L
    then writes as bytes $08, VA, $09, VB to the YM ports), the bytes
    in RP storage must be pre-swapped per word: [VA, $08, VB, $09]."""
    return [va & 0x0F, 0x08, vb & 0x0F, 0x09]


def _float_to_ym_single_a(
    samples: list[float], pcm_gain: float = 1.0
) -> list[int]:
    """Single-channel A DAC, byte-per-sample buffer layout, with
    YM-amplitude inverse mapping (measured logarithmic LUT). For
    each signed PCM sample [-1, +1]: bias to [0, 1] then pick the
    YM volume nibble v whose acoustic amplitude is closest to the
    target.

    The amplitude table comes from Ayumi
    (https://github.com/true-grue/ayumi), which uses values derived
    from real-chip measurements rather than the 2^((v-15)/2)
    textbook approximation; same table MAME's ay8910 driver and
    Hatari use (within rounding). Volume mode is 4-bit (16 levels);
    Ayumi stores 32 entries for envelope-mode 5-bit precision so we
    pick every other entry starting at index 1 to recover the
    volume-mode values."""
    ym_amp = [
        0.0,                # v=0  (silence)
        0.00772106507973,   # v=1
        0.0139620050355,    # v=2
        0.0200198367285,    # v=3
        0.029694056611,     # v=4
        0.0403906767737,    # v=5
        0.0583352407111,    # v=6
        0.0777652789246,    # v=7
        0.111085679408,     # v=8
        0.148340921749,     # v=9
        0.211551079576,     # v=10
        0.281101701381,     # v=11
        0.400427252613,     # v=12
        0.53443198291,      # v=13
        0.75800717174,      # v=14
        1.0,                # v=15 (full)
    ]
    out: list[int] = []
    for s in samples:
        boosted = s * pcm_gain
        if boosted < -1.0:
            boosted = -1.0
        elif boosted > 1.0:
            boosted = 1.0
        target = (boosted + 1.0) * 0.5
        best_v = 0
        best_diff = abs(ym_amp[0] - target)
        for v in range(1, 16):
            d = abs(ym_amp[v] - target)
            if d < best_diff:
                best_diff = d
                best_v = v
        out.append(best_v)
    return out


def _float_to_ym_nibble(samples: list[float]) -> list[int]:
    """Simplest 2-channel split: convert signed PCM [-1, 1] to
    unsigned 8-bit (0..255 with 128 = silence), then map the high
    nibble to channel A and the low nibble to channel B. Naive but
    canonical -- per AY/YM datasheet wisdom on the Atari ST."""
    out: list[int] = []
    for s in samples:
        # Signed [-1, 1] -> unsigned 8-bit [0, 255].
        byte = int((s + 1.0) * 128.0)
        if byte < 0:
            byte = 0
        elif byte > 255:
            byte = 255
        va = (byte >> 4) & 0x0F   # high nibble -> ch A
        vb = byte & 0x0F          # low nibble  -> ch B
        out.extend(_pack_ym_bytes(va, vb))
    return out


def _float_to_ym_pair(
    samples: list[float], lut_scale: float = 1.0, pcm_gain: float = 1.0
) -> list[int]:
    """Search-based 2-channel pseudo-DAC mapping. For each signed PCM
    sample [-1, 1], pick the (A_vol, B_vol) pair whose summed YM
    amplitudes best match the linear-biased target.

    YM amp per vol: 2^((v-15)/2) for v >= 1, 0 for v = 0. Two
    channels sum acoustically; achievable amplitude range is
    [0, 2 * amp(15)] = [0, 2.0]. We map PCM bias [0, 1] -> target
    sum [0, 2 * lut_scale] and choose the closest (A, B) pair from
    all 256."""
    ym_amp = [0.0] + [2.0 ** ((v - 15) / 2.0) for v in range(1, 16)]
    pairs = [(a, b, ym_amp[a] + ym_amp[b]) for a in range(16) for b in range(16)]

    max_sum = 2.0 * lut_scale
    out: list[int] = []
    for s in samples:
        boosted = s * pcm_gain
        if boosted < -1.0:
            boosted = -1.0
        elif boosted > 1.0:
            boosted = 1.0
        target = (boosted + 1.0) * lut_scale
        if target < 0.0:
            target = 0.0
        elif target > max_sum:
            target = max_sum
        best_a, best_b = 0, 0
        best_diff = abs(0.0 - target)
        for a, b, total in pairs:
            diff = abs(total - target)
            if diff < best_diff:
                best_diff = diff
                best_a = a
                best_b = b
        out.extend(_pack_ym_bytes(best_a, best_b))
    return out


def _header_guard_from_path(path: pathlib.Path) -> str:
    guard = path.name.upper()
    for char in ".-/\\ ":
        guard = guard.replace(char, "_")
    return f"{guard}_"


# YMS mode tags: must stay in sync with AUDIO_YMS_MODE_* in
# rp/src/audio.c. The RP-side audio_play_yms_file() currently only
# accepts tag 1 (dual-ghost); other tags are reserved for future
# m68k handler variants.
YMS_MODE_TAGS: dict[str, int] = {
    "dual-ghost": 1,
    "single-a":   2,
    "raw-byte":   3,
    "ghostbusters": 4,
    "best-pair":  5,
    "nibble":     6,
}


def _write_yms_file(
    samples: list[int],
    out_path: pathlib.Path,
    target_rate: int,
    mode: str,
) -> None:
    """Write a binary .YMS file (16-byte header + raw byte body) for
    SD streaming via audio_play_yms_file() on the RP side. Format:
        off  0:  'Y' 'M' 'S' '1'        magic
        off  4:  uint32 rate_hz         LE
        off  8:  uint32 data_len_bytes  LE
        off 12:  uint8  mode_tag        (1 = dual-ghost)
        off 13:  uint8[3]              reserved (must be 0)
        off 16:  raw byte body
    """
    mode_tag = YMS_MODE_TAGS.get(mode, 0)
    data = bytes(samples)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    with out_path.open("wb") as out:
        out.write(b"YMS1")
        out.write(int(target_rate).to_bytes(4, "little"))
        out.write(len(data).to_bytes(4, "little"))
        out.write(bytes([mode_tag, 0, 0, 0]))
        out.write(data)


def _write_c_header(
    samples: list[int],
    out_path: pathlib.Path,
    symbol: str,
    count_symbol: str,
    target_rate: int,
) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    guard = _header_guard_from_path(out_path)
    with out_path.open("w", encoding="utf-8", newline="\n") as out:
        out.write("/* Auto-generated by tools/wav_to_ym4.py */\n")
        out.write(f"#ifndef {guard}\n")
        out.write(f"#define {guard}\n\n")
        out.write("#include <stdint.h>\n\n")
        out.write(f"#define AUDIO_SAMPLE_RATE_HZ {target_rate}u\n\n")
        out.write(
            "/* Two bytes per sample: byte 2n = channel A volume, byte 2n+1\n"
            " * = channel B volume. The (A, B) pairs are precomputed so the\n"
            " * summed YM acoustic amplitudes best fit the linear-biased\n"
            " * PCM value (Ghostbusters-style 2-channel pseudo-DAC). */\n"
        )
        out.write(f"static const uint8_t {symbol}[] = {{\n")
        per_line = 16
        for start in range(0, len(samples), per_line):
            row = samples[start : start + per_line]
            out.write("    " + ", ".join(f"0x{v:X}" for v in row) + ",\n")
        out.write("};\n\n")
        out.write(
            f"static const uint32_t {count_symbol} = "
            f"(uint32_t)(sizeof({symbol}) / sizeof({symbol}[0]));\n\n"
        )
        out.write(f"#endif  /* {guard} */\n")


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Convert WAV (or raw Atari .SAM) to a C header of "
                    "MOVEP.L-packed 4-bit YM volume pairs."
    )
    p.add_argument("input_path", type=pathlib.Path,
                   help="Input WAV (.wav) or raw 8-bit unsigned PCM (.sam)")
    p.add_argument("--source-rate", type=int, default=12500,
                   help="Source sample rate for .SAM input (ignored for WAV); "
                        "Atari ST samplers typically used 6250/12500/18500/25000 Hz")
    p.add_argument("--target-rate", type=int, default=15350)
    p.add_argument("--mode",
                   choices=["nibble", "best-pair", "ghostbusters",
                            "dual-ghost", "single-a", "raw-byte"],
                   default="ghostbusters",
                   help="PCM-to-YM mapping. 'nibble': split 8-bit sample "
                        "into high/low nibbles -> (chA, chB). 'best-pair': "
                        "search all 256 (A,B) pairs for closest acoustic "
                        "sum. 'ghostbusters': 64-entry LUT lifted from the "
                        "1988 ghostbusters demo, 4-byte MOVEP.L layout. "
                        "'dual-ghost': same LUT as 'ghostbusters' but 2 "
                        "bytes/sample (vA, vB) for non-MOVEP handlers. "
                        "'single-a': single-channel 4-bit DAC on ch A only "
                        "(ch B always 0; pair with mixer R7=$FE). "
                        "'raw-byte': 1 byte/sample = unsigned 8-bit PCM "
                        "verbatim (no LUT, no nibble split) -- m68k "
                        "extracts nibbles at runtime.")
    p.add_argument("--lut-scale", type=float, default=1.0,
                   help="Range compression for the (A,B) pair search: "
                        "1.0 = full YM sum range [0, 2.0] (default); "
                        "0.3125 = Ghostbusters-style compression to "
                        "[0, 0.625] for finer voice-amplitude resolution")
    p.add_argument("--pcm-gain", type=float, default=1.0,
                   help="Pre-amplify PCM input (peaks clip to +-1.0). "
                        "1.0 = no boost (default); 2.0 = +6 dB (typical "
                        "voice content boosted to fill the lut-scale "
                        "range; peaks saturate with mild distortion)")
    p.add_argument("--header-output", type=pathlib.Path, default=None,
                   help="Write a C header (uint8_t array + sample-count) to "
                        "PATH. Embed in firmware via #include. Default: skip.")
    p.add_argument("--yms-output", type=pathlib.Path, default=None,
                   help="Write a binary .YMS file (16-byte header + raw "
                        "byte body) to PATH, suitable for streaming from SD "
                        "via audio_play_yms_file() on the RP side. Default: "
                        "skip. At least one of --header-output / --yms-output "
                        "must be supplied.")
    p.add_argument("--symbol", type=str, default="audio_sample_data")
    p.add_argument("--count-symbol", type=str, default="audio_sample_count")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    if not args.input_path.exists():
        print(f"error: input not found: {args.input_path}", file=sys.stderr)
        return 1
    if args.target_rate <= 0:
        print("error: --target-rate must be > 0", file=sys.stderr)
        return 1
    if not args.header_output and not args.yms_output:
        print("error: at least one of --header-output / --yms-output is required",
              file=sys.stderr)
        return 1

    suffix = args.input_path.suffix.lower()
    if suffix == ".sam":
        mono, src_rate = _read_sam_as_mono_float(args.input_path, args.source_rate)
    else:
        mono, src_rate = _read_wav_as_mono_float(args.input_path)
    resampled = _resample_linear(mono, src_rate, args.target_rate)
    if args.mode == "nibble":
        ym4 = _float_to_ym_nibble(resampled)
    elif args.mode == "ghostbusters":
        ym4 = _float_to_ym_ghostbusters(resampled, args.pcm_gain)
    elif args.mode == "dual-ghost":
        ym4 = _float_to_ym_dual_ghost(resampled, args.pcm_gain)
    elif args.mode == "single-a":
        ym4 = _float_to_ym_single_a(resampled, args.pcm_gain)
    elif args.mode == "raw-byte":
        ym4 = _float_to_ym_raw_byte(resampled, args.pcm_gain)
    else:
        ym4 = _float_to_ym_pair(resampled, args.lut_scale, args.pcm_gain)

    if args.header_output:
        _write_c_header(
            ym4, args.header_output, args.symbol, args.count_symbol, args.target_rate
        )
    if args.yms_output:
        _write_yms_file(ym4, args.yms_output, args.target_rate, args.mode)

    # bytes per sample:
    #   single-a, raw-byte -> 1 byte
    #   dual-ghost         -> 2 bytes (vA, vB)
    #   other              -> 4 bytes (MOVEP.L layout)
    if args.mode in ("single-a", "raw-byte"):
        bytes_per_sample = 1
    elif args.mode == "dual-ghost":
        bytes_per_sample = 2
    else:
        bytes_per_sample = 4
    sample_count = len(ym4) // bytes_per_sample
    duration_ms = (sample_count * 1000.0) / args.target_rate if args.target_rate else 0.0
    print(f"Input:        {args.input_path}")
    print(f"Source rate:  {src_rate} Hz")
    print(f"Target rate:  {args.target_rate} Hz")
    print(f"Mode:         {args.mode}")
    if args.mode in ("best-pair", "ghostbusters", "dual-ghost", "single-a"):
        if args.mode == "best-pair":
            print(f"LUT scale:    {args.lut_scale:.4f}  (max YM sum amp = {2.0 * args.lut_scale:.3f})")
        import math
        gain_db = 20.0 * math.log10(max(args.pcm_gain, 1e-9))
        print(f"PCM gain:     {args.pcm_gain:.4f}  ({gain_db:+.1f} dB peak boost; peaks clip)")
    print(f"Samples:      {sample_count} ({len(ym4)} bytes @ {bytes_per_sample} byte/sample, {duration_ms:.1f} ms)")
    if args.header_output:
        print(f"Header:       {args.header_output}")
    if args.yms_output:
        mode_tag = YMS_MODE_TAGS.get(args.mode, 0)
        print(f"YMS file:     {args.yms_output} (mode tag {mode_tag}, "
              f"{16 + len(ym4)} bytes total)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
