#pragma once
#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "frame_encoder.h"
#include "media_reader.h"
#include "metrics.h"
#include "preview_renderer.h"

struct QueueItem {
    enum class State { Probing, Ready, Queued, Encoding, Done, Failed, Cancelled };
    int id = 0;
    std::string path; // file handed to MediaReader (sequences are detected from one frame)
    std::string name; // shown in the list
    State state = State::Probing;
    ghap::MediaInfo info;
    std::string message; // error or result line
    std::string output;
    float progress = 0;
    double fps = 0, eta = 0;
};

struct UserSettings {
    int format = 0; // 0 auto, 1 Hap Q, 2 Hap Q Alpha
    int quality = 1; // Fast, Normal, High, Max
    float dither = 1.0f;
    bool animateDither = true;
    float compact = 0.0f;
    bool readOnly = false; // test captures: never write settings.ini
    std::string outputDir; // empty = next to the source
    int chunks = 0;
    bool snappy = true;
    void load();
    void save() const;
    ghap::EncoderSettings encoder(bool sourceHasAlpha) const;
};

class App {
public:
    void init(HWND hwnd, ID3D11Device* dev, ID3D11DeviceContext* ctx, float dpiScale);
    void shutdown();
    void addPaths(const std::vector<std::wstring>& paths);
    void frame();
    bool busy() const; // something is running in the background (keeps redrawing)
    bool previewSettled(); // preview picture and metrics are up to date
    PreviewRenderer::Params& viewParams() { return view_; }
    void setFit(bool fit) { fit_ = fit; }
    void setPreviewFrame(int64_t f) { previewFrame_ = f; }
    // Test captures: never write settings.ini; optionally override the size setting.
    void setCaptureMode(float compact) { settings_.readOnly = true; if (compact >= 0) settings_.compact = compact; }
    // Test hook: start the queue once every item is probed; true when all are finished.
    bool autoEncodeStep();

private:
    // --- queue ---
    std::mutex itemsMutex_;
    std::vector<std::shared_ptr<QueueItem>> items_;
    int nextId_ = 1;
    int selectedId_ = 0;

    // --- probing ---
    std::thread probeThread_;
    std::mutex probeMutex_;
    std::condition_variable probeCv_;
    std::vector<std::shared_ptr<QueueItem>> probeQueue_;
    void probeLoop();

    // --- encoding ---
    std::thread encodeThread_;
    std::atomic<bool> encoding_{false};
    std::atomic<bool> cancel_{false};
    void startEncoding();
    void encodeLoop();
    std::string outputPathFor(const QueueItem& item, ghap::HapFormat fmt) const;

    // --- preview ---
    struct PreviewRequest {
        std::string path;
        int64_t frame = 0;
        ghap::EncoderSettings settings;
        bool ffmpeg = true;
        uint64_t gen = 0;
    };
    struct PreviewResult {
        uint64_t gen = 0;
        bool ok = false;
        std::string error;
        bool newSource = false, newFFmpeg = false;
        ghap::Frame16 source;
        ghap::EncodedTextures ours, ffmpeg;
        bool hasFFmpeg = false;
        double encodeMs = 0;
    };
    struct PreviewMetrics {
        uint64_t gen = 0;
        bool hasOurs = false, hasFFmpeg = false;
        ghap::Metrics ours, ffmpeg;
        double oursBytes = 0, ffmpegBytes = 0; // one frame after snappy
    };
    std::thread previewThread_;
    std::mutex previewMutex_;
    std::condition_variable previewCv_;
    std::optional<PreviewRequest> previewPending_;
    std::unique_ptr<PreviewResult> previewReady_;
    PreviewMetrics previewMetrics_;
    std::atomic<bool> previewBusy_{false};
    uint64_t previewGen_ = 0;
    std::string previewPath_;
    int64_t previewFrame_ = 0;
    std::string previewError_;
    double previewEncodeMs_ = 0;
    std::string lastRequestKey_;
    void previewLoop();
    void requestPreview();

    std::atomic<bool> quit_{false};

    // --- UI state ---
    HWND hwnd_ = nullptr;
    float dpi_ = 1;
    UserSettings settings_;
    PreviewRenderer renderer_;
    PreviewRenderer::Params view_;
    bool fit_ = true;
    bool advancedOpen_ = false;
    float settingsHeight_ = 400;

    void drawSidebar(float width);
    void drawQueue(float height);
    void drawSettings();
    void drawEncodeButton(float height);
    void drawPreview();
    std::shared_ptr<QueueItem> selected();
    void chooseOutputFolder();
};
