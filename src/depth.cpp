// Bathos - the depth core (see depth.h)
#include "depth.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <urlmon.h>
#pragma comment(lib, "urlmon.lib")
#else
#include <dlfcn.h>
#endif
#include "onnxruntime_c_api.h"

namespace fs = std::filesystem;

namespace Depth {

static const char* MODEL_FILE = "depth_anything_v2_vits.onnx";
static const char* MODEL_URL = "https://huggingface.co/onnx-community/depth-anything-v2-small/resolve/main/onnx/model.onnx";

static fs::path u8(const std::string& s) { return fs::u8path(s); }

static std::string exeDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return fs::path(buf).parent_path().u8string();
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec).parent_path().u8string();
#endif
}

// next to the exe (models/), then F:\AIModels\depth (this PC's model store), then the user's app data
static std::vector<std::string> spots() {
    std::error_code ec;
    std::vector<std::string> s = {exeDir() + "/models/" + MODEL_FILE};
#ifdef _WIN32
    if (fs::exists("F:/AIModels", ec)) s.push_back(std::string("F:/AIModels/depth/") + MODEL_FILE);
    const char* app = std::getenv("APPDATA");
    s.push_back(std::string(app ? app : ".") + "/Atelier/models/" + MODEL_FILE);
#else
    const char* home = std::getenv("HOME");
    s.push_back(std::string(home ? home : ".") + "/.config/Atelier/models/" + MODEL_FILE);
#endif
    return s;
}

std::string modelPath() {
    std::error_code ec;
    const auto s = spots();
    for (const auto& p : s)
        if (fs::exists(u8(p), ec) && fs::file_size(u8(p), ec) > 10000000) return p;
    return s.size() > 2 ? s[1] : s.back();   // where it'll be downloaded to: F: when there, else app data
}

static const OrtApi* ort = nullptr;
static OrtEnv* env = nullptr;
static OrtSession* session = nullptr;
static std::mutex lock;   // one run at a time (the window runs it on a worker thread)
static int lastFaces = 0;
static std::string faceErr;

static bool ok(OrtStatus* s, std::string& err) {
    if (!s) return true;
    err = std::string("ONNX Runtime: ") + ort->GetErrorMessage(s);
    ort->ReleaseStatus(s);
    return false;
}

bool loaded() { return session != nullptr; }

const OrtApi* api() { return ort; }

bool loadRuntime(std::string& err) {
    if (!ort) {
        typedef const OrtApiBase*(ORT_API_CALL * GetBase)();
        GetBase getBase = nullptr;
#ifdef _WIN32
        HMODULE h = LoadLibraryW((u8(exeDir()) / "onnxruntime.dll").wstring().c_str());
        if (!h) h = LoadLibraryW(L"onnxruntime.dll");
        if (h) getBase = (GetBase)GetProcAddress(h, "OrtGetApiBase");
#else
        void* h = dlopen("libonnxruntime.so", RTLD_NOW);
        if (h) getBase = (GetBase)dlsym(h, "OrtGetApiBase");
#endif
        if (getBase) ort = getBase()->GetApi(ORT_API_VERSION);
        if (!ort) { err = "ONNX Runtime (onnxruntime.dll) isn't next to Bathos"; return false; }
    }
    if (!env && !ok(ort->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "Bathos", &env), err)) return false;
    return true;
}

OrtSession* openSession(const std::string& model, std::string& err) {
    if (!loadRuntime(err)) return nullptr;
    OrtSessionOptions* so;
    if (!ok(ort->CreateSessionOptions(&so), err)) return nullptr;
    ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL);
    OrtSession* s = nullptr;
#ifdef _WIN32
    const bool made = ok(ort->CreateSession(env, u8(model).wstring().c_str(), so, &s), err);
#else
    const bool made = ok(ort->CreateSession(env, model.c_str(), so, &s), err);
#endif
    ort->ReleaseSessionOptions(so);
    return made ? s : nullptr;
}

// a small model that ships with Bathos (models/ next to the exe), or this PC's F:\AIModels copy
std::string shippedModel(const std::string& file, const std::string& aiModels) {
    std::error_code ec;
    const std::string here = exeDir() + "/models/" + file;
    if (fs::exists(u8(here), ec)) return here;
#ifdef _WIN32
    if (!aiModels.empty() && fs::exists(u8("F:/AIModels/" + aiModels), ec)) return "F:/AIModels/" + aiModels;
#endif
    return here;
}

bool load(std::string& err, const std::function<void(const std::string&)>& say) {
    std::lock_guard<std::mutex> g(lock);
    if (session) return true;
    if (!loadRuntime(err)) return false;
    const std::string model = modelPath();
    std::error_code ec;
    if (!fs::exists(u8(model), ec)) {
        fs::create_directories(u8(model).parent_path(), ec);
        if (say) say("Getting the depth model once (~99 MB) into " + model + " ...");
#ifdef _WIN32
        const bool got = URLDownloadToFileW(nullptr, fs::path(MODEL_URL).wstring().c_str(), u8(model).wstring().c_str(), 0, nullptr) == S_OK;
#else
        const bool got = std::system(("curl -sL -o '" + model + "' " + MODEL_URL).c_str()) == 0;
#endif
        if (!got || !fs::exists(u8(model), ec)) { err = std::string("Couldn't get the depth model (") + MODEL_URL + ")"; return false; }
    }
    if (say) say("Loading the depth model...");
    session = openSession(model, err);
    return session != nullptr;
}

// box mean of a w x h float field, radius r, through an integral image
static std::vector<float> boxMean(const std::vector<float>& f, int w, int h, int r) {
    std::vector<double> I((size_t)(w + 1) * (h + 1), 0.0);
    for (int y = 0; y < h; ++y) {
        double row = 0;
        for (int x = 0; x < w; ++x) { row += f[(size_t)y * w + x]; I[(size_t)(y + 1) * (w + 1) + x + 1] = I[(size_t)y * (w + 1) + x + 1] + row; }
    }
    std::vector<float> o((size_t)w * h);
    for (int y = 0; y < h; ++y) {
        const int y0 = std::max(0, y - r), y1 = std::min(h, y + r + 1);
        for (int x = 0; x < w; ++x) {
            const int x0 = std::max(0, x - r), x1 = std::min(w, x + r + 1);
            const double s = I[(size_t)y1 * (w + 1) + x1] - I[(size_t)y0 * (w + 1) + x1] - I[(size_t)y1 * (w + 1) + x0] + I[(size_t)y0 * (w + 1) + x0];
            o[(size_t)y * w + x] = (float)(s / ((double)(x1 - x0) * (y1 - y0)));
        }
    }
    return o;
}

// He et al.'s guided filter: the depth, enlarged from the model's ~518 px, snaps its edges to the picture's own
// outlines (grey guide), so no blocky stair-steps round hair, branches and skylines
static void guidedRefine(std::vector<float>& p, const std::vector<float>& g, int w, int h, int r, float eps) {
    std::vector<float> gp((size_t)w * h), gg((size_t)w * h);
    for (size_t i = 0; i < p.size(); ++i) { gp[i] = g[i] * p[i]; gg[i] = g[i] * g[i]; }
    const auto mg = boxMean(g, w, h, r), mp = boxMean(p, w, h, r), mgp = boxMean(gp, w, h, r), mgg = boxMean(gg, w, h, r);
    std::vector<float> a((size_t)w * h), b((size_t)w * h);
    for (size_t i = 0; i < p.size(); ++i) {
        a[i] = (mgp[i] - mg[i] * mp[i]) / (mgg[i] - mg[i] * mg[i] + eps);
        b[i] = mp[i] - a[i] * mg[i];
    }
    const auto ma = boxMean(a, w, h, r), mb = boxMean(b, w, h, r);
    for (size_t i = 0; i < p.size(); ++i) p[i] = std::max(0.0f, ma[i] * g[i] + mb[i]);
}

bool estimate(const uint8_t* px, int w, int h, const Params& prm, std::vector<float>& out, std::string& err) {
    if (!session && !load(err)) return false;
    std::lock_guard<std::mutex> g(lock);
    const int size = std::clamp(prm.size, 140, 2016);

    // the model's view: long side ~size, both sides a multiple of 14 (its patch size), ImageNet-normalised
    const float scale = (float)size / std::max(w, h);
    const int mw = std::max(14, (int)std::lround(w * scale / 14) * 14), mh = std::max(14, (int)std::lround(h * scale / 14) * 14);
    std::vector<float> input((size_t)3 * mw * mh);
    const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
    for (int y = 0; y < mh; ++y)
        for (int x = 0; x < mw; ++x) {
            const float sx = std::clamp((x + 0.5f) * w / mw - 0.5f, 0.0f, w - 1.0f), sy = std::clamp((y + 0.5f) * h / mh - 0.5f, 0.0f, h - 1.0f);
            const int x0 = (int)sx, y0 = (int)sy, x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
            const float fx = sx - x0, fy = sy - y0;
            for (int c = 0; c < 3; ++c) {
                auto P = [&](int xx, int yy) { return (float)px[((size_t)yy * w + xx) * 3 + c]; };
                const float v = (P(x0, y0) * (1 - fx) + P(x1, y0) * fx) * (1 - fy) + (P(x0, y1) * (1 - fx) + P(x1, y1) * fx) * fy;
                input[(size_t)c * mw * mh + (size_t)y * mw + x] = (v / 255.0f - mean[c]) / sd[c];
            }
        }

    OrtMemoryInfo* mem;
    if (!ok(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), err)) return false;
    const int64_t shape[4] = {1, 3, mh, mw};
    OrtValue* tin = nullptr;
    const bool made = ok(ort->CreateTensorWithDataAsOrtValue(mem, input.data(), input.size() * sizeof(float), shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &tin), err);
    ort->ReleaseMemoryInfo(mem);
    if (!made) return false;
    OrtAllocator* alloc;
    ort->GetAllocatorWithDefaultOptions(&alloc);
    char *inName, *outName;
    ort->SessionGetInputName(session, 0, alloc, &inName);
    ort->SessionGetOutputName(session, 0, alloc, &outName);
    const char* inNames[] = {inName};
    const char* outNames[] = {outName};
    OrtValue* res = nullptr;
    const bool ran = ok(ort->Run(session, nullptr, inNames, &tin, 1, outNames, 1, &res), err);
    alloc->Free(alloc, inName); alloc->Free(alloc, outName);
    ort->ReleaseValue(tin);
    if (!ran) return false;

    // the output's own size (1 x H x W): read it rather than assume
    OrtTensorTypeAndShapeInfo* info;
    ort->GetTensorTypeAndShape(res, &info);
    size_t dims = 0;
    ort->GetDimensionsCount(info, &dims);
    std::vector<int64_t> d(dims);
    ort->GetDimensions(info, d.data(), dims);
    ort->ReleaseTensorTypeAndShapeInfo(info);
    const int ow = dims >= 2 ? (int)d[dims - 1] : mw, oh = dims >= 2 ? (int)d[dims - 2] : mh;
    float* pred;
    ort->GetTensorMutableData(res, (void**)&pred);

    // stretch to 0..1 (1% / 99%, so a few stray pixels don't flatten it), back up to the picture's size
    std::vector<float> sorted(pred, pred + (size_t)ow * oh);
    std::sort(sorted.begin(), sorted.end());
    const float lo = sorted[sorted.size() / 100], hi = sorted.back();   // the near end unclipped: faces and close things keep their shape
    const float span = std::max(1e-6f, hi - lo);
    out.assign((size_t)w * h, 0.0f);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float sx = std::clamp((x + 0.5f) * ow / w - 0.5f, 0.0f, ow - 1.0f), sy = std::clamp((y + 0.5f) * oh / h - 0.5f, 0.0f, oh - 1.0f);
            const int x0 = (int)sx, y0 = (int)sy, x1 = std::min(ow - 1, x0 + 1), y1 = std::min(oh - 1, y0 + 1);
            const float fx = sx - x0, fy = sy - y0;
            const float v = (pred[y0 * ow + x0] * (1 - fx) + pred[y0 * ow + x1] * fx) * (1 - fy) + (pred[y1 * ow + x0] * (1 - fx) + pred[y1 * ow + x1] * fx) * fy;
            out[(size_t)y * w + x] = std::max(0.0f, (v - lo) / span);   // the model gives nearness: near = 1
        }
    ort->ReleaseValue(res);

    if (prm.refine) {
        std::vector<float> guide((size_t)w * h);
        for (size_t i = 0; i < guide.size(); ++i) guide[i] = (0.299f * px[i * 3] + 0.587f * px[i * 3 + 1] + 0.114f * px[i * 3 + 2]) / 255.0f;
        // radius ~ one model pixel and a bit, so the steps from enlarging are covered
        guidedRefine(out, guide, w, h, std::max(2, (int)std::lround(1.5f * std::max(w, h) / size)), 1e-3f);
    }
    lastFaces = 0;
    if (prm.faces) {
        std::string ferr;
        lastFaces = faceDepth(px, w, h, out, prm.faceRelief, ferr);
        if (lastFaces < 0) { lastFaces = 0; faceErr = ferr; } else faceErr.clear();
    }
    // back to 0..1: the nearest point (often a nose now) is 1
    float top = 1e-6f;
    for (float v : out) top = std::max(top, v);
    for (float& v : out) v = std::min(1.0f, v / top);
    return true;
}

int facesFound() { return lastFaces; }
std::string faceError() { return faceErr; }

// Turbo-like ramp (dark blue -> cyan -> green -> yellow -> red), 0..1
static void ramp(float t, uint8_t* o) {
    static const float k[6][3] = {{0.19f, 0.07f, 0.23f}, {0.16f, 0.47f, 0.93f}, {0.10f, 0.85f, 0.70f},
                                  {0.64f, 0.98f, 0.24f}, {0.98f, 0.69f, 0.16f}, {0.70f, 0.07f, 0.02f}};
    t = std::clamp(t, 0.0f, 1.0f) * 5;
    const int i = std::min(4, (int)t);
    const float f = t - i;
    for (int c = 0; c < 3; ++c) o[c] = (uint8_t)std::lround((k[i][c] * (1 - f) + k[i + 1][c] * f) * 255);
}

std::vector<uint8_t> render(const std::vector<float>& depth, const Look& look) {
    const int ch = channels(look);
    const float gamma = 1.0f / std::clamp(look.contrast, 0.2f, 5.0f);
    std::vector<uint8_t> img(depth.size() * ch);
    for (size_t i = 0; i < depth.size(); ++i) {
        float t = std::pow(depth[i], gamma);
        if (look.farBright) t = 1 - t;
        if (look.colour) ramp(t, &img[i * 3]); else img[i] = (uint8_t)std::lround(t * 255);
    }
    return img;
}

}  // namespace Depth
