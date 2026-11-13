// Bathos - the depth core: Depth Anything V2 (Small) through ONNX Runtime's C API (onnxruntime.dll, loaded at run
// time), then an edge refine that snaps the depth to the picture's own outlines. Shared by Bathos (the window),
// bathos-cli (the command line, which Chimera's Filter > Bathos runs) and anything else in Atelier.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Depth {

struct Params {
    int size = 518;        // the long side the model sees (rounded to a multiple of 14); more = finer, slower
    bool refine = true;    // snap the depth's edges to the picture's (guided filter)
    bool faces = true;     // proper faces: a 3D face mesh per face, its relief blended into the depth
    float faceRelief = 1.0f;   // how strongly the faces stand out (1 = natural)
};

struct Look {
    bool colour = false;     // a colour ramp instead of grey
    bool farBright = false;  // flip it: far is bright
    float contrast = 1.0f;   // a gamma on the depth: >1 spreads the near end, <1 the far end
};

// where the model is (or will be downloaded to)
std::string modelPath();
// load ONNX Runtime and the model; downloads the model once (~99 MB) when it's nowhere. say() reports progress.
bool load(std::string& err, const std::function<void(const std::string&)>& say = nullptr);
bool loaded();
// rgb: w*h*3 bytes. out: w*h nearness values 0..1 (near = 1), the picture's own size
bool estimate(const uint8_t* rgb, int w, int h, const Params& p, std::vector<float>& out, std::string& err);
// the depth as pixels: 1 byte per pixel (grey) or 3 (colour)
std::vector<uint8_t> render(const std::vector<float>& depth, const Look& look);
inline int channels(const Look& look) { return look.colour ? 3 : 1; }
// faces in the last estimate(), and why face depth didn't run (empty when it did)
int facesFound();
std::string faceError();

// ---- shared with face.cpp ----
// rgb in, depth (near = 1) changed in place: each face's mesh relief blended in. Returns the faces found, -1 on error.
int faceDepth(const uint8_t* rgb, int w, int h, std::vector<float>& depth, float relief, std::string& err);

}  // namespace Depth
