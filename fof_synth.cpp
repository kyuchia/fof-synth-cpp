// fof_synth.cpp
//
// FOF (Fonction d'Onde Formantique) vowel synthesiser, C++ port.
// Port of an existing MATLAB/Python implementation.
//
// Build:  g++ -std=c++17 -O2 -Wall -Wextra -o fof_synth fof_synth.cpp
// Run:    ./fof_synth --vowel a --f0 220 --dur 2.0 --out vowel_a.wav

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Formant description
// ---------------------------------------------------------------------------

struct Formant {
    double freq;       // centre frequency, Hz
    double bandwidth;  // -6 dB bandwidth, Hz  (controls grain decay)
    double skirt;      // skirt width beta, Hz (controls attack, spectral skirt)
    double amp;        // linear amplitude
};

// Mandarin vowel formants, Chen & Wang 2011 (ported from vowel_data.py).
// amp converted from dB to linear, 10^(dB/20):
//   0 dB = 1.000, -7 = 0.447, -9 = 0.355, -12 = 0.251, -16 = 0.158
// skirt field unused for now (beta is fixed at ~3 ms attack in generate_grain).
static std::vector<Formant> vowel_table(const std::string& vowel) {
    if (vowel == "a")
        return {{753, 100, 0, 1.000}, {1298, 120, 0, 0.447}, {3068, 150, 0, 0.251}};
    if (vowel == "i")
        return {{309,  90, 0, 1.000}, {2416, 110, 0, 0.355}, {3438, 130, 0, 0.158}};
    if (vowel == "u")
        return {{349,  80, 0, 1.000}, { 759, 100, 0, 0.355}, {2883, 120, 0, 0.158}};
    if (vowel == "o")
        return {{505,  90, 0, 1.000}, { 856, 110, 0, 0.447}, {3052, 130, 0, 0.251}};
    return {};
}

// ---------------------------------------------------------------------------
// Synthesis core
// ---------------------------------------------------------------------------

// Render one FOF grain for a single formant into `out`, starting at the
// (possibly fractional) sample position `onset`, and mix it in additively
// (overlap-add).
//
// The grain is a damped sinusoid at the formant centre frequency with a
// raised-cosine attack (Rodet 1984), with k = time since onset in samples:
//   attack (k <= pi/beta): 0.5*(1 - cos(beta*k)) * exp(-alpha*k) * sin(omega_c*k)
//   decay  (k >  pi/beta): exp(-alpha*k) * sin(omega_c*k)
// alpha = pi*BW/fs sets the -6 dB bandwidth; the grain is truncated at T60.
//
// The grain is evaluated analytically at k = idx - onset, so a fractional
// onset needs no interpolation: this is what lets the pitch period be
// non-integer.
static void generate_grain(std::vector<double>& out,
                           double onset,
                           const Formant& f,
                           double sample_rate) {
    // Parameters (mirrors fof_burst.py)
    const double alpha   = M_PI * f.bandwidth / sample_rate;   // exponential decay rate
    const double omega_c = 2.0 * M_PI * f.freq / sample_rate;  // angular centre frequency
    const double beta    = M_PI / (0.003 * sample_rate);       // ~3 ms attack (skirt unused for now)

    // Burst length from T60 decay criterion
    const double t60 = 6.91 / (M_PI * f.bandwidth);
    size_t fof_len = static_cast<size_t>(std::ceil(t60 * sample_rate));
    if (fof_len < 64) fof_len = 64;        // minimum length

    // Attack duration in samples
    const double k_a = M_PI / beta;

    // First output sample at or after the onset; k runs from (first - onset) in [0, 1).
    const size_t first = static_cast<size_t>(std::ceil(onset));

    // Per-sample loop (replaces numpy vectorisation)
    for (size_t idx = first; idx < out.size(); ++idx) {   // bounds check: C++ won't clip like a Python slice
        const double k = static_cast<double>(idx) - onset;
        if (k >= static_cast<double>(fof_len)) break;

        const double env     = std::exp(-alpha * k);
        const double carrier = std::sin(omega_c * k);
        const double attack  = (k <= k_a)
            ? 0.5 * (1.0 - std::cos(beta * k))
            : 1.0;

        out[idx] += f.amp * attack * env * carrier;   // overlap-add
    }
}

static std::vector<double> synthesise(const std::string& vowel,
                                      double f0,
                                      double duration,
                                      double sample_rate) {
    std::vector<Formant> formants = vowel_table(vowel);
    if (formants.empty()) {
        std::fprintf(stderr, "unknown vowel: %s (try a, i, u, o)\n", vowel.c_str());
        return {};
    }

    const size_t total = static_cast<size_t>(duration * sample_rate);
    std::vector<double> buffer(total, 0.0);

    // One excitation per fundamental period; every formant fires a grain.
    // The period is kept fractional (no rounding to whole samples), and each
    // onset is computed as p * period rather than accumulated, so rounding
    // error does not build up over long notes.
    const double period = sample_rate / f0;
    for (size_t p = 0; ; ++p) {
        const double onset = static_cast<double>(p) * period;
        if (onset >= static_cast<double>(total)) break;
        for (const Formant& f : formants)
            generate_grain(buffer, onset, f, sample_rate);
    }

    std::printf("Synthesising: %zu formants, f0=%.1fHz, %.1fs @ %.0fHz\n",
                formants.size(), f0, duration, sample_rate);
    return buffer;
}

// Plain 440 Hz sine, for checking the WAV writer independently of the synthesis.
static std::vector<double> test_tone(double duration, double sample_rate) {
    const size_t total = static_cast<size_t>(duration * sample_rate);
    std::vector<double> buffer(total);
    for (size_t n = 0; n < total; ++n)
        buffer[n] = 0.5 * std::sin(2.0 * M_PI * 440.0 * n / sample_rate);
    return buffer;
}

// ---------------------------------------------------------------------------
// WAV output (16-bit PCM mono)
// ---------------------------------------------------------------------------

static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}
static void put_u16(std::vector<uint8_t>& v, uint16_t x) {
    for (int i = 0; i < 2; ++i) v.push_back((x >> (8 * i)) & 0xFF);
}

static bool write_wav(const std::string& path,
                      const std::vector<double>& samples,
                      double sample_rate) {
    // normalise to avoid clipping
    double peak = 0.0;
    for (double s : samples) peak = std::max(peak, std::fabs(s));
    const double gain = (peak > 1e-12) ? (0.89 / peak) : 1.0;

    std::vector<uint8_t> out;
    const uint32_t data_bytes = static_cast<uint32_t>(samples.size() * 2);

    for (char c : std::string("RIFF")) out.push_back(c);
    put_u32(out, 36 + data_bytes);
    for (char c : std::string("WAVEfmt ")) out.push_back(c);
    put_u32(out, 16);                                        // fmt chunk size
    put_u16(out, 1);                                         // PCM
    put_u16(out, 1);                                         // mono
    put_u32(out, static_cast<uint32_t>(sample_rate));
    put_u32(out, static_cast<uint32_t>(sample_rate) * 2);    // byte rate
    put_u16(out, 2);                                         // block align
    put_u16(out, 16);                                        // bits per sample
    for (char c : std::string("data")) out.push_back(c);
    put_u32(out, data_bytes);

    for (double s : samples) {
        double v = s * gain;
        if (v > 1.0) v = 1.0;
        if (v < -1.0) v = -1.0;
        put_u16(out, static_cast<uint16_t>(static_cast<int16_t>(v * 32767.0)));
    }

    std::FILE* fp = std::fopen(path.c_str(), "wb");
    if (!fp) { std::perror("fopen"); return false; }
    std::fwrite(out.data(), 1, out.size(), fp);
    std::fclose(fp);
    std::printf("Wrote %s (%zu bytes)\n", path.c_str(), out.size());
    return true;
}

// ---------------------------------------------------------------------------
// CLI
// ---------------------------------------------------------------------------

static void usage() {
    std::printf(
        "usage: fof_synth [options]\n"
        "  --vowel <a|i|u|o> vowel to synthesise   (default a)\n"
        "  --f0 <Hz>         fundamental frequency (default 220)\n"
        "  --dur <seconds>   duration              (default 2.0)\n"
        "  --sr <Hz>         sample rate           (default 48000)\n"
        "  --out <file.wav>  output path           (default out.wav)\n"
        "  --test-tone       write a 440Hz sine instead, to check WAV output\n");
}

// Parse a whole string as a finite double; false on empty, trailing junk, inf or nan.
static bool parse_double(const std::string& s, double& out) {
    try {
        size_t pos = 0;
        const double v = std::stod(s, &pos);
        if (pos != s.size() || !std::isfinite(v)) return false;
        out = v;
        return true;
    } catch (...) {
        return false;
    }
}

int main(int argc, char** argv) {
    std::string vowel = "a", out_path = "out.wav";
    double f0 = 220.0, duration = 2.0, sample_rate = 48000.0;
    bool tone = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        auto number = [&](double& dst) -> bool {
            const std::string v = next();
            if (parse_double(v, dst)) return true;
            std::fprintf(stderr, "invalid value for %s: '%s'\n", a.c_str(), v.c_str());
            return false;
        };
        if      (a == "--vowel")      vowel = next();
        else if (a == "--f0")         { if (!number(f0)) return 1; }
        else if (a == "--dur")        { if (!number(duration)) return 1; }
        else if (a == "--sr")         { if (!number(sample_rate)) return 1; }
        else if (a == "--out")        out_path = next();
        else if (a == "--test-tone")  tone = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return 1; }
    }

    // Reject values that would divide by zero, loop forever or overflow the WAV header.
    if (sample_rate < 8000.0 || sample_rate > 192000.0) {
        std::fprintf(stderr, "--sr must be between 8000 and 192000 Hz\n");
        return 1;
    }
    if (duration <= 0.0 || duration > 600.0) {
        std::fprintf(stderr, "--dur must be greater than 0 and at most 600 s\n");
        return 1;
    }
    if (!tone && (f0 <= 0.0 || f0 >= sample_rate / 2.0)) {
        std::fprintf(stderr, "--f0 must be greater than 0 and below sr/2 (%.0f Hz)\n",
                     sample_rate / 2.0);
        return 1;
    }
    if (out_path.empty()) {
        std::fprintf(stderr, "--out needs a file name\n");
        return 1;
    }

    std::vector<double> samples = tone
        ? test_tone(duration, sample_rate)
        : synthesise(vowel, f0, duration, sample_rate);

    if (samples.empty()) return 1;
    return write_wav(out_path, samples, sample_rate) ? 0 : 1;
}
