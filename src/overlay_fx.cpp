#include "overlay_fx.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

std::vector<OverlayFx> gOverlays;
int gOverlaySel = -1;

void OverlayClear() {
    gOverlays.clear();
    gOverlaySel = -1;
}

int OverlayKindCount() { return 9; }

const char* OverlayKindName(int kind) {
    switch (kind) {
    case 0: return "Shake";
    case 1: return "Rotate";
    case 2: return "Matrix";
    case 3: return "Zoom In";
    case 4: return "Zoom Out";
    case 5: return "Blur";
    case 6: return "Blur In";
    case 7: return "Blur Out";
    case 8: return "VHS";
    default: return "Effect";
    }
}

void OverlayAdd(int kind, float start) {
    if (kind < 0 || kind >= OverlayKindCount()) kind = 0;
    if (start < 0) start = 0;
    OverlayFx o;
    o.kind = kind;
    o.start = start;
    o.duration = 4.0f;
    o.scale = 1.0f;
    o.opacity = 1.0f;
    o.amount = (kind == 0 || kind == 5 || kind == 6 || kind == 7) ? 0.65f : 0.75f;
    o.dir = 1;
    gOverlays.push_back(o);
    gOverlaySel = (int)gOverlays.size() - 1;
}

void OverlayBox(const OverlayFx& o, float x0, float y0, float x1, float y1, float& ax, float& ay, float& bx, float& by) {
    float fw = std::max(1.0f, x1 - x0);
    float fh = std::max(1.0f, y1 - y0);
    float sc = std::max(0.05f, o.scale);
    float cx = (x0 + x1) * 0.5f + o.x * fw * 0.5f;
    float cy = (y0 + y1) * 0.5f + o.y * fh * 0.5f;
    float hw = fw * 0.5f * sc;
    float hh = fh * 0.5f * sc;
    ax = cx - hw;
    ay = cy - hh;
    bx = cx + hw;
    by = cy + hh;
}

static float Fract(float x) { return x - std::floor(x); }

static float Hashf(float n) {
    return Fract(std::sin(n * 12.9898f) * 43758.5453f);
}

struct FxEval {
    float shiftX = 0;
    float shiftY = 0;
    float rot = 0;
    float scale = 1;
    float radius = 0;
    int matrix = 0;
    int vhs = 0;
    float opacity = 1;
    float amount = 0.7f;
};

static FxEval EvalFx(const OverlayFx& o, float time, float shortSide) {
    FxEval e;
    float dur = std::max(0.05f, o.duration);
    float local = (time - o.start) / dur;
    if (local < 0) local = 0;
    if (local > 1) local = 1;
    float eased = local * local * (3.0f - 2.0f * local);
    float amount = o.amount;
    if (amount < 0) amount = 0;
    if (amount > 1) amount = 1;
    e.amount = amount;
    e.shiftX = o.x * 0.5f;
    e.shiftY = o.y * 0.5f;
    e.rot = o.rot * 0.01745329252f;
    e.scale = std::max(0.05f, o.scale);
    e.opacity = o.opacity < 0 ? 0 : (o.opacity > 1 ? 1 : o.opacity);
    if (o.kind == 0) {
        float bucket = std::floor(time * 24.0f);
        float n1 = Hashf(bucket + 1.3f);
        float n2 = Hashf(bucket + 9.1f);
        float amp = amount * 0.06f;
        e.shiftX += (n1 - 0.5f) * 2.0f * amp;
        e.shiftY += (n2 - 0.5f) * 2.0f * amp;
        e.rot += (n1 - 0.5f) * amount * 0.10f;
    } else if (o.kind == 1) {
        float spin = o.dir < 0 ? -1.0f : 1.0f;
        e.rot += spin * local * 6.2831853f * (0.35f + amount * 1.75f);
    } else if (o.kind == 2) {
        e.matrix = 1;
    } else if (o.kind == 3) {
        e.scale *= 1.0f + amount * 1.55f * eased;
    } else if (o.kind == 4) {
        float z0 = 1.0f + amount * 1.55f;
        e.scale *= z0 + (1.0f - z0) * eased;
    } else if (o.kind == 5) {
        e.radius = amount * 0.05f * shortSide;
    } else if (o.kind == 6) {
        e.radius = amount * eased * 0.05f * shortSide;
    } else if (o.kind == 7) {
        e.radius = amount * (1.0f - eased) * 0.05f * shortSide;
    } else if (o.kind == 8) {
        e.vhs = 1;
        float n = Hashf(std::floor(time * 12.0f) + 4.0f);
        e.shiftY += (n - 0.5f) * amount * 0.012f;
    }
    return e;
}

typedef unsigned int (APIENTRY *PFN_glCreateShader_t)(unsigned int);
typedef void (APIENTRY *PFN_glShaderSource_t)(unsigned int, int, const char* const*, const int*);
typedef void (APIENTRY *PFN_glCompileShader_t)(unsigned int);
typedef unsigned int (APIENTRY *PFN_glCreateProgram_t)(void);
typedef void (APIENTRY *PFN_glAttachShader_t)(unsigned int, unsigned int);
typedef void (APIENTRY *PFN_glLinkProgram_t)(unsigned int);
typedef void (APIENTRY *PFN_glDeleteShader_t)(unsigned int);
typedef void (APIENTRY *PFN_glGetShaderiv_t)(unsigned int, unsigned int, int*);
typedef void (APIENTRY *PFN_glGetProgramiv_t)(unsigned int, unsigned int, int*);
typedef int (APIENTRY *PFN_glGetUniformLocation_t)(unsigned int, const char*);
typedef void (APIENTRY *PFN_glUniform1f_t)(int, float);
typedef void (APIENTRY *PFN_glUniform1i_t)(int, int);
typedef void (APIENTRY *PFN_glUniform2f_t)(int, float, float);
typedef void (APIENTRY *PFN_glUseProgram_t)(unsigned int);

static PFN_glCreateShader_t pCreateShader = nullptr;
static PFN_glShaderSource_t pShaderSource = nullptr;
static PFN_glCompileShader_t pCompileShader = nullptr;
static PFN_glCreateProgram_t pCreateProgram = nullptr;
static PFN_glAttachShader_t pAttachShader = nullptr;
static PFN_glLinkProgram_t pLinkProgram = nullptr;
static PFN_glDeleteShader_t pDeleteShader = nullptr;
static PFN_glGetShaderiv_t pGetShaderiv = nullptr;
static PFN_glGetProgramiv_t pGetProgramiv = nullptr;
static PFN_glGetUniformLocation_t pGetUniform = nullptr;
static PFN_glUniform1f_t pUniform1f = nullptr;
static PFN_glUniform1i_t pUniform1i = nullptr;
static PFN_glUniform2f_t pUniform2f = nullptr;
static PFN_glUseProgram_t pUse = nullptr;

static GLuint fxProg = 0;
static int fxReady = 0;
static int uTex = -1, uTexel = -1, uShift = -1, uRot = -1, uScale = -1, uPass = -1, uRadius = -1;
static int uMatrix = -1, uVhs = -1, uOpacity = -1, uAmount = -1, uTime = -1;
static GLuint grabA = 0, grabB = 0;
static int grabW = 0, grabH = 0;

static bool CompileStage(unsigned int type, const char* src, unsigned int& out) {
    out = pCreateShader(type);
    pShaderSource(out, 1, &src, nullptr);
    pCompileShader(out);
    int ok = 0;
    pGetShaderiv(out, 0x8B81, &ok);
    return ok == 1;
}

static bool EnsureFxShader() {
    if (fxReady == 1) return fxProg != 0;
    if (fxReady == -1) return false;
    fxReady = -1;
    pCreateShader = (PFN_glCreateShader_t)SDL_GL_GetProcAddress("glCreateShader");
    pShaderSource = (PFN_glShaderSource_t)SDL_GL_GetProcAddress("glShaderSource");
    pCompileShader = (PFN_glCompileShader_t)SDL_GL_GetProcAddress("glCompileShader");
    pCreateProgram = (PFN_glCreateProgram_t)SDL_GL_GetProcAddress("glCreateProgram");
    pAttachShader = (PFN_glAttachShader_t)SDL_GL_GetProcAddress("glAttachShader");
    pLinkProgram = (PFN_glLinkProgram_t)SDL_GL_GetProcAddress("glLinkProgram");
    pDeleteShader = (PFN_glDeleteShader_t)SDL_GL_GetProcAddress("glDeleteShader");
    pGetShaderiv = (PFN_glGetShaderiv_t)SDL_GL_GetProcAddress("glGetShaderiv");
    pGetProgramiv = (PFN_glGetProgramiv_t)SDL_GL_GetProcAddress("glGetProgramiv");
    pGetUniform = (PFN_glGetUniformLocation_t)SDL_GL_GetProcAddress("glGetUniformLocation");
    pUniform1f = (PFN_glUniform1f_t)SDL_GL_GetProcAddress("glUniform1f");
    pUniform1i = (PFN_glUniform1i_t)SDL_GL_GetProcAddress("glUniform1i");
    pUniform2f = (PFN_glUniform2f_t)SDL_GL_GetProcAddress("glUniform2f");
    pUse = (PFN_glUseProgram_t)SDL_GL_GetProcAddress("glUseProgram");
    if (!pCreateShader || !pShaderSource || !pCompileShader || !pCreateProgram || !pAttachShader || !pLinkProgram || !pGetShaderiv || !pGetProgramiv || !pGetUniform || !pUniform1f || !pUniform1i || !pUniform2f || !pUse)
        return false;
    const char* vs =
        "#version 130\n"
        "varying vec2 vUV;\n"
        "void main() {\n"
        "  gl_Position = ftransform();\n"
        "  vUV = gl_MultiTexCoord0.xy;\n"
        "}\n";
    const char* fs =
        "#version 130\n"
        "varying vec2 vUV;\n"
        "uniform sampler2D tex;\n"
        "uniform vec2 texel;\n"
        "uniform vec2 shift;\n"
        "uniform float rot, scale, pass, radius, matrix, vhs, opacity, amount, time;\n"
        "void main() {\n"
        "  if (pass > 0.5) {\n"
        "    vec2 step = pass < 1.5 ? vec2(texel.x, 0.0) : vec2(0.0, texel.y);\n"
        "    float span = max(radius, 0.0) / 6.0;\n"
        "    vec3 acc = vec3(0.0);\n"
        "    float wsum = 0.0;\n"
        "    for (int i = -6; i <= 6; i++) {\n"
        "      float w = exp(-float(i * i) / 8.0);\n"
        "      acc += texture2D(tex, vUV + step * (float(i) * span)).rgb * w;\n"
        "      wsum += w;\n"
        "    }\n"
        "    gl_FragColor = vec4(acc / max(wsum, 0.0001), 1.0);\n"
        "    return;\n"
        "  }\n"
        "  vec2 p = vec2(vUV.x, 1.0 - vUV.y);\n"
        "  vec2 d = p - vec2(0.5) - shift;\n"
        "  float cs = cos(-rot);\n"
        "  float sn = sin(-rot);\n"
        "  d = vec2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) / max(scale, 0.05);\n"
        "  vec2 src = d + vec2(0.5);\n"
        "  float inside = (src.x >= 0.0 && src.x <= 1.0 && src.y >= 0.0 && src.y <= 1.0) ? 1.0 : 0.0;\n"
        "  if (matrix > 0.5 && inside > 0.5) src = fract(src * 3.0);\n"
        "  vec3 base = texture2D(tex, vUV).rgb;\n"
        "  vec3 col = vec3(0.0);\n"
        "  if (inside > 0.5) {\n"
        "    vec2 tuv = vec2(src.x, 1.0 - src.y);\n"
        "    col = texture2D(tex, tuv).rgb;\n"
        "    if (vhs > 0.5) {\n"
        "      float shiftPx = (1.5 + amount * 5.0) * texel.x;\n"
        "      float cr = texture2D(tex, tuv + vec2(shiftPx, 0.0)).r;\n"
        "      float cb = texture2D(tex, tuv - vec2(shiftPx, 0.0)).b;\n"
        "      col.r = mix(col.r, cr, 0.9);\n"
        "      col.b = mix(col.b, cb, 0.9);\n"
        "      float scan = 0.90 + 0.10 * sin(gl_FragCoord.y * 1.7);\n"
        "      col *= scan;\n"
        "      float n = fract(sin(dot(floor(gl_FragCoord.xy * 0.45) + floor(time * 11.0) * 17.0, vec2(12.9898, 78.233))) * 43758.5453);\n"
        "      if (n > 0.996) col = vec3(0.92, 0.92, 0.88);\n"
        "      else if (n > 0.989) col *= 0.15;\n"
        "      float bar = fract(time * 0.15);\n"
        "      if (abs(src.y - bar) < 0.015) {\n"
        "        float tear = (fract(sin(floor(time * 7.0) * 9.2) * 43758.5) - 0.5) * amount * 0.10;\n"
        "        vec2 tv = vec2(clamp(src.x + tear, 0.0, 1.0), 1.0 - src.y);\n"
        "        col = mix(col, texture2D(tex, tv).rgb, 0.7) + vec3(0.12, 0.10, 0.08);\n"
        "      }\n"
        "      float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
        "      col = mix(vec3(l), col, 0.72);\n"
        "      col = col * 0.90 + vec3(0.05, 0.035, 0.06);\n"
        "    }\n"
        "  }\n"
        "  vec3 outc = inside > 0.5 ? col : vec3(0.0);\n"
        "  outc = mix(base, outc, opacity);\n"
        "  gl_FragColor = vec4(clamp(outc, 0.0, 1.0), 1.0);\n"
        "}\n";
    unsigned int vert = 0, frag = 0;
    if (!CompileStage(0x8B31, vs, vert) || !CompileStage(0x8B30, fs, frag)) {
        if (pDeleteShader) {
            if (vert) pDeleteShader(vert);
            if (frag) pDeleteShader(frag);
        }
        return false;
    }
    fxProg = pCreateProgram();
    pAttachShader(fxProg, vert);
    pAttachShader(fxProg, frag);
    pLinkProgram(fxProg);
    int ok = 0;
    pGetProgramiv(fxProg, 0x8B82, &ok);
    if (pDeleteShader) { pDeleteShader(vert); pDeleteShader(frag); }
    if (!ok) { fxProg = 0; return false; }
    uTex = pGetUniform(fxProg, "tex");
    uTexel = pGetUniform(fxProg, "texel");
    uShift = pGetUniform(fxProg, "shift");
    uRot = pGetUniform(fxProg, "rot");
    uScale = pGetUniform(fxProg, "scale");
    uPass = pGetUniform(fxProg, "pass");
    uRadius = pGetUniform(fxProg, "radius");
    uMatrix = pGetUniform(fxProg, "matrix");
    uVhs = pGetUniform(fxProg, "vhs");
    uOpacity = pGetUniform(fxProg, "opacity");
    uAmount = pGetUniform(fxProg, "amount");
    uTime = pGetUniform(fxProg, "time");
    fxReady = 1;
    return true;
}

static void EnsureGrab(int w, int h) {
    if (!grabA) glGenTextures(1, &grabA);
    if (!grabB) glGenTextures(1, &grabB);
    if (w == grabW && h == grabH) return;
    GLuint ids[2] = { grabA, grabB };
    for (int i = 0; i < 2; i++) {
        glBindTexture(GL_TEXTURE_2D, ids[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
    grabW = w;
    grabH = h;
}

static void CopyGrab(GLuint tex, int rx, int gy, int rw, int rh) {
    glBindTexture(GL_TEXTURE_2D, tex);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, rx, gy, rw, rh);
}

static void DrawFxQuad(float x0, float y0, float x1, float y1, const FxEval& e, float pass, float time, int rw, int rh) {
    pUse(fxProg);
    if (uTex >= 0) pUniform1i(uTex, 0);
    if (uTexel >= 0) pUniform2f(uTexel, rw > 0 ? 1.0f / (float)rw : 0, rh > 0 ? 1.0f / (float)rh : 0);
    if (uShift >= 0) pUniform2f(uShift, e.shiftX, e.shiftY);
    if (uRot >= 0) pUniform1f(uRot, e.rot);
    if (uScale >= 0) pUniform1f(uScale, e.scale);
    if (uPass >= 0) pUniform1f(uPass, pass);
    if (uRadius >= 0) pUniform1f(uRadius, e.radius);
    if (uMatrix >= 0) pUniform1f(uMatrix, e.matrix ? 1.0f : 0.0f);
    if (uVhs >= 0) pUniform1f(uVhs, e.vhs ? 1.0f : 0.0f);
    if (uOpacity >= 0) pUniform1f(uOpacity, e.opacity);
    if (uAmount >= 0) pUniform1f(uAmount, e.amount);
    if (uTime >= 0) pUniform1f(uTime, time);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 1); glVertex2f(x0, y0);
    glTexCoord2f(1, 1); glVertex2f(x1, y0);
    glTexCoord2f(1, 0); glVertex2f(x1, y1);
    glTexCoord2f(0, 0); glVertex2f(x0, y1);
    glEnd();
}

static void ApplyOneGL(const OverlayFx& o, float x0, float y0, float x1, float y1, int rx, int gy, int rw, int rh, float time) {
    FxEval e = EvalFx(o, time, (float)std::min(rw, rh));
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    CopyGrab(grabA, rx, gy, rw, rh);
    glEnable(GL_SCISSOR_TEST);
    glScissor(rx, gy, rw, rh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, grabA);
    DrawFxQuad(x0, y0, x1, y1, e, 0, time, rw, rh);
    if (e.radius >= 0.8f) {
        glDisable(GL_SCISSOR_TEST);
        CopyGrab(grabB, rx, gy, rw, rh);
        glEnable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, grabB);
        DrawFxQuad(x0, y0, x1, y1, e, 1, time, rw, rh);
        glDisable(GL_SCISSOR_TEST);
        CopyGrab(grabA, rx, gy, rw, rh);
        glEnable(GL_SCISSOR_TEST);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindTexture(GL_TEXTURE_2D, grabA);
        DrawFxQuad(x0, y0, x1, y1, e, 2, time, rw, rh);
    }
}

void OverlayDrawGL(float x0, float y0, float x1, float y1, float time, int screenW, int screenH) {
    if (gOverlays.empty() || x1 - x0 < 4.0f || y1 - y0 < 4.0f || screenW < 2 || screenH < 2) return;
    bool any = false;
    int selectedLive = -1;
    for (int i = 0; i < (int)gOverlays.size(); i++) {
        const OverlayFx& o = gOverlays[i];
        if (time < o.start || time >= o.start + o.duration) continue;
        any = true;
        if (i == gOverlaySel) selectedLive = i;
    }
    if (!any) return;
    if (!EnsureFxShader()) return;
    int rx = (int)std::floor(x0);
    int ry = (int)std::floor(y0);
    int rw = (int)std::ceil(x1) - rx;
    int rh = (int)std::ceil(y1) - ry;
    if (rw < 2 || rh < 2) return;
    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > screenW) rw = screenW - rx;
    if (ry + rh > screenH) rh = screenH - ry;
    if (rw < 2 || rh < 2) return;
    int gy = screenH - (ry + rh);
    if (gy < 0) gy = 0;

    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0, screenW, screenH, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    EnsureGrab(rw, rh);
    for (const OverlayFx& o : gOverlays) {
        if (time < o.start || time >= o.start + o.duration) continue;
        ApplyOneGL(o, (float)rx, (float)ry, (float)(rx + rw), (float)(ry + rh), rx, gy, rw, rh, time);
    }
    if (pUse) pUse(0);
    glDisable(GL_TEXTURE_2D);
    glDisable(GL_SCISSOR_TEST);
    if (selectedLive >= 0) {
        const OverlayFx& o = gOverlays[selectedLive];
        float ax, ay, bx, by;
        OverlayBox(o, x0, y0, x1, y1, ax, ay, bx, by);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glColor4f(1, 1, 1, 0.95f);
        glBegin(GL_LINE_LOOP);
        glVertex2f(ax, ay); glVertex2f(bx, ay); glVertex2f(bx, by); glVertex2f(ax, by);
        glEnd();
        float hx = (ax + bx) * 0.5f;
        float hy = ay + 16.0f;
        glBegin(GL_LINE_LOOP);
        glVertex2f(hx - 5, hy - 5); glVertex2f(hx + 5, hy - 5); glVertex2f(hx + 5, hy + 5); glVertex2f(hx - 5, hy + 5);
        glEnd();
        glBegin(GL_QUADS);
        glVertex2f(bx - 8, by - 8); glVertex2f(bx, by - 8); glVertex2f(bx, by); glVertex2f(bx - 8, by);
        glEnd();
        glDisable(GL_BLEND);
        glColor4f(1, 1, 1, 1);
    }
    glPopMatrix();
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
}

static void SampleRGBA(const unsigned char* src, int w, int h, float fx, float fy, float& r, float& g, float& b) {
    if (fx < 0 || fy < 0 || fx > (float)(w - 1) || fy > (float)(h - 1)) { r = g = b = 0; return; }
    int x0 = (int)fx;
    int y0 = (int)fy;
    int x1 = std::min(w - 1, x0 + 1);
    int y1 = std::min(h - 1, y0 + 1);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    auto at = [&](int x, int y, int c) -> float {
        return src[((size_t)y * w + x) * 4 + c] / 255.0f;
    };
    for (int c = 0; c < 3; c++) {
        float v = at(x0, y0, c) * (1 - tx) * (1 - ty) + at(x1, y0, c) * tx * (1 - ty)
                + at(x0, y1, c) * (1 - tx) * ty + at(x1, y1, c) * tx * ty;
        if (c == 0) r = v;
        else if (c == 1) g = v;
        else b = v;
    }
}

static void BoxBlur(const unsigned char* src, unsigned char* dst, int w, int h, int radius) {
    if (radius < 1) {
        std::memcpy(dst, src, (size_t)w * h * 4);
        return;
    }
    std::vector<unsigned char> tmp((size_t)w * h * 4);
    int diam = radius * 2 + 1;
    for (int y = 0; y < h; y++) {
        int sum[3] = {0, 0, 0};
        const unsigned char* row = src + (size_t)y * w * 4;
        for (int k = -radius; k <= radius; k++) {
            int xx = std::clamp(k, 0, w - 1);
            sum[0] += row[xx * 4];
            sum[1] += row[xx * 4 + 1];
            sum[2] += row[xx * 4 + 2];
        }
        for (int x = 0; x < w; x++) {
            unsigned char* o = tmp.data() + ((size_t)y * w + x) * 4;
            o[0] = (unsigned char)(sum[0] / diam);
            o[1] = (unsigned char)(sum[1] / diam);
            o[2] = (unsigned char)(sum[2] / diam);
            o[3] = 255;
            int remove = std::clamp(x - radius, 0, w - 1);
            int add = std::clamp(x + radius + 1, 0, w - 1);
            sum[0] += row[add * 4] - row[remove * 4];
            sum[1] += row[add * 4 + 1] - row[remove * 4 + 1];
            sum[2] += row[add * 4 + 2] - row[remove * 4 + 2];
        }
    }
    for (int x = 0; x < w; x++) {
        int sum[3] = {0, 0, 0};
        for (int k = -radius; k <= radius; k++) {
            int yy = std::clamp(k, 0, h - 1);
            const unsigned char* p = tmp.data() + ((size_t)yy * w + x) * 4;
            sum[0] += p[0];
            sum[1] += p[1];
            sum[2] += p[2];
        }
        for (int y = 0; y < h; y++) {
            unsigned char* o = dst + ((size_t)y * w + x) * 4;
            o[0] = (unsigned char)(sum[0] / diam);
            o[1] = (unsigned char)(sum[1] / diam);
            o[2] = (unsigned char)(sum[2] / diam);
            o[3] = 255;
            int remove = std::clamp(y - radius, 0, h - 1);
            int add = std::clamp(y + radius + 1, 0, h - 1);
            const unsigned char* pr = tmp.data() + ((size_t)remove * w + x) * 4;
            const unsigned char* pa = tmp.data() + ((size_t)add * w + x) * 4;
            sum[0] += pa[0] - pr[0];
            sum[1] += pa[1] - pr[1];
            sum[2] += pa[2] - pr[2];
        }
    }
}

void OverlayStamp(unsigned char* canvas, int w, int h, float time) {
    if (!canvas || w < 2 || h < 2) return;
    bool any = false;
    for (const OverlayFx& o : gOverlays) {
        if (time >= o.start && time < o.start + o.duration) { any = true; break; }
    }
    if (!any) return;
    const size_t bytes = (size_t)w * (size_t)h * 4;
    std::vector<unsigned char> work(canvas, canvas + bytes);
    std::vector<unsigned char> dst(bytes);
    std::vector<unsigned char> scratch;
    float shortSide = (float)std::min(w, h);
    for (const OverlayFx& o : gOverlays) {
        if (time < o.start || time >= o.start + o.duration) continue;
        FxEval e = EvalFx(o, time, shortSide);
        float cs = std::cos(-e.rot);
        float sn = std::sin(-e.rot);
        float bar = Fract(time * 0.15f);
        float tear = (Fract(std::sin(std::floor(time * 7.0f) * 9.2f) * 43758.5f) - 0.5f) * e.amount * 0.10f;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float u = ((float)x + 0.5f) / (float)w;
                float v = ((float)y + 0.5f) / (float)h;
                float dx = u - 0.5f - e.shiftX;
                float dy = v - 0.5f - e.shiftY;
                float rx = (dx * cs - dy * sn) / std::max(0.05f, e.scale);
                float ry = (dx * sn + dy * cs) / std::max(0.05f, e.scale);
                float sx = rx + 0.5f;
                float sy = ry + 0.5f;
                bool inside = sx >= 0.0f && sx <= 1.0f && sy >= 0.0f && sy <= 1.0f;
                if (inside && e.matrix) {
                    sx = Fract(sx * 3.0f);
                    sy = Fract(sy * 3.0f);
                }
                float r = 0, g = 0, b = 0;
                if (inside) {
                    SampleRGBA(work.data(), w, h, sx * (float)(w - 1), sy * (float)(h - 1), r, g, b);
                    if (e.vhs) {
                        float rr, gg, bb, br, bg, b2;
                        float shift = 1.5f + e.amount * 5.0f;
                        SampleRGBA(work.data(), w, h, sx * (float)(w - 1) + shift, sy * (float)(h - 1), rr, gg, bb);
                        SampleRGBA(work.data(), w, h, sx * (float)(w - 1) - shift, sy * (float)(h - 1), br, bg, b2);
                        (void)gg; (void)bb; (void)br; (void)bg;
                        r = r * 0.1f + rr * 0.9f;
                        b = b * 0.1f + b2 * 0.9f;
                        float scan = 0.90f + 0.10f * std::sin((float)y * 1.7f);
                        r *= scan; g *= scan; b *= scan;
                        float n = Fract(std::sin((std::floor(x * 0.45f) + std::floor(y * 0.45f) * 13.0f + std::floor(time * 11.0f) * 17.0f) * 12.9898f) * 43758.5453f);
                        if (n > 0.996f) { r = 0.92f; g = 0.92f; b = 0.88f; }
                        else if (n > 0.989f) { r *= 0.15f; g *= 0.15f; b *= 0.15f; }
                        if (std::fabs(sy - bar) < 0.015f) {
                            float tr, tg, tb;
                            float tx = std::clamp(sx + tear, 0.0f, 1.0f) * (float)(w - 1);
                            SampleRGBA(work.data(), w, h, tx, sy * (float)(h - 1), tr, tg, tb);
                            r = r * 0.3f + tr * 0.7f + 0.12f;
                            g = g * 0.3f + tg * 0.7f + 0.10f;
                            b = b * 0.3f + tb * 0.7f + 0.08f;
                        }
                        float l = r * 0.299f + g * 0.587f + b * 0.114f;
                        r = l * 0.28f + r * 0.72f;
                        g = l * 0.28f + g * 0.72f;
                        b = l * 0.28f + b * 0.72f;
                        r = r * 0.90f + 0.05f;
                        g = g * 0.90f + 0.035f;
                        b = b * 0.90f + 0.06f;
                    }
                }
                const unsigned char* base = work.data() + ((size_t)y * w + x) * 4;
                float k = e.opacity;
                auto sat = [](float v) { return (unsigned char)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
                unsigned char* out = dst.data() + ((size_t)y * w + x) * 4;
                out[0] = sat(base[0] / 255.0f * (1.0f - k) + r * k);
                out[1] = sat(base[1] / 255.0f * (1.0f - k) + g * k);
                out[2] = sat(base[2] / 255.0f * (1.0f - k) + b * k);
                out[3] = 255;
            }
        }
        if (e.radius >= 0.8f) {
            int rad = (int)std::lround(e.radius);
            if (rad < 1) rad = 1;
            if (rad > 64) rad = 64;
            scratch.resize(bytes);
            BoxBlur(dst.data(), scratch.data(), w, h, rad);
            work.swap(scratch);
        } else {
            work.swap(dst);
        }
    }
    std::memcpy(canvas, work.data(), bytes);
}
