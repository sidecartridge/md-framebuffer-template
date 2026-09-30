"""tools/wav_to_ym4.py, the sample converter behind the .YMS files the RP
streams from the SD card and the built-in jingle: its 64-entry table, the
length it resamples to, and a .YMS file the RP's reader (audio.c) accepts."""

import importlib.util
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

from test_layout import REPO, RP_SRC, c_defines

TOOL = os.path.join(REPO, "tools", "wav_to_ym4.py")
_spec = importlib.util.spec_from_file_location("wav_to_ym4", TOOL)
ym = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(ym)


def audio_defines():
    return c_defines(os.path.join(RP_SRC, "audio.c"))


class Table(unittest.TestCase):

    def test_sixty_four_volume_pairs(self):
        self.assertEqual(len(ym.GHOSTBUSTERS_LUT), 64)
        for va, vb in ym.GHOSTBUSTERS_LUT:
            self.assertTrue(0 <= va <= 15 and 0 <= vb <= 15, (va, vb))

    def test_every_pcm_byte_picks_its_entry(self):
        """PCM byte b (0..255) plays table entry b >> 2, as (vA, vB)."""
        for byte in range(256):
            sample = (byte + 0.5) / 128.0 - 1.0
            self.assertEqual(ym._float_to_ym_dual_ghost([sample]),
                             list(ym.GHOSTBUSTERS_LUT[byte >> 2]), byte)

    def test_silence_and_clipping(self):
        mid = list(ym.GHOSTBUSTERS_LUT[32])
        self.assertEqual(ym._float_to_ym_dual_ghost([0.0]), mid)
        self.assertEqual(ym._float_to_ym_dual_ghost([5.0]), list(ym.GHOSTBUSTERS_LUT[63]))
        self.assertEqual(ym._float_to_ym_dual_ghost([-5.0]), list(ym.GHOSTBUSTERS_LUT[0]))


class Pcm(unittest.TestCase):
    """--mode pcm: signed 8-bit samples for audio_play_pcm_loop()."""

    def test_values(self):
        self.assertEqual(ym._float_to_pcm8([0.0, 1.0, -1.0, 0.5, 9.0, -9.0]),
                         [0, 127, -127, 64, 127, -127])

    def test_header(self):
        with tempfile.TemporaryDirectory() as tmp:
            wav = os.path.join(tmp, "in.wav")
            with wave.open(wav, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(1)
                w.setframerate(12517)
                w.writeframes(bytes([128, 255, 1, 192]))
            out = os.path.join(tmp, "pcm.h")
            subprocess.run([sys.executable, TOOL, wav, "--mode", "pcm", "--target-rate",
                            "12517", "--header-output", out, "--symbol", "tone"],
                           check=True, capture_output=True)
            text = open(out).read()
            self.assertIn("static const int8_t tone[] = {", text)
            self.assertIn("#define AUDIO_SAMPLE_RATE_HZ 12517u", text)
            body = text[text.index("{") + 1:text.index("};")]
            self.assertEqual([int(v) for v in body.replace(",", " ").split()], [0, 126, -126, 64])
            refused = subprocess.run([sys.executable, TOOL, wav, "--mode", "pcm",
                                      "--yms-output", os.path.join(tmp, "x.yms")],
                                     capture_output=True)
            self.assertNotEqual(refused.returncode, 0)


class RpTables(unittest.TestCase):
    """audio.c converts at runtime with copies of the tool's tables."""

    def _c_array(self, name):
        text = open(os.path.join(RP_SRC, "audio.c")).read()
        body = text[text.index(name):]
        body = body[body.index("{") + 1:body.index("};")]
        return [int(n) for n in re.findall(r"-?\d+", body)]

    def test_pairs(self):
        flat = self._c_array("k_ghost_pairs[64][2]")
        self.assertEqual(list(zip(flat[0::2], flat[1::2])), [tuple(p) for p in ym.GHOSTBUSTERS_LUT])

    def test_ym_curve(self):
        amps = self._c_array("k_ym_amp[16]")
        want = [0] + [round(10000 * 2.0 ** ((v - 15) / 2.0)) for v in range(1, 16)]
        for got, w in zip(amps, want):
            self.assertLessEqual(abs(got - w), 1)
        self.assertEqual(len(amps), 16)


class Resample(unittest.TestCase):

    def test_length_follows_the_rate(self):
        for n, src, dst in ((1000, 11025, 5585), (4410, 44100, 5585),
                            (777, 8000, 16000), (1, 22050, 5585)):
            out = ym._resample_linear([0.25] * n, src, dst)
            self.assertEqual(len(out), max(1, round(n * dst / src)), (n, src, dst))

    def test_same_rate_is_unchanged(self):
        samples = [0.1, -0.2, 0.3]
        self.assertEqual(ym._resample_linear(samples, 5585, 5585), samples)


class YmsFile(unittest.TestCase):

    def test_a_wav_becomes_a_yms_the_rp_accepts(self):
        """A half-second tone at 11,025 Hz, converted for the RP's rate."""
        rp = audio_defines()
        rate = rp["AUDIO_YM_SOURCE_RATE_HZ"]
        src_rate, n = 11025, 5512
        with tempfile.TemporaryDirectory() as tmp:
            wav_path = os.path.join(tmp, "tone.wav")
            with wave.open(wav_path, "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(src_rate)
                w.writeframes(b"".join(
                    struct.pack("<h", int(12000 * math.sin(2 * math.pi * 440 * i / src_rate)))
                    for i in range(n)))
            yms_path = os.path.join(tmp, "TONE.YMS")
            run = subprocess.run([sys.executable, TOOL, wav_path, "--mode", "dual-ghost",
                                  "--target-rate", str(rate), "--yms-output", yms_path],
                                 capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            with open(yms_path, "rb") as f:
                data = f.read()
        header = rp["AUDIO_YMS_HEADER_SIZE"]
        self.assertEqual(data[:4], b"YMS1")
        self.assertEqual(struct.unpack_from("<I", data, 4)[0], rate)
        self.assertEqual(struct.unpack_from("<I", data, 8)[0], len(data) - header)
        self.assertEqual(data[12], rp["AUDIO_YMS_MODE_DUAL_GHOST"])
        self.assertEqual(data[13:16], b"\0\0\0")
        body = data[header:]
        self.assertEqual(len(body), 2 * round(n * rate / src_rate))
        self.assertTrue(all(b <= 15 for b in body))


if __name__ == "__main__":
    unittest.main()
