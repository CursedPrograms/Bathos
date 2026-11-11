// Bathos - proper depth in faces. General depth models see a face as a near-flat blob; here each face gets a real
// 3D shape: YuNet finds it (and its eyes), MediaPipe's Face Mesh fits 468 points in 3D on an upright crop, the mesh is
// drawn as a height field, and that relief is blended into the scene's depth (following the scene's own tilt across
// the face), so noses, eye sockets, cheeks and chins come out.
#include "depth.h"
#include "face_mesh_tris.h"
#include "onnxruntime_c_api.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace Depth {

const OrtApi* api();
OrtSession* openSession(const std::string& model, std::string& err);
std::string shippedModel(const std::string& file, const std::string& aiModels);

static OrtSession* yunet = nullptr;
static OrtSession* mesh = nullptr;

static bool check(OrtStatus* s, std::string& err) {
    if (!s) return true;
    err = std::string("ONNX Runtime: ") + api()->GetErrorMessage(s);
    api()->ReleaseStatus(s);
    return false;
}

// run a one-input session; outputs come back as float vectors in the session's output order
static bool run(OrtSession* s, std::vector<float>& input, const std::vector<int64_t>& shape, std::vector<std::vector<float>>& outs, std::string& err) {
    const OrtApi* ort = api();
    OrtMemoryInfo* mem;
    if (!check(ort->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem), err)) return false;
    OrtValue* in = nullptr;
    const bool made = check(ort->CreateTensorWithDataAsOrtValue(mem, input.data(), input.size() * sizeof(float), shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in), err);
    ort->ReleaseMemoryInfo(mem);
    if (!made) return false;
    OrtAllocator* alloc;
    ort->GetAllocatorWithDefaultOptions(&alloc);
    size_t nOut = 0;
    ort->SessionGetOutputCount(s, &nOut);
    char* inName;
    ort->SessionGetInputName(s, 0, alloc, &inName);
    std::vector<char*> outNames(nOut);
    for (size_t i = 0; i < nOut; ++i) ort->SessionGetOutputName(s, i, alloc, &outNames[i]);
    std::vector<OrtValue*> res(nOut, nullptr);
    const char* inNames[] = {inName};
    const bool ran = check(ort->Run(s, nullptr, inNames, &in, 1, (const char* const*)outNames.data(), nOut, res.data()), err);
    alloc->Free(alloc, inName);
    for (auto n : outNames) alloc->Free(alloc, n);
    ort->ReleaseValue(in);
    if (!ran) return false;
    outs.assign(nOut, {});
    for (size_t i = 0; i < nOut; ++i) {
        OrtTensorTypeAndShapeInfo* info;
        ort->GetTensorTypeAndShape(res[i], &info);
        size_t count = 0;
        ort->GetTensorShapeElementCount(info, &count);
        ort->ReleaseTensorTypeAndShapeInfo(info);
        float* d;
        ort->GetTensorMutableData(res[i], (void**)&d);
        outs[i].assign(d, d + count);
        ort->ReleaseValue(res[i]);
    }
    return true;
}

struct Face { float x, y, w, h, score; float kp[10]; };   // kp: right eye, left eye, nose, mouth corners (x, y)

static float sampleBilinear(const uint8_t* rgb, int w, int h, float x, float y, int c) {
    x = std::clamp(x, 0.0f, w - 1.0f); y = std::clamp(y, 0.0f, h - 1.0f);
    const int x0 = (int)x, y0 = (int)y, x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
    const float fx = x - x0, fy = y - y0;
    auto P = [&](int xx, int yy) { return (float)rgb[((size_t)yy * w + xx) * 3 + c]; };
    return (P(x0, y0) * (1 - fx) + P(x1, y0) * fx) * (1 - fy) + (P(x0, y1) * (1 - fx) + P(x1, y1) * fx) * fy;
}

// YuNet (OpenCV Zoo, MIT): the picture letterboxed into 640 x 640, BGR 0..255; outputs per stride 8/16/32:
// cls, obj, bbox (dx, dy, log w, log h), kps (5 points)
static std::vector<Face> detect(const uint8_t* rgb, int w, int h, std::string& err) {
    const int S = 640;
    const float scale = (float)S / std::max(w, h);
    std::vector<float> in((size_t)3 * S * S, 0.0f);
    const int sw = (int)std::lround(w * scale), sh = (int)std::lround(h * scale);
    for (int y = 0; y < sh; ++y)
        for (int x = 0; x < sw; ++x)
            for (int c = 0; c < 3; ++c)   // BGR
                in[(size_t)c * S * S + (size_t)y * S + x] = sampleBilinear(rgb, w, h, (x + 0.5f) / scale - 0.5f, (y + 0.5f) / scale - 0.5f, 2 - c);
    std::vector<std::vector<float>> o;
    if (!run(yunet, in, {1, 3, S, S}, o, err) || o.size() < 12) { if (err.empty()) err = "YuNet gave too few outputs"; return {}; }
    std::vector<Face> found;
    const int strides[3] = {8, 16, 32};
    for (int k = 0; k < 3; ++k) {
        const int st = strides[k], cols = S / st, n = cols * cols;
        const auto &cls = o[k], &obj = o[3 + k], &box = o[6 + k], &kps = o[9 + k];
        for (int i = 0; i < n; ++i) {
            const float sc = std::sqrt(std::clamp(cls[i], 0.0f, 1.0f) * std::clamp(obj[i], 0.0f, 1.0f));
            if (sc < 0.6f) continue;
            const int r = i / cols, c = i % cols;
            Face f;
            const float cx = (c + box[i * 4]) * st, cy = (r + box[i * 4 + 1]) * st;
            const float bw = std::exp(box[i * 4 + 2]) * st, bh = std::exp(box[i * 4 + 3]) * st;
            f.x = (cx - bw / 2) / scale; f.y = (cy - bh / 2) / scale; f.w = bw / scale; f.h = bh / scale; f.score = sc;
            for (int p = 0; p < 5; ++p) { f.kp[p * 2] = (c + kps[i * 10 + p * 2]) * st / scale; f.kp[p * 2 + 1] = (r + kps[i * 10 + p * 2 + 1]) * st / scale; }
            found.push_back(f);
        }
    }
    // keep the best of overlapping boxes
    std::sort(found.begin(), found.end(), [](const Face& a, const Face& b) { return a.score > b.score; });
    std::vector<Face> keep;
    for (const auto& f : found) {
        bool clash = false;
        for (const auto& k : keep) {
            const float ix = std::max(0.0f, std::min(f.x + f.w, k.x + k.w) - std::max(f.x, k.x)), iy = std::max(0.0f, std::min(f.y + f.h, k.y + k.h) - std::max(f.y, k.y));
            if (ix * iy > 0.3f * std::min(f.w * f.h, k.w * k.h)) { clash = true; break; }
        }
        if (!clash && f.w >= 24 && f.h >= 24) keep.push_back(f);   // too small to shape
    }
    return keep;
}

// MediaPipe Face Mesh (Apache-2.0) on a square crop: centre (cx, cy), side, turned by angle (radians). The 468
// points come back in picture pixels; z in pixels too, smaller = nearer the camera. Returns its face score (logit).
static bool fitMesh(const uint8_t* rgb, int w, int h, float cx, float cy, float side, float angle, std::vector<float>& pts, float& score, std::string& err) {
    const int N = 192;
    const float ca = std::cos(angle), sa = std::sin(angle), k = side / N;
    std::vector<float> in((size_t)N * N * 3);   // NHWC, 0..1
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float u = (x + 0.5f - N / 2.0f) * k, v = (y + 0.5f - N / 2.0f) * k;
            const float px = cx + u * ca - v * sa, py = cy + u * sa + v * ca;
            for (int c = 0; c < 3; ++c) in[((size_t)y * N + x) * 3 + c] = sampleBilinear(rgb, w, h, px, py, c) / 255.0f;
        }
    std::vector<std::vector<float>> o;
    if (!run(mesh, in, {1, N, N, 3}, o, err) || o.size() < 2) return false;
    const auto& lm = o[0].size() >= 1404 ? o[0] : o[1];
    score = (o[0].size() >= 1404 ? o[1] : o[0])[0];
    pts.resize(468 * 3);
    for (int i = 0; i < 468; ++i) {
        const float u = (lm[i * 3] - N / 2.0f) * k, v = (lm[i * 3 + 1] - N / 2.0f) * k;
        pts[i * 3] = cx + u * ca - v * sa;
        pts[i * 3 + 1] = cy + u * sa + v * ca;
        pts[i * 3 + 2] = lm[i * 3 + 2] * k;
    }
    return true;
}

static void boxBlur(std::vector<float>& f, int w, int h, int r) {
    if (r < 1) return;
    std::vector<float> t(f.size());
    for (int pass = 0; pass < 2; ++pass) {   // two box passes, close to a Gaussian
        for (int y = 0; y < h; ++y) {
            double s = 0; int n = 0;
            for (int x = -r; x <= r; ++x) if (x >= 0 && x < w) { s += f[(size_t)y * w + x]; ++n; }
            for (int x = 0; x < w; ++x) {
                t[(size_t)y * w + x] = (float)(s / n);
                const int a = x - r, b = x + r + 1;
                if (a >= 0) { s -= f[(size_t)y * w + a]; --n; }
                if (b < w) { s += f[(size_t)y * w + b]; ++n; }
            }
        }
        for (int x = 0; x < w; ++x) {
            double s = 0; int n = 0;
            for (int y = -r; y <= r; ++y) if (y >= 0 && y < h) { s += t[(size_t)y * w + x]; ++n; }
            for (int y = 0; y < h; ++y) {
                f[(size_t)y * w + x] = (float)(s / n);
                const int a = y - r, b = y + r + 1;
                if (a >= 0) { s -= t[(size_t)a * w + x]; --n; }
                if (b < h) { s += t[(size_t)b * w + x]; ++n; }
            }
        }
    }
}

// the face's relief and coverage, in a box round it (bx, by, bw, bh)
static void rasterise(const std::vector<float>& pts, int bx, int by, int bw, int bh, std::vector<float>& nearness, std::vector<float>& cover) {
    nearness.assign((size_t)bw * bh, -1e9f);
    cover.assign((size_t)bw * bh, 0.0f);
    for (int t = 0; t < FACE_TRI_COUNT; ++t) {
        const float* a = &pts[FACE_TRIS[t][0] * 3];
        const float* b = &pts[FACE_TRIS[t][1] * 3];
        const float* c = &pts[FACE_TRIS[t][2] * 3];
        const float x0 = std::floor(std::min({a[0], b[0], c[0]})), x1 = std::ceil(std::max({a[0], b[0], c[0]}));
        const float y0 = std::floor(std::min({a[1], b[1], c[1]})), y1 = std::ceil(std::max({a[1], b[1], c[1]}));
        const float den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1]);
        if (std::fabs(den) < 1e-6f) continue;
        for (int y = std::max(by, (int)y0); y <= std::min(by + bh - 1, (int)y1); ++y)
            for (int x = std::max(bx, (int)x0); x <= std::min(bx + bw - 1, (int)x1); ++x) {
                const float px = x + 0.5f, py = y + 0.5f;
                const float l0 = ((b[1] - c[1]) * (px - c[0]) + (c[0] - b[0]) * (py - c[1])) / den;
                const float l1 = ((c[1] - a[1]) * (px - c[0]) + (a[0] - c[0]) * (py - c[1])) / den;
                const float l2 = 1 - l0 - l1;
                if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f) continue;
                const float n = -(l0 * a[2] + l1 * b[2] + l2 * c[2]);   // nearness: MediaPipe's z grows away from the camera
                float& cur = nearness[(size_t)(y - by) * bw + (x - bx)];
                if (n > cur) cur = n;
                cover[(size_t)(y - by) * bw + (x - bx)] = 1.0f;
            }
    }
}

int faceDepth(const uint8_t* rgb, int w, int h, std::vector<float>& depth, float relief, std::string& err) {
    if (!yunet) { yunet = openSession(shippedModel("face_yunet.onnx", "face/face_detection_yunet_2023mar.onnx"), err); if (!yunet) return -1; }
    if (!mesh) { mesh = openSession(shippedModel("face_mesh.onnx", "face/face_mesh_mediapipe.onnx"), err); if (!mesh) return -1; }
    const auto faces = detect(rgb, w, h, err);
    if (!err.empty()) return -1;
    int done = 0;
    for (const auto& f : faces) {
        // first fit: an upright crop from the detector's box and eyes
        float angle = std::atan2(f.kp[3] - f.kp[1], f.kp[2] - f.kp[0]);
        float cx = f.x + f.w / 2, cy = f.y + f.h / 2, side = std::max(f.w, f.h) * 1.5f;
        std::vector<float> pts;
        float score = 0;
        if (!fitMesh(rgb, w, h, cx, cy, side, angle, pts, score, err)) return -1;
        if (score < 0) continue;   // not a face after all
        // second fit, on the mesh's own box (as MediaPipe tracks), for a tighter crop: eyes' outer corners 33 / 263
        {
            angle = std::atan2(pts[263 * 3 + 1] - pts[33 * 3 + 1], pts[263 * 3] - pts[33 * 3]);
            const float ca = std::cos(-angle), sa = std::sin(-angle);
            float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
            for (int i = 0; i < 468; ++i) {
                const float u = pts[i * 3] * ca - pts[i * 3 + 1] * sa, v = pts[i * 3] * sa + pts[i * 3 + 1] * ca;
                lo[0] = std::min(lo[0], u); hi[0] = std::max(hi[0], u); lo[1] = std::min(lo[1], v); hi[1] = std::max(hi[1], v);
            }
            const float mu = (lo[0] + hi[0]) / 2, mv = (lo[1] + hi[1]) / 2;
            cx = mu * std::cos(angle) - mv * std::sin(angle); cy = mu * std::sin(angle) + mv * std::cos(angle);
            side = std::max(hi[0] - lo[0], hi[1] - lo[1]) * 1.5f;
            std::vector<float> again;
            float s2 = 0;
            if (fitMesh(rgb, w, h, cx, cy, side, angle, again, s2, err) && s2 >= 0) pts.swap(again);
        }

        // the face's box, with room for the feathering
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (int i = 0; i < 468; ++i) { lo[0] = std::min(lo[0], pts[i * 3]); hi[0] = std::max(hi[0], pts[i * 3]); lo[1] = std::min(lo[1], pts[i * 3 + 1]); hi[1] = std::max(hi[1], pts[i * 3 + 1]); }
        const float faceW = hi[0] - lo[0];
        if (faceW < 16) continue;
        const int feather = std::max(2, (int)std::lround(faceW * 0.05f));
        const int bx = std::max(0, (int)lo[0] - feather * 3), by = std::max(0, (int)lo[1] - feather * 3);
        const int bw = std::min(w, (int)hi[0] + feather * 3 + 1) - bx, bh = std::min(h, (int)hi[1] + feather * 3 + 1) - by;
        if (bw < 8 || bh < 8) continue;
        std::vector<float> near, cover;
        rasterise(pts, bx, by, bw, bh, near, cover);

        // fill the eye and mouth openings (the mesh leaves them out) from round about; the mesh itself stays sharp
        std::vector<float> nw(near.size()), wt = cover;
        for (size_t i = 0; i < near.size(); ++i) nw[i] = cover[i] > 0 ? near[i] : 0.0f;
        boxBlur(nw, bw, bh, feather); boxBlur(wt, bw, bh, feather);
        // the mask: the mesh's area with its holes closed, feathered at the edge (hair and ears keep the scene's depth)
        std::vector<float> mask = cover;
        boxBlur(mask, bw, bh, feather);
        for (auto& m : mask) m = std::clamp((m - 0.15f) / 0.5f, 0.0f, 1.0f);
        std::vector<float> inner = mask;   // well inside the face (for the edge level below)
        boxBlur(mask, bw, bh, feather);
        std::vector<float> filled(near.size(), 0.0f);
        for (size_t i = 0; i < near.size(); ++i) filled[i] = cover[i] > 0 ? near[i] : (wt[i] > 1e-4f ? nw[i] / wt[i] : 0.0f);

        // the relief, measured from the face's outline (so it fades out where the face meets the head and hair),
        // in face widths: a nose stands ~0.25 out
        double se = 0, ne = 0, sd = 0, nd = 0;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const size_t i = (size_t)y * bw + x;
                if (cover[i] > 0 && inner[i] < 0.95f) { se += filled[i]; ++ne; }   // the outline band
                if (mask[i] > 0.5f) { sd += depth[(size_t)(y + by) * w + (x + bx)]; ++nd; }
            }
        if (ne < 10 || nd < 50) continue;
        const float edgeN = (float)(se / ne);
        // the nearer the face, the more its shape shows in the depth
        const float base = (float)std::clamp(sd / nd, 0.05, 1.0);
        const float gain = 0.6f * relief * base;
        // added on top of the model's own depth, which keeps what it gets right (lips, brows, the head's tilt)
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const size_t i = (size_t)y * bw + x;
                const float m = mask[i];
                if (m <= 0.0f) continue;
                const float r = std::max(0.0f, filled[i] - edgeN) / faceW;
                float& d = depth[(size_t)(y + by) * w + (x + bx)];
                d = std::max(0.0f, d + gain * r * m);
            }
        ++done;
    }
    return done;
}

}  // namespace Depth
