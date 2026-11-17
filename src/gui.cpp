// Bathos - a picture in, its depth map out. Part of Atelier (Cursed Entertainment's creative suite).
// The window: drop a picture, see its depth beside it (drag the wipe), tune it, save it. Drop several at once and
// each gets <name>_depth.jpg beside it. SDL2 + OpenGL 3 + Dear ImGui, like Chimera.
//
//   Bathos [picture]
//   Bathos --capture out.bmp [picture]     a screenshot from a hidden window, then quit
#include "depth.h"
#include "save.h"
#include "stb_image.h"

#include <SDL.h>
#include <SDL_opengl.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace fs = std::filesystem;

// ---------- colours: the Atelier look (Changeling greys, the Cursed blue #007bb7; solid, no gradients, no shadows) ----------
static const ImU32 C_BG = IM_COL32(0x1a, 0x1a, 0x1a, 255), C_PANEL = IM_COL32(0x24, 0x24, 0x24, 255),
                   C_LINE = IM_COL32(0x3a, 0x3a, 0x3a, 255), C_TEXT = IM_COL32(0xe6, 0xe6, 0xe6, 255),
                   C_DIM = IM_COL32(0x9a, 0x9a, 0x9a, 255), C_PINK = IM_COL32(0x00, 0x7b, 0xb7, 255),
                   C_DEEP = IM_COL32(0x00, 0x51, 0x84, 255), C_WELL = IM_COL32(0x14, 0x14, 0x14, 255);
static ImVec4 col4(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

static void applyStyle() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0; s.FrameRounding = 2; s.GrabRounding = 2; s.PopupRounding = 2; s.ScrollbarRounding = 2;
    s.WindowBorderSize = 0; s.FrameBorderSize = 0; s.PopupBorderSize = 1;
    s.WindowPadding = ImVec2(12, 10); s.FramePadding = ImVec2(7, 4); s.ItemSpacing = ImVec2(6, 7);
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = col4(C_TEXT); c[ImGuiCol_TextDisabled] = col4(C_DIM);
    c[ImGuiCol_WindowBg] = col4(C_PANEL); c[ImGuiCol_ChildBg] = col4(C_PANEL); c[ImGuiCol_PopupBg] = col4(IM_COL32(0x21, 0x21, 0x21, 255));
    c[ImGuiCol_Border] = col4(C_LINE); c[ImGuiCol_Separator] = col4(C_LINE);
    c[ImGuiCol_FrameBg] = col4(C_WELL); c[ImGuiCol_FrameBgHovered] = col4(IM_COL32(0x2e, 0x2e, 0x2e, 255)); c[ImGuiCol_FrameBgActive] = col4(IM_COL32(0x33, 0x33, 0x33, 255));
    c[ImGuiCol_MenuBarBg] = col4(C_BG);
    c[ImGuiCol_CheckMark] = col4(C_PINK); c[ImGuiCol_SliderGrab] = col4(C_PINK); c[ImGuiCol_SliderGrabActive] = col4(C_DEEP);
    c[ImGuiCol_Button] = col4(IM_COL32(0x33, 0x33, 0x33, 255)); c[ImGuiCol_ButtonHovered] = col4(C_DEEP); c[ImGuiCol_ButtonActive] = col4(C_PINK);
    c[ImGuiCol_Header] = col4(IM_COL32(0x33, 0x33, 0x33, 255)); c[ImGuiCol_HeaderHovered] = col4(C_DEEP); c[ImGuiCol_HeaderActive] = col4(C_PINK);
    c[ImGuiCol_PlotHistogram] = col4(C_PINK);
}

static bool fileExists(const char* f) {
    if (FILE* t = fopen(f, "rb")) { fclose(t); return true; }
    return false;
}

static void loadFont() {
    ImGuiIO& io = ImGui::GetIO();
    const char* fonts[] = {"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/System/Library/Fonts/Helvetica.ttc"};
    for (const char* f : fonts)
        if (fileExists(f)) {
            static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2190, 0x21FF, 0x2600, 0x26FF, 0};
            io.Fonts->AddFontFromFileTTF(f, 16.0f, nullptr, ranges);
            if (fileExists("C:/Windows/Fonts/seguisym.ttf")) {   // the credit's star
                ImFontConfig merge; merge.MergeMode = true;
                static const ImWchar sym[] = {0x2190, 0x21FF, 0x2600, 0x26FF, 0};
                io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/seguisym.ttf", 16.0f, &merge, sym);
            }
            return;
        }
    io.Fonts->AddFontDefault();
}

#ifdef _WIN32
static std::string fromW(const wchar_t* w) { return fs::path(w).u8string(); }
#endif

static std::string fileDialog(bool save, const char* filter, const char* defExt, const std::string& start) {
#ifdef _WIN32
    std::wstring f;
    for (const char* p = filter; *p; ++p) f += (*p == '|') ? L'\0' : (wchar_t)*p;
    f += L'\0'; f += L'\0';
    wchar_t buf[4096] = {0};
    if (!start.empty()) { std::wstring s = fs::u8path(start).wstring(); wcsncpy(buf, s.c_str(), 4095); }
    OPENFILENAMEW o{};
    o.lStructSize = sizeof(o);
    o.lpstrFilter = f.c_str(); o.lpstrFile = buf; o.nMaxFile = 4096;
    std::wstring ext = defExt ? std::wstring(defExt, defExt + strlen(defExt)) : L"";
    o.lpstrDefExt = defExt ? ext.c_str() : nullptr;
    o.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR) : (OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR);
    const BOOL ok = save ? GetSaveFileNameW(&o) : GetOpenFileNameW(&o);
    return ok ? fromW(buf) : std::string();
#else
    std::string cmd = save ? "zenity --file-selection --save --confirm-overwrite 2>/dev/null" : "zenity --file-selection 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return {};
    char line[4096] = {0};
    std::string r = fgets(line, sizeof line, p) ? line : "";
    pclose(p);
    while (!r.empty() && (r.back() == '\n' || r.back() == '\r')) r.pop_back();
    if (save && !r.empty() && defExt && r.find('.') == std::string::npos) r += std::string(".") + defExt;
    return r;
#endif
}

// ---------- state ----------
static SDL_Window* window = nullptr;
static bool quit = false;

struct Picture {
    std::string path;
    int w = 0, h = 0;
    std::vector<uint8_t> rgb;      // w*h*3
    std::vector<float> depth;      // w*h, near = 1 (empty until worked out)
};
static Picture pic;
static bool havePic = false;
static Depth::Params params;
static Depth::Look look;
static int jpegQuality = 95;

enum View { V_WIPE, V_DEPTH, V_PICTURE };
static int viewMode = V_WIPE;
static float wipe = 0.5f;                       // where the wipe is, 0..1 across the picture

static GLuint picTex = 0, depthTex = 0;
static bool depthTexStale = true;

// the worker: one job at a time (a picture's depth, or a batch)
static std::thread worker;
static std::atomic<bool> busy{false};
static std::mutex mtx;
static std::string status = "Drop a picture to start";
static std::atomic<float> progress{-1.0f};      // a batch's progress, 0..1 (-1: none)
static bool resultReady = false;                // the worker finished a depth: pick it up on the main thread
static std::vector<float> resultDepth;
static std::string resultFor;
static std::vector<std::string> pending;        // pictures dropped while busy
static bool rerun = false;                      // settings changed while busy

static void say(const std::string& s) { std::lock_guard<std::mutex> g(mtx); status = s; }

static void upload(GLuint& tex, const uint8_t* px, int w, int h, int ch) {
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    // grey goes up as RGB so it shows grey, not red
    std::vector<uint8_t> rgb;
    if (ch == 1) { rgb.resize((size_t)w * h * 3); for (size_t i = 0; i < (size_t)w * h; ++i) rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = px[i]; px = rgb.data(); }
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, px);
}

static void joinWorker() { if (worker.joinable()) worker.join(); }

static void startDepth() {
    if (!havePic) return;
    if (busy) { rerun = true; return; }
    joinWorker();
    busy = true;
    const auto rgb = pic.rgb; const int w = pic.w, h = pic.h; const auto prm = params; const std::string path = pic.path;
    worker = std::thread([rgb, w, h, prm, path] {
        std::string err;
        if (!Depth::loaded() && !Depth::load(err, say)) { say(err); busy = false; return; }
        say("Working out the depth...");
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<float> d;
        if (!Depth::estimate(rgb.data(), w, h, prm, d, err)) { say(err); busy = false; return; }
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        char msg[256];
        const int nf = Depth::facesFound();
        const std::string faces = nf == 1 ? ", 1 face shaped" : nf > 1 ? ", " + std::to_string(nf) + " faces shaped" : "";
        snprintf(msg, sizeof msg, "%d x %d, depth in %.1f s%s", w, h, s, faces.c_str());
        {
            std::lock_guard<std::mutex> g(mtx);
            resultDepth = std::move(d); resultFor = path; resultReady = true; status = msg;
        }
        busy = false;
    });
}

static bool openPicture(const std::string& path) {
    int w, h, n;
    uint8_t* px = stbi_load(path.c_str(), &w, &h, &n, 3);
    if (!px) { say("Can't open " + fs::u8path(path).filename().u8string() + " (" + stbi_failure_reason() + ")"); return false; }
    pic = Picture();
    pic.path = path; pic.w = w; pic.h = h;
    pic.rgb.assign(px, px + (size_t)w * h * 3);
    stbi_image_free(px);
    havePic = true;
    upload(picTex, pic.rgb.data(), w, h, 3);
    depthTexStale = true;
    SDL_SetWindowTitle(window, ("Bathos - " + fs::u8path(path).filename().u8string()).c_str());
    startDepth();
    return true;
}

static std::string defaultOut(const std::string& path) {
    const fs::path p = fs::u8path(path);
    return (p.parent_path() / (p.stem().u8string() + "_depth.jpg")).u8string();
}

// several pictures at once: each gets <name>_depth.jpg beside it, with the settings as they are; the last one shows
static void startBatch(std::vector<std::string> files) {
    joinWorker();
    busy = true; progress = 0.0f;
    const auto prm = params; const auto lk = look; const int q = jpegQuality;
    worker = std::thread([files, prm, lk, q] {
        std::string err;
        if (!Depth::loaded() && !Depth::load(err, say)) { say(err); busy = false; progress = -1.0f; return; }
        int done = 0;
        for (size_t i = 0; i < files.size(); ++i) {
            say("Batch: " + fs::u8path(files[i]).filename().u8string() + " (" + std::to_string(i + 1) + " of " + std::to_string(files.size()) + ")");
            int w, h, n;
            uint8_t* px = stbi_load(files[i].c_str(), &w, &h, &n, 3);
            if (!px) continue;
            std::vector<float> d;
            if (Depth::estimate(px, w, h, prm, d, err) && saveDepth(defaultOut(files[i]), Depth::render(d, lk), w, h, Depth::channels(lk), q)) ++done;
            stbi_image_free(px);
            progress = (float)(i + 1) / files.size();
        }
        say("Batch done: " + std::to_string(done) + " of " + std::to_string(files.size()) + " depth maps saved beside their pictures");
        busy = false; progress = -1.0f;
    });
}

static void dropFiles(const std::vector<std::string>& files) {
    if (files.empty()) return;
    if (busy) { pending.insert(pending.end(), files.begin(), files.end()); return; }
    if (files.size() == 1) { openPicture(files[0]); return; }
    startBatch(files);
}

static void saveAs() {
    if (!havePic || pic.depth.empty()) return;
    const std::string p = fileDialog(true, "JPEG|*.jpg|PNG (lossless)|*.png|", "jpg", defaultOut(pic.path));
    if (p.empty()) return;
    if (saveDepth(p, Depth::render(pic.depth, look), pic.w, pic.h, Depth::channels(look), jpegQuality)) say("Saved " + p);
    else say("Can't write " + p);
}

static void quickSave() {
    if (!havePic || pic.depth.empty()) return;
    const std::string p = defaultOut(pic.path);
    if (saveDepth(p, Depth::render(pic.depth, look), pic.w, pic.h, Depth::channels(look), jpegQuality)) say("Saved " + p);
    else say("Can't write " + p);
}

// ---------- the frame ----------
static bool showAbout = false;

static void menuBar() {
    if (!ImGui::BeginMainMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Open...", "Ctrl+O")) { const std::string p = fileDialog(false, "Pictures|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*|", nullptr, ""); if (!p.empty()) dropFiles({p}); }
        if (ImGui::MenuItem("Save Depth Beside the Picture", "Ctrl+S", false, havePic && !pic.depth.empty())) quickSave();
        if (ImGui::MenuItem("Save Depth As...", "Ctrl+Shift+S", false, havePic && !pic.depth.empty())) saveAs();
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Ctrl+Q")) quit = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        if (ImGui::MenuItem("Wipe: picture | depth", "1", viewMode == V_WIPE)) viewMode = V_WIPE;
        if (ImGui::MenuItem("Depth", "2", viewMode == V_DEPTH)) viewMode = V_DEPTH;
        if (ImGui::MenuItem("Picture", "3", viewMode == V_PICTURE)) viewMode = V_PICTURE;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Bathos")) showAbout = true;
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

static void keys() {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) return;
    const bool ctrl = io.KeyCtrl;
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_O)) { const std::string p = fileDialog(false, "Pictures|*.png;*.jpg;*.jpeg;*.bmp;*.tga|All files|*.*|", nullptr, ""); if (!p.empty()) dropFiles({p}); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_S)) { if (io.KeyShift) saveAs(); else quickSave(); }
    if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Q)) quit = true;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_1)) viewMode = V_WIPE;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_2)) viewMode = V_DEPTH;
    if (!ctrl && ImGui::IsKeyPressed(ImGuiKey_3)) viewMode = V_PICTURE;
}

static void canvas(ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col4(C_BG));
    ImGui::Begin("##canvas", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (!havePic) {
        // the empty start: a drop zone
        const ImVec2 c(pos.x + size.x / 2, pos.y + size.y / 2);
        const float bw = std::min(520.0f, size.x - 80), bh = 260;
        const ImVec2 a(c.x - bw / 2, c.y - bh / 2), b(c.x + bw / 2, c.y + bh / 2);
        for (float x = a.x; x < b.x; x += 12) { dl->AddLine(ImVec2(x, a.y), ImVec2(std::min(x + 6, b.x), a.y), C_LINE, 1.5f); dl->AddLine(ImVec2(x, b.y), ImVec2(std::min(x + 6, b.x), b.y), C_LINE, 1.5f); }
        for (float y = a.y; y < b.y; y += 12) { dl->AddLine(ImVec2(a.x, y), ImVec2(a.x, std::min(y + 6, b.y)), C_LINE, 1.5f); dl->AddLine(ImVec2(b.x, y), ImVec2(b.x, std::min(y + 6, b.y)), C_LINE, 1.5f); }
        auto centred = [&](const char* t, float y, ImU32 col) { const ImVec2 ts = ImGui::CalcTextSize(t); dl->AddText(ImVec2(c.x - ts.x / 2, y), col, t); };
        centred("Drop a picture here", c.y - 40, C_TEXT);
        centred("Bathos works out how far away everything in it is.", c.y - 12, C_DIM);
        centred("Near is bright, far is dark.", c.y + 10, C_DIM);
        centred("Drop several at once: each gets <name>_depth.jpg beside it.", c.y + 40, C_DIM);
        ImGui::End(); ImGui::PopStyleColor();
        return;
    }
    // fit the picture
    const float pad = 20;
    const float s = std::min((size.x - pad * 2) / pic.w, (size.y - pad * 2) / pic.h);
    const float dw = pic.w * s, dh = pic.h * s;
    const ImVec2 a(pos.x + (size.x - dw) / 2, pos.y + (size.y - dh) / 2), b(a.x + dw, a.y + dh);
    const bool haveDepth = !pic.depth.empty() && depthTex;
    if (viewMode == V_PICTURE || !haveDepth) dl->AddImage((ImTextureID)(intptr_t)picTex, a, b);
    else if (viewMode == V_DEPTH) dl->AddImage((ImTextureID)(intptr_t)depthTex, a, b);
    else {
        // the wipe: the picture left of it, the depth right of it; drag anywhere on the picture to move it
        const float x = a.x + dw * wipe;
        dl->AddImage((ImTextureID)(intptr_t)picTex, a, ImVec2(x, b.y), ImVec2(0, 0), ImVec2(wipe, 1));
        dl->AddImage((ImTextureID)(intptr_t)depthTex, ImVec2(x, a.y), b, ImVec2(wipe, 0), ImVec2(1, 1));
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), C_PINK, 2.0f);
        dl->AddCircleFilled(ImVec2(x, a.y + dh / 2), 9, C_PINK);
        dl->AddTriangleFilled(ImVec2(x - 6, a.y + dh / 2), ImVec2(x - 2, a.y + dh / 2 - 4), ImVec2(x - 2, a.y + dh / 2 + 4), C_TEXT);
        dl->AddTriangleFilled(ImVec2(x + 6, a.y + dh / 2), ImVec2(x + 2, a.y + dh / 2 - 4), ImVec2(x + 2, a.y + dh / 2 + 4), C_TEXT);
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("wipe", ImVec2(dw, dh));
        if (ImGui::IsItemActive()) wipe = std::clamp((ImGui::GetIO().MousePos.x - a.x) / dw, 0.0f, 1.0f);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        dl->AddText(ImVec2(a.x + 8, a.y + 6), C_TEXT, "Picture");
        const char* dt = "Depth";
        dl->AddText(ImVec2(b.x - 8 - ImGui::CalcTextSize(dt).x, a.y + 6), C_TEXT, dt);
    }
    if (busy && progress < 0) {   // a spinner while the depth comes
        const float t = (float)ImGui::GetTime() * 6;
        const ImVec2 c(b.x - 22, b.y - 22);
        dl->AddCircleFilled(c, 16, IM_COL32(0x1a, 0x1a, 0x1a, 200));
        dl->PathArcTo(c, 10, t, t + 4.2f, 24);
        dl->PathStroke(C_PINK, 0, 2.5f);
    }
    ImGui::End(); ImGui::PopStyleColor();
}

static void panel(ImVec2 pos, ImVec2 size) {
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::Begin("##panel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove);
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_PINK)); ImGui::TextUnformatted("DEPTH"); ImGui::PopStyleColor();
    ImGui::PushItemWidth(w);
    ImGui::TextDisabled("Detail (what the model sees)");
    ImGui::SliderInt("##size", &params.size, 280, 1036, "%d px");
    if (ImGui::IsItemDeactivatedAfterEdit()) startDepth();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("More picks out finer shapes but takes longer. 518 is what the model was trained on.");
    if (ImGui::Checkbox("Refine edges", &params.refine)) startDepth();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Snaps the depth's edges to the picture's own outlines (hair, branches, skylines).");
    if (ImGui::Checkbox("Proper faces", &params.faces)) startDepth();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fits a 3D face mesh to every face, so noses, eye sockets, cheeks and chins have real depth.");
    ImGui::BeginDisabled(!params.faces);
    ImGui::TextDisabled("Face relief");
    ImGui::SliderFloat("##relief", &params.faceRelief, 0.0f, 3.0f, "%.2f");
    if (ImGui::IsItemDeactivatedAfterEdit()) startDepth();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("How strongly faces stand out. 1 is natural.");
    ImGui::EndDisabled();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_PINK)); ImGui::TextUnformatted("LOOK"); ImGui::PopStyleColor();
    bool lookChanged = false;
    lookChanged |= ImGui::Checkbox("Colour ramp", &look.colour);
    lookChanged |= ImGui::Checkbox("Far is bright", &look.farBright);
    ImGui::TextDisabled("Contrast");
    lookChanged |= ImGui::SliderFloat("##contrast", &look.contrast, 0.3f, 3.0f, "%.2f");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Above 1 spreads out the near end, below 1 the far end. Double-click a slider to type.");
    if (ImGui::Button("Reset look", ImVec2(w, 0))) { look = Depth::Look(); lookChanged = true; }
    if (lookChanged) depthTexStale = true;
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, col4(C_PINK)); ImGui::TextUnformatted("SAVE"); ImGui::PopStyleColor();
    ImGui::TextDisabled("JPEG quality");
    ImGui::SliderInt("##q", &jpegQuality, 60, 100);
    ImGui::PopItemWidth();
    const bool can = havePic && !pic.depth.empty();
    ImGui::BeginDisabled(!can);
    if (ImGui::Button("Save beside the picture", ImVec2(w, 0))) quickSave();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && havePic) ImGui::SetTooltip("%s", defaultOut(pic.path).c_str());
    if (ImGui::Button("Save as...", ImVec2(w, 0))) saveAs();
    ImGui::EndDisabled();

    // the foot: status, credit
    const float footH = 86;
    ImGui::SetCursorPosY(size.y - footH);
    ImGui::Separator();
    if (progress >= 0) ImGui::ProgressBar(progress, ImVec2(w, 6), "");
    { std::lock_guard<std::mutex> g(mtx); ImGui::PushTextWrapPos(0); ImGui::TextDisabled("%s", status.c_str()); ImGui::PopTextWrapPos(); }
    ImGui::SetCursorPosY(size.y - 26);
    ImGui::TextDisabled("Bathos " BATHOS_VERSION " - part of Atelier");
    ImGui::End();
}

static void about() {
    if (showAbout) { ImGui::OpenPopup("About Bathos"); showAbout = false; }
    ImGui::SetNextWindowSize(ImVec2(470, 0));
    if (ImGui::BeginPopupModal("About Bathos", nullptr, ImGuiWindowFlags_NoResize)) {
        ImGui::PushStyleColor(ImGuiCol_Text, col4(C_PINK)); ImGui::TextUnformatted("Bathos " BATHOS_VERSION); ImGui::PopStyleColor();
        ImGui::TextWrapped("A picture in, its depth map out. Part of Atelier, Cursed Entertainment's creative suite.");
        ImGui::Spacing();
        ImGui::TextWrapped("Depth: Depth Anything V2 Small (Yang et al., Apache-2.0), run by ONNX Runtime (MIT). "
                           "Faces: YuNet (OpenCV Zoo, MIT) and MediaPipe Face Mesh (Google, Apache-2.0). "
                           "Edges: the guided filter (He, Sun and Tang). Window: SDL2, Dear ImGui, stb.");
        ImGui::Spacing();
        ImGui::TextDisabled("Model: %s", Depth::modelPath().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted("\xC2\xA9 Cursed Entertainment");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(-1, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

static void frame() {
    // the worker's result, picked up here (textures belong to this thread)
    {
        std::lock_guard<std::mutex> g(mtx);
        if (resultReady) {
            resultReady = false;
            if (havePic && resultFor == pic.path) { pic.depth = std::move(resultDepth); depthTexStale = true; }
        }
    }
    if (!busy) {
        if (rerun) { rerun = false; startDepth(); }
        else if (!pending.empty()) { auto f = std::move(pending); pending.clear(); dropFiles(f); }
    }
    if (depthTexStale && !pic.depth.empty()) {
        upload(depthTex, Depth::render(pic.depth, look).data(), pic.w, pic.h, Depth::channels(look));
        depthTexStale = false;
    }
    keys();
    menuBar();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float panelW = 280, top = ImGui::GetFrameHeight();
    // the controls on the left (as in Simulacrum and Firmament), the picture beside them
    panel(ImVec2(vp->Pos.x, vp->Pos.y + top), ImVec2(panelW, vp->Size.y - top));
    canvas(ImVec2(vp->Pos.x + panelW, vp->Pos.y + top), ImVec2(vp->Size.x - panelW, vp->Size.y - top));
    about();
}

int main(int argc, char** argv) {
    std::string file, capture;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--capture" && i + 1 < argc) capture = argv[++i];
        else file = a;
    }
#ifdef _WIN32
    {   // the file name as UTF-8 (argv is the ANSI code page)
        int n; LPWSTR* wa = CommandLineToArgvW(GetCommandLineW(), &n);
        if (wa) { for (int i = 1; i < n; ++i) { const std::string a = fromW(wa[i]); if (a == "--capture") ++i; else file = a; } LocalFree(wa); }
    }
#endif
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER) != 0) return 1;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (capture.empty() ? 0 : SDL_WINDOW_HIDDEN);
    window = SDL_CreateWindow("Bathos", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1400, 860, flags);
    if (!window) return 1;
    SDL_SetWindowMinimumSize(window, 820, 520);
    SDL_GLContext gl = SDL_GL_CreateContext(window);
    SDL_GL_MakeCurrent(window, gl);
    SDL_GL_SetSwapInterval(1);
    char* bp = SDL_GetBasePath();
    if (bp) { if (SDL_Surface* icon = SDL_LoadBMP((std::string(bp) + "assets/icon.bmp").c_str())) { SDL_SetWindowIcon(window, icon); SDL_FreeSurface(icon); } SDL_free(bp); }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    loadFont();
    applyStyle();
    ImGui_ImplSDL2_InitForOpenGL(window, gl);
    ImGui_ImplOpenGL3_Init("#version 330");
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    if (!file.empty()) openPicture(file);

    int frames = 0;
    std::vector<std::string> drops;
    while (!quit) {
        SDL_Event e;
        drops.clear();
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) quit = true;
            if (e.type == SDL_DROPFILE) { drops.push_back(e.drop.file); SDL_free(e.drop.file); }
        }
        dropFiles(drops);   // a multi-file drop arrives as several events in one go
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        frame();
        ImGui::Render();
        int w, h;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.1f, 0.1f, 0.1f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        ++frames;
        if (!capture.empty() && frames >= 8 && !busy && (!havePic || (!pic.depth.empty() && !depthTexStale))) {
            std::vector<unsigned char> px((size_t)w * h * 4);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            std::vector<unsigned char> flip(px.size());
            for (int y = 0; y < h; ++y) memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
            SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(flip.data(), w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
            SDL_SaveBMP(s, capture.c_str());
            SDL_FreeSurface(s);
            break;
        }
        SDL_GL_SwapWindow(window);
    }
    joinWorker();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_GL_DeleteContext(gl);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
