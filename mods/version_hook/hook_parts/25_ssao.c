// ---- SSAO injection (v24) -------------------------------------------------
// The payoff of the AO recon (24_ao_recon.c). Everything here rests on what
// those six flights established:
//
//   - MS_SHADOW's final output is a full-res A8R8G8B8 composite, bound once
//     per frame at sampler 14 and consumed by ~119 material draws.
//   - Its RGB is the ENTITY/NEAR shadow term (greyscale, hard floor at 0.5 =
//     the engine's own "shadows never darken past 50%" envelope), its ALPHA
//     is the world sun-shadow mask. Two systems, one texture.
//   - The R32F linear-depth prepass texture is bindable and full-res.
//
// Injection (v25, blurred pipeline): at the s14 bind,
//   1. estimator pass  -> RT A   (AO term as grey, opaque)
//   2. horizontal blur -> RT B   (depth-aware bilateral, opaque)
//   3. vertical blur   -> the composite, dst*src multiply with the alpha
//      path pinned ZERO/ONE so the world sun-shadow mask survives bit-exact
//      (THE v24-series bug: the engine leaves SEPARATEALPHABLENDENABLE on
//      with its own factors, and an inherited alpha-replace wipes the mask).
// The bilateral blur is the standard cure for raw-estimator grain: 9-tap
// gaussian per direction, each tap weighted down by relative depth
// difference so the smoothing never bleeds across silhouettes. AoBlur=0
// falls back to the v24 single-pass direct multiply (the A/B lever).
//
// RT A/B are D3DPOOL_DEFAULT and MUST be released before a device Reset -
// holding one blocks the Reset outright (same rule as the MSAA surfaces,
// FEATURES.md "Attempt 5 step 1"). SsaoReleaseRts() rides AoReconReset(),
// which HookedDeviceReset calls BEFORE forwarding the Reset, and which the
// SSAA screen-set rebuild path calls too - so the pair is also recreated at
// the right size when the composite's resolution changes.
//
// Estimator: Alchemy/SAO-style spiral (McGuire et al.) - the best fit for
// depth-only ps_3_0: view-space position from linear depth, normal from
// ddx/ddy, 12 spiral taps with per-pixel interleaved-gradient rotation.
// Deliberately isolated in one HLSL function so an HBAO horizon-march can
// replace it later without touching any plumbing.
//
// The shaders are compiled AT RUNTIME with D3DXCompileShader from
// d3dx9_43.dll - which the GAME imports, so it is guaranteed present and
// loaded. The HLSL lives in these strings: nothing is loaded from disk, and
// the tunables are live ini values rather than recompiles.

#if ENABLE_AO_SSAO

// ---- tunables (config keys in 08, declared in 01 for TU order) ------------
// AoEnable      0/1      master switch (menu toggle)
// AoDebug       0/1      draw raw AO opaquely instead of blending -
//                        the only sane way to tune radius/strength by eye
// AoStrengthPct 0..200   how much of the [0.5..1] envelope AO may use
// AoRadius100   world-units radius x100 (engine units - tuned by eye)
// AoProj100     projection scale x100 (cot(fovY/2)); wrong values show as
//                        AO that stretches with screen position in debug view
// AoBlur        0/1      bilateral blur pass (the noise cure; 0 = v24 direct)
// AoBlurSharp   0..400   depth edge-stop: how hard the blur refuses to cross
//                        depth discontinuities (0 = plain gaussian)

static const char *g_ssaoHlsl =
"sampler2D depthTex : register(s0);\n"
"float4 cParam0 : register(c0);\n"   // x=texelW y=texelH z=radius w=strength
"float4 cParam1 : register(c1);\n"   // x=projX  y=projY  z=bias   w=intensity
"float3 ViewPos(float2 uv) {\n"
"    float z = tex2Dlod(depthTex, float4(uv, 0, 0)).r;\n"
"    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);\n"
"    return float3(ndc.x * z / cParam1.x, ndc.y * z / cParam1.y, z);\n"
"}\n"
"float4 cParam2 : register(c2);\n"   // x=debug mode (0/1), yzw unused
"float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR {\n"
"    float3 P = ViewPos(uv);\n"
"    float zRaw = P.z;\n"
// cross(ddx, ddy), NOT (ddy, ddx): view space is x-right/y-up/z-into-screen
// and screen v runs DOWN, so the other order points normals AWAY from the
// camera - every dot(v,N) clamps to zero and AO is white everywhere.
// (Depth units were correct all along: measured 4..2000 world units.)
// NaN guard (v24f): on flat-depth regions (sky, surfaces parallel to a
// derivative axis) cross() is ~zero and normalize(0) is NaN; NaN written to
// the shadow term renders geometry BLACK (user-observed in every flight).
"    float3 g = cross(ddx(P), ddy(P));\n"
"    float gl = length(g);\n"
"    float3 N = (gl > 1e-6) ? g / gl : float3(0, 0, -1);\n"
"    float ign = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));\n"
"    float ca = cos(ign * 6.2831853), sa = sin(ign * 6.2831853);\n"
"    float occ = 0.0;\n"
"    float rPix = cParam0.z * cParam1.y / P.z;\n"      // world radius -> uv
"#if ESTIMATOR == 1\n"
// HBAO (horizon-based): 4 rotated directions, 4 marching steps each. Each
// direction contributes its HORIZON - the highest elevation above the
// tangent plane found along the ray - attenuated by how far away that
// horizon point sits. Rays accumulate monotonically instead of Alchemy's
// independent per-tap coin flips, which is exactly why it is less noisy at
// a comparable tap count (16 vs 12).
"    [unroll] for (int di = 0; di < 4; di++) {\n"
"        float ang = (di + 0.5) * 1.5707963;\n"
"        float2 d0 = float2(cos(ang), sin(ang));\n"
"        float2 dir = float2(d0.x * ca - d0.y * sa, d0.x * sa + d0.y * ca);\n"
"        float sinH = 0.0, wH = 0.0;\n"
"        [unroll] for (int st = 1; st <= 4; st++) {\n"
"            float2 duv = dir * (rPix * 0.5 * (float)st / 4.0);\n"
"            duv.y *= cParam0.y / cParam0.x;\n"        // aspect-correct
"            float3 Q = ViewPos(uv + duv);\n"
"            float3 v = Q - P;\n"
"            float vl2 = dot(v, v) + 1e-5;\n"
"            float sinS = dot(v, N) * rsqrt(vl2);\n"
"            [flatten] if (sinS > sinH) {\n"
"                sinH = sinS;\n"
"                wH = saturate(1.0 - vl2 / (cParam0.z * cParam0.z));\n"
"            }\n"
"        }\n"
// Sin-space bias (c1.z, ~0.15 here vs Alchemy's 0.02 depth-proportional):
// suppresses the tangent-plane self-occlusion the ddx/ddy faceted normals
// would otherwise manufacture on every smooth surface.
"        occ += saturate(sinH - cParam1.z) * wH;\n"
"    }\n"
"    float aoBase = saturate(1.0 - cParam1.w * occ * 0.25);\n"
"#else\n"
"    [unroll] for (int i = 0; i < 12; i++) {\n"
"        float ang = (i + 0.5) * (6.2831853 * 0.3819661);\n"
"        float rad = sqrt((i + 0.5) / 12.0) * rPix * 0.5;\n"
"        float2 d0 = float2(cos(ang), sin(ang));\n"
"        float2 duv = float2(d0.x * ca - d0.y * sa, d0.x * sa + d0.y * ca) * rad;\n"
"        duv.y *= cParam0.y / cParam0.x;\n"            // aspect-correct
"        float3 Q = ViewPos(uv + duv);\n"
"        float3 v = Q - P;\n"
"        occ += max(0.0, dot(v, N) - cParam1.z * P.z)\n"
"             / (dot(v, v) + 0.01);\n"
"    }\n"
"    float aoBase = saturate(1.0 - cParam1.w * occ / 12.0);\n"
"#endif\n"
"    if (zRaw > 1500.0) aoBase = 1.0;\n"               // sky/far (far ~2000)
// Strength deliberately UNsaturated: >100% pushes the term below the
// engine's 0.5 floor for deeper-than-stock creases (output clamps at 0).
"    float ao = 1.0 - cParam0.w * (1.0 - aoBase);\n"
"    float term = 0.5 + 0.5 * ao;\n"                   // map into [0.5..1]
// Mode 3: RAW view - the estimator's own output as full-range grey, drawn
// over the finished frame: no albedo, no shadow term, no [0.5..1] mapping.
"    if (cParam2.x > 2.5) return float4(aoBase, aoBase, aoBase, 1.0);\n"
// Debug = one screen, four vertical bands, each a pipeline stage:
//   [0-25%]  depth stripes: a grey cycle per 20 world units. FLAT GREY here
//            means the depth sample itself is broken (bind or sampler).
//   [25-50%] normals as colour. BLACK means derivatives/normal broke;
//            uniform single colour means depth was flat.
//   [50-75%] raw occlusion sum x2. BLACK means the estimator finds nothing
//            even though depth+normals work (radius/units problem).
//   [75-100%] the final term as it would be written.
"    if (cParam2.x > 0.5) {\n"
"        if (uv.x < 0.25)      { float s = frac(zRaw * 0.05); return float4(s, s, s, 1); }\n"
"        else if (uv.x < 0.5)  { float3 nc = N * 0.5 + 0.5; return float4(nc, 1); }\n"
"        else if (uv.x < 0.75) { float s = saturate(occ * 2.0 / 12.0); return float4(s, s, s, 1); }\n"
"        return float4(term, term, term, 1);\n"
"    }\n"
"    return float4(term, term, term, 1.0);\n"          // alpha 1: mult keeps sun mask
"}\n";

// Separable bilateral blur, one shader for both directions (c0.zw selects).
// 9 taps: centre + 4 each side at 1px spacing, gaussian sigma ~2.3px. Each
// tap's weight is cut by RELATIVE depth difference (dz/z, so the edge-stop
// behaves the same at 5 units and 500), which is what keeps AO from
// bleeding across silhouettes - a plain gaussian here reads as haloes
// around every character against the sky.
static const char *g_aoBlurHlsl =
"sampler2D aoTex    : register(s0);\n"
"sampler2D depthTex : register(s1);\n"
"float4 cB0 : register(c0);\n"   // x=texelW y=texelH z=dirX w=dirY
"float4 cB1 : register(c1);\n"   // x=edge-stop sharpness (AoBlurSharp)
"float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
"    float z0 = tex2Dlod(depthTex, float4(uv, 0, 0)).r;\n"
"    float2 stp = cB0.zw * cB0.xy;\n"
"    float sum = tex2Dlod(aoTex, float4(uv, 0, 0)).r;\n"
"    float wsum = 1.0;\n"
"    static const float gw[5] = { 1.0, 0.84, 0.49, 0.20, 0.06 };\n"
"    [unroll] for (int i = 1; i <= 4; i++) {\n"
"        [unroll] for (int s = 0; s < 2; s++) {\n"
"            float2 u2 = uv + stp * (float)((s * 2 - 1) * i);\n"
"            float zi = tex2Dlod(depthTex, float4(u2, 0, 0)).r;\n"
"            float w = gw[i] * saturate(1.0 - cB1.x * abs(zi - z0) / max(z0, 1.0));\n"
"            sum += tex2Dlod(aoTex, float4(u2, 0, 0)).r * w;\n"
"            wsum += w;\n"
"        }\n"
"    }\n"
"    float v = sum / wsum;\n"
"    return float4(v, v, v, 1.0);\n"
"}\n";

typedef struct ID3DXBuffer ID3DXBuffer;   // vtable slots used: 3 GetBufferPointer, 4 GetBufferSize
typedef struct { const char *Name, *Definition; } AoHlslMacro;   // D3DXMACRO layout
typedef HRESULT (WINAPI *PFN_D3DXCompileShader)(
    const char *, UINT, const void *, const void *, const char *, const char *,
    DWORD, ID3DXBuffer **, ID3DXBuffer **, void *);

// Estimator shaders by index: 0 = Alchemy spiral (AoEnable=1), 1 = HBAO
// horizon march (AoEnable=2). Same HLSL string, selected by the ESTIMATOR
// define at compile time; each compiles lazily on first use.
static IDirect3DPixelShader9 *g_aoPs[2] = { NULL, NULL };
static LONG g_aoPsState[2] = { 0, 0 };   // 0 not tried, 1 ok, -1 failed
static IDirect3DPixelShader9 *g_aoBlurPs = NULL;
static LONG g_aoBlurState = 0;
static volatile LONG g_ssaoDraws = 0;

// The blur ping-pong pair, sized to the composite. D3DPOOL_DEFAULT: released
// via SsaoReleaseRts() before every Reset/screen-set rebuild, recreated
// lazily (which is also what resizes them when SSAA changes the composite).
static IDirect3DTexture9 *g_aoRtA = NULL, *g_aoRtB = NULL;
static LONG g_aoRtW = 0, g_aoRtH = 0;

static void *BufPtr(ID3DXBuffer *b)
{
    void **vtbl = *(void ***)b;
    typedef void *(STDMETHODCALLTYPE *PFN_GetPtr)(ID3DXBuffer *);
    return ((PFN_GetPtr)vtbl[3])(b);
}
static void BufRelease(ID3DXBuffer *b)
{
    void **vtbl = *(void ***)b;
    typedef ULONG (STDMETHODCALLTYPE *PFN_Rel)(ID3DXBuffer *);
    ((PFN_Rel)vtbl[2])(b);
}

static IDirect3DPixelShader9 *AoCompilePs(
    IDirect3DDevice9 *dev, const char *src, const AoHlslMacro *defs, const char *what)
{
    HMODULE hDx = GetModuleHandleA("d3dx9_43.dll");
    if (!hDx) { LogLine("[ssao] d3dx9_43.dll not loaded - cannot compile"); return NULL; }
    PFN_D3DXCompileShader compile =
        (PFN_D3DXCompileShader)GetProcAddress(hDx, "D3DXCompileShader");
    if (!compile) { LogLine("[ssao] D3DXCompileShader not found"); return NULL; }

    ID3DXBuffer *code = NULL, *errs = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    HRESULT hr = compile(src, (UINT)strlen(src), defs, NULL,
                         "main", "ps_3_0", 0, &code, &errs, NULL);
    if (FAILED(hr) || !code) {
        char l[320];
        sprintf(l, "[ssao] %s compile FAILED hr=0x%08lX: %.200s",
                what, (unsigned long)hr, errs ? (const char *)BufPtr(errs) : "(no error text)");
        LogLine(l);
        if (errs) BufRelease(errs);
        if (code) BufRelease(code);
        return NULL;
    }
    hr = IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)BufPtr(code), &ps);
    BufRelease(code);
    if (errs) BufRelease(errs);
    if (FAILED(hr) || !ps) {
        char l[128];
        sprintf(l, "[ssao] %s CreatePixelShader FAILED hr=0x%08lX", what, (unsigned long)hr);
        LogLine(l);
        return NULL;
    }
    {
        char l[96];
        sprintf(l, "[ssao] %s pixel shader compiled and created (ps_3_0)", what);
        LogLine(l);
    }
    return ps;
}

static void AoEnsureShaders(IDirect3DDevice9 *dev, int est)
{
    if (g_aoPsState[est] == 0) {
        AoHlslMacro defs[2];
        defs[0].Name = "ESTIMATOR";
        defs[0].Definition = est ? "1" : "0";
        defs[1].Name = NULL;
        defs[1].Definition = NULL;
        g_aoPs[est] = AoCompilePs(dev, g_ssaoHlsl, defs,
                                  est ? "HBAO (4-dir horizon march)"
                                      : "SSAO (Alchemy spiral, 12 taps)");
        g_aoPsState[est] = g_aoPs[est] ? 1 : -1;
    }
    if (g_aoBlurState == 0) {
        g_aoBlurPs = AoCompilePs(dev, g_aoBlurHlsl, NULL,
                                 "bilateral blur (9-tap separable)");
        g_aoBlurState = g_aoBlurPs ? 1 : -1;
    }
}

// Pixel shaders survive Reset (pool-independent storage); the RT pair does
// not get to. Called from AoReconReset() - i.e. BEFORE HookedDeviceReset
// forwards the Reset, and at both SSAA screen-set rebuild sites.
static void SsaoReleaseRts(void)
{
    if (g_aoRtA) { IDirect3DTexture9_Release(g_aoRtA); g_aoRtA = NULL; }
    if (g_aoRtB) { IDirect3DTexture9_Release(g_aoRtB); g_aoRtB = NULL; }
    g_aoRtW = g_aoRtH = 0;
}

static int AoEnsureRts(IDirect3DDevice9 *dev, UINT w, UINT h)
{
    if (g_aoRtA && g_aoRtB && g_aoRtW == (LONG)w && g_aoRtH == (LONG)h) return 1;
    SsaoReleaseRts();
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_aoRtA, NULL)) || !g_aoRtA) {
        SsaoReleaseRts();
        LogLine("[ssao] blur RT A creation failed - falling back to direct");
        return 0;
    }
    if (FAILED(IDirect3DDevice9_CreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_aoRtB, NULL)) || !g_aoRtB) {
        SsaoReleaseRts();
        LogLine("[ssao] blur RT B creation failed - falling back to direct");
        return 0;
    }
    g_aoRtW = (LONG)w;
    g_aoRtH = (LONG)h;
    {
        char l[96];
        sprintf(l, "[ssao] blur RT pair created %ux%u A8R8G8B8", w, h);
        LogLine(l);
    }
    return 1;
}

static void AoBindTex(IDirect3DDevice9 *dev, DWORD stage, IDirect3DBaseTexture9 *t)
{
    g_origSetTexture(dev, stage, t);
    IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    IDirect3DDevice9_SetSamplerState(dev, stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

static int AoTarget(IDirect3DDevice9 *dev, IDirect3DSurface9 *surf, UINT w, UINT h)
{
    if (FAILED(g_origSetRT(dev, 0, surf))) return 0;
    {
        D3DVIEWPORT9 vp;
        vp.X = 0; vp.Y = 0;
        vp.Width = w; vp.Height = h;
        vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
        g_origSetViewport(dev, &vp);
    }
    return 1;
}

static void AoDrawFsQuad(IDirect3DDevice9 *dev, UINT w, UINT h)
{
    // Half-texel offset: D3D9 maps texels to pixel CENTRES.
    struct { float x, y, z, rhw, u, v; } q[4];
    float W = (float)w, H = (float)h;
    for (int k = 0; k < 4; k++) {
        q[k].x = ((k & 1) ? W : 0.0f) - 0.5f;
        q[k].y = ((k & 2) ? H : 0.0f) - 0.5f;
        q[k].z = 0.0f; q[k].rhw = 1.0f;
        q[k].u = (k & 1) ? 1.0f : 0.0f;
        q[k].v = (k & 2) ? 1.0f : 0.0f;
    }
    IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
}

static void AoSetEstimatorConsts(IDirect3DDevice9 *dev, UINT w, UINT h,
                                 float mode, int est)
{
    float c0[4], c1[4], c2[4];
    c0[0] = 1.0f / (float)w;
    c0[1] = 1.0f / (float)h;
    c0[2] = (float)g_aoRadius100 / 100.0f;
    c0[3] = (float)g_aoStrengthPct / 100.0f;
    c1[0] = ((float)g_aoProj100 / 100.0f) * ((float)h / (float)w);
    c1[1] = (float)g_aoProj100 / 100.0f;
    // Bias units differ per estimator: Alchemy multiplies by P.z inside the
    // shader (depth-proportional), HBAO compares in sin-of-elevation space.
    c1[2] = est ? 0.15f : 0.02f;
    c1[3] = (float)g_aoIntensity100 / 100.0f;   // estimator gain, live-tunable
    c2[0] = mode;
    c2[1] = c2[2] = c2[3] = 0.0f;
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, c1, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 2, c2, 1);
    if (g_ssaoDraws == 0) {
        char l[192];
        sprintf(l, "[ssao] consts est=%d c0=(%.5f %.5f %.2f %.2f) c1=(%.3f %.3f %.3f %.3f)",
                est, c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3]);
        LogLine(l);
    }
}

static void AoSetBlurConsts(IDirect3DDevice9 *dev, UINT w, UINT h,
                            float dx, float dy)
{
    float c0[4], c1[4];
    c0[0] = 1.0f / (float)w;
    c0[1] = 1.0f / (float)h;
    c0[2] = dx; c0[3] = dy;
    c1[0] = (float)g_aoBlurSharp;
    c1[1] = c1[2] = c1[3] = 0.0f;
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, c1, 1);
}

// The alpha pin, needed on every draw whose target is the composite: the
// engine runs with SEPARATEALPHABLENDENABLE on and its own factors, and an
// inherited alpha-replace wipes the world sun-shadow mask (the entire
// v24-series "SSAO removes shadows" saga, found by the post-injection dump:
// alpha measured 255 at every pixel while RGB multiplied correctly).
static void AoBlendMultiply(IDirect3DDevice9 *dev)
{
    g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
    g_origSetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_ADD);
    g_origSetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_DESTCOLOR);
    g_origSetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_ZERO);
    g_origSetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    g_origSetRenderState(dev, D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD);
    g_origSetRenderState(dev, D3DRS_SRCBLENDALPHA, D3DBLEND_ZERO);
    g_origSetRenderState(dev, D3DRS_DESTBLENDALPHA, D3DBLEND_ONE);
    g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x0F);
}

static void AoBlendOpaque(IDirect3DDevice9 *dev, DWORD writeMask)
{
    g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
    g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, writeMask);
}

// Runs at the s14 bind, before the engine's consumers. dev-state discipline
// identical to the tint probe: D3DSBT_ALL block plus hand-restored RT0.
// raw != 0: draw the estimator's output straight onto the CURRENT render
// target (the backbuffer at DRAW_MENU time) - the true-raw debug view the
// in-buffer debug mode cannot provide, since that one is always seen
// through the materials' albedo multiply. The raw view goes through the
// same blur the applied AO gets (when AoBlur=1), so it shows what actually
// lands in the composite, not a prettier or uglier cousin of it.
static void SsaoApply(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *tex, int raw)
{
    static LONG lastFrame = -1, lastRawFrame = -1;
    LONG fr = g_msFrameSeq;
    if (raw) { if (fr == lastRawFrame) return; lastRawFrame = fr; }
    else     { if (fr == lastFrame)    return; lastFrame = fr; }

    int est = (g_aoEnable == 2) ? 1 : 0;
    if (g_aoPsState[est] == 0 || (g_aoBlur && g_aoBlurState == 0))
        AoEnsureShaders(dev, est);
    if (g_aoPsState[est] != 1) return;
    if (!g_aoDepthTex) return;

    // Debug bands are a pipeline diagnostic - never blurred. Blur also
    // degrades to direct if its shader or RTs failed.
    int useBlur = (g_aoBlur && !g_aoDebug && g_aoBlurState == 1) ? 1 : 0;

    IDirect3DSurface9 *dstSurf = NULL, *oldRt = NULL;
    IDirect3DSurface9 *surfA = NULL, *surfB = NULL;
    IDirect3DStateBlock9 *sb = NULL;
    __try {
        D3DSURFACE_DESC d;
        if (FAILED(g_origGetRenderTarget(dev, 0, &oldRt)) || !oldRt) goto done;
        if (raw) {
            // Current RT0 (the backbuffer during DRAW_MENU) is the target.
            dstSurf = oldRt;
            if (FAILED(IDirect3DSurface9_GetDesc(dstSurf, &d))) goto done;
        } else {
            if (FAILED(IDirect3DTexture9_GetSurfaceLevel(
                    (IDirect3DTexture9 *)tex, 0, &dstSurf)) || !dstSurf) goto done;
            if (FAILED(IDirect3DSurface9_GetDesc(dstSurf, &d))) goto done;
        }
        if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb)) || !sb) goto done;

        if (useBlur) {
            if (raw) {
                // The raw view rides the RT pair the normal path owns (it
                // only draws while AoEnable is on, so the pair exists at
                // composite size). Creating a second pair at backbuffer
                // size would thrash recreation every frame under SSAA.
                if (!g_aoRtA || !g_aoRtB) useBlur = 0;
            } else {
                if (!AoEnsureRts(dev, d.Width, d.Height)) useBlur = 0;
            }
        }
        if (useBlur) {
            if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_aoRtA, 0, &surfA)) || !surfA)
                useBlur = 0;
            else if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_aoRtB, 0, &surfB)) || !surfB)
                useBlur = 0;
        }

        // States shared by every pass.
        IDirect3DDevice9_SetVertexShader(dev, NULL);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
        g_origSetRenderState(dev, D3DRS_ZENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);

        if (useBlur) {
            UINT rw = (UINT)g_aoRtW, rh = (UINT)g_aoRtH;
            // Pass 1: estimator -> RT A, opaque. Raw mode writes aoBase
            // (mode 3), normal writes the [0.5..1] term (mode 0) - the
            // mapping is linear, so blurring the term equals blurring ao.
            if (!AoTarget(dev, surfA, rw, rh)) goto done;
            IDirect3DDevice9_SetPixelShader(dev, g_aoPs[est]);
            AoBindTex(dev, 0, (IDirect3DBaseTexture9 *)g_aoDepthTex);
            AoBlendOpaque(dev, 0x0F);
            AoSetEstimatorConsts(dev, rw, rh, raw ? 3.0f : 0.0f, est);
            AoDrawFsQuad(dev, rw, rh);
            // Pass 2: horizontal blur, RT A -> RT B, opaque.
            IDirect3DDevice9_SetPixelShader(dev, g_aoBlurPs);
            if (!AoTarget(dev, surfB, rw, rh)) goto done;
            AoBindTex(dev, 0, (IDirect3DBaseTexture9 *)g_aoRtA);
            AoBindTex(dev, 1, (IDirect3DBaseTexture9 *)g_aoDepthTex);
            AoSetBlurConsts(dev, rw, rh, 1.0f, 0.0f);
            AoDrawFsQuad(dev, rw, rh);
            // Pass 3: vertical blur, RT B -> destination. Multiply with the
            // alpha pin into the composite; opaque overwrite for raw view.
            if (!AoTarget(dev, dstSurf, d.Width, d.Height)) goto done;
            AoBindTex(dev, 0, (IDirect3DBaseTexture9 *)g_aoRtB);
            AoSetBlurConsts(dev, rw, rh, 0.0f, 1.0f);
            if (raw) AoBlendOpaque(dev, 0x0F);
            else     AoBlendMultiply(dev);
            AoDrawFsQuad(dev, d.Width, d.Height);
        } else {
            // Single-pass direct path (AoBlur=0, debug bands, or blur infra
            // unavailable) - the v24 behavior, unchanged.
            if (!AoTarget(dev, dstSurf, d.Width, d.Height)) goto done;
            IDirect3DDevice9_SetPixelShader(dev, g_aoPs[est]);
            AoBindTex(dev, 0, (IDirect3DBaseTexture9 *)g_aoDepthTex);
            if (raw) {
                // Opaque overwrite; the UI draws after this, stays readable.
                AoBlendOpaque(dev, 0x0F);
            } else if (g_aoDebug) {
                // Opaque replace: the raw AO term fills the buffer so the
                // whole screen SHOWS it (materials multiply it in) - the
                // tuning view. Colour channels only: replacing ALPHA wipes
                // the world sun-shadow mask, which is a diagnostic
                // contaminating the very thing being diagnosed.
                AoBlendOpaque(dev, 0x07);
            } else {
                AoBlendMultiply(dev);
            }
            AoSetEstimatorConsts(dev, d.Width, d.Height,
                                 raw ? 3.0f : (g_aoDebug ? 1.0f : 0.0f), est);
            AoDrawFsQuad(dev, d.Width, d.Height);
        }

        InterlockedIncrement(&g_ssaoDraws);
        if (g_ssaoDraws == 1) {
            char l[128];
            sprintf(l, "[ssao] first draw: est=%s path=%s",
                    est ? "HBAO" : "Alchemy",
                    useBlur ? "estimator->blurH->blurV->composite"
                            : (g_aoDebug ? "DEBUG bands direct" : "direct multiply"));
            LogLine(l);
        }
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (oldRt) { g_origSetRT(dev, 0, oldRt); }
    if (sb) { IDirect3DStateBlock9_Apply(sb); IDirect3DStateBlock9_Release(sb); }
    if (surfA) IDirect3DSurface9_Release(surfA);
    if (surfB) IDirect3DSurface9_Release(surfB);
    if (dstSurf && dstSurf != oldRt) IDirect3DSurface9_Release(dstSurf);
    if (oldRt) IDirect3DSurface9_Release(oldRt);
}

// Device Reset invalidates the shaders? No - pixel shaders live in
// D3DPOOL-independent storage and survive Reset. The state block does not,
// but it is created and released within one call. The RT pair is the one
// default-pool holding, handled by SsaoReleaseRts() above.

#endif // ENABLE_AO_SSAO
