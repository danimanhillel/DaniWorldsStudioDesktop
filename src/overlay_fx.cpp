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

int OverlayKindCount() { return 26; }

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
    case 9: return "Glitch";
    case 10: return "Pixelate";
    case 11: return "Mirror";
    case 12: return "Invert";
    case 13: return "Sepia";
    case 14: return "Vignette";
    case 15: return "Wave";
    case 16: return "Mono";
    case 17: return "Fisheye";
    case 18: return "Light Leak";
    case 19: return "Dust";
    case 20: return "Cassette";
    case 21: return "Frame";
    case 22: return "Film Dust";
    case 23: return "Retro Dust";
    case 24: return "Old Film";
    case 25: return "Record";
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
    if (kind == 12 || kind == 13 || kind == 16) o.amount = 1.0f;
    else if (kind == 0 || kind == 5 || kind == 6 || kind == 7) o.amount = 0.65f;
    else o.amount = 0.75f;
    o.dir = 1;
    gOverlays.push_back(o);
    gOverlaySel = (int)gOverlays.size() - 1;
}

float OverlaySample(const std::vector<FxKey>& keys, float local, float fallback) {
    if (keys.empty()) return fallback;
    if (local <= keys.front().t) return local < keys.front().t - 0.0001f ? fallback : keys.front().v;
    if (local >= keys.back().t) return keys.back().v;
    int lo = 0;
    int hi = (int)keys.size() - 1;
    while (lo + 1 < hi) {
        int mid = (lo + hi) / 2;
        if (keys[mid].t <= local) lo = mid;
        else hi = mid;
    }
    float span = keys[hi].t - keys[lo].t;
    float u = span > 0.0001f ? (local - keys[lo].t) / span : 0.0f;
    return keys[lo].v + (keys[hi].v - keys[lo].v) * u;
}

void OverlayRecord(std::vector<FxKey>& keys, float local, float v) {
    if (local < 0) local = 0;
    for (auto it = keys.begin(); it != keys.end(); ++it) {
        if (std::fabs(it->t - local) <= 0.02f) { it->t = local; it->v = v; return; }
        if (it->t > local) { keys.insert(it, FxKey{local, v}); return; }
    }
    keys.push_back(FxKey{local, v});
}

void OverlayWrite(OverlayFx& o, int channel, float before, float now, float time) {
    if (channel < 0 || channel > 6) return;
    if (!o.holdOn[channel]) {
        o.hold[channel] = before;
        o.holdOn[channel] = true;
    }
    float local = time - o.start;
    if (local < 0) local = 0;
    if (local > o.duration) local = o.duration;
    OverlayRecord(o.anim[channel], local, now);
}

void OverlayResetAnim(OverlayFx& o) {
    for (int i = 0; i < 7; i++) {
        o.anim[i].clear();
        o.holdOn[i] = false;
        o.hold[i] = 0;
    }
}

void OverlayBox(const OverlayFx& o, float x0, float y0, float x1, float y1, float& ax, float& ay, float& bx, float& by, float time) {
    float fw = std::max(1.0f, x1 - x0);
    float fh = std::max(1.0f, y1 - y0);
    float ox = o.x, oy = o.y, osc = o.scale;
    if (time > -10000.0f) {
        float local = time - o.start;
        ox = OverlaySample(o.anim[0], local, o.holdOn[0] ? o.hold[0] : o.x);
        oy = OverlaySample(o.anim[1], local, o.holdOn[1] ? o.hold[1] : o.y);
        osc = OverlaySample(o.anim[2], local, o.holdOn[2] ? o.hold[2] : o.scale);
    }
    float sc = std::max(0.05f, osc);
    float cx = (x0 + x1) * 0.5f + ox * fw * 0.5f;
    float cy = (y0 + y1) * 0.5f + oy * fh * 0.5f;
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
    float mode = 0;
    float dir = 1;
};

static FxEval EvalFx(const OverlayFx& o, float time, float shortSide) {
    FxEval e;
    float dur = std::max(0.05f, o.duration);
    float local = (time - o.start) / dur;
    if (local < 0) local = 0;
    if (local > 1) local = 1;
    float eased = local * local * (3.0f - 2.0f * local);
    float localT = time - o.start;
    auto anim = [&](int ch, float field) {
        return OverlaySample(o.anim[ch], localT, o.holdOn[ch] ? o.hold[ch] : field);
    };
    float x = anim(0, o.x);
    float y = anim(1, o.y);
    float scale = anim(2, o.scale);
    float rot = anim(3, o.rot);
    float amount = anim(4, o.amount);
    float opacity = anim(5, o.opacity);
    float dir = anim(6, (float)o.dir);
    if (amount < 0) amount = 0;
    if (amount > 1) amount = 1;
    if (opacity < 0) opacity = 0;
    if (opacity > 1) opacity = 1;
    e.amount = amount;
    e.shiftX = x * 0.5f;
    e.shiftY = y * 0.5f;
    e.rot = rot * 0.01745329252f;
    e.scale = std::max(0.05f, scale);
    e.opacity = opacity;
    e.mode = (float)o.kind;
    e.dir = dir < 0 ? -1.0f : 1.0f;
    if (o.kind == 0) {
        float bucket = std::floor(time * 24.0f);
        float n1 = Hashf(bucket + 1.3f);
        float n2 = Hashf(bucket + 9.1f);
        float amp = amount * 0.06f;
        e.shiftX += (n1 - 0.5f) * 2.0f * amp;
        e.shiftY += (n2 - 0.5f) * 2.0f * amp;
        e.rot += (n1 - 0.5f) * amount * 0.10f;
    } else if (o.kind == 1) {
        float spin = dir < 0 ? -1.0f : 1.0f;
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
    } else if (o.kind == 20) {
        float n = Hashf(std::floor(time * 18.0f) + 2.2f);
        e.shiftX += (n - 0.5f) * amount * 0.010f;
    } else if (o.kind == 21 || o.kind == 24) {
        float bucket = std::floor(time * (o.kind == 24 ? 12.0f : 8.0f));
        float n1 = Hashf(bucket + 2.1f);
        float n2 = Hashf(bucket + 6.7f);
        float amp = amount * (o.kind == 24 ? 0.010f : 0.004f);
        e.shiftX += (n1 - 0.5f) * 2.0f * amp;
        e.shiftY += (n2 - 0.5f) * 2.0f * amp;
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
static int uMatrix = -1, uVhs = -1, uOpacity = -1, uAmount = -1, uTime = -1, uMode = -1, uDir = -1;
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
        "uniform float rot, scale, pass, radius, matrix, vhs, opacity, amount, time, mode, dir;\n"
        "float hn(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }\n"
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
        "  if (mode > 8.5 && mode < 9.5 && inside > 0.5) {\n"
        "    float bands = mix(6.0, 28.0, amount);\n"
        "    float band = floor(src.y * bands);\n"
        "    float gn = fract(sin(band * 17.13 + floor(time * 14.0) * 9.2) * 43758.5453);\n"
        "    if (gn > 0.62) src.x = clamp(src.x + (gn - 0.5) * amount * 0.45, 0.0, 1.0);\n"
        "  }\n"
        "  if (mode > 9.5 && mode < 10.5 && inside > 0.5) {\n"
        "    float cells = mix(140.0, 6.0, amount);\n"
        "    src = (floor(src * cells) + 0.5) / cells;\n"
        "  }\n"
        "  if (mode > 10.5 && mode < 11.5 && inside > 0.5) {\n"
        "    if (dir >= 0.0) { if (src.x > 0.5) src.x = 1.0 - src.x; }\n"
        "    else { if (src.y > 0.5) src.y = 1.0 - src.y; }\n"
        "  }\n"
        "  if (mode > 14.5 && mode < 15.5 && inside > 0.5) {\n"
        "    src.x += sin(src.y * mix(6.0, 22.0, amount) + time * 2.6) * amount * 0.045;\n"
        "    src.y += cos(src.x * mix(5.0, 16.0, amount) - time * 2.1) * amount * 0.028;\n"
        "    src = clamp(src, 0.0, 1.0);\n"
        "  }\n"
        "  if (mode > 16.5 && mode < 17.5) {\n"
        "    vec2 q = (src - 0.5) * 2.0;\n"
        "    float rr = length(q);\n"
        "    q *= 1.0 + amount * 0.75 * rr * rr;\n"
        "    src = q * 0.5 + 0.5;\n"
        "    inside = (src.x >= 0.0 && src.x <= 1.0 && src.y >= 0.0 && src.y <= 1.0) ? 1.0 : 0.0;\n"
        "  }\n"
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
        "      float tq = floor(time * 18.0);\n"
        "      float n1 = fract(sin(dot(gl_FragCoord.xy + vec2(tq * 19.0, tq * 7.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
        "      float n2 = fract(sin(dot(gl_FragCoord.yx + vec2(tq * 3.1, tq * 11.0), vec2(39.3467, 11.135))) * 24634.6345);\n"
        "      float spark = smoothstep(0.9984, 0.9997, n1) * (0.06 + 0.16 * n2) * (0.22 + amount * 0.35);\n"
        "      col += vec3(0.55, 0.52, 0.46) * spark;\n"
        "      vec2 cell = gl_FragCoord.xy * (0.22 + n2 * 0.18) + vec2(tq * 0.17, tq * 0.09);\n"
        "      vec2 cf = fract(cell) - 0.5;\n"
        "      float n3 = fract(sin(dot(floor(cell) + tq * vec2(5.0, 9.0), vec2(12.9898, 78.233))) * 43758.5453);\n"
        "      float rad = 0.06 + fract(n3 * 7.13) * 0.22;\n"
        "      float blob = smoothstep(0.988, 0.998, n3) * smoothstep(rad, rad * 0.2, length(cf));\n"
        "      float dust = blob * (0.05 + 0.12 * amount);\n"
        "      col = mix(col, col * (0.86 + 0.14 * n2) + vec3(0.22, 0.20, 0.16), dust);\n"
        "      col *= 1.0 - smoothstep(0.996, 0.9995, n2) * (0.03 + 0.06 * amount);\n"
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
        "    if (mode > 8.5 && mode < 9.5) {\n"
        "      float split = (1.0 + amount * 6.0) * texel.x;\n"
        "      col.r = mix(col.r, texture2D(tex, tuv + vec2(split, 0.0)).r, 0.85);\n"
        "      col.b = mix(col.b, texture2D(tex, tuv - vec2(split, 0.0)).b, 0.85);\n"
        "    }\n"
        "    if (mode > 11.5 && mode < 12.5) col = mix(col, vec3(1.0) - col, amount);\n"
        "    if (mode > 12.5 && mode < 13.5) {\n"
        "      float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
        "      col = mix(col, vec3(l * 1.08, l * 0.78, l * 0.48), amount);\n"
        "    }\n"
        "    if (mode > 13.5 && mode < 14.5) {\n"
        "      float vig = smoothstep(0.25, 0.85, distance(src, vec2(0.5)));\n"
        "      col *= 1.0 - vig * (0.25 + amount * 0.75);\n"
        "    }\n"
        "    if (mode > 15.5 && mode < 16.5) {\n"
        "      float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
        "      col = mix(col, vec3(l), amount);\n"
        "    }\n"
        "    if (mode > 17.5 && mode < 18.5) {\n"
        "      float sweep = fract(time * 0.08);\n"
        "      vec2 lc = vec2(mix(-0.15, 1.15, sweep), 0.42 + 0.18 * sin(time * 0.6));\n"
        "      float ld = length((src - lc) * vec2(0.65, 1.25));\n"
        "      float leak = exp(-ld * ld * mix(6.0, 2.2, amount));\n"
        "      col += vec3(1.0, 0.42, 0.10) * leak * (0.25 + amount * 0.7);\n"
        "    }\n"
        "    if (mode > 18.5 && mode < 19.5) {\n"
        "      float tq = floor(time * 12.0);\n"
        "      vec2 g = src * vec2(48.0, 28.0) + vec2(tq * 0.37, tq * 0.11);\n"
        "      vec2 f = fract(g) - 0.5;\n"
        "      float n = hn(floor(g) + vec2(tq, 0.0));\n"
        "      float spec = smoothstep(0.975, 0.995, n) * smoothstep(0.22, 0.02, length(f));\n"
        "      vec2 bg = src * vec2(9.0, 6.0) + vec2(time * 0.02, time * 0.01);\n"
        "      float big = hn(floor(src * vec2(9.0, 6.0)) + vec2(0.0, tq * 0.15));\n"
        "      float mote = smoothstep(0.92, 0.98, big) * smoothstep(0.45, 0.05, length(fract(bg) - 0.5));\n"
        "      col += vec3(0.85, 0.82, 0.75) * spec * (0.25 + amount * 0.9);\n"
        "      col = mix(col, col * 0.72 + vec3(0.55, 0.50, 0.42), mote * amount * 0.55);\n"
        "    }\n"
        "    if (mode > 19.5 && mode < 20.5) {\n"
        "      float wob = sin(src.y * 40.0 + time * 9.0) * amount * 0.004;\n"
        "      vec2 tv = vec2(clamp(src.x + wob, 0.0, 1.0), 1.0 - src.y);\n"
        "      float shiftPx = (1.0 + amount * 3.0) * texel.x;\n"
        "      col.r = mix(col.r, texture2D(tex, tv + vec2(shiftPx, 0.0)).r, 0.65);\n"
        "      col.b = mix(col.b, texture2D(tex, tv - vec2(shiftPx, 0.0)).b, 0.65);\n"
        "      col *= 0.94 + 0.06 * sin(gl_FragCoord.y * 2.4);\n"
        "      float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
        "      col = mix(vec3(l), col, 0.55);\n"
        "      col = col * (0.92 - amount * 0.08) + vec3(0.08, 0.045, 0.02);\n"
        "      if (abs(src.y - fract(time * 0.22)) < 0.008) col += vec3(0.15, 0.10, 0.06) * amount;\n"
        "    }\n"
        "    if (mode > 20.5 && mode < 21.5) {\n"
        "      float sideL = 1.0 - smoothstep(0.045, 0.062, src.x);\n"
        "      float sideR = smoothstep(0.938, 0.955, src.x);\n"
        "      float side = max(sideL, sideR);\n"
        "      float row = fract(src.y * 11.0);\n"
        "      float holeY = (row >= 0.30 && row <= 0.70) ? 1.0 : 0.0;\n"
        "      float holeL = (src.x >= 0.010 && src.x <= 0.046) ? 1.0 : 0.0;\n"
        "      float holeR = (src.x >= 0.954 && src.x <= 0.990) ? 1.0 : 0.0;\n"
        "      float hole = holeY * max(holeL, holeR);\n"
        "      col = mix(col, vec3(0.02, 0.015, 0.012), side * (0.55 + amount * 0.45));\n"
        "      col = mix(col, vec3(0.82, 0.80, 0.74), hole);\n"
        "    }\n"
        "    if (mode > 21.5 && mode < 24.5) {\n"
        "      float tq = floor(time * 8.0);\n"
        "      float s1 = hn(vec2(tq, 1.7));\n"
        "      float s2 = hn(vec2(tq, 9.2));\n"
        "      float sc = smoothstep(0.0015, 0.0, abs(src.x - s1)) * (hn(vec2(tq, 3.0)) > 0.15 ? 1.0 : 0.0);\n"
        "      sc += smoothstep(0.0008, 0.0, abs(src.x - s2)) * (hn(vec2(floor(src.y * 30.0), tq)) < 0.70 ? 1.0 : 0.0);\n"
        "      float scratchGain = mode < 22.5 ? (0.35 + amount * 0.65) : (mode < 23.5 ? (0.20 + amount * 0.35) : (0.45 + amount * 0.55));\n"
        "      if (mode > 22.5 && mode < 23.5) {\n"
        "        float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
        "        vec3 faded = vec3(l * 1.05 + 0.06, l * 0.78 + 0.05, l * 0.62 + 0.08);\n"
        "        col = mix(col, faded, 0.35 + amount * 0.50);\n"
        "        col = col * 0.90 + vec3(0.06, 0.04, 0.05);\n"
        "        float vig = smoothstep(0.35, 0.85, distance(src, vec2(0.5)));\n"
        "        col *= 1.0 - vig * (0.15 + amount * 0.35);\n"
        "      }\n"
        "      if (mode > 23.5) {\n"
        "        float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
        "        col = mix(col, vec3(l * 1.05, l * 0.82, l * 0.55), 0.55 + amount * 0.40);\n"
        "        col *= 0.86 + 0.14 * hn(vec2(floor(time * 24.0), 4.4));\n"
        "        float vig = smoothstep(0.25, 0.82, distance(src, vec2(0.5)));\n"
        "        col *= 1.0 - vig * (0.25 + amount * 0.55);\n"
        "      }\n"
        "      col = mix(col, vec3(0.92, 0.90, 0.86), clamp(sc, 0.0, 1.0) * scratchGain);\n"
        "      float density = mode < 22.5 ? 0.85 : (mode < 23.5 ? 0.45 : 1.0);\n"
        "      vec2 dg = src * vec2(36.0, 22.0) + vec2(tq * 0.21, tq * 0.07);\n"
        "      float dn = hn(floor(dg) + vec2(0.0, tq));\n"
        "      float thresh = 0.992 - density * 0.03;\n"
        "      float spec = smoothstep(thresh, thresh + 0.012, dn) * smoothstep(0.28, 0.0, length(fract(dg) - 0.5));\n"
        "      col = mix(col, vec3(1.0, 0.96, 0.90), spec * (0.35 + amount));\n"
        "      float bn = hn(floor(src * vec2(70.0, 40.0)) + vec2(tq, 0.0));\n"
        "      col *= 1.0 - smoothstep(0.997, 1.0, bn) * amount * 0.8;\n"
        "    }\n"
        "    if (mode > 24.5 && mode < 25.5) {\n"
        "      float rad = length(src - vec2(0.5));\n"
        "      float groove = 0.5 + 0.5 * sin(rad * mix(90.0, 220.0, amount) - time * 1.6);\n"
        "      col *= 1.0 - (0.04 + amount * 0.08) * groove;\n"
        "      float l = dot(col, vec3(0.299, 0.587, 0.114));\n"
        "      col = mix(col, vec3(l * 0.95, l * 0.78, l * 0.55), 0.25 + amount * 0.35);\n"
        "      float hole = 1.0 - smoothstep(0.02, 0.045, rad);\n"
        "      col = mix(col, vec3(0.08, 0.06, 0.05), hole * 0.85);\n"
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
    uMode = pGetUniform(fxProg, "mode");
    uDir = pGetUniform(fxProg, "dir");
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
    if (uMode >= 0) pUniform1f(uMode, e.mode);
    if (uDir >= 0) pUniform1f(uDir, e.dir);
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
    if (selectedLive >= 0) {
        const OverlayFx& o = gOverlays[selectedLive];
        float ax, ay, bx, by;
        OverlayBox(o, x0, y0, x1, y1, ax, ay, bx, by, time);
        glEnable(GL_SCISSOR_TEST);
        glScissor(rx, gy, rw, rh);
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
    glDisable(GL_SCISSOR_TEST);
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

static float Hash2(float a, float b) {
    return Fract(std::sin(a * 127.1f + b * 311.7f) * 43758.5453f);
}

static void FilmGrade(int mode, float amount, float time, float sx, float sy,
                      int x, int y, int w, int h, const unsigned char* src,
                      float& r, float& g, float& b) {
    auto luma = [&]() { return r * 0.299f + g * 0.587f + b * 0.114f; };
    auto scratchDust = [&](float density, float scratchGain) {
        float tq = std::floor(time * 8.0f);
        float s1 = Hash2(tq, 1.7f);
        float s2 = Hash2(tq, 9.2f);
        float sc = 0.0f;
        float d1 = std::fabs(sx - s1);
        if (d1 < 0.0015f && Hash2(tq, 3.0f) > 0.15f) sc += 1.0f - d1 / 0.0015f;
        float d2 = std::fabs(sx - s2);
        if (d2 < 0.0008f && Hash2(std::floor(sy * 30.0f), tq) < 0.70f) sc += 1.0f - d2 / 0.0008f;
        if (sc > 1.0f) sc = 1.0f;
        float add = sc * scratchGain;
        r = r * (1.0f - add) + 0.92f * add;
        g = g * (1.0f - add) + 0.90f * add;
        b = b * (1.0f - add) + 0.86f * add;
        float gx = sx * 36.0f + tq * 0.21f;
        float gy = sy * 22.0f + tq * 0.07f;
        float fx = gx - std::floor(gx) - 0.5f;
        float fy = gy - std::floor(gy) - 0.5f;
        float n = Hash2(std::floor(gx), std::floor(gy) + tq);
        float dist = std::sqrt(fx * fx + fy * fy);
        float thresh = 0.992f - density * 0.03f;
        float spec = 0.0f;
        if (n > thresh) spec = std::min(1.0f, (n - thresh) / 0.012f);
        if (dist >= 0.28f) spec = 0.0f;
        else spec *= (0.28f - dist) / 0.28f;
        float spot = spec * (0.35f + amount);
        r = r * (1.0f - spot) + spot;
        g = g * (1.0f - spot) + spot * 0.96f;
        b = b * (1.0f - spot) + spot * 0.90f;
        float bn = Hash2(std::floor(sx * 70.0f) + tq, std::floor(sy * 40.0f));
        if (bn > 0.997f) {
            float dark = std::min(1.0f, (bn - 0.997f) / 0.003f) * amount * 0.8f;
            r *= 1.0f - dark;
            g *= 1.0f - dark;
            b *= 1.0f - dark;
        }
    };
    if (mode == 19) {
        float tq = std::floor(time * 12.0f);
        float gx = sx * 48.0f + tq * 0.37f;
        float gy = sy * 28.0f + tq * 0.11f;
        float fx = gx - std::floor(gx) - 0.5f;
        float fy = gy - std::floor(gy) - 0.5f;
        float n = Hash2(std::floor(gx) + tq, std::floor(gy));
        float dist = std::sqrt(fx * fx + fy * fy);
        float spec = 0.0f;
        if (n > 0.975f) spec = std::min(1.0f, (n - 0.975f) / 0.020f);
        if (dist >= 0.22f) spec = 0.0f;
        else spec *= (0.22f - dist) / 0.20f;
        float bgx = sx * 9.0f + time * 0.02f;
        float bgy = sy * 6.0f + time * 0.01f;
        float bfx = bgx - std::floor(bgx) - 0.5f;
        float bfy = bgy - std::floor(bgy) - 0.5f;
        float big = Hash2(std::floor(sx * 9.0f), std::floor(sy * 6.0f) + tq * 0.15f);
        float md = std::sqrt(bfx * bfx + bfy * bfy);
        float mote = 0.0f;
        if (big > 0.92f) mote = std::min(1.0f, (big - 0.92f) / 0.06f);
        if (md >= 0.45f) mote = 0.0f;
        else mote *= (0.45f - md) / 0.40f;
        float add = spec * (0.25f + amount * 0.9f);
        r += 0.85f * add;
        g += 0.82f * add;
        b += 0.75f * add;
        float m = mote * amount * 0.55f;
        r = r * (1.0f - m) + (r * 0.72f + 0.55f) * m;
        g = g * (1.0f - m) + (g * 0.72f + 0.50f) * m;
        b = b * (1.0f - m) + (b * 0.72f + 0.42f) * m;
    } else if (mode == 20) {
        float wob = std::sin(sy * 40.0f + time * 9.0f) * amount * 0.004f;
        float uu = std::clamp(sx + wob, 0.0f, 1.0f);
        float rr, gg, bb, br, bg, b2;
        float shift = 1.0f + amount * 3.0f;
        SampleRGBA(src, w, h, uu * (float)(w - 1) + shift, sy * (float)(h - 1), rr, gg, bb);
        SampleRGBA(src, w, h, uu * (float)(w - 1) - shift, sy * (float)(h - 1), br, bg, b2);
        (void)gg; (void)bb; (void)br; (void)bg;
        r = r * 0.35f + rr * 0.65f;
        b = b * 0.35f + b2 * 0.65f;
        float scan = 0.94f + 0.06f * std::sin((float)y * 2.4f);
        r *= scan; g *= scan; b *= scan;
        float l = luma();
        r = l * 0.45f + r * 0.55f;
        g = l * 0.45f + g * 0.55f;
        b = l * 0.45f + b * 0.55f;
        float gain = 0.92f - amount * 0.08f;
        r = r * gain + 0.08f;
        g = g * gain + 0.045f;
        b = b * gain + 0.02f;
        if (std::fabs(sy - Fract(time * 0.22f)) < 0.008f) {
            r += 0.15f * amount;
            g += 0.10f * amount;
            b += 0.06f * amount;
        }
    } else if (mode == 21) {
        float sideL = sx < 0.045f ? 1.0f : (sx > 0.062f ? 0.0f : (0.062f - sx) / 0.017f);
        float sideR = sx > 0.955f ? 1.0f : (sx < 0.938f ? 0.0f : (sx - 0.938f) / 0.017f);
        float side = std::max(sideL, sideR);
        float row = sy * 11.0f;
        row -= std::floor(row);
        float holeY = (row >= 0.30f && row <= 0.70f) ? 1.0f : 0.0f;
        float holeL = (sx >= 0.010f && sx <= 0.046f) ? 1.0f : 0.0f;
        float holeR = (sx >= 0.954f && sx <= 0.990f) ? 1.0f : 0.0f;
        float hole = holeY * std::max(holeL, holeR);
        float k = side * (0.55f + amount * 0.45f);
        r = r * (1.0f - k) + 0.02f * k;
        g = g * (1.0f - k) + 0.015f * k;
        b = b * (1.0f - k) + 0.012f * k;
        r = r * (1.0f - hole) + 0.82f * hole;
        g = g * (1.0f - hole) + 0.80f * hole;
        b = b * (1.0f - hole) + 0.74f * hole;
    } else if (mode == 22) {
        scratchDust(0.85f, 0.35f + amount * 0.65f);
    } else if (mode == 23) {
        float l = luma();
        float k = 0.35f + amount * 0.50f;
        r = r * (1.0f - k) + (l * 1.05f + 0.06f) * k;
        g = g * (1.0f - k) + (l * 0.78f + 0.05f) * k;
        b = b * (1.0f - k) + (l * 0.62f + 0.08f) * k;
        r = r * 0.90f + 0.06f;
        g = g * 0.90f + 0.04f;
        b = b * 0.90f + 0.05f;
        float dxv = sx - 0.5f, dyv = sy - 0.5f;
        float dist = std::sqrt(dxv * dxv + dyv * dyv);
        float vig = dist <= 0.35f ? 0.0f : (dist >= 0.85f ? 1.0f : (dist - 0.35f) / 0.50f);
        float mul = 1.0f - vig * (0.15f + amount * 0.35f);
        r *= mul; g *= mul; b *= mul;
        scratchDust(0.45f, 0.20f + amount * 0.35f);
    } else if (mode == 24) {
        float l = luma();
        float k = 0.55f + amount * 0.40f;
        r = r * (1.0f - k) + l * 1.05f * k;
        g = g * (1.0f - k) + l * 0.82f * k;
        b = b * (1.0f - k) + l * 0.55f * k;
        float flick = 0.86f + 0.14f * Hash2(std::floor(time * 24.0f), 4.4f);
        r *= flick; g *= flick; b *= flick;
        float dxv = sx - 0.5f, dyv = sy - 0.5f;
        float dist = std::sqrt(dxv * dxv + dyv * dyv);
        float vig = dist <= 0.25f ? 0.0f : (dist >= 0.82f ? 1.0f : (dist - 0.25f) / 0.57f);
        float mul = 1.0f - vig * (0.25f + amount * 0.55f);
        r *= mul; g *= mul; b *= mul;
        scratchDust(1.0f, 0.45f + amount * 0.55f);
    } else if (mode == 25) {
        float dxv = sx - 0.5f, dyv = sy - 0.5f;
        float rad = std::sqrt(dxv * dxv + dyv * dyv);
        float groove = 0.5f + 0.5f * std::sin(rad * (90.0f + amount * 130.0f) - time * 1.6f);
        float gmul = 1.0f - (0.04f + amount * 0.08f) * groove;
        r *= gmul; g *= gmul; b *= gmul;
        float l = luma();
        float k = 0.25f + amount * 0.35f;
        r = r * (1.0f - k) + l * 0.95f * k;
        g = g * (1.0f - k) + l * 0.78f * k;
        b = b * (1.0f - k) + l * 0.55f * k;
        if (rad < 0.045f) {
            float hole = rad <= 0.02f ? 1.0f : (0.045f - rad) / 0.025f;
            hole *= 0.85f;
            r = r * (1.0f - hole) + 0.08f * hole;
            g = g * (1.0f - hole) + 0.06f * hole;
            b = b * (1.0f - hole) + 0.05f * hole;
        }
    }
    (void)x;
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
                int mode = (int)std::lround(e.mode);
                if (inside && mode == 9) {
                    float bands = 6.0f + e.amount * 22.0f;
                    float band = std::floor(sy * bands);
                    float gn = Fract(std::sin(band * 17.13f + std::floor(time * 14.0f) * 9.2f) * 43758.5453f);
                    if (gn > 0.62f) sx = std::clamp(sx + (gn - 0.5f) * e.amount * 0.45f, 0.0f, 1.0f);
                }
                if (inside && mode == 10) {
                    float cells = 140.0f + (6.0f - 140.0f) * e.amount;
                    sx = (std::floor(sx * cells) + 0.5f) / cells;
                    sy = (std::floor(sy * cells) + 0.5f) / cells;
                }
                if (inside && mode == 11) {
                    if (e.dir >= 0.0f) { if (sx > 0.5f) sx = 1.0f - sx; }
                    else { if (sy > 0.5f) sy = 1.0f - sy; }
                }
                if (inside && mode == 15) {
                    sx += std::sin(sy * (6.0f + e.amount * 16.0f) + time * 2.6f) * e.amount * 0.045f;
                    sy += std::cos(sx * (5.0f + e.amount * 11.0f) - time * 2.1f) * e.amount * 0.028f;
                    sx = std::clamp(sx, 0.0f, 1.0f);
                    sy = std::clamp(sy, 0.0f, 1.0f);
                }
                if (mode == 17) {
                    float qx = (sx - 0.5f) * 2.0f;
                    float qy = (sy - 0.5f) * 2.0f;
                    float rr = std::sqrt(qx * qx + qy * qy);
                    float f = 1.0f + e.amount * 0.75f * rr * rr;
                    sx = qx * f * 0.5f + 0.5f;
                    sy = qy * f * 0.5f + 0.5f;
                    inside = sx >= 0.0f && sx <= 1.0f && sy >= 0.0f && sy <= 1.0f;
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
                        float tq = std::floor(time * 18.0f);
                        float n1 = Fract(std::sin((x + tq * 19.0f) * 12.9898f + (y + tq * 7.0f) * 78.233f) * 43758.5453f);
                        float n2 = Fract(std::sin((y + tq * 3.1f) * 39.3467f + (x + tq * 11.0f) * 11.135f) * 24634.6345f);
                        float spark = 0.0f;
                        if (n1 > 0.9984f) spark = ((n1 - 0.9984f) / 0.0013f);
                        if (spark > 1.0f) spark = 1.0f;
                        spark *= (0.06f + 0.16f * n2) * (0.22f + e.amount * 0.35f);
                        r += 0.55f * spark; g += 0.52f * spark; b += 0.46f * spark;
                        float cellx = x * (0.22f + n2 * 0.18f) + tq * 0.17f;
                        float celly = y * (0.22f + n2 * 0.18f) + tq * 0.09f;
                        float cfx = cellx - std::floor(cellx) - 0.5f;
                        float cfy = celly - std::floor(celly) - 0.5f;
                        float n3 = Fract(std::sin((std::floor(cellx) + tq * 5.0f) * 12.9898f + (std::floor(celly) + tq * 9.0f) * 78.233f) * 43758.5453f);
                        float rad = 0.06f + Fract(n3 * 7.13f) * 0.22f;
                        float dist = std::sqrt(cfx * cfx + cfy * cfy);
                        float blob = 0.0f;
                        if (n3 > 0.988f) blob = std::min(1.0f, (n3 - 0.988f) / 0.010f);
                        float edge = rad - dist;
                        float span = std::max(0.001f, rad * 0.8f);
                        float fall = edge <= 0 ? 0.0f : (edge >= span ? 1.0f : edge / span);
                        blob *= fall;
                        float dust = blob * (0.05f + 0.12f * e.amount);
                        r = r * (1.0f - dust) + (r * (0.86f + 0.14f * n2) + 0.22f) * dust;
                        g = g * (1.0f - dust) + (g * (0.86f + 0.14f * n2) + 0.20f) * dust;
                        b = b * (1.0f - dust) + (b * (0.86f + 0.14f * n2) + 0.16f) * dust;
                        float dark = n2 > 0.996f ? std::min(1.0f, (n2 - 0.996f) / 0.0035f) * (0.03f + 0.06f * e.amount) : 0.0f;
                        r *= 1.0f - dark; g *= 1.0f - dark; b *= 1.0f - dark;
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
                    if (mode == 9) {
                        float rr, gg, bb, br, bg, b2;
                        float split = 1.0f + e.amount * 6.0f;
                        SampleRGBA(work.data(), w, h, sx * (float)(w - 1) + split, sy * (float)(h - 1), rr, gg, bb);
                        SampleRGBA(work.data(), w, h, sx * (float)(w - 1) - split, sy * (float)(h - 1), br, bg, b2);
                        (void)gg; (void)bb; (void)br; (void)bg;
                        r = r * 0.15f + rr * 0.85f;
                        b = b * 0.15f + b2 * 0.85f;
                    }
                    if (mode == 12) {
                        r = r * (1.0f - e.amount) + (1.0f - r) * e.amount;
                        g = g * (1.0f - e.amount) + (1.0f - g) * e.amount;
                        b = b * (1.0f - e.amount) + (1.0f - b) * e.amount;
                    }
                    if (mode == 13) {
                        float l = r * 0.299f + g * 0.587f + b * 0.114f;
                        r = r * (1.0f - e.amount) + l * 1.08f * e.amount;
                        g = g * (1.0f - e.amount) + l * 0.78f * e.amount;
                        b = b * (1.0f - e.amount) + l * 0.48f * e.amount;
                    }
                    if (mode == 14) {
                        float dxv = sx - 0.5f, dyv = sy - 0.5f;
                        float dist = std::sqrt(dxv * dxv + dyv * dyv);
                        float vig = dist <= 0.25f ? 0.0f : (dist >= 0.85f ? 1.0f : (dist - 0.25f) / 0.60f);
                        float mul = 1.0f - vig * (0.25f + e.amount * 0.75f);
                        r *= mul; g *= mul; b *= mul;
                    }
                    if (mode == 16) {
                        float l = r * 0.299f + g * 0.587f + b * 0.114f;
                        r = r * (1.0f - e.amount) + l * e.amount;
                        g = g * (1.0f - e.amount) + l * e.amount;
                        b = b * (1.0f - e.amount) + l * e.amount;
                    }
                    if (mode == 18) {
                        float sweep = Fract(time * 0.08f);
                        float lx = -0.15f + 1.30f * sweep;
                        float ly = 0.42f + 0.18f * std::sin(time * 0.6f);
                        float ldx = (sx - lx) * 0.65f;
                        float ldy = (sy - ly) * 1.25f;
                        float ld = ldx * ldx + ldy * ldy;
                        float tight = 6.0f + (2.2f - 6.0f) * e.amount;
                        float leak = std::exp(-ld * tight);
                        float gain = leak * (0.25f + e.amount * 0.7f);
                        r += gain; g += 0.42f * gain; b += 0.10f * gain;
                    }
                    if (mode >= 19 && mode <= 25)
                        FilmGrade(mode, e.amount, time, sx, sy, x, y, w, h, work.data(), r, g, b);
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
