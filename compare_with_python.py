"""compare_with_python.py - sample-by-sample comparison of the Python and C++ FOF output.

Usage: python3 compare_with_python.py <path to fof-synthesis repo> <fof_synth executable>

Example: python3 compare_with_python.py ../fof-synthesis ./fof_synth

The Python signal is quantised the same way the C++ WAV writer does it
(scale to 0.89 of full scale, truncate to int16), so the two can be
compared in 16-bit LSBs.
"""
import os, subprocess, sys, tempfile
import numpy as np
from scipy.io import wavfile

sys.path.insert(0, os.path.abspath(sys.argv[1]))
from fof_synthesis import synthesize_vowel
from vowel_data import MANDARIN_VOWELS

cpp = os.path.abspath(sys.argv[2])
cases = [(v, f0, sr) for v in "aiuo" for f0, sr in [(220, 44100), (330, 48000), (660, 44100), (261.63, 48000)]]

worst = 0
with tempfile.TemporaryDirectory() as d:
    out = os.path.join(d, "c.wav")
    for vowel, f0, sr in cases:
        subprocess.run([cpp, "--vowel", vowel, "--f0", str(f0), "--dur", "1",
                        "--sr", str(sr), "--out", out], capture_output=True, check=True)
        _, y = wavfile.read(out)
        x = synthesize_vowel(MANDARIN_VOWELS[vowel], f0, 1.0, sr,
                             vibrato_depth_cents=0, jitter_amount=0)
        xq = np.trunc(x * 0.89 * 32767.0)
        n = min(len(xq), len(y))
        lsb = int(np.max(np.abs(xq[:n] - y[:n].astype(float))))
        worst = max(worst, lsb)
        print(f"  /{vowel}/ f0={f0:7.2f} sr={sr}: length py={len(x)} cpp={len(y)}, "
              f"max diff = {lsb} LSB")
print(f"\nworst case over {len(cases)} cases: {worst} LSB "
      f"({'sample-exact' if worst == 0 else 'within %d LSB' % worst})")
