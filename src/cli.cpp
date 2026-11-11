// bathos-cli - Bathos on the command line: a picture in, its depth map out. Near is bright, far is dark.
//
//   bathos-cli picture.jpg [more pictures...] [-o out.jpg|out.png] [--size 518] [--colour] [--far-bright]
//              [--contrast 1.0] [--no-refine] [--quality 95]
//
// Or drop pictures onto bathos-cli.exe: each one gets <name>_depth.jpg beside it. LayerTack's Filter > Bathos runs it.
#include "depth.h"
#include "save.h"
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

static void usage() {
    std::printf(
        "bathos-cli - a picture in, its depth map out. Depth Anything V2 Small, on the CPU. Part of Atelier.\n\n"
        "  bathos-cli picture.jpg [more...] [options]\n"
        "  or drop pictures onto bathos-cli.exe: each gets <name>_depth.jpg beside it\n\n"
        "  -o FILE         where to write it (one picture only); .png is lossless, anything else is a JPEG\n"
        "  --size N        the long side the model sees, default 518 (more = finer detail, slower)\n"
        "  --colour        a colour ramp instead of grey\n"
        "  --far-bright    flip it: far is bright, near is dark\n"
        "  --contrast X    above 1 spreads out the near end, below 1 the far end (default 1)\n"
        "  --no-refine     skip the edge refine (by default the depth's edges follow the picture's)\n"
        "  --no-faces      skip the face shapes (by default each face gets a 3D face mesh's relief)\n"
        "  --face-relief X how strongly faces stand out, 0..3 (default 1)\n"
        "  --quality N     JPEG quality, default 95\n");
}

static int run(const std::vector<std::string>& args) {
    std::vector<std::string> inputs;
    std::string out;
    Depth::Params prm;
    Depth::Look look;
    int quality = 95;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        if ((a == "-o" || a == "--out") && i + 1 < args.size()) out = args[++i];
        else if (a == "--size" && i + 1 < args.size()) prm.size = std::clamp(std::atoi(args[++i].c_str()), 140, 2016);
        else if (a == "--quality" && i + 1 < args.size()) quality = std::clamp(std::atoi(args[++i].c_str()), 1, 100);
        else if (a == "--contrast" && i + 1 < args.size()) look.contrast = (float)std::atof(args[++i].c_str());
        else if (a == "--colour" || a == "--color") look.colour = true;
        else if (a == "--far-bright") look.farBright = true;
        else if (a == "--no-refine") prm.refine = false;
        else if (a == "--no-faces") prm.faces = false;
        else if (a == "--face-relief" && i + 1 < args.size()) prm.faceRelief = std::clamp((float)std::atof(args[++i].c_str()), 0.0f, 3.0f);
        else if (a == "-h" || a == "--help" || a == "/?") { usage(); return 0; }
        else inputs.push_back(a);
    }
    if (inputs.empty()) { usage(); return 1; }
    if (!out.empty() && inputs.size() > 1) { std::printf("-o works with one picture; leave it out for several.\n"); return 1; }

    std::string err;
    if (!Depth::load(err, [](const std::string& s) { std::printf("%s\n", s.c_str()); })) { std::printf("%s\n", err.c_str()); return 1; }
    int done = 0;
    for (const auto& in : inputs) {
        const fs::path p = fs::u8path(in);
        const std::string dest = !out.empty() ? out : (p.parent_path() / (p.stem().u8string() + "_depth.jpg")).u8string();
        const auto t0 = std::chrono::steady_clock::now();
        int w, h, n;
        uint8_t* px = stbi_load(in.c_str(), &w, &h, &n, 3);
        if (!px) { std::printf("can't read %s (%s)\n", in.c_str(), stbi_failure_reason()); continue; }
        std::vector<float> depth;
        const bool ok = Depth::estimate(px, w, h, prm, depth, err);
        stbi_image_free(px);
        if (!ok) { std::printf("%s: %s\n", in.c_str(), err.c_str()); continue; }
        std::error_code ec;
        if (dest.find_first_of("/\\") != std::string::npos) fs::create_directories(fs::u8path(dest).parent_path(), ec);
        if (!saveDepth(dest, Depth::render(depth, look), w, h, Depth::channels(look), quality)) { std::printf("can't write %s\n", dest.c_str()); continue; }
        ++done;
        const int nf = Depth::facesFound();
        const std::string faces = nf == 1 ? ", 1 face" : nf > 1 ? ", " + std::to_string(nf) + " faces" : "";
        std::printf("%s -> %s (%.1f s%s)\n", in.c_str(), dest.c_str(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), faces.c_str());
        if (!Depth::faceError().empty()) std::printf("  (faces skipped: %s)\n", Depth::faceError().c_str());
    }
    return done == (int)inputs.size() ? 0 : 1;
}

#ifdef _WIN32
// wide arguments, so names with accents or Japanese work; pause when started by a drop (its own console window)
int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(fs::path(argv[i]).u8string());
    const int rc = run(args);
    DWORD procs[2];
    // only when it has a console of its own on screen (a drop on the exe): not when LayerTack runs it with its output piped
    if (GetConsoleProcessList(procs, 2) == 1 && GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR) { std::printf("\nPress Enter to close."); (void)std::getchar(); }
    return rc;
}
#else
int main(int argc, char** argv) { return run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
