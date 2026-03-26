#define _CRT_SECURE_NO_WARNINGS
#define _WIN32_WINNT 0x0A00
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stddef.h>
#include <stdbool.h>

#include "fontcache.h"
#include "std.c"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")

#define WIDTH 1280
#define HEIGHT 720
#define FONT_FIRST_CHAR FONTCACHE_FIRST_CHAR
#define FONT_CHAR_COUNT FONTCACHE_CHAR_COUNT
#define FONT_ATLAS_W FONTCACHE_ATLAS_WIDTH
#define FONT_ATLAS_H FONTCACHE_ATLAS_HEIGHT
#define FONT_ATLAS_BASE_PX FONTCACHE_ATLAS_BASE_PX
#define FONT_CACHE_MAGIC FONTCACHE_MAGIC
#define FONT_CACHE_VERSION FONTCACHE_VERSION
#define SDF_PADDING FONTCACHE_SDF_PADDING
#define SDF_ON_EDGE_VALUE FONTCACHE_SDF_ON_EDGE_VALUE
#define SDF_PIXEL_DIST_SCALE FONTCACHE_SDF_PIXEL_DIST_SCALE
#define ATLAS_GAP FONTCACHE_ATLAS_GAP
#define FONT_KERNING_COUNT (FONT_CHAR_COUNT * FONT_CHAR_COUNT)
#define RECT_REQUEST_CAPACITY 32
#define RECT_VERTEX_CAPACITY (RECT_REQUEST_CAPACITY * 6)
#define TEXT_REQUEST_CAPACITY 16
#define TEXT_VERTEX_CAPACITY 16384
#define TEXT_QUAD_CAPACITY (TEXT_VERTEX_CAPACITY / 4)
#define TEXT_INDEX_CAPACITY (TEXT_QUAD_CAPACITY * 6)
#define RELEASE(x) do { if ((x) != NULL) { IUnknown_Release((IUnknown *)(x)); (x) = NULL; } } while (0)

typedef struct RectF {
    float x;
    float y;
    float w;
    float h;
} RectF;

typedef struct Vertex {
    float position[2];
    float color[4];
} Vertex;

typedef struct TextVertex {
    float position[2];
    float uv[2];
} TextVertex;

typedef struct FontGlyph {
    float u0;
    float v0;
    float u1;
    float v1;
    float xoff;
    float yoff;
    float width;
    float height;
    float advance;
    bool visible;
} FontGlyph;

typedef struct TextDrawRequest {
    const char *text;
    RectF rect;
    float size_dip;
} TextDrawRequest;

typedef struct RectDrawRequest {
    RectF rect;
    float color[4];
} RectDrawRequest;

typedef struct DrawList {
    RectDrawRequest rect_requests[RECT_REQUEST_CAPACITY];
    UINT rect_request_count;
    Vertex shape_vertices[RECT_VERTEX_CAPACITY];
    UINT shape_vertex_count;
    TextDrawRequest text_requests[TEXT_REQUEST_CAPACITY];
    UINT text_request_count;
    TextVertex text_vertices[TEXT_VERTEX_CAPACITY];
    UINT text_vertex_count;
} DrawList;

typedef struct D3DState {
    IDXGISwapChain *swap_chain;
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11RenderTargetView *rtv;
    ID3D11VertexShader *shape_vs;
    ID3D11PixelShader *shape_ps;
    ID3D11InputLayout *shape_layout;
    ID3D11Buffer *shape_vb;
    UINT shape_vertex_count;
    D3D11_VIEWPORT viewport;
    FLOAT clear[4];
    UINT shape_stride;
    UINT shape_offset;
    ID3D11VertexShader *text_vs;
    ID3D11PixelShader *text_ps;
    ID3D11InputLayout *text_layout;
    ID3D11Buffer *text_vb;
    ID3D11Buffer *text_ib;
    UINT text_vertex_count;
    UINT text_index_count;
    UINT text_stride;
    UINT text_offset;
    ID3D11ShaderResourceView *text_srv;
    ID3D11SamplerState *text_sampler;
    ID3D11BlendState *text_blend;
    float font_scale;
    float font_ascent;
    float font_line_advance;
    FontGlyph glyphs[FONT_CHAR_COUNT];
    float kerning[FONT_KERNING_COUNT];
    UINT dpi;
    UINT client_width;
    UINT client_height;
} D3DState;

static const char g_class_name[] = "ArcanusWindowClass";
static const char g_title[] = "Arcanus";
static const char g_font_cache_path[] = "resources/Roboto-Regular.fontcache";
static const char g_shape_shader_src[] =
    "struct VSIn { float2 pos : POSITION; float4 col : COLOR; };"
    "struct PSIn { float4 pos : SV_POSITION; float4 col : COLOR; };"
    "PSIn VSMain(VSIn v) { PSIn o; o.pos = float4(v.pos, 0.0, 1.0); o.col = v.col; return o; }"
    "float4 PSMain(PSIn p) : SV_TARGET { return p.col; }";
static const char g_text_shader_src[] =
    "struct VSIn { float2 pos : POSITION; float2 uv : TEXCOORD0; };"
    "struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };"
    "PSIn VSMain(VSIn v) { PSIn o; o.pos = float4(v.pos, 0.0, 1.0); o.uv = v.uv; return o; }"
    "Texture2D tex0 : register(t0);"
    "SamplerState samp0 : register(s0);"
    "float Contour(float dist, float edge, float width) {"
    "    return clamp(smoothstep(edge - width, edge + width, dist), 0.0f, 1.0f);"
    "}"
    "float GetSample(float2 uv, float edge, float width) {"
    "    return Contour(tex0.Sample(samp0, uv).r, edge, width);"
    "}"
    "float4 PSMain(PSIn p) : SV_TARGET {"
    "    float dist = tex0.Sample(samp0, p.uv).r;"
    "    float width = fwidth(dist);"
    "    float outerEdge = 1.0f - (120.0f / 255.0f);"
    "    float alpha = Contour(dist, outerEdge, width);"
    "    float dscale = 0.354f;"
    "    float2 uv = p.uv.xy;"
    "    float2 duv = dscale * (ddx(uv) + ddy(uv));"
    "    float4 box = float4(uv - duv, uv + duv);"
    "    float asum = GetSample(box.xy, outerEdge, width)"
    "               + GetSample(box.zw, outerEdge, width)"
    "               + GetSample(box.xw, outerEdge, width)"
    "               + GetSample(box.zy, outerEdge, width);"
    "    alpha = (alpha + 0.5f * asum) / 3.0f;"
    "    float4 textColor = float4(1.0f, 1.0f, 1.0f, alpha);"
    "    textColor.xyz = pow(textColor.xyz, 2.2f);"
    "    return textColor;"
    "}";

static bool g_running = true;
static HWND g_window = NULL;
static float g_text_dpi_scale = 1.0f;
static D3DState g_d3d = {
    .viewport = {0.0f, 0.0f, (FLOAT)WIDTH, (FLOAT)HEIGHT, 0.0f, 1.0f},
    .clear = {0.05f, 0.07f, 0.13f, 1.0f},
    .shape_stride = sizeof(Vertex),
    .text_stride = sizeof(TextVertex),
    .dpi = 96,
    .client_width = WIDTH,
    .client_height = HEIGHT,
};
static DrawList g_draw_list = {0};

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

static void ShowError(const char *message)
{
    MessageBoxA(g_window, message, g_title, MB_OK | MB_ICONERROR);
}

static void ShowHrError(const char *context, HRESULT hr)
{
    char text[256];

    AppFormatHrMessage(text, context, hr);
    ShowError(text);
}

static bool CompileShader(
    const char *source,
    size_t source_length,
    const char *entry,
    const char *target,
    ID3DBlob **blob)
{
    ID3DBlob *err_blob = NULL;
    HRESULT hr;

    hr = D3DCompile(source, source_length, NULL, NULL, NULL, entry, target, 0, 0, blob, &err_blob);
    if (FAILED(hr)) {
        ShowError(err_blob != NULL ? (const char *)ID3D10Blob_GetBufferPointer(err_blob) : "Shader compile failed.");
        RELEASE(err_blob);
        return false;
    }

    RELEASE(err_blob);
    return true;
}

static RectF MakeRect(float x, float y, float w, float h)
{
    RectF rect = {x, y, w, h};

    return rect;
}

static void SetClientSize(UINT width, UINT height)
{
    g_d3d.client_width = width > 0 ? width : 1u;
    g_d3d.client_height = height > 0 ? height : 1u;
    g_d3d.viewport.TopLeftX = 0.0f;
    g_d3d.viewport.TopLeftY = 0.0f;
    g_d3d.viewport.Width = (FLOAT)g_d3d.client_width;
    g_d3d.viewport.Height = (FLOAT)g_d3d.client_height;
    g_d3d.viewport.MinDepth = 0.0f;
    g_d3d.viewport.MaxDepth = 1.0f;
}

static void UpdateClientMetrics(void)
{
    RECT rect = {0};

    if (g_window == NULL) {
        return;
    }

    GetClientRect(g_window, &rect);
    SetClientSize(
        rect.right > rect.left ? (UINT)(rect.right - rect.left) : 1u,
        rect.bottom > rect.top ? (UINT)(rect.bottom - rect.top) : 1u);
    g_d3d.dpi = GetDpiForWindow(g_window);
}

static float ToClipX(float x)
{
    return (x / ((float)g_d3d.client_width * 0.5f)) - 1.0f;
}

static float ToClipY(float y)
{
    return 1.0f - (y / ((float)g_d3d.client_height * 0.5f));
}

static float DipToPixels(float dips)
{
    return dips * (float)g_d3d.dpi / 96.0f;
}

static void AdjustTextDpiScale(float delta)
{
    g_text_dpi_scale += delta;
    if (g_text_dpi_scale < 0.5f) {
        g_text_dpi_scale = 0.5f;
    }
    if (g_text_dpi_scale > 3.0f) {
        g_text_dpi_scale = 3.0f;
    }
}

static bool CreateAppWindow(HINSTANCE instance)
{
    WNDCLASSEXA wc = {0};
    UINT dpi = GetDpiForSystem();
    RECT rect = {0, 0, MulDiv(WIDTH, (int)dpi, 96), MulDiv(HEIGHT, (int)dpi, 96)};
    DWORD style = WS_OVERLAPPEDWINDOW & ~(WS_THICKFRAME | WS_MAXIMIZEBOX);

    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorA(NULL, IDC_ARROW);
    wc.lpszClassName = g_class_name;

    if (RegisterClassExA(&wc) == 0) {
        ShowError("RegisterClassExA failed.");
        return false;
    }

    if (AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi) == 0) {
        AdjustWindowRect(&rect, style, FALSE);
    }

    g_window = CreateWindowExA(
        0,
        g_class_name,
        g_title,
        style,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        rect.right - rect.left,
        rect.bottom - rect.top,
        NULL,
        NULL,
        instance,
        NULL);
    if (g_window == NULL) {
        ShowError("CreateWindowExA failed.");
        return false;
    }

    ShowWindow(g_window, SW_SHOWDEFAULT);
    UpdateClientMetrics();
    return true;
}

static bool CreateRenderTargetView(void)
{
    ID3D11Texture2D *back_buffer = NULL;
    HRESULT hr;

    hr = IDXGISwapChain_GetBuffer(g_d3d.swap_chain, 0, &IID_ID3D11Texture2D, (void **)&back_buffer);
    if (FAILED(hr)) {
        ShowHrError("IDXGISwapChain_GetBuffer", hr);
        return false;
    }

    hr = ID3D11Device_CreateRenderTargetView(
        g_d3d.device,
        (ID3D11Resource *)back_buffer,
        NULL,
        &g_d3d.rtv);
    RELEASE(back_buffer);

    if (FAILED(hr)) {
        ShowHrError("ID3D11Device_CreateRenderTargetView", hr);
        return false;
    }

    return true;
}

static bool CreateDeviceAndSwapChain(void)
{
    DXGI_SWAP_CHAIN_DESC scd = {0};
    HRESULT hr;

    scd.BufferDesc.Width = g_d3d.client_width;
    scd.BufferDesc.Height = g_d3d.client_height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 1;
    scd.OutputWindow = g_window;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    hr = D3D11CreateDeviceAndSwapChain(
        NULL,
        D3D_DRIVER_TYPE_HARDWARE,
        NULL,
        0,
        NULL,
        0,
        D3D11_SDK_VERSION,
        &scd,
        &g_d3d.swap_chain,
        &g_d3d.device,
        NULL,
        &g_d3d.context);
    if (FAILED(hr)) {
        ShowHrError("D3D11CreateDeviceAndSwapChain", hr);
        return false;
    }

    return CreateRenderTargetView();
}

static bool CreateShapeShaders(void)
{
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    D3D11_INPUT_ELEMENT_DESC elems[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    HRESULT hr;
    bool ok = false;

    if (!CompileShader(g_shape_shader_src, sizeof(g_shape_shader_src) - 1, "VSMain", "vs_4_0", &vs_blob)) {
        goto cleanup;
    }

    if (!CompileShader(g_shape_shader_src, sizeof(g_shape_shader_src) - 1, "PSMain", "ps_4_0", &ps_blob)) {
        goto cleanup;
    }

    hr = ID3D11Device_CreateVertexShader(
        g_d3d.device,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        NULL,
        &g_d3d.shape_vs);
    if (FAILED(hr)) {
        ShowHrError("CreateVertexShader", hr);
        goto cleanup;
    }

    hr = ID3D11Device_CreatePixelShader(
        g_d3d.device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob),
        NULL,
        &g_d3d.shape_ps);
    if (FAILED(hr)) {
        ShowHrError("CreatePixelShader", hr);
        goto cleanup;
    }

    hr = ID3D11Device_CreateInputLayout(
        g_d3d.device,
        elems,
        2,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        &g_d3d.shape_layout);
    if (FAILED(hr)) {
        ShowHrError("CreateInputLayout", hr);
        goto cleanup;
    }

    ok = true;

cleanup:
    RELEASE(vs_blob);
    RELEASE(ps_blob);
    return ok;
}

static bool CreateShapeVertexBuffer(void)
{
    D3D11_BUFFER_DESC bd = {0};
    HRESULT hr;

    bd.ByteWidth = RECT_VERTEX_CAPACITY * (UINT)sizeof(Vertex);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.StructureByteStride = sizeof(Vertex);

    hr = ID3D11Device_CreateBuffer(g_d3d.device, &bd, NULL, &g_d3d.shape_vb);
    if (FAILED(hr)) {
        ShowHrError("CreateBuffer", hr);
        return false;
    }

    return true;
}

static bool CreateTextPipeline(void)
{
    ID3DBlob *vs_blob = NULL;
    ID3DBlob *ps_blob = NULL;
    unsigned short *indices = NULL;
    D3D11_INPUT_ELEMENT_DESC elems[2] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    D3D11_SAMPLER_DESC sampler_desc = {0};
    D3D11_BLEND_DESC blend_desc = {0};
    D3D11_BUFFER_DESC vb_desc = {0};
    D3D11_BUFFER_DESC ib_desc = {0};
    D3D11_SUBRESOURCE_DATA ib_data = {0};
    HRESULT hr;
    bool ok = false;

    if (!CompileShader(g_text_shader_src, sizeof(g_text_shader_src) - 1, "VSMain", "vs_4_0", &vs_blob)) {
        goto cleanup;
    }

    if (!CompileShader(g_text_shader_src, sizeof(g_text_shader_src) - 1, "PSMain", "ps_4_0", &ps_blob)) {
        goto cleanup;
    }

    hr = ID3D11Device_CreateVertexShader(
        g_d3d.device,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        NULL,
        &g_d3d.text_vs);
    if (FAILED(hr)) {
        ShowHrError("CreateTextVertexShader", hr);
        goto cleanup;
    }

    hr = ID3D11Device_CreatePixelShader(
        g_d3d.device,
        ID3D10Blob_GetBufferPointer(ps_blob),
        ID3D10Blob_GetBufferSize(ps_blob),
        NULL,
        &g_d3d.text_ps);
    if (FAILED(hr)) {
        ShowHrError("CreateTextPixelShader", hr);
        goto cleanup;
    }

    hr = ID3D11Device_CreateInputLayout(
        g_d3d.device,
        elems,
        2,
        ID3D10Blob_GetBufferPointer(vs_blob),
        ID3D10Blob_GetBufferSize(vs_blob),
        &g_d3d.text_layout);
    if (FAILED(hr)) {
        ShowHrError("CreateTextInputLayout", hr);
        goto cleanup;
    }

    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;

    hr = ID3D11Device_CreateSamplerState(g_d3d.device, &sampler_desc, &g_d3d.text_sampler);
    if (FAILED(hr)) {
        ShowHrError("CreateSamplerState", hr);
        goto cleanup;
    }

    blend_desc.RenderTarget[0].BlendEnable = TRUE;
    blend_desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend_desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend_desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blend_desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    hr = ID3D11Device_CreateBlendState(g_d3d.device, &blend_desc, &g_d3d.text_blend);
    if (FAILED(hr)) {
        ShowHrError("CreateBlendState", hr);
        goto cleanup;
    }

    vb_desc.ByteWidth = TEXT_VERTEX_CAPACITY * (UINT)sizeof(TextVertex);
    vb_desc.Usage = D3D11_USAGE_DYNAMIC;
    vb_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    vb_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    vb_desc.StructureByteStride = sizeof(TextVertex);

    hr = ID3D11Device_CreateBuffer(g_d3d.device, &vb_desc, NULL, &g_d3d.text_vb);
    if (FAILED(hr)) {
        ShowHrError("CreateTextVertexBuffer", hr);
        goto cleanup;
    }

    indices = (unsigned short *)AppMalloc(TEXT_INDEX_CAPACITY * sizeof(unsigned short));
    if (indices == NULL) {
        ShowError("Out of memory while building text index buffer.");
        goto cleanup;
    }

    {
        UINT quad;

        for (quad = 0; quad < TEXT_QUAD_CAPACITY; ++quad) {
            unsigned short base_vertex = (unsigned short)(quad * 4u);
            size_t base_index = (size_t)quad * 6u;

            indices[base_index + 0] = (unsigned short)(base_vertex + 0u);
            indices[base_index + 1] = (unsigned short)(base_vertex + 1u);
            indices[base_index + 2] = (unsigned short)(base_vertex + 2u);
            indices[base_index + 3] = (unsigned short)(base_vertex + 0u);
            indices[base_index + 4] = (unsigned short)(base_vertex + 2u);
            indices[base_index + 5] = (unsigned short)(base_vertex + 3u);
        }
    }

    ib_desc.ByteWidth = TEXT_INDEX_CAPACITY * (UINT)sizeof(unsigned short);
    ib_desc.Usage = D3D11_USAGE_IMMUTABLE;
    ib_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    ib_data.pSysMem = indices;

    hr = ID3D11Device_CreateBuffer(g_d3d.device, &ib_desc, &ib_data, &g_d3d.text_ib);
    if (FAILED(hr)) {
        ShowHrError("CreateTextIndexBuffer", hr);
        goto cleanup;
    }

    ok = true;

cleanup:
    AppFree(indices);
    RELEASE(vs_blob);
    RELEASE(ps_blob);
    return ok;
}

static float Fixed8_8ToFloat(short value)
{
    return (float)value / (float)FONTCACHE_FIXED_SCALE;
}

static size_t GetCacheGlyphBitmapSize(const FontCacheGlyph *glyph)
{
    return (size_t)glyph->width * (size_t)glyph->height;
}

static bool DecodeGlyphBitmapToAtlas(
    const FontCacheGlyph *glyph,
    const unsigned char *bitmap_data,
    unsigned int atlas_width,
    unsigned char *atlas)
{
    size_t expected_size = GetCacheGlyphBitmapSize(glyph);
    size_t in_pos = 0;
    size_t out_pos = 0;

    while (in_pos < (size_t)glyph->bitmap_size && out_pos < expected_size) {
        unsigned char tag = bitmap_data[in_pos++];
        size_t run_length = (size_t)(tag & 0x7Fu) + 1u;

        if (run_length > expected_size - out_pos) {
            return false;
        }

        if ((tag & 0x80u) != 0) {
            out_pos += run_length;
            continue;
        }

        if (run_length > (size_t)glyph->bitmap_size - in_pos) {
            return false;
        }

        while (run_length > 0) {
            size_t row = out_pos / (size_t)glyph->width;
            size_t col = out_pos % (size_t)glyph->width;
            size_t row_space = (size_t)glyph->width - col;
            size_t chunk = run_length < row_space ? run_length : row_space;
            unsigned char *dst = atlas +
                ((size_t)glyph->atlas_y + row) * (size_t)atlas_width +
                (size_t)glyph->atlas_x + col;

            AppMemcpy(dst, bitmap_data + in_pos, chunk);
            in_pos += chunk;
            out_pos += chunk;
            run_length -= chunk;
        }
    }

    return in_pos == (size_t)glyph->bitmap_size && out_pos == expected_size;
}

static bool PopulateRuntimeGlyph(FontGlyph *glyph, const FontCacheGlyph *cache_glyph, const FontCacheHeader *header)
{
    AppMemset(glyph, 0, sizeof(*glyph));
    glyph->xoff = Fixed8_8ToFloat(cache_glyph->xoff_8_8);
    glyph->yoff = Fixed8_8ToFloat(cache_glyph->yoff_8_8);
    glyph->width = (float)cache_glyph->width;
    glyph->height = (float)cache_glyph->height;
    glyph->advance = Fixed8_8ToFloat(cache_glyph->advance_8_8);
    glyph->visible = cache_glyph->width != 0 && cache_glyph->height != 0;

    if (!glyph->visible) {
        return true;
    }

    if ((unsigned int)cache_glyph->atlas_x + (unsigned int)cache_glyph->width > header->atlas_width ||
            (unsigned int)cache_glyph->atlas_y + (unsigned int)cache_glyph->height > header->atlas_height) {
        return false;
    }

    glyph->u0 = (float)cache_glyph->atlas_x / (float)header->atlas_width;
    glyph->v0 = (float)cache_glyph->atlas_y / (float)header->atlas_height;
    glyph->u1 = (float)(cache_glyph->atlas_x + cache_glyph->width) / (float)header->atlas_width;
    glyph->v1 = (float)(cache_glyph->atlas_y + cache_glyph->height) / (float)header->atlas_height;
    return true;
}

static bool CreateFontAtlasTexture(const unsigned char *atlas, unsigned int atlas_width, unsigned int atlas_height)
{
    ID3D11Texture2D *texture = NULL;
    D3D11_TEXTURE2D_DESC texture_desc = {0};
    D3D11_SUBRESOURCE_DATA texture_data = {0};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc = {0};
    HRESULT hr;
    bool ok = false;

    texture_desc.Width = atlas_width;
    texture_desc.Height = atlas_height;
    texture_desc.MipLevels = 1;
    texture_desc.ArraySize = 1;
    texture_desc.Format = DXGI_FORMAT_R8_UNORM;
    texture_desc.SampleDesc.Count = 1;
    texture_desc.Usage = D3D11_USAGE_IMMUTABLE;
    texture_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    texture_data.pSysMem = atlas;
    texture_data.SysMemPitch = atlas_width;

    hr = ID3D11Device_CreateTexture2D(g_d3d.device, &texture_desc, &texture_data, &texture);
    if (FAILED(hr)) {
        ShowHrError("CreateTexture2D", hr);
        goto cleanup;
    }

    srv_desc.Format = texture_desc.Format;
    srv_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv_desc.Texture2D.MipLevels = 1;

    hr = ID3D11Device_CreateShaderResourceView(
        g_d3d.device,
        (ID3D11Resource *)texture,
        &srv_desc,
        &g_d3d.text_srv);
    if (FAILED(hr)) {
        ShowHrError("CreateShaderResourceView", hr);
        goto cleanup;
    }

    ok = true;

cleanup:
    RELEASE(texture);
    return ok;
}

static bool LoadFontAtlasCache(void)
{
    unsigned char *cache_data = NULL;
    unsigned char *atlas = NULL;
    size_t cache_size = 0;
    const FontCacheHeader *header;
    const FontCacheGlyph *cache_glyphs;
    const FontCacheKerningPair *cache_kern_pairs;
    const unsigned char *bitmap_data;
    size_t glyph_table_size;
    size_t kerning_table_size;
    size_t expected_size;
    unsigned int i;
    bool ok = false;

    if (!AppLoadEntireFile(g_font_cache_path, &cache_data, &cache_size)) {
        ShowError("Could not open resources/Roboto-Regular.fontcache. Run generate-fonts.bat.");
        return false;
    }

    if (cache_size < sizeof(FontCacheHeader)) {
        ShowError("Font cache is too small.");
        goto cleanup;
    }

    header = (const FontCacheHeader *)cache_data;
    if (header->magic != FONT_CACHE_MAGIC ||
            header->version != FONT_CACHE_VERSION ||
            header->atlas_width != FONT_ATLAS_W ||
            header->atlas_height != FONT_ATLAS_H ||
            header->first_char != FONT_FIRST_CHAR ||
            header->char_count != FONT_CHAR_COUNT ||
            header->glyph_count != FONT_CHAR_COUNT ||
            header->bitmap_codec != FONTCACHE_BITMAP_CODEC_ZERO_RLE ||
            header->sdf_padding != SDF_PADDING ||
            header->sdf_on_edge_value != SDF_ON_EDGE_VALUE ||
            header->atlas_gap != ATLAS_GAP ||
            header->atlas_base_px != FONT_ATLAS_BASE_PX ||
            header->sdf_pixel_dist_scale != SDF_PIXEL_DIST_SCALE) {
        ShowError("Font cache format does not match this build. Run generate-fonts.bat.");
        goto cleanup;
    }

    glyph_table_size = (size_t)header->glyph_count * sizeof(FontCacheGlyph);
    kerning_table_size = (size_t)header->kern_pair_count * sizeof(FontCacheKerningPair);
    expected_size = sizeof(FontCacheHeader) + glyph_table_size + kerning_table_size + (size_t)header->bitmap_data_size;
    if (cache_size != expected_size) {
        ShowError("Font cache file size is invalid.");
        goto cleanup;
    }

    cache_glyphs = (const FontCacheGlyph *)(cache_data + sizeof(FontCacheHeader));
    cache_kern_pairs = (const FontCacheKerningPair *)(cache_data + sizeof(FontCacheHeader) + glyph_table_size);
    bitmap_data = cache_data + sizeof(FontCacheHeader) + glyph_table_size + kerning_table_size;

    atlas = (unsigned char *)AppMalloc((size_t)header->atlas_width * (size_t)header->atlas_height);
    if (atlas == NULL) {
        ShowError("Out of memory while loading font cache.");
        goto cleanup;
    }
    AppMemset(atlas, 0, (size_t)header->atlas_width * (size_t)header->atlas_height);

    AppMemset(g_d3d.kerning, 0, sizeof(g_d3d.kerning));

    for (i = 0; i < header->char_count; ++i) {
        const FontCacheGlyph *cache_glyph = &cache_glyphs[i];
        size_t bitmap_size = GetCacheGlyphBitmapSize(cache_glyph);

        if (!PopulateRuntimeGlyph(&g_d3d.glyphs[i], cache_glyph, header)) {
            ShowError("Font cache glyph data is invalid.");
            goto cleanup;
        }

        if (bitmap_size == 0) {
            if (cache_glyph->bitmap_size != 0) {
                ShowError("Font cache bitmap data is invalid.");
                goto cleanup;
            }
            continue;
        }

        if ((size_t)cache_glyph->bitmap_offset > (size_t)header->bitmap_data_size ||
                (size_t)cache_glyph->bitmap_size > (size_t)header->bitmap_data_size - (size_t)cache_glyph->bitmap_offset) {
            ShowError("Font cache bitmap data is invalid.");
            goto cleanup;
        }

        if (!DecodeGlyphBitmapToAtlas(
                cache_glyph,
                bitmap_data + cache_glyph->bitmap_offset,
                header->atlas_width,
                atlas)) {
            ShowError("Font cache bitmap stream is invalid.");
            goto cleanup;
        }
    }

    for (i = 0; i < header->kern_pair_count; ++i) {
        const FontCacheKerningPair *pair = &cache_kern_pairs[i];

        if (pair->left_char < FONT_FIRST_CHAR || pair->left_char >= FONT_FIRST_CHAR + FONT_CHAR_COUNT ||
                pair->right_char < FONT_FIRST_CHAR || pair->right_char >= FONT_FIRST_CHAR + FONT_CHAR_COUNT) {
            ShowError("Font cache kerning data is invalid.");
            goto cleanup;
        }

        g_d3d.kerning[(pair->left_char - FONT_FIRST_CHAR) * FONT_CHAR_COUNT + (pair->right_char - FONT_FIRST_CHAR)] =
            Fixed8_8ToFloat(pair->amount_8_8);
    }

    g_d3d.font_scale = header->font_scale;
    g_d3d.font_ascent = header->font_ascent;
    g_d3d.font_line_advance = header->font_line_advance;

    if (!CreateFontAtlasTexture(atlas, header->atlas_width, header->atlas_height)) {
        goto cleanup;
    }

    ok = true;
    AppWriteStdoutLine("Loaded font cache.");

cleanup:
    AppFree(atlas);
    AppFree(cache_data);
    return ok;
}

static bool CreateFontAtlas(void)
{
    return LoadFontAtlasCache();
}

static bool ResizeBackbuffer(UINT width, UINT height)
{
    HRESULT hr;

    if (g_d3d.swap_chain == NULL || width == 0 || height == 0) {
        return true;
    }

    SetClientSize(width, height);
    ID3D11DeviceContext_OMSetRenderTargets(g_d3d.context, 0, NULL, NULL);
    RELEASE(g_d3d.rtv);

    hr = IDXGISwapChain_ResizeBuffers(g_d3d.swap_chain, 0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        ShowHrError("ResizeBuffers", hr);
        return false;
    }

    return CreateRenderTargetView();
}

static void ResetDrawList(void)
{
    g_draw_list.rect_request_count = 0;
    g_draw_list.shape_vertex_count = 0;
    g_draw_list.text_request_count = 0;
    g_draw_list.text_vertex_count = 0;
}

static bool PushRectDrawRequest(RectF rect, float r, float g, float b, float a)
{
    RectDrawRequest *request;

    if (g_draw_list.rect_request_count >= RECT_REQUEST_CAPACITY) {
        ShowError("Rect request capacity exceeded.");
        return false;
    }

    request = &g_draw_list.rect_requests[g_draw_list.rect_request_count++];
    request->rect = rect;
    request->color[0] = r;
    request->color[1] = g;
    request->color[2] = b;
    request->color[3] = a;
    return true;
}

static bool PushTextDrawRequest(RectF rect, float size_dip, const char *text)
{
    TextDrawRequest *request;

    if (text == NULL || text[0] == '\0') {
        return true;
    }

    if (g_draw_list.text_request_count >= TEXT_REQUEST_CAPACITY) {
        ShowError("Text request capacity exceeded.");
        return false;
    }

    request = &g_draw_list.text_requests[g_draw_list.text_request_count++];
    request->text = text;
    request->rect = rect;
    request->size_dip = size_dip;
    return true;
}

static bool AppendRectRequest(const RectDrawRequest *request)
{
    float left_px;
    float top_px;
    float right_px;
    float bottom_px;
    float left;
    float top;
    float right;
    float bottom;
    size_t base;
    int i;

    if (request->rect.w <= 0.0f || request->rect.h <= 0.0f) {
        return true;
    }

    left_px = DipToPixels(request->rect.x);
    top_px = DipToPixels(request->rect.y);
    right_px = DipToPixels(request->rect.x + request->rect.w);
    bottom_px = DipToPixels(request->rect.y + request->rect.h);

    left = ToClipX(left_px);
    top = ToClipY(top_px);
    right = ToClipX(right_px);
    bottom = ToClipY(bottom_px);

    if (g_draw_list.shape_vertex_count > RECT_VERTEX_CAPACITY - 6u) {
        ShowError("Rect vertex capacity exceeded.");
        return false;
    }

    base = g_draw_list.shape_vertex_count;

    g_draw_list.shape_vertices[base + 0].position[0] = left;
    g_draw_list.shape_vertices[base + 0].position[1] = top;
    g_draw_list.shape_vertices[base + 1].position[0] = right;
    g_draw_list.shape_vertices[base + 1].position[1] = top;
    g_draw_list.shape_vertices[base + 2].position[0] = right;
    g_draw_list.shape_vertices[base + 2].position[1] = bottom;
    g_draw_list.shape_vertices[base + 3].position[0] = left;
    g_draw_list.shape_vertices[base + 3].position[1] = top;
    g_draw_list.shape_vertices[base + 4].position[0] = right;
    g_draw_list.shape_vertices[base + 4].position[1] = bottom;
    g_draw_list.shape_vertices[base + 5].position[0] = left;
    g_draw_list.shape_vertices[base + 5].position[1] = bottom;

    for (i = 0; i < 6; ++i) {
        g_draw_list.shape_vertices[base + i].color[0] = request->color[0];
        g_draw_list.shape_vertices[base + i].color[1] = request->color[1];
        g_draw_list.shape_vertices[base + i].color[2] = request->color[2];
        g_draw_list.shape_vertices[base + i].color[3] = request->color[3];
    }

    g_draw_list.shape_vertex_count += 6;
    return true;
}

static unsigned char ResolveFontChar(unsigned char ch)
{
    if (ch < FONT_FIRST_CHAR || ch >= FONT_FIRST_CHAR + FONT_CHAR_COUNT) {
        ch = '?';
    }

    return ch;
}

static const FontGlyph *GetFontGlyph(unsigned char ch, unsigned char *resolved_ch)
{
    ch = ResolveFontChar(ch);
    *resolved_ch = ch;
    return &g_d3d.glyphs[ch - FONT_FIRST_CHAR];
}

static float GetFontKerning(unsigned char left, unsigned char right)
{
    left = ResolveFontChar(left);
    right = ResolveFontChar(right);
    return g_d3d.kerning[(left - FONT_FIRST_CHAR) * FONT_CHAR_COUNT + (right - FONT_FIRST_CHAR)];
}

static bool AppendTextQuad(
    float left,
    float top,
    float right,
    float bottom,
    float u0,
    float v0,
    float u1,
    float v1,
    float clip_left,
    float clip_top,
    float clip_right,
    float clip_bottom)
{
    float clipped_left;
    float clipped_top;
    float clipped_right;
    float clipped_bottom;
    float du;
    float dv;
    float clipped_u0;
    float clipped_v0;
    float clipped_u1;
    float clipped_v1;
    size_t base;

    if (right <= left || bottom <= top) {
        return true;
    }

    if (right <= clip_left || left >= clip_right || bottom <= clip_top || top >= clip_bottom) {
        return true;
    }

    clipped_left = left > clip_left ? left : clip_left;
    clipped_top = top > clip_top ? top : clip_top;
    clipped_right = right < clip_right ? right : clip_right;
    clipped_bottom = bottom < clip_bottom ? bottom : clip_bottom;

    if (clipped_right <= clipped_left || clipped_bottom <= clipped_top) {
        return true;
    }

    du = u1 - u0;
    dv = v1 - v0;
    clipped_u0 = u0 + du * ((clipped_left - left) / (right - left));
    clipped_v0 = v0 + dv * ((clipped_top - top) / (bottom - top));
    clipped_u1 = u0 + du * ((clipped_right - left) / (right - left));
    clipped_v1 = v0 + dv * ((clipped_bottom - top) / (bottom - top));

    if (g_draw_list.text_vertex_count > TEXT_VERTEX_CAPACITY - 4u) {
        ShowError("Text vertex capacity exceeded.");
        return false;
    }

    base = g_draw_list.text_vertex_count;

    g_draw_list.text_vertices[base + 0].position[0] = ToClipX(clipped_left);
    g_draw_list.text_vertices[base + 0].position[1] = ToClipY(clipped_top);
    g_draw_list.text_vertices[base + 0].uv[0] = clipped_u0;
    g_draw_list.text_vertices[base + 0].uv[1] = clipped_v0;

    g_draw_list.text_vertices[base + 1].position[0] = ToClipX(clipped_right);
    g_draw_list.text_vertices[base + 1].position[1] = ToClipY(clipped_top);
    g_draw_list.text_vertices[base + 1].uv[0] = clipped_u1;
    g_draw_list.text_vertices[base + 1].uv[1] = clipped_v0;

    g_draw_list.text_vertices[base + 2].position[0] = ToClipX(clipped_right);
    g_draw_list.text_vertices[base + 2].position[1] = ToClipY(clipped_bottom);
    g_draw_list.text_vertices[base + 2].uv[0] = clipped_u1;
    g_draw_list.text_vertices[base + 2].uv[1] = clipped_v1;

    g_draw_list.text_vertices[base + 3].position[0] = ToClipX(clipped_left);
    g_draw_list.text_vertices[base + 3].position[1] = ToClipY(clipped_bottom);
    g_draw_list.text_vertices[base + 3].uv[0] = clipped_u0;
    g_draw_list.text_vertices[base + 3].uv[1] = clipped_v1;

    g_draw_list.text_vertex_count += 4;
    return true;
}

static bool AppendTextRequest(const TextDrawRequest *request)
{
    float rect_left;
    float rect_top;
    float rect_right;
    float rect_bottom;
    float size_px;
    float scale;
    float ascent;
    float line_advance;
    float x;
    float baseline;
    size_t i;

    if (request->text == NULL || request->size_dip <= 0.0f) {
        return true;
    }

    rect_left = DipToPixels(request->rect.x);
    rect_top = DipToPixels(request->rect.y);
    rect_right = DipToPixels(request->rect.x + request->rect.w);
    rect_bottom = DipToPixels(request->rect.y + request->rect.h);

    if (rect_right <= rect_left || rect_bottom <= rect_top) {
        return true;
    }

    size_px = DipToPixels(request->size_dip) * g_text_dpi_scale;
    if (size_px <= 0.0f) {
        return true;
    }

    scale = size_px / FONT_ATLAS_BASE_PX;
    ascent = g_d3d.font_ascent * scale;
    line_advance = g_d3d.font_line_advance * scale;
    x = rect_left;
    baseline = rect_top + ascent;

    for (i = 0; request->text[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)request->text[i];
        unsigned char next = (unsigned char)request->text[i + 1];
        const FontGlyph *glyph;
        float advance;

        if (baseline - ascent >= rect_bottom) {
            break;
        }

        if (ch == '\r') {
            continue;
        }

        if (ch == '\n') {
            x = rect_left;
            baseline += line_advance;
            continue;
        }

        if (ch == '\t') {
            ch = ' ';
        }

        glyph = GetFontGlyph(ch, &ch);
        advance = glyph->advance * scale;

        if (x > rect_left && x + advance > rect_right) {
            x = rect_left;
            baseline += line_advance;

            if (baseline - ascent >= rect_bottom) {
                break;
            }

            if (ch == ' ') {
                continue;
            }
        }

        if (glyph->visible) {
            float left = x + glyph->xoff * scale;
            float top = baseline + glyph->yoff * scale;
            float right = left + glyph->width * scale;
            float bottom = top + glyph->height * scale;

            if (!AppendTextQuad(
                    left,
                    top,
                    right,
                    bottom,
                    glyph->u0,
                    glyph->v0,
                    glyph->u1,
                    glyph->v1,
                    rect_left,
                    rect_top,
                    rect_right,
                    rect_bottom)) {
                return false;
            }
        }

        x += advance;

        if (next != 0 && next != '\r' && next != '\n') {
            if (next == '\t') {
                next = ' ';
            }
            x += GetFontKerning(ch, next) * scale;
        }
    }

    return true;
}

static bool UploadTextVertices(void)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    UINT i;
    HRESULT hr;

    g_draw_list.text_vertex_count = 0;

    for (i = 0; i < g_draw_list.text_request_count; ++i) {
        if (!AppendTextRequest(&g_draw_list.text_requests[i])) {
            return false;
        }
    }

    g_d3d.text_vertex_count = g_draw_list.text_vertex_count;
    g_d3d.text_index_count = (g_d3d.text_vertex_count / 4u) * 6u;
    if (g_d3d.text_vertex_count == 0) {
        return true;
    }

    hr = ID3D11DeviceContext_Map(
        g_d3d.context,
        (ID3D11Resource *)g_d3d.text_vb,
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped);
    if (FAILED(hr)) {
        ShowHrError("MapTextVertexBuffer", hr);
        return false;
    }

    AppMemcpy(mapped.pData, g_draw_list.text_vertices, g_d3d.text_vertex_count * sizeof(TextVertex));
    ID3D11DeviceContext_Unmap(g_d3d.context, (ID3D11Resource *)g_d3d.text_vb, 0);
    return true;
}

static bool UploadRectVertices(void)
{
    D3D11_MAPPED_SUBRESOURCE mapped = {0};
    UINT i;
    HRESULT hr;

    g_draw_list.shape_vertex_count = 0;

    for (i = 0; i < g_draw_list.rect_request_count; ++i) {
        if (!AppendRectRequest(&g_draw_list.rect_requests[i])) {
            return false;
        }
    }

    g_d3d.shape_vertex_count = g_draw_list.shape_vertex_count;
    if (g_d3d.shape_vertex_count == 0) {
        return true;
    }

    hr = ID3D11DeviceContext_Map(
        g_d3d.context,
        (ID3D11Resource *)g_d3d.shape_vb,
        0,
        D3D11_MAP_WRITE_DISCARD,
        0,
        &mapped);
    if (FAILED(hr)) {
        ShowHrError("MapShapeVertexBuffer", hr);
        return false;
    }

    AppMemcpy(mapped.pData, g_draw_list.shape_vertices, g_d3d.shape_vertex_count * sizeof(Vertex));
    ID3D11DeviceContext_Unmap(g_d3d.context, (ID3D11Resource *)g_d3d.shape_vb, 0);
    return true;
}

static bool UploadDrawList(void)
{
    if (!UploadRectVertices()) {
        return false;
    }

    return UploadTextVertices();
}

static bool BuildDemoDrawList(void)
{
    char scale_label[64];

    ResetDrawList();

    AppFormatScaleLabel(scale_label, g_text_dpi_scale);

    if (!PushRectDrawRequest(
            MakeRect(12.0f, 168.0f, 540.0f, 108.0f),
            0.15f, 0.19f, 0.29f, 1.0f)) {
        return false;
    }

    if (!PushRectDrawRequest(
            MakeRect(12.0f, 280.0f, 240.0f, 196.0f),
            0.09f, 0.11f, 0.18f, 1.0f)) {
        return false;
    }

    if (!PushRectDrawRequest(
            MakeRect(628.0f, 48.0f, 548.0f, 490.0f),
            0.08f, 0.10f, 0.16f, 1.0f)) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(18.0f, 18.0f, 280.0f, 42.0f),
            24.0f,
            "Hello, world!")) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(300.0f, 18.0f, 220.0f, 28.0f),
            14.0f,
            scale_label)) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(18.0f, 180.0f, 520.0f, 84.0f),
            42.0f,
            "Arcanus SDF")) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(18.0f, 286.0f, 220.0f, 180.0f),
            22.0f,
            "This request uses a narrow layout rect so the text wraps across several lines inside the box. lorem ipsum whatever whatever whatever, (22 dip)")) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(640.0f, 60.0f, 520.0f, 220.0f),
            28.0f,
            "Text draw requests now carry a layout rect and a target size in DIPs, then geometry is generated from the shared atlas each frame.")) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(700.0f, 332.0f, 500.0f, 100.0f),
            56.0f,
            "56 dip")) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(640.0f, 430.0f, 260.0f, 24.0f),
            12.0f,
            "the quick brown fox jumps over the lazy dog (12 DIP)")) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(640.0f, 458.0f, 260.0f, 28.0f),
            14.0f,
            "14 DIP small text sample")) {
        return false;
    }

    if (!PushTextDrawRequest(
            MakeRect(640.0f, 490.0f, 320.0f, 32.0f),
            16.0f,
            "16 DIP small text sample")) {
        return false;
    }

    return UploadDrawList();
}

static void DrawShape(void)
{
    if (g_d3d.shape_vb == NULL || g_d3d.shape_vertex_count == 0) {
        return;
    }

    ID3D11DeviceContext_IASetInputLayout(g_d3d.context, g_d3d.shape_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_d3d.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g_d3d.context, 0, 1, &g_d3d.shape_vb, &g_d3d.shape_stride, &g_d3d.shape_offset);
    ID3D11DeviceContext_VSSetShader(g_d3d.context, g_d3d.shape_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_d3d.context, g_d3d.shape_ps, NULL, 0);
    ID3D11DeviceContext_Draw(g_d3d.context, g_d3d.shape_vertex_count, 0);
}

static void RenderTextPass(void)
{
    FLOAT blend_factor[4] = {0, 0, 0, 0};
    ID3D11ShaderResourceView *null_srv = NULL;

    if (g_d3d.text_vb == NULL || g_d3d.text_ib == NULL || g_d3d.text_srv == NULL || g_d3d.text_index_count == 0) {
        return;
    }

    ID3D11DeviceContext_IASetInputLayout(g_d3d.context, g_d3d.text_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_d3d.context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetVertexBuffers(g_d3d.context, 0, 1, &g_d3d.text_vb, &g_d3d.text_stride, &g_d3d.text_offset);
    ID3D11DeviceContext_IASetIndexBuffer(g_d3d.context, g_d3d.text_ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_VSSetShader(g_d3d.context, g_d3d.text_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_d3d.context, g_d3d.text_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_d3d.context, 0, 1, &g_d3d.text_srv);
    ID3D11DeviceContext_PSSetSamplers(g_d3d.context, 0, 1, &g_d3d.text_sampler);
    ID3D11DeviceContext_OMSetBlendState(g_d3d.context, g_d3d.text_blend, blend_factor, 0xffffffffu);
    ID3D11DeviceContext_DrawIndexed(g_d3d.context, g_d3d.text_index_count, 0, 0);
    ID3D11DeviceContext_PSSetShaderResources(g_d3d.context, 0, 1, &null_srv);
    ID3D11DeviceContext_OMSetBlendState(g_d3d.context, NULL, blend_factor, 0xffffffffu);
}

static bool DrawFrame(void)
{
    if (!BuildDemoDrawList()) {
        return false;
    }

    ID3D11DeviceContext_OMSetRenderTargets(g_d3d.context, 1, &g_d3d.rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(g_d3d.context, 1, &g_d3d.viewport);
    ID3D11DeviceContext_ClearRenderTargetView(g_d3d.context, g_d3d.rtv, g_d3d.clear);
    DrawShape();
    RenderTextPass();
    return true;
}

static void RunLoop(void)
{
    MSG msg = {0};

    while (g_running) {
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }

        if (!g_running) {
            break;
        }

        if (!DrawFrame()) {
            DestroyWindow(g_window);
            break;
        }

        IDXGISwapChain_Present(g_d3d.swap_chain, 1, 0);
    }
}

static void Cleanup(void)
{
    RELEASE(g_d3d.text_blend);
    RELEASE(g_d3d.text_sampler);
    RELEASE(g_d3d.text_srv);
    RELEASE(g_d3d.text_ib);
    RELEASE(g_d3d.text_vb);
    RELEASE(g_d3d.text_layout);
    RELEASE(g_d3d.text_ps);
    RELEASE(g_d3d.text_vs);
    RELEASE(g_d3d.shape_vb);
    RELEASE(g_d3d.shape_layout);
    RELEASE(g_d3d.shape_ps);
    RELEASE(g_d3d.shape_vs);
    RELEASE(g_d3d.rtv);

    if (g_d3d.context != NULL) {
        ID3D11DeviceContext_ClearState(g_d3d.context);
        ID3D11DeviceContext_Flush(g_d3d.context);
    }

    RELEASE(g_d3d.context);
    RELEASE(g_d3d.device);
    RELEASE(g_d3d.swap_chain);

    if (g_window != NULL) {
        DestroyWindow(g_window);
        g_window = NULL;
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        g_running = false;
        g_window = NULL;
        PostQuitMessage(0);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            DestroyWindow(hwnd);
            return 0;
        }
        if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT) {
            AdjustTextDpiScale(-0.1f);
            return 0;
        }
        if (wParam == VK_OEM_PLUS || wParam == VK_ADD) {
            AdjustTextDpiScale(0.1f);
            return 0;
        }
        break;

    case WM_SIZE:
        if (g_d3d.swap_chain != NULL && wParam != SIZE_MINIMIZED) {
            if (!ResizeBackbuffer((UINT)LOWORD(lParam), (UINT)HIWORD(lParam))) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        break;

    case WM_DPICHANGED:
        g_d3d.dpi = HIWORD(wParam);
        if (lParam != 0) {
            RECT *suggested = (RECT *)lParam;
            SetWindowPos(
                hwnd,
                NULL,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        UpdateClientMetrics();
        return 0;
    }

    return DefWindowProcA(hwnd, message, wParam, lParam);
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE prev, LPSTR cmd, int show)
{
    int exit_code = 1;

    (void)prev;
    (void)cmd;
    (void)show;

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    if (!CreateAppWindow(instance)) {
        goto cleanup;
    }

    if (!CreateDeviceAndSwapChain()) {
        goto cleanup;
    }

    if (!CreateShapeShaders()) {
        goto cleanup;
    }

    if (!CreateShapeVertexBuffer()) {
        goto cleanup;
    }

    if (!CreateTextPipeline()) {
        goto cleanup;
    }

    if (!CreateFontAtlas()) {
        goto cleanup;
    }

    RunLoop();
    exit_code = 0;

cleanup:
    Cleanup();
    return exit_code;
}

void WINAPI WinMainCRTStartup(void)
{
    ExitProcess((UINT)WinMain(GetModuleHandleA(NULL), NULL, GetCommandLineA(), SW_SHOWDEFAULT));
}
