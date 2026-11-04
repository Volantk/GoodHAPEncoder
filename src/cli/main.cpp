#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "media_reader.h"
#include "transcode.h"

extern "C" {
#include <libavutil/log.h>
}

using namespace ghap;
namespace fs = std::filesystem;

static std::atomic<bool> g_cancel{false};

static void usage(FILE* f) {
    std::fprintf(f,
        "GoodHAP: Hap Q / Hap Q Alpha encoder that keeps gradients and fine detail\n"
        "\n"
        "usage: goodhap-cli [options] <input>... [-o <output.mov | folder>]\n"
        "\n"
        "  input              video file, still image, or one frame of a numbered\n"
        "                     image sequence (shot_0001.png picks up the whole sequence)\n"
        "  -o, --output PATH  output file (one input) or folder. Default: next to each\n"
        "                     input as <name>_HapQ.mov / <name>_HapQAlpha.mov\n"
        "  --format q|qa      force Hap Q or Hap Q Alpha (default: Alpha if the source has alpha)\n"
        "  --quality fast|normal|high|max   (default normal)\n"
        "  --dither 0..1      dither strength (default 1, 0 = off)\n"
        "  --static-dither    same noise pattern every frame\n"
        "  --chunks N         Hap chunks per texture (default auto)\n"
        "  --no-snappy        skip the snappy stage\n"
        "  --frames N         stop after N frames\n"
        "\n"
        "examples:\n"
        "  goodhap-cli clip.mov\n"
        "  goodhap-cli --quality high *.mov -o ~/Desktop/hap\n"
        "  goodhap-cli renders/shot_0001.exr -o shot.mov\n");
}

static bool hasMovExt(const std::string& s) {
    if (s.size() < 4) return false;
    std::string e = s.substr(s.size() - 4);
    for (auto& c : e) c = char(tolower((unsigned char)c));
    return e == ".mov";
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(stderr); return 2; }
    av_log_set_level(AV_LOG_ERROR);
    std::vector<std::string> inputs;
    std::string output;
    EncoderSettings settings;
    settings.quality = Quality::Normal;
    bool forceFormat = false;
    int64_t maxFrames = 0;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { usage(stdout); return 0; }
        else if (a == "-o" || a == "--output") output = next();
        else if (a == "--format") {
            std::string f = next();
            if (f != "q" && f != "qa") { std::fprintf(stderr, "--format is q or qa\n"); return 2; }
            settings.format = f == "qa" ? HapFormat::HapQAlpha : HapFormat::HapQ;
            forceFormat = true;
        } else if (a == "--quality") {
            std::string q = next();
            if (q == "fast") settings.quality = Quality::Fast;
            else if (q == "normal") settings.quality = Quality::Normal;
            else if (q == "high") settings.quality = Quality::High;
            else if (q == "max") settings.quality = Quality::Max;
            else { std::fprintf(stderr, "--quality is fast, normal, high or max\n"); return 2; }
        } else if (a == "--dither") settings.dither = float(std::atof(next().c_str()));
        else if (a == "--static-dither") settings.animateDither = false;
        else if (a == "--chunks") settings.chunks = std::atoi(next().c_str());
        else if (a == "--no-snappy") settings.snappy = false;
        else if (a == "--frames") maxFrames = std::atoll(next().c_str());
        else if (a.size() > 1 && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n\n", a.c_str()); usage(stderr); return 2; }
        else inputs.push_back(a);
    }
    // Old form: goodhap-cli in.mov out.mov
    std::error_code ec;
    if (output.empty() && inputs.size() == 2 && hasMovExt(inputs[1]) && !fs::exists(inputs[1], ec)) {
        output = inputs[1];
        inputs.pop_back();
    }
    if (inputs.empty()) { usage(stderr); return 2; }
    bool outputIsFile = !output.empty() && hasMovExt(output) && !fs::is_directory(output, ec);
    if (outputIsFile && inputs.size() > 1) {
        std::fprintf(stderr, "-o %s is a file, but there are %zu inputs; give a folder instead\n", output.c_str(), inputs.size());
        return 2;
    }
    if (!output.empty()) {
        fs::path dir = outputIsFile ? fs::path(output).parent_path() : fs::path(output);
        if (!dir.empty()) fs::create_directories(dir, ec);
    }

    std::signal(SIGINT, [](int) { g_cancel = true; });

    int failed = 0;
    for (size_t n = 0; n < inputs.size() && !g_cancel; ++n) {
        const std::string& in = inputs[n];
        MediaReader probe;
        std::string err;
        if (!probe.open(in, err)) {
            std::fprintf(stderr, "%s: %s\n", in.c_str(), err.c_str());
            ++failed;
            continue;
        }
        const MediaInfo info = probe.info();
        TranscodeJob job;
        job.input = in;
        job.settings = settings;
        if (!forceFormat) job.settings.format = info.hasAlpha ? HapFormat::HapQAlpha : HapFormat::HapQ;
        job.autoAlpha = false;
        job.maxFrames = maxFrames;
        job.output = outputIsFile ? output : defaultOutputPath(in, info.isImageSequence, job.settings.format, output);

        std::fprintf(stderr, "[%zu/%zu] %s -> %s\n      %dx%d, %d-bit%s, %s\n", n + 1, inputs.size(), in.c_str(), job.output.c_str(),
                     info.width, info.height, info.bitDepth, info.hasAlpha ? " + alpha" : "", formatName(job.settings.format));
        auto t0 = std::chrono::steady_clock::now();
        bool ok = transcode(job, [](const TranscodeProgress& p) {
            if (p.total > 0) {
                double eta = p.fps > 0 ? (p.total - p.done) / p.fps : 0;
                std::fprintf(stderr, "\r      %lld/%lld frames  %.1f fps  %d:%02d left   ", (long long)p.done, (long long)p.total, p.fps,
                             int(eta) / 60, int(eta) % 60);
            } else {
                std::fprintf(stderr, "\r      %lld frames  %.1f fps   ", (long long)p.done, p.fps);
            }
        }, &g_cancel, err);
        double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "\n");
        if (ok) {
            double mb = double(fs::file_size(job.output, ec)) / (1024.0 * 1024.0);
            std::fprintf(stderr, "      done in %.1f s, %.1f MB\n", secs, mb);
        } else {
            fs::remove(job.output, ec); // a partial file has no index and won't play
            std::fprintf(stderr, "      %s: %s\n", g_cancel ? "stopped" : "failed", err.c_str());
            ++failed;
        }
    }
    if (g_cancel) return 130;
    return failed ? 1 : 0;
}
