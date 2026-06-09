// Butterscotch Xbox 360 — XDK Entry Point
// Uses official Xbox 360 SDK: D3D9, XAudio2, XInputGetState

#include <xtl.h>
#include <d3d9.h>
#include <d3dx9.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>

// DbgPrint is a C-linkage kernel function — declare it explicitly since
// we compile .c files as C++ and removed extern "C" wrappers.
extern "C" ULONG __cdecl DbgPrint(const char* format, ...);

// Core headers — compiled as C++ alongside the .c files (via /TP flag)
#include "runner.h"
#include "runner_keyboard.h"
#include "vm.h"
#include "data_win.h"
#include "json_reader.h"
#include "utils.h"
#include "stb_ds.h"

#include "d3d9_renderer.h"
#include "xaudio2_audio.h"
#include "xdk_file_system.h"
#include "debug_font/debug_font.h"
#include "stb_image.h"

// ===[ POSIX clock polyfill implementation ]===
double _xdk_monotonic_ms(void) {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)freq.QuadPart * 1000.0;
}

// Screen dimensions (720p native)
#define SCREEN_WIDTH  1280
#define SCREEN_HEIGHT 720

static HANDLE gDiagLog = INVALID_HANDLE_VALUE;
static FILE* gDiagFile = NULL;
static bool gDiagTriedFallback = false;
static char gLastParseChunk[5] = "NONE";
static int gLastParseChunkIndex = -1;
static int gLastParseChunkTotal = 0;

struct LoadingVertex {
    float x, y, z, w;
    float u, v;
    float r, g, b, a;
};

typedef struct LoadingScreen {
    IDirect3DDevice9* dev;
    IDirect3DTexture9* splashTex;
    IDirect3DTexture9* fontTex;
    IDirect3DTexture9* whiteTex;
    IDirect3DVertexShader9* vertexShader;
    IDirect3DPixelShader9* pixelShader;
    IDirect3DVertexDeclaration9* vertexDecl;
    int splashW;
    int splashH;
    bool available;
    char stage[128];
} LoadingScreen;

static LoadingScreen gLoadingScreen;

static bool diagOpenPath(const char* path, bool overwrite) {
    FILE* f = fopen(path, overwrite ? "wb" : "ab");
    if (f) {
        if (gDiagFile) fclose(gDiagFile);
        if (gDiagLog != INVALID_HANDLE_VALUE) {
            CloseHandle(gDiagLog);
            gDiagLog = INVALID_HANDLE_VALUE;
        }
        gDiagFile = f;
        return true;
    }
    int crtErr = errno;

    HANDLE h = CreateFileA(path,
                           GENERIC_WRITE,
                           FILE_SHARE_READ,
                           NULL,
                           overwrite ? CREATE_ALWAYS : OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL,
                           NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DbgPrint("BS: open log failed at %s errno=%d gle=%lu\n", path, crtErr, GetLastError());
        return false;
    }
    if (!overwrite) SetFilePointer(h, 0, NULL, FILE_END);
    if (gDiagFile) {
        fclose(gDiagFile);
        gDiagFile = NULL;
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) CloseHandle(gDiagLog);
    gDiagLog = h;
    return true;
}

static void diagLog(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 2, fmt, args);
    va_end(args);
    line[sizeof(line) - 2] = '\0';

    size_t len = strlen(line);
    if (len == 0 || line[len - 1] != '\n') {
        line[len++] = '\n';
        line[len] = '\0';
    }

    DbgPrint("%s", line);
    if (gDiagFile) {
        fputs(line, gDiagFile);
        fflush(gDiagFile);
    }
    if (gDiagLog != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        WriteFile(gDiagLog, line, (DWORD)strlen(line), &written, NULL);
        FlushFileBuffers(gDiagLog);
    }
}

static void diagOpenFallback(void) {
    if (gDiagLog != INVALID_HANDLE_VALUE || gDiagFile || gDiagTriedFallback) return;
    gDiagTriedFallback = true;

    static const char* paths[] = {
        "game:\\bs360_refresh.log",
        "d:\\bs360_refresh.log",
        "hdd:\\bs360_refresh.log",
        "cache:\\bs360_refresh.log",
        "uda:\\bs360_refresh.log",
        "uda:/bs360_refresh.log",
        "usb0:\\bs360_refresh.log",
        NULL,
    };
    for (int i = 0; paths[i]; i++) {
        if (diagOpenPath(paths[i], true)) {
            diagLog("BS: fallback log opened at %s", paths[i]);
            return;
        }
    }
    DbgPrint("BS: WARNING: no writable diagnostic log path found\n");
}

static void diagOpenNextToDataWin(const char* dataWinPath) {
    if (gDiagFile || gDiagLog != INVALID_HANDLE_VALUE) {
        diagLog("BS: keeping existing log while data.win is at %s", dataWinPath);
        return;
    }

    char logPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen >= sizeof(logPath) - 32) return;
        memcpy(logPath, dataWinPath, dirLen);
        strcpy(logPath + dirLen, "bs360_refresh.log");
    } else {
        strcpy(logPath, "bs360_refresh.log");
    }

    if (diagOpenPath(logPath, true)) {
        diagLog("BS: logging to %s", logPath);
    } else {
        diagLog("BS: WARNING: failed to open log next to data.win at %s gle=%lu", logPath, GetLastError());
    }
}

static void loadingSetVertex(LoadingVertex* v, float x, float y, float u, float vv,
                             float r, float g, float b, float a) {
    v->x = x - 0.5f;
    v->y = y - 0.5f;
    v->z = 0.0f;
    v->w = 1.0f;
    v->u = u;
    v->v = vv;
    v->r = r;
    v->g = g;
    v->b = b;
    v->a = a;
}

static void loadingApplyState(LoadingScreen* ls) {
    IDirect3DDevice9* dev = ls->dev;
    D3DVIEWPORT9 vp;
    vp.X = 0;
    vp.Y = 0;
    vp.Width = SCREEN_WIDTH;
    vp.Height = SCREEN_HEIGHT;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    dev->SetViewport(&vp);
    dev->SetVertexShader(ls->vertexShader);
    dev->SetPixelShader(ls->pixelShader);
    dev->SetVertexDeclaration(ls->vertexDecl);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_VIEWPORTENABLE, FALSE);
    for (DWORD sampler = 0; sampler < 8; sampler++) {
        dev->SetSamplerState(sampler, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_MIPFILTER, D3DTEXF_POINT);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(sampler, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
}

static IDirect3DTexture9* loadingCreateTextureFromRgba(IDirect3DDevice9* dev, const uint8_t* pixels, int w, int h) {
    IDirect3DTexture9* tex = NULL;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_LIN_A8R8G8B8, D3DPOOL_DEFAULT, &tex, NULL)) || !tex) {
        return NULL;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0))) {
        tex->Release();
        return NULL;
    }

    for (int y = 0; y < h; y++) {
        const uint8_t* src = pixels + y * w * 4;
        DWORD* dst = (DWORD*)((uint8_t*)lr.pBits + y * lr.Pitch);
        for (int x = 0; x < w; x++) {
            uint8_t r = src[x * 4 + 0];
            uint8_t g = src[x * 4 + 1];
            uint8_t b = src[x * 4 + 2];
            uint8_t a = src[x * 4 + 3];
            if (a == 0) { r = 0; g = 0; b = 0; }
            dst[x] = D3DCOLOR_ARGB(a, r, g, b);
        }
    }

    tex->UnlockRect(0);
    return tex;
}

static IDirect3DTexture9* loadingLoadPng(IDirect3DDevice9* dev, const char* path, int* outW, int* outH) {
    int w = 0;
    int h = 0;
    int channels = 0;
    uint8_t* pixels = stbi_load(path, &w, &h, &channels, 4);
    if (!pixels) return NULL;
    IDirect3DTexture9* tex = loadingCreateTextureFromRgba(dev, pixels, w, h);
    stbi_image_free(pixels);
    if (tex) {
        *outW = w;
        *outH = h;
    }
    return tex;
}

static IDirect3DTexture9* loadingCreateFontTexture(IDirect3DDevice9* dev) {
    uint8_t* rgba = (uint8_t*)malloc(DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H * 4);
    if (!rgba) return NULL;
    for (int i = 0; i < DEBUGFONT_ATLAS_W * DEBUGFONT_ATLAS_H; i++) {
        uint8_t a = debugFontPixels[i];
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = a;
    }
    IDirect3DTexture9* tex = loadingCreateTextureFromRgba(dev, rgba, DEBUGFONT_ATLAS_W, DEBUGFONT_ATLAS_H);
    free(rgba);
    return tex;
}

static bool loadingInit(LoadingScreen* ls, IDirect3DDevice9* dev, const char* dataWinPath) {
    memset(ls, 0, sizeof(*ls));
    ls->dev = dev;
    strcpy(ls->stage, "Starting");

    static const char* vs =
        "struct VS_IN  { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "struct VS_OUT { float4 Pos : POSITION; float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "VS_OUT main(VS_IN i) { VS_OUT o; o.Pos = i.Pos; o.Tex = i.Tex; o.Col = i.Col; return o; }\n";
    static const char* ps =
        "sampler2D s0 : register(s0) = sampler_state { MinFilter = POINT; MagFilter = POINT; MipFilter = POINT; AddressU = CLAMP; AddressV = CLAMP; };\n"
        "struct PS_IN { float2 Tex : TEXCOORD0; float4 Col : TEXCOORD1; };\n"
        "float4 main(PS_IN i) : COLOR0 { return tex2D(s0, i.Tex) * i.Col; }\n";

    ID3DXBuffer* code = NULL;
    ID3DXBuffer* err = NULL;
    HRESULT hr = D3DXCompileShader(vs, (UINT)strlen(vs), NULL, NULL, "main", "vs_2_0", 0, &code, &err, NULL);
    if (FAILED(hr)) {
        if (err) err->Release();
        diagLog("LOAD: vertex shader compile failed hr=0x%08X", hr);
        return false;
    }
    dev->CreateVertexShader((const DWORD*)code->GetBufferPointer(), &ls->vertexShader);
    code->Release();

    hr = D3DXCompileShader(ps, (UINT)strlen(ps), NULL, NULL, "main", "ps_2_0", 0, &code, &err, NULL);
    if (FAILED(hr)) {
        if (err) err->Release();
        diagLog("LOAD: pixel shader compile failed hr=0x%08X", hr);
        return false;
    }
    dev->CreatePixelShader((const DWORD*)code->GetBufferPointer(), &ls->pixelShader);
    code->Release();

    static const D3DVERTEXELEMENT9 decl[] = {
        { 0,  0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 },
        { 0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 },
        { 0, 24, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1 },
        D3DDECL_END()
    };
    if (FAILED(dev->CreateVertexDeclaration(decl, &ls->vertexDecl))) {
        diagLog("LOAD: vertex declaration failed");
        return false;
    }

    ls->fontTex = loadingCreateFontTexture(dev);
    if (!ls->fontTex) diagLog("LOAD: debug font texture failed");
    {
        uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        ls->whiteTex = loadingCreateTextureFromRgba(dev, whitePixel, 1, 1);
    }

    char splashPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        if (dirLen < sizeof(splashPath) - 16) {
            memcpy(splashPath, dataWinPath, dirLen);
            strcpy(splashPath + dirLen, "splash.png");
            ls->splashTex = loadingLoadPng(dev, splashPath, &ls->splashW, &ls->splashH);
        }
    }
    diagLog("LOAD: splash %s", ls->splashTex ? "loaded" : "not found");

    ls->available = (ls->vertexShader && ls->pixelShader && ls->vertexDecl);
    return ls->available;
}

static void loadingDestroy(LoadingScreen* ls) {
    if (ls->dev) {
        ls->dev->SetTexture(0, NULL);
        ls->dev->SetVertexShader(NULL);
        ls->dev->SetPixelShader(NULL);
        ls->dev->SetVertexDeclaration(NULL);
    }
    if (ls->splashTex) ls->splashTex->Release();
    if (ls->fontTex) ls->fontTex->Release();
    if (ls->whiteTex) ls->whiteTex->Release();
    if (ls->vertexShader) ls->vertexShader->Release();
    if (ls->pixelShader) ls->pixelShader->Release();
    if (ls->vertexDecl) ls->vertexDecl->Release();
    memset(ls, 0, sizeof(*ls));
}

static void loadingDrawQuad(LoadingScreen* ls, IDirect3DTexture9* tex,
                            float x0, float y0, float x1, float y1,
                            float u0, float v0, float u1, float v1,
                            float r, float g, float b, float a) {
    LoadingVertex verts[4];
    loadingSetVertex(&verts[0], x0, y0, u0, v0, r, g, b, a);
    loadingSetVertex(&verts[1], x1, y0, u1, v0, r, g, b, a);
    loadingSetVertex(&verts[2], x1, y1, u1, v1, r, g, b, a);
    loadingSetVertex(&verts[3], x0, y1, u0, v1, r, g, b, a);
    ls->dev->SetTexture(0, tex ? tex : ls->whiteTex);
    ls->dev->DrawPrimitiveUP(D3DPT_QUADLIST, 1, verts, sizeof(LoadingVertex));
}

static void loadingDrawText(LoadingScreen* ls, const char* text, float x, float y, float scale,
                            float r, float g, float b, float a) {
    if (!ls->fontTex || !text) return;
    float penX = x;
    for (const char* p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '\n') {
            penX = x;
            y += (float)DEBUGFONT_LINE_HEIGHT * scale;
            continue;
        }
        if (ch < DEBUGFONT_FIRST_CP || ch > DEBUGFONT_LAST_CP) ch = '?';
        const DebugFontGlyphEntry* glyph = &debugFontGlyphs[ch - DEBUGFONT_FIRST_CP];
        float gx0 = penX + (float)glyph->xoffset * scale;
        float gy0 = y + (float)glyph->yoffset * scale;
        float gx1 = gx0 + (float)glyph->w * scale;
        float gy1 = gy0 + (float)glyph->h * scale;
        float u0 = ((float)glyph->x + 0.5f) / (float)DEBUGFONT_ATLAS_W;
        float v0 = ((float)glyph->y + 0.5f) / (float)DEBUGFONT_ATLAS_H;
        float u1 = ((float)glyph->x + (float)glyph->w - 0.5f) / (float)DEBUGFONT_ATLAS_W;
        float v1 = ((float)glyph->y + (float)glyph->h - 0.5f) / (float)DEBUGFONT_ATLAS_H;
        loadingDrawQuad(ls, ls->fontTex, gx0, gy0, gx1, gy1, u0, v0, u1, v1, r, g, b, a);
        penX += (float)glyph->xadvance * scale;
    }
}

static float loadingTextWidth(const char* text, float scale) {
    float w = 0.0f;
    if (!text) return w;
    for (const char* p = text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch < DEBUGFONT_FIRST_CP || ch > DEBUGFONT_LAST_CP) ch = '?';
        w += (float)debugFontGlyphs[ch - DEBUGFONT_FIRST_CP].xadvance * scale;
    }
    return w;
}

static void loadingDraw(LoadingScreen* ls, float progress, const char* stage) {
    if (!ls || !ls->available) return;
    if (stage && stage[0]) {
        _snprintf(ls->stage, sizeof(ls->stage) - 1, "%s", stage);
        ls->stage[sizeof(ls->stage) - 1] = '\0';
    }
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    IDirect3DDevice9* dev = ls->dev;
    dev->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);
    if (FAILED(dev->BeginScene())) return;
    loadingApplyState(ls);

    if (ls->splashTex && ls->splashW > 0 && ls->splashH > 0) {
        float scaleX = (float)SCREEN_WIDTH / (float)ls->splashW;
        float scaleY = (float)SCREEN_HEIGHT / (float)ls->splashH;
        float scale = (scaleX < scaleY) ? scaleX : scaleY;
        float w = (float)ls->splashW * scale;
        float h = (float)ls->splashH * scale;
        float x = ((float)SCREEN_WIDTH - w) * 0.5f;
        float y = ((float)SCREEN_HEIGHT - h) * 0.5f;
        loadingDrawQuad(ls, ls->splashTex, x, y, x + w, y + h, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    }

    float barW = 720.0f;
    float barH = 18.0f;
    float barX = ((float)SCREEN_WIDTH - barW) * 0.5f;
    float barY = (float)SCREEN_HEIGHT - 96.0f;
    loadingDrawQuad(ls, NULL, barX - 3.0f, barY - 3.0f, barX + barW + 3.0f, barY + barH + 3.0f,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.70f);
    loadingDrawQuad(ls, NULL, barX, barY, barX + barW, barY + barH,
                    0.0f, 0.0f, 1.0f, 1.0f, 0.12f, 0.12f, 0.12f, 0.95f);
    loadingDrawQuad(ls, NULL, barX, barY, barX + barW * progress, barY + barH,
                    0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.73f, 0.18f, 1.0f);

    float textScale = 0.42f;
    float textW = loadingTextWidth(ls->stage, textScale);
    loadingDrawText(ls, ls->stage, ((float)SCREEN_WIDTH - textW) * 0.5f, barY + 30.0f,
                    textScale, 1.0f, 1.0f, 1.0f, 0.92f);

    dev->EndScene();
    dev->Present(NULL, NULL, NULL, NULL);
}

extern "C" void Butterscotch_xdkAbort(const char* file, int line) {
    diagOpenFallback();
    diagLog("BS: FATAL abort at %s:%d lastChunk=%s index=%d/%d", file ? file : "(null)", line, gLastParseChunk, gLastParseChunkIndex, gLastParseChunkTotal);
    for (;;) {
        Sleep(1000);
    }
}

extern "C" void Butterscotch_xdkDataWinTrace(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    diagLog("DW: %s", line);
}

extern "C" void Butterscotch_xdkDiagTrace(const char* fmt, ...) {
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(line, sizeof(line) - 1, fmt, args);
    va_end(args);
    line[sizeof(line) - 1] = '\0';
    diagLog("%s", line);
}

static void dataWinParseProgress(const char* chunkName, int chunkIndex, int totalChunks, DataWin* dataWin, void* userData) {
    (void)dataWin;
    memcpy(gLastParseChunk, chunkName, 4);
    gLastParseChunk[4] = '\0';
    gLastParseChunkIndex = chunkIndex;
    gLastParseChunkTotal = totalChunks;
    diagLog("PARSE chunk %d/%d %.4s", chunkIndex + 1, totalChunks, chunkName);
    LoadingScreen* loading = (LoadingScreen*)userData;
    if (loading && loading->available) {
        char stage[128];
        _snprintf(stage, sizeof(stage) - 1, "Loading data.win: %.4s %d/%d", chunkName, chunkIndex + 1, totalChunks);
        stage[sizeof(stage) - 1] = '\0';
        float progress = (totalChunks > 0) ? ((float)(chunkIndex + 1) / (float)totalChunks) : 0.0f;
        loadingDraw(loading, progress * 0.82f, stage);
    }
}

static DataWin* parseDataWinGuarded(const char* dataWinPath, DataWinParserOptions parseOpts) {
    DataWin* dataWin = NULL;
    unsigned int exceptionCode = 0;
    __try {
        dataWin = DataWin_parse(dataWinPath, parseOpts);
    } __except (exceptionCode = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        diagLog("BS: FATAL exception 0x%08X during DataWin_parse lastChunk=%s index=%d/%d", exceptionCode, gLastParseChunk, gLastParseChunkIndex, gLastParseChunkTotal);
        dataWin = NULL;
    }
    return dataWin;
}

// ===[ Controller Mapping ]===

typedef struct {
    WORD xpadButton;
    int32_t gmlKey;
} XpadMapping;

static XpadMapping* xpadMappings = NULL;
static int xpadMappingCount = 0;
static WORD prevButtons = 0;
static BYTE prevLeftTrigger = 0;
static BYTE prevRightTrigger = 0;

static void setupDefaultMappings(void) {
    static XpadMapping defaults[] = {
        { XINPUT_GAMEPAD_DPAD_UP,    38 },  // VK_UP
        { XINPUT_GAMEPAD_DPAD_DOWN,  40 },  // VK_DOWN
        { XINPUT_GAMEPAD_DPAD_LEFT,  37 },  // VK_LEFT
        { XINPUT_GAMEPAD_DPAD_RIGHT, 39 },  // VK_RIGHT
        { XINPUT_GAMEPAD_A,          13 },  // VK_RETURN (confirm)
        { XINPUT_GAMEPAD_B,          16 },  // VK_SHIFT (cancel)
        { XINPUT_GAMEPAD_X,          17 },  // VK_CONTROL
        { XINPUT_GAMEPAD_Y,          88 },  // 'X' key
        { XINPUT_GAMEPAD_START,      27 },  // VK_ESCAPE (menu)
        { XINPUT_GAMEPAD_BACK,       27 },  // VK_ESCAPE
    };
    xpadMappingCount = sizeof(defaults) / sizeof(XpadMapping);
    xpadMappings = (XpadMapping*)malloc(sizeof(defaults));
    memcpy(xpadMappings, defaults, sizeof(defaults));
}

static void drawRunnerFrame(Runner* runner, Renderer* renderer, int32_t gameW, int32_t gameH) {
    float displayScaleX;
    float displayScaleY;
    runner->renderGameW = gameW;
    runner->renderGameH = gameH;
    Runner_drawPre(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
    Runner_computeViewDisplayScale(runner, gameW, gameH, &displayScaleX, &displayScaleY);
    Runner_beginFrame(runner, gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);
    Runner_drawViews(runner, gameW, gameH, displayScaleX, displayScaleY, false);
    renderer->vtable->endFrameInit(renderer);
    Runner_drawPost(runner, SCREEN_WIDTH, SCREEN_HEIGHT);
    renderer->vtable->endFrameEnd(renderer);
    Runner_drawGUI(runner, SCREEN_WIDTH, SCREEN_HEIGHT, gameW, gameH);
}

// ===[ Main Entry Point ]===

VOID __cdecl main() {
    diagOpenFallback();
    diagLog("BUILD parse_guard_diag_v2 %s %s", __DATE__, __TIME__);
    diagLog("BS: guard v2 active; log is overwritten on each launch");
    diagLog("BS: 01 main() entered");

    IDirect3D9* pD3D = Direct3DCreate9(D3D_SDK_VERSION);
    if (!pD3D) { diagLog("BS: FATAL: D3D create failed"); return; }
    diagLog("BS: 02 D3D9 created");

    D3DPRESENT_PARAMETERS d3dpp;
    ZeroMemory(&d3dpp, sizeof(d3dpp));
    d3dpp.BackBufferWidth        = SCREEN_WIDTH;
    d3dpp.BackBufferHeight       = SCREEN_HEIGHT;
    d3dpp.BackBufferFormat       = D3DFMT_A8R8G8B8;
    d3dpp.FrontBufferFormat      = D3DFMT_LE_X8R8G8B8;
    d3dpp.MultiSampleType        = D3DMULTISAMPLE_NONE;
    d3dpp.BackBufferCount        = 1;
    d3dpp.EnableAutoDepthStencil = TRUE;
    d3dpp.AutoDepthStencilFormat = D3DFMT_D24S8;
    d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE;

    IDirect3DDevice9* pd3dDevice = NULL;
    HRESULT hr = pD3D->CreateDevice(0, D3DDEVTYPE_HAL, NULL,
                                    D3DCREATE_HARDWARE_VERTEXPROCESSING,
                                    &d3dpp, &pd3dDevice);
    if (FAILED(hr)) {
        diagLog("BS: FATAL: CreateDevice failed hr=0x%08X", hr);
        return;
    }
    diagLog("BS: 03 D3D device created");


    // ===[ Locate data.win ]===
    // On Xbox 360, game content is at game:\ (DVD/HDD) or d:\ (dev kit)
    const char* dataWinPath = NULL;

    // Try multiple paths. Use fopen() for detection since it goes through
    // the Xbox CRT which handles game:\ paths reliably on both hardware and emulators.
    static const char* searchPaths[] = {
        "game:\\data.win",
        "game:\\butterscotch\\data.win",
        "d:\\data.win",
        "d:\\butterscotch\\data.win",
        NULL,
    };

    diagLog("BS: 04 searching for data.win");
    char msg[256];
    for (int i = 0; searchPaths[i]; i++) {
        diagLog("BS: try %s", searchPaths[i]);

        FILE* testFile = fopen(searchPaths[i], "rb");
        if (testFile) {
            fclose(testFile);
            dataWinPath = searchPaths[i];
            diagOpenNextToDataWin(dataWinPath);
            diagLog("BS: 05 found data.win at %s", dataWinPath);
            break;
        }
    }

    if (!dataWinPath) {
        diagLog("BS: FATAL: data.win not found");
        for (;;) { } // hang instead of crash for debugging
    }

    bool loadingOk = loadingInit(&gLoadingScreen, pd3dDevice, dataWinPath);
    if (loadingOk) loadingDraw(&gLoadingScreen, 0.02f, "Starting Butterscotch360");

    diagLog("BS: 06 parsing data.win");
    if (loadingOk) loadingDraw(&gLoadingScreen, 0.05f, "Opening data.win");

    DataWinParserOptions parseOpts;
    memset(&parseOpts, 0, sizeof(parseOpts));
    parseOpts.parseGen8 = true;  parseOpts.parseOptn = true;  parseOpts.parseLang = true;
    parseOpts.parseExtn = true;  parseOpts.parseSond = true;  parseOpts.parseAgrp = true;
    parseOpts.parseSprt = true;  parseOpts.parseBgnd = true;  parseOpts.parsePath = true;
    parseOpts.parseScpt = true;  parseOpts.parseGlob = true;  parseOpts.parseShdr = true;
    parseOpts.parseFont = true;  parseOpts.parseTmln = true;  parseOpts.parseObjt = true;
    parseOpts.parseRoom = true;  parseOpts.parseTpag = true;  parseOpts.parseCode = true;
    parseOpts.parseVari = true;  parseOpts.parseFunc = true;  parseOpts.parseStrg = true;
    parseOpts.parseTxtr = true;  parseOpts.parseAudo = true;
    parseOpts.skipLoadingPreciseMasksForNonPreciseSprites = true;
    parseOpts.progressCallback = dataWinParseProgress;
    parseOpts.progressCallbackUserData = loadingOk ? &gLoadingScreen : NULL;
    DataWin* dataWin = parseDataWinGuarded(dataWinPath, parseOpts);

    if (!dataWin) {
        diagLog("BS: FATAL: DataWin_parse returned NULL");
        for (;;) { }
    }
    diagLog("BS: 07 data.win parsed OK");
    if (loadingOk) {
        loadingDraw(&gLoadingScreen, 1.0f, "data.win loaded");
        loadingDestroy(&gLoadingScreen);
        loadingOk = false;
    }

    diagLog("BS: game=%s", dataWin->gen8.displayName ? dataWin->gen8.displayName : "Unknown");

    // ===[ Load CONFIG.JSN (optional) ]===
    char configPath[512];
    const char* lastSlash = strrchr(dataWinPath, '\\');
    if (!lastSlash) lastSlash = strrchr(dataWinPath, '/');
    if (lastSlash) {
        size_t dirLen = (size_t)(lastSlash - dataWinPath + 1);
        memcpy(configPath, dataWinPath, dirLen);
        sprintf(configPath + dirLen, "CONFIG.JSN");
    } else {
        strcpy(configPath, "CONFIG.JSN");
    }

    JsonValue* configRoot = NULL;
    HANDLE hConfig = CreateFileA(configPath, GENERIC_READ, FILE_SHARE_READ,
                                 NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hConfig != INVALID_HANDLE_VALUE) {
        DWORD configSize = GetFileSize(hConfig, NULL);
        char* configText = (char*)malloc(configSize + 1);
        DWORD bytesRead;
        ReadFile(hConfig, configText, configSize, &bytesRead, NULL);
        CloseHandle(hConfig);
        configText[bytesRead] = '\0';
        configRoot = JsonReader_parse(configText);
        free(configText);
        diagLog("BS: Loaded CONFIG.JSN");
    }

    // ===[ Create Subsystems ]===
    diagLog("BS: 08 creating subsystems");
    XdkFileSystem* xdkFs = XdkFileSystem_create(dataWinPath);
    FileSystem* fileSystem = (FileSystem*)xdkFs;

    diagLog("BS: 09 creating renderer");
    Renderer* renderer = D3D9Renderer_create(pd3dDevice);

    diagLog("BS: 10 creating audio");
    XdkAudioSystem* xdkAudio = XdkAudioSystem_create();
    AudioSystem* audioSystem = (AudioSystem*)xdkAudio;

    diagLog("BS: 11 creating VM");
    VMContext* vm = VM_create(dataWin);
    diagLog("BS: 12 creating runner");
    Runner* runner = Runner_create(dataWin, vm, renderer, fileSystem, audioSystem);

    // Parse CONFIG.JSN options
    if (configRoot) {
        JsonValue* disabledArr = JsonReader_getObject(configRoot, "disabledObjects");
        if (disabledArr && JsonReader_isArray(disabledArr)) {
            sh_new_strdup(runner->disabledObjects);
            int count = JsonReader_arrayLength(disabledArr);
            for (int i = 0; i < count; i++) {
                JsonValue* elem = JsonReader_getArrayElement(disabledArr, i);
                if (elem && JsonReader_isString(elem)) {
                    const char* name = JsonReader_getString(elem);
                    shput(runner->disabledObjects, name, 1);
                }
            }
        }

        JsonValue* mappingsObj = JsonReader_getObject(configRoot, "controllerMappings");
        if (mappingsObj && JsonReader_isObject(mappingsObj)) {
            xpadMappingCount = JsonReader_objectLength(mappingsObj);
            xpadMappings = (XpadMapping*)malloc(sizeof(XpadMapping) * xpadMappingCount);
            for (int i = 0; i < xpadMappingCount; i++) {
                const char* btnStr = JsonReader_getObjectKey(mappingsObj, i);
                JsonValue* gmlVal = JsonReader_getObjectValue(mappingsObj, i);
                xpadMappings[i].xpadButton = (WORD)atoi(btnStr);
                xpadMappings[i].gmlKey = (int32_t)JsonReader_getInt(gmlVal);
            }
        }
    }

    if (!xpadMappings) setupDefaultMappings();

    // Initialize audio
    audioSystem->vtable->init(audioSystem, dataWin, fileSystem);
    diagLog("BS: 13 audio OK");

    // Initialize renderer
    diagLog("BS: 14 init renderer");
    renderer->vtable->init(renderer, dataWin);
    diagLog("BS: 15 renderer OK");

    // Initialize first room
    diagLog("BS: 16 init first room");
    Runner_initFirstRoom(runner);
    diagLog("BS: 17 first room OK");

    Gen8* gen8 = &dataWin->gen8;
    int32_t gameW = (int32_t)gen8->defaultWindowWidth;
    int32_t gameH = (int32_t)gen8->defaultWindowHeight;
    diagLog("BS: gameW=%d gameH=%d screenW=%d screenH=%d", gameW, gameH, SCREEN_WIDTH, SCREEN_HEIGHT);

    // Parse deferDrawToAfterAllSteps
    bool deferDraw = false;
    if (configRoot) {
        JsonValue* deferVal = JsonReader_getObject(configRoot, "deferDrawToAfterAllSteps");
        if (deferVal) deferDraw = JsonReader_getBool(deferVal);
    }

    diagLog("BS: 18 entering main loop");

    // ===[ Main Loop ]===
    LARGE_INTEGER freq, lastTime, currentTime;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&lastTime);
    LARGE_INTEGER startTime = lastTime;
    LARGE_INTEGER lastHeartbeatTime = lastTime;
    double accumulator = 0.0;
    uint32_t heartbeatFrame = 0;
    int32_t lastRoomId = runner->currentRoomIndex;

    while (!runner->shouldExit) {
        QueryPerformanceCounter(&currentTime);
        double deltaTime = (double)(currentTime.QuadPart - lastTime.QuadPart) / (double)freq.QuadPart;
        lastTime = currentTime;

        // ===[ Poll Controller ]===
        XINPUT_STATE state;
        if (XInputGetState(0, &state) == ERROR_SUCCESS) {
            WORD buttons = state.Gamepad.wButtons;

            // Left thumbstick as dpad
            #define STICK_DEADZONE 16384
            SHORT lx = state.Gamepad.sThumbLX;
            SHORT ly = state.Gamepad.sThumbLY;
            if (lx < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (lx >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (ly >  STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (ly < -STICK_DEADZONE) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;

            for (int i = 0; i < xpadMappingCount; i++) {
                WORD mask = xpadMappings[i].xpadButton;
                int32_t gmlKey = xpadMappings[i].gmlKey;

                bool wasPressed = (prevButtons & mask) != 0;
                bool isPressed = (buttons & mask) != 0;

                if (isPressed && !wasPressed)
                    RunnerKeyboard_onKeyDown(runner->keyboard, gmlKey);
                else if (!isPressed && wasPressed)
                    RunnerKeyboard_onKeyUp(runner->keyboard, gmlKey);
            }
            prevButtons = buttons;
            prevRightTrigger = state.Gamepad.bRightTrigger;
        }

        bool speedCapRemoved = prevRightTrigger > 128;

        // ===[ Frame Pacing ]===
        uint32_t roomSpeed = runner->currentRoom->speed;
        double targetFrameTime = (roomSpeed > 0) ? (1.0 / roomSpeed) : (1.0 / 60.0);

        if (deltaTime > targetFrameTime * 2.0) {
            accumulator = targetFrameTime;
            diagLog("TIMING: dropped catch-up dt=%.3f target=%.3f room=%d",
                deltaTime, targetFrameTime, runner->currentRoom ? runner->currentRoomIndex : -1);
        } else {
            accumulator += deltaTime;
            double maxAccumulator = targetFrameTime * 2.0;
            if (accumulator > maxAccumulator) accumulator = maxAccumulator;
        }
        if (speedCapRemoved && targetFrameTime > accumulator) accumulator = targetFrameTime;

        int gameFramesRan = 0;
        while (accumulator >= targetFrameTime) {
            if (gameFramesRan > 0)
                RunnerKeyboard_beginFrame(runner->keyboard);

            Runner_step(runner);
            if (runner->currentRoom && runner->currentRoomIndex != lastRoomId) {
                lastRoomId = runner->currentRoomIndex;
                diagLog("ROOM_CHANGED id=%d name=%s", lastRoomId, runner->currentRoom->name ? runner->currentRoom->name : "(null)");
                XdkAudioSystem_onRoomChanged(runner->audioSystem, lastRoomId, runner->currentRoom->name);
            }

            if (!deferDraw) {
                drawRunnerFrame(runner, renderer, gameW, gameH);
            }

            accumulator -= targetFrameTime;
            gameFramesRan++;
        }

        // Deferred draw: render once after all catch-up steps
        if (deferDraw && gameFramesRan > 0) {
            drawRunnerFrame(runner, renderer, gameW, gameH);
        }

        // Update audio
        if (runner->audioSystem) {
            float dt = (float)deltaTime;
            if (dt < 0.0f) dt = 0.0f;
            if (dt > 0.1f) dt = 0.1f;
            runner->audioSystem->vtable->update(runner->audioSystem, dt);
        }

        if (gameFramesRan > 0) {
            heartbeatFrame += (uint32_t) gameFramesRan;
            if ((heartbeatFrame % 120) < (uint32_t) gameFramesRan) {
                double elapsed = (double)(currentTime.QuadPart - startTime.QuadPart) / (double)freq.QuadPart;
                double hbElapsed = (double)(currentTime.QuadPart - lastHeartbeatTime.QuadPart) / (double)freq.QuadPart;
                double hbFps = hbElapsed > 0.0 ? 120.0 / hbElapsed : 0.0;
                lastHeartbeatTime = currentTime;
                diagLog("HB frame %u t=%.3f dt120=%.3f fps=%.2f room=%d speed=%u pending=%d inst=%d", heartbeatFrame, elapsed, hbElapsed, hbFps, runner->currentRoom ? runner->currentRoomIndex : -1, runner->currentRoom ? runner->currentRoom->speed : 0, runner->pendingRoom, (int32_t) arrlen(runner->instances));
            }
            RunnerKeyboard_beginFrame(runner->keyboard);
        }
    }

    // ===[ Cleanup ]===
    if (runner->audioSystem) {
        runner->audioSystem->vtable->destroy(runner->audioSystem);
        runner->audioSystem = NULL;
    }
    renderer->vtable->destroy(renderer);
    DataWin_free(dataWin);
    pd3dDevice->Release();
    pD3D->Release();

    free(xpadMappings);
}
