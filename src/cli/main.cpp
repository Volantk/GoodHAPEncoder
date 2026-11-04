#include <cstdio>
#include <cstring>
#include <string>

#include "media_reader.h"
#include "transcode.h"

using namespace ghap;

static void usage() {
    std::fprintf(stderr,
        "goodhap <input> <output.mov> [options]\n"
        "  --format q|qa        Hap Q or Hap Q Alpha (default: Hap Q, Alpha if the source has alpha)\n"
        "  --quality fast|normal|high|max   (default high)\n"
        "  --dither 0..1        dither strength (default 1, 0 = off)\n"
        "  --static-dither      same noise every frame\n"
        "  --chunks N           Hap chunks per texture (default auto)\n"
        "  --no-snappy          skip the snappy stage\n"
        "  --frames N           stop after N frames\n");
}

int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    TranscodeJob job;
    job.input = argv[1];
    job.output = argv[2];
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--format") {
            std::string f = next();
            job.settings.format = f == "qa" ? HapFormat::HapQAlpha : HapFormat::HapQ;
            job.autoAlpha = false;
        } else if (a == "--quality") {
            std::string q = next();
            job.settings.quality = q == "fast" ? Quality::Fast : q == "normal" ? Quality::Normal : q == "max" ? Quality::Max : Quality::High;
        } else if (a == "--dither") job.settings.dither = float(atof(next()));
        else if (a == "--static-dither") job.settings.animateDither = false;
        else if (a == "--chunks") job.settings.chunks = atoi(next());
        else if (a == "--no-snappy") job.settings.snappy = false;
        else if (a == "--frames") job.maxFrames = atoll(next());
        else { usage(); return 2; }
    }
    std::string err;
    bool ok = transcode(job, [](const TranscodeProgress& p) {
        std::fprintf(stderr, "\r%lld/%lld  %.1f fps   ", (long long)p.done, (long long)p.total, p.fps);
    }, nullptr, err);
    std::fprintf(stderr, "\n");
    if (!ok) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    return 0;
}
