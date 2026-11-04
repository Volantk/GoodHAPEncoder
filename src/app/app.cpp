#include "app.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

#include "imgui.h"
#include "imgui_internal.h"
#include "transcode.h"

namespace fs = std::filesystem;
using namespace ghap;

// ---------------------------------------------------------------------------
// helpers

static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}
static std::wstring toWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
static fs::path u8path(const std::string& s) { return fs::path(toWide(s)); }
static std::string u8str(const fs::path& p) { return toUtf8(p.wstring()); }

static bool isVideoExt(std::string e) {
    for (auto& c : e) c = char(tolower(c));
    static const std::set<std::string> v = {".mov", ".mp4", ".m4v", ".mxf", ".mkv", ".avi", ".webm", ".mpg", ".mpeg", ".ts", ".gif", ".wmv"};
    return v.count(e) > 0;
}
static bool isStillExt(std::string e) {
    for (auto& c : e) c = char(tolower(c));
    static const std::set<std::string> v = {".png", ".tif", ".tiff", ".exr", ".dpx", ".jpg", ".jpeg", ".tga", ".bmp", ".webp"};
    return v.count(e) > 0;
}

static std::string humanSize(double bytes) {
    char buf[32];
    if (bytes >= 1024.0 * 1024 * 1024) snprintf(buf, sizeof buf, "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024.0 * 1024) snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024));
    else snprintf(buf, sizeof buf, "%.0f KB", bytes / 1024.0);
    return buf;
}

static std::string formatDuration(double s) {
    if (s < 0 || !std::isfinite(s)) return "?";
    int t = int(s + 0.5);
    char buf[32];
    if (t >= 3600) snprintf(buf, sizeof buf, "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else snprintf(buf, sizeof buf, "%d:%02d", t / 60, t % 60);
    return buf;
}

namespace col {
const ImVec4 bg = {0.075f, 0.078f, 0.086f, 1};
const ImVec4 panel = {0.102f, 0.110f, 0.125f, 1};
const ImVec4 text = {0.90f, 0.91f, 0.92f, 1};
const ImVec4 dim = {0.62f, 0.64f, 0.67f, 1};
const ImVec4 accent = {0.24f, 0.55f, 0.99f, 1};
const ImVec4 accentHover = {0.33f, 0.62f, 1.00f, 1};
const ImVec4 ok = {0.30f, 0.76f, 0.54f, 1};
const ImVec4 bad = {0.95f, 0.38f, 0.40f, 1};
} // namespace col

// ---------------------------------------------------------------------------
// settings

static fs::path settingsPath() {
    PWSTR appdata = nullptr;
    fs::path p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appdata))) p = fs::path(appdata) / L"GoodHAP";
    CoTaskMemFree(appdata);
    return p / L"settings.ini";
}

void UserSettings::load() {
    std::ifstream f(settingsPath());
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "format") format = std::clamp(atoi(v.c_str()), 0, 2);
        else if (k == "quality") quality = std::clamp(atoi(v.c_str()), 0, 3);
        else if (k == "dither") dither = std::clamp(float(atof(v.c_str())), 0.f, 1.f);
        else if (k == "animateDither") animateDither = v == "1";
        else if (k == "compact") compact = std::clamp(float(atof(v.c_str())), 0.f, 4.f);
        else if (k == "outputDir") outputDir = v;
        else if (k == "chunks") chunks = std::clamp(atoi(v.c_str()), 0, 64);
        else if (k == "snappy") snappy = v == "1";
    }
}

void UserSettings::save() const {
    if (readOnly) return;
    std::error_code ec;
    fs::create_directories(settingsPath().parent_path(), ec);
    std::ofstream f(settingsPath());
    f << "format=" << format << "\nquality=" << quality << "\ndither=" << dither << "\nanimateDither=" << animateDither
      << "\ncompact=" << compact
      << "\noutputDir=" << outputDir << "\nchunks=" << chunks << "\nsnappy=" << snappy << "\n";
}

EncoderSettings UserSettings::encoder(bool sourceHasAlpha) const {
    EncoderSettings s;
    s.format = format == 2 || (format == 0 && sourceHasAlpha) ? HapFormat::HapQAlpha : HapFormat::HapQ;
    s.quality = Quality(quality);
    s.dither = dither;
    s.animateDither = animateDither;
    s.compact = compact;
    s.chunks = chunks;
    s.snappy = snappy;
    return s;
}

// ---------------------------------------------------------------------------
// lifecycle

void App::init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, float dpiScale) {
    hwnd_ = hwnd;
    dpi_ = dpiScale;
    settings_.load();
    renderer_.init(dev, ctx);
    probeThread_ = std::thread([this] { probeLoop(); });
    previewThread_ = std::thread([this] { previewLoop(); });
}

void App::shutdown() {
    settings_.save();
    quit_ = true;
    cancel_ = true;
    probeCv_.notify_all();
    previewCv_.notify_all();
    if (probeThread_.joinable()) probeThread_.join();
    if (previewThread_.joinable()) previewThread_.join();
    if (encodeThread_.joinable()) encodeThread_.join();
}

bool App::busy() const { return encoding_ || previewBusy_; }

bool App::autoEncodeStep() {
    bool probing = false, pending = false;
    {
        std::lock_guard l(itemsMutex_);
        for (auto& it : items_) {
            probing |= it->state == QueueItem::State::Probing;
            pending |= it->state == QueueItem::State::Ready || it->state == QueueItem::State::Queued ||
                       it->state == QueueItem::State::Encoding;
        }
    }
    if (probing) return false;
    if (!encoding_ && pending) {
        static bool started = false;
        if (!started) { started = true; startEncoding(); }
        return false;
    }
    return !encoding_;
}

bool App::previewSettled() {
    std::lock_guard l(previewMutex_);
    return !previewBusy_ && !previewPending_ && !previewReady_ && previewMetrics_.gen == previewGen_ && previewGen_ > 0;
}

// ---------------------------------------------------------------------------
// adding files

void App::addPaths(const std::vector<std::wstring>& paths) {
    // Expand folders one level, collapse numbered stills into one sequence each.
    std::vector<fs::path> files;
    for (const auto& w : paths) {
        fs::path p(w);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            std::vector<fs::path> inside;
            for (auto& e : fs::directory_iterator(p, ec))
                if (e.is_regular_file()) inside.push_back(e.path());
            std::sort(inside.begin(), inside.end());
            files.insert(files.end(), inside.begin(), inside.end());
        } else {
            files.push_back(p);
        }
    }
    std::set<std::string> seenPatterns;
    std::vector<std::shared_ptr<QueueItem>> added;
    for (const auto& f : files) {
        std::string ext = u8str(f.extension());
        std::string path = u8str(f);
        auto item = std::make_shared<QueueItem>();
        if (isStillExt(ext)) {
            std::string pattern;
            int first = 0, count = 0;
            if (detectImageSequence(path, pattern, first, count) && count > 1) {
                if (!seenPatterns.insert(pattern).second) continue;
                item->name = u8str(u8path(pattern).filename());
                char buf[64];
                snprintf(buf, sizeof buf, "  [%d frames]", count);
                item->name += buf;
            } else {
                item->name = u8str(f.filename());
            }
        } else if (isVideoExt(ext)) {
            item->name = u8str(f.filename());
        } else {
            continue;
        }
        item->path = path;
        added.push_back(item);
    }
    {
        std::lock_guard l(itemsMutex_);
        for (auto& it : added) {
            it->id = nextId_++;
            items_.push_back(it);
            if (!selectedId_) selectedId_ = it->id;
        }
    }
    {
        std::lock_guard l(probeMutex_);
        probeQueue_.insert(probeQueue_.end(), added.begin(), added.end());
    }
    probeCv_.notify_one();
}

void App::probeLoop() {
    for (;;) {
        std::shared_ptr<QueueItem> item;
        {
            std::unique_lock l(probeMutex_);
            probeCv_.wait(l, [&] { return quit_ || !probeQueue_.empty(); });
            if (quit_) return;
            item = probeQueue_.front();
            probeQueue_.erase(probeQueue_.begin());
        }
        MediaReader r;
        std::string err;
        bool ok = r.open(item->path, err);
        std::lock_guard l(itemsMutex_);
        if (ok) {
            item->info = r.info();
            item->state = QueueItem::State::Ready;
        } else {
            item->state = QueueItem::State::Failed;
            item->message = err;
        }
    }
}

// ---------------------------------------------------------------------------
// encoding

std::string App::outputPathFor(const QueueItem& item, HapFormat fmt) const {
    return defaultOutputPath(item.path, item.info.isImageSequence, fmt, settings_.outputDir);
}

void App::startEncoding() {
    if (encoding_) return;
    {
        std::lock_guard l(itemsMutex_);
        bool any = false;
        for (auto& it : items_)
            if (it->state == QueueItem::State::Ready || it->state == QueueItem::State::Cancelled) {
                it->state = QueueItem::State::Queued;
                it->progress = 0;
                it->message.clear();
                any = true;
            }
        if (!any) return;
    }
    if (encodeThread_.joinable()) encodeThread_.join();
    cancel_ = false;
    encoding_ = true;
    settings_.save();
    encodeThread_ = std::thread([this] { encodeLoop(); });
}

void App::encodeLoop() {
    for (;;) {
        std::shared_ptr<QueueItem> item;
        TranscodeJob job;
        {
            std::lock_guard l(itemsMutex_);
            for (auto& it : items_)
                if (it->state == QueueItem::State::Queued) { item = it; break; }
            if (!item || cancel_) break;
            job.input = item->path;
            job.settings = settings_.encoder(item->info.hasAlpha);
            job.autoAlpha = false;
            job.output = outputPathFor(*item, job.settings.format);
            item->output = job.output;
            item->state = QueueItem::State::Encoding;
        }
        std::string err;
        bool ok = transcode(job, [&](const TranscodeProgress& p) {
            std::lock_guard l(itemsMutex_);
            item->progress = p.total ? float(p.done) / float(p.total) : 0.f;
            item->fps = p.fps;
            item->eta = p.fps > 0 && p.total ? (p.total - p.done) / p.fps : -1;
        }, &cancel_, err);
        std::lock_guard l(itemsMutex_);
        if (ok) {
            item->state = QueueItem::State::Done;
            item->progress = 1;
            std::error_code ec;
            double mb = double(fs::file_size(u8path(job.output), ec)) / (1024.0 * 1024.0);
            char buf[512];
            snprintf(buf, sizeof buf, "%s  ·  %.0f MB", u8str(u8path(job.output).filename()).c_str(), mb);
            item->message = buf;
        } else if (cancel_) {
            item->state = QueueItem::State::Cancelled;
            item->message = "Cancelled";
            std::error_code ec;
            fs::remove(u8path(job.output), ec); // partial file is useless
        } else {
            item->state = QueueItem::State::Failed;
            item->message = err;
        }
    }
    {
        std::lock_guard l(itemsMutex_);
        for (auto& it : items_)
            if (it->state == QueueItem::State::Queued) it->state = QueueItem::State::Ready;
    }
    encoding_ = false;
}

// ---------------------------------------------------------------------------
// preview

void App::requestPreview() {
    auto item = selected();
    if (!item || item->state == QueueItem::State::Probing || item->info.width == 0) return;
    PreviewRequest r;
    r.path = item->path;
    r.frame = std::clamp<int64_t>(previewFrame_, 0, std::max<int64_t>(0, item->info.frameCount - 1));
    r.settings = settings_.encoder(item->info.hasAlpha);
    char key[512];
    snprintf(key, sizeof key, "%s|%lld|%d|%d|%.3f|%d|%.2f", r.path.c_str(), (long long)r.frame, int(r.settings.format),
             int(r.settings.quality), r.settings.dither, int(r.settings.animateDither), r.settings.compact);
    if (lastRequestKey_ == key) return;
    lastRequestKey_ = key;
    r.gen = ++previewGen_;
    {
        std::lock_guard l(previewMutex_);
        previewPending_ = r;
    }
    previewCv_.notify_one();
}

void App::previewLoop() {
    std::unique_ptr<MediaReader> reader;
    std::string readerPath;
    int64_t nextFrame = -1;
    std::string srcKey; // path|frame of the cached source
    Frame16 src;
    ImageF srcImage;
    EncodedTextures ffTex;
    Metrics ffMetrics{};
    double ffBytes = 0;
    bool ffOk = false;
    std::vector<uint8_t> packed;

    for (;;) {
        PreviewRequest req;
        {
            std::unique_lock l(previewMutex_);
            previewCv_.wait(l, [&] { return quit_ || previewPending_.has_value(); });
            if (quit_) return;
            req = *previewPending_;
            previewPending_.reset();
        }
        previewBusy_ = true;
        auto res = std::make_unique<PreviewResult>();
        res->gen = req.gen;
        std::string key = req.path + "|" + std::to_string(req.frame);
        bool ok = true;
        if (key != srcKey) {
            std::string err;
            if (req.path != readerPath) {
                reader = std::make_unique<MediaReader>();
                if (!reader->open(req.path, err)) { ok = false; reader.reset(); readerPath.clear(); }
                else { readerPath = req.path; nextFrame = 0; }
            }
            if (ok && req.frame != nextFrame) {
                if (!reader->seekFrame(req.frame)) { ok = false; err = "seek failed"; }
            }
            if (ok && !reader->read(src)) { ok = false; err = "could not decode this frame"; }
            if (!ok) {
                res->error = err.empty() ? "could not read source" : err;
                readerPath.clear();
                srcKey.clear();
            } else {
                nextFrame = req.frame + 1;
                srcKey = key;
                srcImage = imageFromFrame(src);
                res->newSource = true;
                res->source = src;
                ffOk = ffmpegHapQReference(src, ffTex);
                res->newFFmpeg = true;
                res->hasFFmpeg = ffOk;
                if (ffOk) res->ffmpeg = ffTex;
            }
        }
        if (ok) {
            auto t0 = std::chrono::steady_clock::now();
            FrameView v{src.rgba.data(), src.width, src.height, src.width * 4};
            encodeTextures(v, req.settings, int(req.frame), res->ours);
            res->encodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            res->ok = true;
        }
        EncodedTextures oursCopy = res->ours;
        bool newSource = res->newSource;
        {
            std::lock_guard l(previewMutex_);
            previewReady_ = std::move(res);
        }
        if (ok) {
            // Metrics are slower; publish the picture first.
            PreviewMetrics m;
            m.gen = req.gen;
            m.ours = measure(srcImage, imageFromTextures(oursCopy, src.width, src.height));
            m.hasOurs = true;
            if (packHapFrame(oursCopy, req.settings, packed)) m.oursBytes = double(packed.size());
            if (newSource && ffOk) {
                ffMetrics = measure(srcImage, imageFromTextures(ffTex, src.width, src.height));
                ffBytes = packHapFrame(ffTex, req.settings, packed) ? double(packed.size()) : 0;
            }
            m.ffmpeg = ffMetrics;
            m.ffmpegBytes = ffBytes;
            m.hasFFmpeg = ffOk;
            std::lock_guard l(previewMutex_);
            previewMetrics_ = m;
        }
        previewBusy_ = previewPending_.has_value();
    }
}

std::shared_ptr<QueueItem> App::selected() {
    std::lock_guard l(itemsMutex_);
    for (auto& it : items_)
        if (it->id == selectedId_) return it;
    return nullptr;
}

void App::chooseOutputFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Output folder for Hap files");
    if (SUCCEEDED(dlg->Show(hwnd_))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                settings_.outputDir = toUtf8(p);
                CoTaskMemFree(p);
                settings_.save();
            }
            item->Release();
        }
    }
    dlg->Release();
}

// ---------------------------------------------------------------------------
// UI

static bool segmented(const char* id, int* value, const char* const* labels, int count, float width) {
    bool changed = false;
    ImGui::PushID(id);
    float w = (width - ImGui::GetStyle().ItemSpacing.x * 0) / count;
    for (int i = 0; i < count; ++i) {
        if (i) ImGui::SameLine(0, 0);
        bool sel = *value == i;
        ImGui::PushStyleColor(ImGuiCol_Button, sel ? col::accent : ImVec4(0.16f, 0.17f, 0.19f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, sel ? col::accentHover : ImVec4(0.21f, 0.22f, 0.25f, 1));
        ImGui::PushStyleColor(ImGuiCol_Text, sel ? ImVec4(1, 1, 1, 1) : col::text);
        if (ImGui::Button(labels[i], ImVec2(w, 0)) && !sel) { *value = i; changed = true; }
        ImGui::PopStyleColor(3);
    }
    ImGui::PopID();
    return changed;
}

static void dimText(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
    ImGui::TextWrappedV(fmt, args);
    ImGui::PopStyleColor();
    va_end(args);
}

void App::frame() {
    // Take finished preview work.
    {
        std::lock_guard l(previewMutex_);
        if (previewReady_) {
            auto& r = *previewReady_;
            if (r.newSource) {
                renderer_.setSource(r.source);
                if (r.hasFFmpeg) renderer_.setEncoded(PreviewRenderer::FFmpeg, r.ffmpeg);
                else renderer_.clearEncoded(PreviewRenderer::FFmpeg);
            }
            if (r.ok) renderer_.setEncoded(PreviewRenderer::Ours, r.ours);
            previewError_ = r.error;
            previewEncodeMs_ = r.encodeMs;
            previewReady_.reset();
        }
    }

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    float sideW = std::round(360 * dpi_);
    drawSidebar(sideW);
    ImGui::SameLine(0, 0);
    drawPreview();
    ImGui::End();
}

void App::drawSidebar(float width) {
    const float pad = std::round(16 * dpi_);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::panel);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("sidebar", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ImGui::TextUnformatted("Queue");
    const float gap = std::round(20 * dpi_), btnH = std::round(40 * dpi_);
    float queueH = std::max(80.0f * dpi_, ImGui::GetContentRegionAvail().y - settingsHeight_ - gap * 2 - btnH);
    drawQueue(queueH);
    ImGui::Dummy(ImVec2(0, gap - ImGui::GetStyle().ItemSpacing.y * 2));
    float y0 = ImGui::GetCursorPosY();
    drawSettings();
    settingsHeight_ = ImGui::GetCursorPosY() - y0;
    drawEncodeButton(btnH);

    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void App::drawQueue(float height) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col::bg);
    ImGui::BeginChild("queue", ImVec2(0, height), ImGuiChildFlags_Borders);
    std::vector<std::shared_ptr<QueueItem>> items;
    {
        std::lock_guard l(itemsMutex_);
        items = items_;
    }
    if (items.empty()) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        const char* t1 = "Drop videos or image sequences here";
        const char* t2 = "MOV, MP4, MXF, ProRes, DNxHR, PNG/TIFF/EXR sequences";
        ImVec2 s1 = ImGui::CalcTextSize(t1), s2 = ImGui::CalcTextSize(t2, nullptr, false, avail.x);
        ImGui::SetCursorPos(ImVec2((avail.x - s1.x) * 0.5f, avail.y * 0.5f - s1.y));
        ImGui::TextUnformatted(t1);
        ImGui::SetCursorPosX(std::max(0.f, (avail.x - s2.x) * 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Text, col::dim);
        ImGui::TextUnformatted(t2);
        ImGui::PopStyleColor();
    }
    int removeId = 0;
    for (auto& itp : items) {
        QueueItem it;
        {
            std::lock_guard l(itemsMutex_);
            it = *itp;
        }
        ImGui::PushID(it.id);
        bool sel = it.id == selectedId_;
        ImVec2 start = ImGui::GetCursorScreenPos();
        float rowH = ImGui::GetTextLineHeight() * 2 + 14 * dpi_ + (it.state == QueueItem::State::Encoding ? 8 * dpi_ : 0);
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(col::accent.x, col::accent.y, col::accent.z, 0.22f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(1, 1, 1, 0.05f));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImVec4(col::accent.x, col::accent.y, col::accent.z, 0.30f));
        if (ImGui::Selectable("##row", sel, ImGuiSelectableFlags_AllowOverlap, ImVec2(0, rowH))) {
            if (selectedId_ != it.id) { selectedId_ = it.id; previewFrame_ = 0; fit_ = true; }
        }
        ImGui::PopStyleColor(3);
        if (ImGui::BeginPopupContextItem()) {
            if (!it.output.empty() && it.state == QueueItem::State::Done && ImGui::MenuItem("Show output in Explorer")) {
                std::wstring args = L"/select,\"" + toWide(it.output) + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            if (ImGui::MenuItem("Show source in Explorer")) {
                std::wstring args = L"/select,\"" + toWide(it.path) + L"\"";
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Remove", nullptr, false, it.state != QueueItem::State::Encoding)) removeId = it.id;
            ImGui::EndPopup();
        }
        ImVec2 end = ImGui::GetCursorScreenPos();
        float x = start.x + 8 * dpi_, y = start.y + 6 * dpi_;
        float w = ImGui::GetContentRegionAvail().x - 16 * dpi_;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(ImVec2(start.x, start.y), ImVec2(start.x + w + 16 * dpi_, end.y), true);
        dl->AddText(ImVec2(x, y), ImGui::GetColorU32(col::text), it.name.c_str());
        y += ImGui::GetTextLineHeight() + 2 * dpi_;
        char line[512];
        ImU32 lineCol = ImGui::GetColorU32(col::dim);
        switch (it.state) {
        case QueueItem::State::Probing: snprintf(line, sizeof line, "Reading…"); break;
        case QueueItem::State::Failed: snprintf(line, sizeof line, "Failed: %s", it.message.c_str()); lineCol = ImGui::GetColorU32(col::bad); break;
        case QueueItem::State::Encoding:
            snprintf(line, sizeof line, "Encoding %d%%  ·  %.1f fps  ·  %s left", int(it.progress * 100), it.fps, formatDuration(it.eta).c_str());
            lineCol = ImGui::GetColorU32(col::text);
            break;
        case QueueItem::State::Done: snprintf(line, sizeof line, "Done  ·  %s", it.message.c_str()); lineCol = ImGui::GetColorU32(col::ok); break;
        default: {
            const auto& i = it.info;
            std::string len = i.frameCount > 1 ? formatDuration(i.duration) : std::string("still");
            char rate[32] = "";
            if (i.frameCount > 1) snprintf(rate, sizeof rate, "  ·  %.3g fps", double(i.fpsNum) / i.fpsDen);
            snprintf(line, sizeof line, "%d×%d%s  ·  %s  ·  %d-bit%s%s%s", i.width, i.height, rate,
                     len.c_str(), i.bitDepth, i.hasAlpha ? " + alpha" : "",
                     it.state == QueueItem::State::Queued ? "  ·  queued" : "",
                     it.state == QueueItem::State::Cancelled ? "  ·  cancelled" : "");
        }
        }
        dl->AddText(ImVec2(x, y), lineCol, line);
        if (it.state == QueueItem::State::Encoding) {
            float by = y + ImGui::GetTextLineHeight() + 4 * dpi_;
            dl->AddRectFilled(ImVec2(x, by), ImVec2(x + w, by + 3 * dpi_), ImGui::GetColorU32(ImVec4(1, 1, 1, 0.1f)), 2);
            dl->AddRectFilled(ImVec2(x, by), ImVec2(x + w * it.progress, by + 3 * dpi_), ImGui::GetColorU32(col::accent), 2);
        }
        dl->PopClipRect();
        ImGui::PopID();
    }
    if (removeId) {
        std::lock_guard l(itemsMutex_);
        items_.erase(std::remove_if(items_.begin(), items_.end(), [&](auto& p) { return p->id == removeId; }), items_.end());
        if (selectedId_ == removeId) selectedId_ = items_.empty() ? 0 : items_.front()->id;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void App::drawSettings() {
    const float w = ImGui::GetContentRegionAvail().x;
    const float labelGap = 4 * dpi_, groupGap = 12 * dpi_;
    bool changed = false;

    ImGui::TextUnformatted("Format");
    ImGui::Dummy(ImVec2(0, labelGap - ImGui::GetStyle().ItemSpacing.y));
    const char* formats[] = {"Auto", "Hap Q", "Hap Q Alpha"};
    changed |= segmented("fmt", &settings_.format, formats, 3, w);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Auto picks Hap Q Alpha when the source has an alpha channel.");

    ImGui::Dummy(ImVec2(0, groupGap - ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextUnformatted("Quality");
    ImGui::Dummy(ImVec2(0, labelGap - ImGui::GetStyle().ItemSpacing.y));
    const char* q[] = {"Fast", "Normal", "High", "Max"};
    changed |= segmented("q", &settings_.quality, q, 4, w);
    static const char* qHelp[] = {"Quickest search. Already far ahead of FFmpeg's encoder.",
                                  "Good balance: about 2x slower than Fast.",
                                  "Tries every chroma scale and a wider luma search.",
                                  "Widest search. Slow: for finals and stills."};
    dimText("%s", qHelp[settings_.quality]);

    ImGui::Dummy(ImVec2(0, groupGap - ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextUnformatted("Dither");
    ImGui::SameLine(w - ImGui::CalcTextSize("Animated").x - ImGui::GetFrameHeight() - ImGui::GetStyle().ItemInnerSpacing.x);
    changed |= ImGui::Checkbox("Animated", &settings_.animateDither);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("New noise pattern every frame. Turn off for stills or loops\nthat must match frame to frame.");
    ImGui::SetNextItemWidth(w);
    changed |= ImGui::SliderFloat("##dither", &settings_.dither, 0.0f, 1.0f, settings_.dither <= 0 ? "Off" : "%.2f");
    dimText("Fine blue noise instead of banding in gradients. Pure black, white and opaque stay exact.");

    ImGui::Dummy(ImVec2(0, groupGap - ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextUnformatted("Smaller files");
    ImGui::SetNextItemWidth(w);
    changed |= ImGui::SliderFloat("##compact", &settings_.compact, 0.0f, 4.0f, settings_.compact <= 0 ? "Off" : "%.1f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lets blocks reuse bytes of their neighbours so the file compresses better.\nThe preview shows the frame size it gives.");
    dimText("Trades quality for size: about -30%% at 1, -50%% at 2, -65%% at 3 on camera footage. Smooth gradients are protected.");

    ImGui::Dummy(ImVec2(0, groupGap - ImGui::GetStyle().ItemSpacing.y));
    ImGui::TextUnformatted("Output");
    ImGui::Dummy(ImVec2(0, labelGap - ImGui::GetStyle().ItemSpacing.y));
    {
        std::string label = settings_.outputDir.empty() ? "Next to each source" : settings_.outputDir;
        float bw = ImGui::CalcTextSize("Change…").x + ImGui::GetStyle().FramePadding.x * 2;
        float rw = settings_.outputDir.empty() ? 0 : ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x;
        ImGui::PushStyleColor(ImGuiCol_Text, settings_.outputDir.empty() ? col::dim : col::text);
        ImGui::SetNextItemWidth(w - bw - rw - ImGui::GetStyle().ItemSpacing.x);
        ImGui::InputText("##out", label.data(), label.size() + 1, ImGuiInputTextFlags_ReadOnly);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button("Change…")) chooseOutputFolder();
        if (!settings_.outputDir.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("×", ImVec2(ImGui::GetFrameHeight(), 0))) { settings_.outputDir.clear(); settings_.save(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Write next to each source again");
        }
    }

    ImGui::Dummy(ImVec2(0, groupGap - ImGui::GetStyle().ItemSpacing.y));
    advancedOpen_ = ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
    if (advancedOpen_) {
        ImGui::SetNextItemWidth(w * 0.4f);
        changed |= ImGui::DragInt("Chunks per frame", &settings_.chunks, 0.1f, 0, 64, settings_.chunks ? "%d" : "Auto");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lets players decompress a frame on several threads.\nAuto: one chunk per ~256 KB.");
        changed |= ImGui::Checkbox("Snappy compression", &settings_.snappy);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lossless second stage. Off: bigger files, slightly less CPU in the player.");
    }

    if (changed) settings_.save();
}

void App::drawEncodeButton(float btnH) {
    // Primary action pinned to the bottom of the sidebar.
    const float w = ImGui::GetContentRegionAvail().x;
    float y = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y - btnH;
    if (ImGui::GetCursorPosY() < y) ImGui::SetCursorPosY(y);
    int ready = 0;
    {
        std::lock_guard l(itemsMutex_);
        for (auto& it : items_)
            if (it->state == QueueItem::State::Ready || it->state == QueueItem::State::Cancelled) ++ready;
    }
    if (encoding_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.21f, 0.24f, 1));
        if (ImGui::Button(cancel_ ? "Cancelling…" : "Cancel encoding", ImVec2(w, btnH))) cancel_ = true;
        ImGui::PopStyleColor();
    } else {
        char label[64];
        snprintf(label, sizeof label, ready == 1 ? "Encode 1 clip" : "Encode %d clips", ready);
        ImGui::BeginDisabled(ready == 0);
        ImGui::PushStyleColor(ImGuiCol_Button, col::accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col::accentHover);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        if (ImGui::Button(ready ? label : "Encode", ImVec2(w, btnH))) startEncoding();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }
}

void App::drawPreview() {
    const float pad = std::round(12 * dpi_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("preview", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    auto item = selected();
    ImGuiIO& io = ImGui::GetIO();
    bool typing = io.WantTextInput;

    // Toolbar: what to show
    int v = int(view_.view);
    const char* views[] = {"Source", "GoodHAP", "FFmpeg"};
    bool flip = !typing && ImGui::IsKeyDown(ImGuiKey_Space);
    if (!typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_1)) v = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_2)) v = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_3)) v = 2;
        if (ImGui::IsKeyPressed(ImGuiKey_D)) view_.difference = !view_.difference;
        if (ImGui::IsKeyPressed(ImGuiKey_F)) fit_ = true;
    }
    segmented("view", &v, views, 3, 300 * dpi_);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Keys 1 2 3. Hold Space to flip to the source.");
    view_.view = PreviewRenderer::View(v);
    ImGui::SameLine(0, 20 * dpi_);
    ImGui::Checkbox("Difference", &view_.difference);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Grey = exact. Shows (encoded - source) x gain. Key D.");
    if (view_.difference) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(110 * dpi_);
        ImGui::SliderFloat("##gain", &view_.diffGain, 1, 64, "×%.0f", ImGuiSliderFlags_Logarithmic);
    }
    ImGui::SameLine(0, 20 * dpi_);
    ImGui::TextUnformatted("Exposure");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * dpi_);
    ImGui::SliderFloat("##exp", &view_.exposure, -2, 8, view_.exposure == 0 ? "0" : "%+.1f stops");
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) view_.exposure = 0;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Brighten to inspect shadows. Double-click to reset.");

    // Image area
    float bottomH = ImGui::GetFrameHeight() * 2 + ImGui::GetStyle().ItemSpacing.y * 2 + 4 * dpi_;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImVec2 area(avail.x, std::max(50.f, avail.y - bottomH));
    ImVec2 origin = ImGui::GetCursorScreenPos();
    PreviewRenderer::Params p = view_;
    if (flip) { p.view = PreviewRenderer::Source; p.difference = false; }
    if (renderer_.hasImage()) {
        float iw = float(renderer_.imageWidth()), ih = float(renderer_.imageHeight());
        if (fit_) {
            view_.zoom = std::min(area.x / iw, area.y / ih);
            view_.centerX = iw * 0.5f;
            view_.centerY = ih * 0.5f;
        }
        ImGui::InvisibleButton("img", area, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        bool hovered = ImGui::IsItemHovered();
        ImVec2 mouse = ImVec2(io.MousePos.x - origin.x - area.x * 0.5f, io.MousePos.y - origin.y - area.y * 0.5f);
        if (hovered && io.MouseWheel != 0) {
            float before = view_.zoom;
            float nz = std::clamp(before * std::pow(1.25f, io.MouseWheel), 0.05f, 64.f);
            if (std::fabs(nz - std::round(nz)) < 0.06f && nz >= 1) nz = std::round(nz); // snap to whole pixels
            float ix = mouse.x / before + view_.centerX, iy = mouse.y / before + view_.centerY;
            view_.zoom = nz;
            view_.centerX = ix - mouse.x / nz;
            view_.centerY = iy - mouse.y / nz;
            fit_ = false;
        }
        if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(0) || ImGui::IsMouseDragging(2))) {
            view_.centerX -= io.MouseDelta.x / view_.zoom;
            view_.centerY -= io.MouseDelta.y / view_.zoom;
            fit_ = false;
        }
        if (hovered && ImGui::IsMouseDoubleClicked(0)) {
            if (fit_ || std::fabs(view_.zoom - 1) > 1e-3f) {
                float ix = mouse.x / view_.zoom + view_.centerX, iy = mouse.y / view_.zoom + view_.centerY;
                view_.zoom = 1;
                view_.centerX = ix - mouse.x;
                view_.centerY = iy - mouse.y;
                fit_ = false;
            } else {
                fit_ = true;
            }
        }
        p.zoom = view_.zoom;
        p.centerX = view_.centerX;
        p.centerY = view_.centerY;
        int rw = int(area.x * io.DisplayFramebufferScale.x), rh = int(area.y * io.DisplayFramebufferScale.y);
        if (ID3D11ShaderResourceView* srv = renderer_.render(rw, rh, p))
            ImGui::GetWindowDrawList()->AddImage((ImTextureID)srv, origin, ImVec2(origin.x + area.x, origin.y + area.y));
        // Corner label: what is on screen
        const char* shown = flip ? "Source (Space)" : views[int(p.view)];
        char tag[96];
        snprintf(tag, sizeof tag, "%s%s  ·  %s", shown, (!flip && p.difference && p.view != 0) ? " difference" : "",
                 fit_ ? "fit" : (std::to_string(int(std::round(view_.zoom * 100))) + "%").c_str());
        ImVec2 ts = ImGui::CalcTextSize(tag);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 a(origin.x + 8 * dpi_, origin.y + 8 * dpi_);
        dl->AddRectFilled(a, ImVec2(a.x + ts.x + 12 * dpi_, a.y + ts.y + 6 * dpi_), IM_COL32(0, 0, 0, 170), 3);
        dl->AddText(ImVec2(a.x + 6 * dpi_, a.y + 3 * dpi_), IM_COL32(255, 255, 255, 230), tag);
        if (p.view == PreviewRenderer::FFmpeg && !flip && !renderer_.hasEncoded(PreviewRenderer::FFmpeg))
            dl->AddText(ImVec2(a.x, a.y + ts.y + 12 * dpi_), ImGui::GetColorU32(col::bad), "FFmpeg reference not available");
    } else {
        ImGui::Dummy(area);
        const char* msg = item ? (previewError_.empty() ? "Loading preview…" : previewError_.c_str())
                               : "Select a clip to preview it";
        ImVec2 ts = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(ImVec2(origin.x + (area.x - ts.x) * 0.5f, origin.y + (area.y - ts.y) * 0.5f),
                                            ImGui::GetColorU32(col::dim), msg);
    }

    // Frame slider + metrics
    ImGui::Dummy(ImVec2(0, 4 * dpi_));
    if (item && item->info.frameCount > 1) {
        int64_t maxF = item->info.frameCount - 1;
        int64_t minF = 0;
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_RightArrow)) previewFrame_ = std::min(previewFrame_ + 1, maxF);
        if (!typing && ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) previewFrame_ = std::max<int64_t>(previewFrame_ - 1, 0);
        ImGui::SetNextItemWidth(-1);
        char fmt[64];
        snprintf(fmt, sizeof fmt, "Frame %%lld / %lld", (long long)maxF);
        ImGui::SliderScalar("##frame", ImGuiDataType_S64, &previewFrame_, &minF, &maxF, fmt);
    } else {
        ImGui::Dummy(ImVec2(0, ImGui::GetFrameHeight()));
    }
    {
        PreviewMetrics m;
        {
            std::lock_guard l(previewMutex_);
            m = previewMetrics_;
        }
        if (item && renderer_.hasImage()) {
            ImGui::AlignTextToFramePadding();
            ImGui::BeginGroup();
            if (m.hasOurs && m.gen == previewGen_) {
                const int64_t frames = std::max<int64_t>(1, item->info.frameCount);
                ImGui::Text("GoodHAP  %.1f dB", m.ours.psnr);
                ImGui::SameLine();
                if (frames > 1)
                    dimText("(low-freq %.1f)  %s/frame, clip ~%s", m.ours.smooth8, humanSize(m.oursBytes).c_str(),
                            humanSize(m.oursBytes * frames).c_str());
                else
                    dimText("(low-freq %.1f)  %s", m.ours.smooth8, humanSize(m.oursBytes).c_str());
                if (m.hasFFmpeg) {
                    ImGui::SameLine(0, 24 * dpi_);
                    ImGui::Text("FFmpeg  %.1f dB", m.ffmpeg.psnr);
                    ImGui::SameLine();
                    dimText("(low-freq %.1f)  %s/frame", m.ffmpeg.smooth8, humanSize(m.ffmpegBytes).c_str());
                }
                ImGui::SameLine(0, 24 * dpi_);
                dimText("frame encoded in %.0f ms", previewEncodeMs_);
            } else {
                dimText(previewBusy_ ? "Encoding preview…" : " ");
            }
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("PSNR against the source for this frame, decoded the way a GPU decodes Hap Q.\n"
                                  "Low-freq: the same after blurring both, so it measures banding, tints and\n"
                                  "blotches rather than fine noise. Higher is better; +6 dB = half the error.");
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (item) requestPreview();
}
