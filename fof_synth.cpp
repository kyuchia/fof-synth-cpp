// fof_synth.cpp
//
// FOF (Fonction d'Onde Formantique) vowel synthesiser, C++ port.
// Port of an existing MATLAB/Python implementation.
//
// Build:  g++ -std=c++17 -O2 -o fof_synth fof_synth.cpp
// Run:    ./fof_synth --vowel a --f0 220 --dur 2.0 --out vowel_a.wav
//
// SCAFFOLD NOTE: everything except generate_grain() is infrastructure.
// The synthesis core is deliberately left empty. See the TODO below.

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
// Synthesis core  <-- THIS IS THE PART YOU WRITE
// ---------------------------------------------------------------------------

// Render one FOF grain for a single formant into `out`, starting at sample
// `start`, and mix it in additively (overlap-add).
//
// TODO: implement. The grain is an excitation impulse response consisting of
// a windowed attack followed by an exponentially decaying sinusoid at the
// formant frequency. The decay rate comes from `bandwidth`; the attack shape
// comes from `skirt`. This is the same maths as your Python version, so the
// only new part is expressing it in C++: index into the vector, accumulate
// with +=, and be careful not to run past out.size().
//
// Suggested order of work:
//   1. compute the decay coefficient from f.bandwidth
//   2. compute the attack length from f.skirt
//   3. loop over grain samples, build the envelope, multiply by the sinusoid
//   4. mix into out[start + n] with bounds checking
static void generate_grain(std::vector<double>& out,
                           size_t start,
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
    const size_t k_a = static_cast<size_t>(std::round(M_PI / beta));

    // Per-sample loop (replaces numpy vectorisation)
    for (size_t n = 0; n < fof_len; ++n) {
        const size_t idx = start + n;
        if (idx >= out.size()) break;      // manual bounds check; C++ won't clip like a Python slice

        const double env     = std::exp(-alpha * static_cast<double>(n));
        const double carrier = std::sin(omega_c * static_cast<double>(n));
        const double attack  = (n <= k_a)
            ? 0.5 * (1.0 - std::cos(beta * static_cast<double>(n)))
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
    const size_t period = static_cast<size_t>(sample_rate / f0);
    for (size_t start = 0; start < total; start += period)
        for (const Formant& f : formants)
            generate_grain(buffer, start, f, sample_rate);

    std::printf("Synthesising: %zu formants, f0=%.1fHz, %.1fs @ %.0fHz\n",
                formants.size(), f0, duration, sample_rate);
    return buffer;
}

// Plain sine, so you can confirm the WAV writer works before the DSP exists.
static std::vector<double> test_tone(double duration, double sample_rate) {
    const size_t total = static_cast<size_t>(duration * sample_rate);
    std::vector<double> buffer(total);
    for (size_t n = 0; n < total; ++n)
        buffer[n] = 0.5 * std::sin(2.0 * M_PI * 440.0 * n / sample_rate);
    return buffer;
}

// ---------------------------------------------------------------------------
// WAV output (16-bit PCM mono) -- boilerplate, nothing to learn here
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

int main(int argc, char** argv) {
    std::string vowel = "a", out_path = "out.wav";
    double f0 = 220.0, duration = 2.0, sample_rate = 48000.0;
    bool tone = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            return (i + 1 < argc) ? argv[++i] : "";
        };
        if      (a == "--vowel")      vowel = next();
        else if (a == "--f0")         f0 = std::stod(next());
        else if (a == "--dur")        duration = std::stod(next());
        else if (a == "--sr")         sample_rate = std::stod(next());
        else if (a == "--out")        out_path = next();
        else if (a == "--test-tone")  tone = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown option: %s\n", a.c_str()); return 1; }
    }

    std::vector<double> samples = tone
        ? test_tone(duration, sample_rate)
        : synthesise(vowel, f0, duration, sample_rate);

    if (samples.empty()) return 1;
    return write_wav(out_path, samples, sample_rate) ? 0 : 1;
}
