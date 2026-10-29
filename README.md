# FOF Vowel Synthesiser (C++)

A single-file C++17 command-line port of the synthesis core of my Python
[FOF synthesis engine](https://github.com/kyuchia/fof-synthesis), implementing
Rodet's time-domain Formant-Wave-Function (FOF) synthesis.
No dependencies - just the standard library. Outputs 16-bit PCM mono WAV.

With vibrato and jitter turned off in the Python version, the two
implementations produce **sample-identical** output
(see [Validation](#validation-against-the-python-version)).

## Build & run

```bash
g++ -std=c++17 -O2 -Wall -Wextra -o fof_synth fof_synth.cpp

./fof_synth --vowel a --f0 220 --dur 1.0 --sr 44100 --out vowel_a.wav
./fof_synth --vowel i --f0 330 --out vowel_i.wav
./fof_synth --help
```

Options: `--vowel <a|i|u|o>` (default `a`), `--f0 <Hz>` (default 220),
`--dur <s>` (default 2.0), `--sr <Hz>` (default 48000),
`--out <file>` (default `out.wav`),
`--test-tone` (writes a 440 Hz sine to check the WAV output).

Invalid arguments are rejected with an error message and exit code 1:
non-numeric values, `--f0` not in (0, sr/2), `--dur` not in (0, 600] s,
and `--sr` outside 8-192 kHz.

## What it does

Each formant is rendered as a repeating FOF grain - an exponentially decaying
sinusoid at the formant centre frequency with a raised-cosine attack, where
`k` is the time since the grain's onset in samples:

- attack (0 <= k <= pi/beta): `0.5 * (1 - cos(beta*k)) * exp(-alpha*k) * sin(omega_c*k)`
- decay (k > pi/beta): `exp(-alpha*k) * sin(omega_c*k)`

where `alpha = pi*BW/fs` sets the -6 dB bandwidth and `beta` sets a fixed
attack time of about 3 ms. Each grain is truncated at the T60 decay time,
`6.91 / (pi*BW)` seconds (at least 64 samples).

Grains for all formants start once per fundamental period, and the period
`fs/f0` is **not rounded to whole samples**: the p-th onset is `p * fs/f0`,
and each grain is evaluated analytically at `k = n - onset`, so a fractional
onset needs no interpolation. The measured pitch error is 0.00 cents from
110 to 880 Hz at both 44.1 and 48 kHz.

The fundamental is constant: this port covers the steady-state FOF synthesis
core only. Vibrato, jitter and vowel morphing remain in the
[Python version](https://github.com/kyuchia/fof-synthesis).

Vowel data: Mandarin vowels /a/, /i/, /u/, /o/ with 3 formants each. The
formant frequencies are measurements from Chen & Wang (2011); the bandwidths
and amplitudes are estimates. Amplitudes are given in dB relative to F1 and
set each grain's time-domain amplitude. They are not the spectral peak
levels, which also depend on each formant's bandwidth.

## Validation against the Python version

`compare_with_python.py` renders /a/, /i/, /u/ and /o/ at four f0 / sample-rate
pairs with both implementations (Python with vibrato and jitter off),
quantises the Python signal exactly as the C++ WAV writer does, and compares
the two sample by sample:

```bash
python3 compare_with_python.py ../fof-synthesis ./fof_synth
# ...
# worst case over 16 cases: 0 LSB (sample-exact)
```

![Python vs C++ spectrogram comparison](compare_a.png)

## Port notes (Python -> C++)

- NumPy vectorised grain synthesis becomes a per-sample loop with explicit
  bounds checking (`operator[]` does not clip like a Python slice).
- Buffers are `std::vector<double>` passed by reference and mixed with `+=`
  (overlap-add).
- Formant amplitudes are stored in dB, exactly as in the Python table, and
  converted with `10^(dB/20)` at start-up. Hard-coding rounded linear values
  (e.g. 0.447 for -7 dB) was enough to make the outputs differ by up to 39 LSB.
- Onsets that land within 1e-6 samples of an integer are snapped to it, using
  the same rule as `pitch_onsets()` in the Python version. Otherwise float
  round-off decides whether a grain starts one sample later, which moves its
  T60 truncation point by one sample.
- The per-formant skirt-width parameter is not yet used; the attack time is fixed.

## References

1. X. Rodet, "Time-Domain Formant-Wave-Function Synthesis," _Computer Music
   Journal_, vol. 8, no. 3, pp. 9-14, 1984.
2. X. Rodet, Y. Potard, J.-B. Barriere, "The CHANT Project: From the Synthesis
   of the Singing Voice to Synthesis in General," _Computer Music Journal_,
   vol. 8, no. 3, pp. 15-31, 1984.
3. H. C. Chen and M. J. Wang, "An acoustic analysis of Chinese and English
   vowels," _CALR Linguistic Journal_, vol. 1, pp. 1-19, Jan. 2011.
