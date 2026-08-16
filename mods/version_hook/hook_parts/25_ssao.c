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
//   1. estimator pass -> RT A    (AO term as grey, opaque)
//   2. N a-trous levels, each a separable H then V with DOUBLED tap
//      spacing, ping-ponging A->B->A
//   3. the last vertical pass -> the composite, dst*src multiply with the
//      alpha path pinned ZERO/ONE so the world sun-shadow mask survives
//      bit-exact (THE v24-series bug: the engine leaves
//      SEPARATEALPHABLENDENABLE on with its own factors, and an inherited
//      alpha-replace wipes the mask).
// The cross-bilateral blur is the standard cure for raw-estimator grain:
// 9-tap gaussian per direction, each tap weighted down by relative depth
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
//                        (per-estimator slot, as are Intensity and Radius:
//                        AoHbao* keys hold HBAO's values)
// AoRadius100   world-units radius x100 (engine units - tuned by eye)
// AoProj100     projection scale x100 (cot(fovY/2)); wrong values show as
//                        AO that stretches with screen position in debug view
// AoBlur        0/1      bilateral blur pass (the noise cure; 0 = v24 direct)
// AoBlurSharp   0..400   depth edge-stop: how hard the blur refuses to cross
//                        depth discontinuities (0 = plain gaussian)

static const char *g_ssaoHlsl =
"sampler2D depthTex : register(s12);\n"
"float4 cParam0 : register(c220);\n"   // x=texelW y=texelH z=radius w=strength
"float4 cParam1 : register(c221);\n"   // x=projX  y=projY  z=bias   w=intensity
"float3 ViewPos(float2 uv) {\n"
"    float z = tex2Dlod(depthTex, float4(uv, 0, 0)).r;\n"
"    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);\n"
"    return float3(ndc.x * z / cParam1.x, ndc.y * z / cParam1.y, z);\n"
"}\n"
"float4 cParam2 : register(c222);\n"   // x=debug mode (0/1), yzw unused
"float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR {\n"
"    float3 P = ViewPos(uv);\n"
"    float zRaw = P.z;\n"
// Guard 1 (v25c): INVALID DEPTH. Everything downstream divides by P.z, so
// a pixel whose depth never reached the prepass (z==0: alpha-blended
// geometry skips depth prepasses, and menu scenes do not always run a full
// one) makes rPix INF, the tap offsets NaN, and the whole occlusion sum
// NaN - which the engine renders as a FULLY BLACK object, uniform and
// hard-edged. That is the 2026-08-16 black-shield report in the main menu,
// and the same class as the v24f normalize(0) black geometry. No valid
// depth means no AO opinion: return white (no darkening).
"    if (!(zRaw > 0.05)) return float4(1, 1, 1, 1);\n"
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
// Rotation noise: WHITE-NOISE hash, not interleaved gradient noise. IGN's
// iso-value contours are parallel diagonal lines, and since all taps in a
// pixel rotate by the same angle, the estimator's residual error inherits
// that structure - user-observed as a diagonal hatch pattern in BOTH
// estimators, surviving the blur (which is axis-aligned separable and
// cannot chase correlation along long diagonal runs). White noise turns
// the lines into per-pixel grain, which a separable gaussian actually
// removes. vpos is wrapped before the sin-hash: sin() of large arguments
// loses precision on some GPUs and re-introduces banding.
"    float2 np = fmod(vpos, 1024.0);\n"
"    float ign = frac(sin(dot(np, float2(12.9898, 78.233))) * 43758.5453);\n"
"    float ca = cos(ign * 6.2831853), sa = sin(ign * 6.2831853);\n"
"    float occ = 0.0;\n"
"    float rPix = cParam0.z * cParam1.y / P.z;\n"      // world radius -> uv
// Guard 2: near-camera geometry. A world-space radius projects to a HUGE
// screen radius up close (0.6 units at z=1 is most of the screen), which
// samples unrelated geometry and manufactures maximum occlusion - black
// objects again, this time with a real number. Menu and cutscene framing
// put geometry far closer to the camera than gameplay ever does.
"    rPix = min(rPix, 0.25);\n"
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
// The engine's [0.5..1] ENVELOPE, and why staying inside it is the default
// (v25e). Measured from an A/B dump pair of the same menu frame: with AO
// off the composite's RGB minimum is EXACTLY 128 across the whole frame -
// the engine never writes below 0.5, which the original recon already
// called its "shadows never darken past 50%" cap. With AO on the minimum
// was 24, so 3.86% of the frame sat under a floor the materials are
// written to assume. Materials that DECODE that range (the natural
// (term-0.5)*2 remap) turn everything at or below 0.5 into pure black and
// double the apparent strength of everything above it - which is the
// black-shield report, and why it looks flat and hard-edged instead of
// like too much AO. cParam2.y = 0 restores the old unsaturated behaviour.
"    float ao = 1.0 - cParam0.w * (1.0 - aoBase);\n"
"    if (cParam2.y > 0.5) ao = saturate(ao);\n"
"    float term = 0.5 + 0.5 * ao;\n"                   // map into [0.5..1]
// Guard 3: catch-all NaN/INF scrub. Guards 1 and 2 close the two known
// sources, but a NaN reaching the shadow term is catastrophic and silent
// (uniform black geometry, no gradient, looks nothing like an AO bug), so
// the last line of defence is unconditional: NaN fails every comparison,
// so this asks "is term a real number" rather than trying to spot NaN.
// The bounds are deliberately far outside the legitimate range instead of
// [0..1]: strength >100% is SUPPOSED to drive the term to 0 (deeper than
// the engine's own floor) and float rounding can put it a hair below, so a
// tight test would flip the deepest creases to white - the opposite bug.
"    term = (term > -1000.0 && term < 1000.0) ? term : 1.0;\n"
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

// Separable cross-bilateral blur, one shader for both directions (c0.zw
// selects) and every a-trous level (c1.y = tap spacing in pixels).
//
// Two weights per tap, multiplied: a fixed spatial GAUSSIAN (sigma ~2.3
// taps) times a RANGE term on depth. The range term is what stops AO
// bleeding across silhouettes - a plain gaussian here reads as haloes
// around every character against the sky - and it is computed from a
// different buffer than the one being filtered, which is what makes this
// "cross"/"joint" bilateral rather than plain bilateral.
//
// A-trous (Dammertz et al., as used by SVGF): instead of one huge kernel,
// run the same 9 taps repeatedly with DOUBLING spacing. Reach grows
// 9/17/33/65 px for a cost that only grows linearly, and because every
// level re-applies the edge-stop, wide smoothing still respects
// silhouettes.
//
// The depth tolerance is scaled by tap spacing CPU-side (see
// AoSetBlurConsts). Without that, a floor at a grazing angle - where depth
// legitimately changes fast per pixel - has every tap rejected as if it
// were a silhouette, so the blur silently turns itself OFF exactly where
// the grain is worst (user-observed 2026-08-16: "top left is fine, bottom
// is still a bit diagonal heavy").
static const char *g_aoBlurHlsl =
"sampler2D aoTex    : register(s12);\n"
"sampler2D depthTex : register(s13);\n"
"float4 cB0 : register(c220);\n"   // x=texelW y=texelH z=dirX w=dirY
"float4 cB1 : register(c221);\n"   // x=edge-stop sharpness y=tap spacing (px)
"float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
"    float z0 = tex2Dlod(depthTex, float4(uv, 0, 0)).r;\n"
"    float2 stp = cB0.zw * cB0.xy * cB1.y;\n"
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
// Same scrub as the estimator: a NaN in the depth buffer would poison every
// tap weight here independently of what the estimator produced.
"    v = (v > -1000.0 && v < 1000.0) ? v : 1.0;\n"
"    return float4(v, v, v, 1.0);\n"
"}\n";

// Combine pass (v25f). THE reason this exists: a blend cannot read its own
// destination, so "multiply, but never below the engine's floor" is not
// expressible as blend state. Measured with an A/B dump pair: the engine's
// own composite minimum is EXACTLY 128 (0.5) frame-wide, and dst*src drove
// it to 64 - engine floor 0.5 times our floor 0.5 - because a MULTIPLY
// STACKS. Clamping our own term (v25e) could not fix that; the product is
// what has to be clamped, so the destination has to be readable. It is
// copied into RT B first and this pass writes the clamped result back.
//
// Alpha is protected here by the WRITE MASK (COLORWRITEENABLE = RGB only)
// rather than by pinned blend factors - strictly safer, since it cannot be
// defeated by whatever the engine left in the blend state.
static const char *g_aoCombineHlsl =
"sampler2D aoTex  : register(s12);\n"
"sampler2D engTex : register(s13);\n"   // copy of the engine's own composite
"float4 cK0 : register(c220);\n"         // x=respect floor  y=passthrough
"float4 main(float2 uv : TEXCOORD0) : COLOR {\n"
"    float3 ao = tex2D(aoTex, uv).rgb;\n"
"    if (cK0.y > 0.5) return float4(ao, 1.0);\n"   // raw view / debug bands
"    float3 eng = tex2D(engTex, uv).rgb;\n"
// Flat-write test (cK0.z > 0): the whole pipeline runs, but the value
// written is a CONSTANT multiplier with no estimator influence. Splits the
// black-model bug's remaining suspect space in one observation: black
// under a flat x0.9 means the mere act of darkening blackens that
// material (content side); clean means the estimator's VALUES are the
// problem, not the machinery.
"    if (cK0.z > 0.0) {\n"
"        float3 f = eng * cK0.z;\n"
"        if (cK0.x > 0.5) f = max(f, 0.5);\n"
"        return float4(f, 1.0);\n"
"    }\n"
"    float3 outc = eng * ao.r;\n"
// Where the engine is already at its maximum darkness, AO adds nothing;
// where it is lit, AO may darken it to that same maximum and no further.
// The two occlusions merge instead of stacking - which was the original
// v24 MIN-blend intent, finally expressible now that dst is readable.
"    if (cK0.x > 0.5) outc = max(outc, 0.5);\n"
"    return float4(outc, 1.0);\n"
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
static IDirect3DPixelShader9 *g_aoCombinePs = NULL;
static LONG g_aoCombineState = 0;
static volatile LONG g_ssaoDraws = 0;

// The blur ping-pong pair, sized to the composite. D3DPOOL_DEFAULT: released
// via SsaoReleaseRts() before every Reset/screen-set rebuild, recreated
// lazily (which is also what resizes them when SSAA changes the composite).
static IDirect3DTexture9 *g_aoRtA = NULL, *g_aoRtB = NULL;
static LONG g_aoRtW = 0, g_aoRtH = 0;
static LONG g_aoMrtLogged = 0;   // one-shot: are MRT slots 1-3 in use here?

// ---- THE SELF-INTERFERENCE RULE (v25g) ------------------------------------
// Every device call this file makes MUST go through the g_orig* pointers,
// never the patched vtable. This is not hygiene, it is the black-model bug:
// the mod's own hooks are STATE MACHINES - HookedSetPixelShader tracks the
// current shader (g_curPsObj/g_curPsIdx, read by draw-time machinery) and
// runs the A2C enable/restore ladder, which SETS RENDER STATES of its own
// when the bound shader changes family. Our injected binds walked that
// ladder mid-frame, and our state block Apply then restored the device
// behind the machinery's back - belief and device permanently desynced,
// wrong alpha-test/coverage state inherited by whichever draws come next
// (user-isolated: the newest-loaded model, black; bisect level 4 proved the
// draws themselves were innocent). HookedCreateTexture feeds the staging
// machinery and HookedCreatePixelShader feeds the shader inventory, so our
// internal RTs and runtime-compiled shaders must not pass through those
// either; HookedStretchRect is the SSAA present-path probe. The calls this
// file may still make through the macro form are exactly the UNHOOKED
// slots: SetVertexShader, SetFVF, SetSamplerState, SetPixelShaderConstantF
// (unhooked while ENABLE_CASCADE_HUNT=0 - revisit if that gate returns),
// DrawPrimitiveUP, CreateStateBlock, GetSurfaceLevel, GetDesc.
static void AoSetPs(IDirect3DDevice9 *dev, IDirect3DPixelShader9 *ps)
{
    if (g_origSetPixelShader) g_origSetPixelShader(dev, ps);
    else IDirect3DDevice9_SetPixelShader(dev, ps);
}

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
    hr = g_origCreatePS
       ? g_origCreatePS(dev, (const DWORD *)BufPtr(code), &ps)
       : IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)BufPtr(code), &ps);
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
    if (g_aoCombineState == 0) {
        g_aoCombinePs = AoCompilePs(dev, g_aoCombineHlsl, NULL,
                                    "combine (floor-clamped merge)");
        g_aoCombineState = g_aoCombinePs ? 1 : -1;
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
    if (!g_origCreateTexture) return 0;   // self-interference rule: never the hook
    if (FAILED(g_origCreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &g_aoRtA, NULL)) || !g_aoRtA) {
        SsaoReleaseRts();
        LogLine("[ssao] blur RT A creation failed - falling back to direct");
        return 0;
    }
    if (FAILED(g_origCreateTexture(dev, w, h, 1, D3DUSAGE_RENDERTARGET,
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
    c0[2] = (float)g_aoRadiusE[est] / 100.0f;
    c0[3] = (float)g_aoStrengthPctE[est] / 100.0f;
    c1[0] = ((float)g_aoProj100 / 100.0f) * ((float)h / (float)w);
    c1[1] = (float)g_aoProj100 / 100.0f;
    // Bias units differ per estimator: Alchemy multiplies by P.z inside the
    // shader (depth-proportional), HBAO compares in sin-of-elevation space.
    c1[2] = est ? 0.15f : 0.02f;
    c1[3] = (float)g_aoIntensityE[est] / 100.0f;   // estimator gain, live-tunable
    c2[0] = mode;
    c2[1] = g_aoRespectFloor ? 1.0f : 0.0f;
    c2[2] = c2[3] = 0.0f;
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 220, c0, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 221, c1, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 222, c2, 1);
    if (g_ssaoDraws == 0) {
        char l[192];
        sprintf(l, "[ssao] consts est=%d c0=(%.5f %.5f %.2f %.2f) c1=(%.3f %.3f %.3f %.3f)",
                est, c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3]);
        LogLine(l);
    }
}

static void AoSetBlurConsts(IDirect3DDevice9 *dev, UINT w, UINT h,
                            float dx, float dy, float spacing)
{
    float c0[4], c1[4];
    c0[0] = 1.0f / (float)w;
    c0[1] = 1.0f / (float)h;
    c0[2] = dx; c0[3] = dy;
    // Tolerance scales with tap DISTANCE: on a planar surface the depth
    // difference to a tap grows linearly with how far away that tap is, so
    // a fixed tolerance rejects everything on grazing-angle geometry (and
    // on every a-trous level past the first). Dividing by spacing keeps the
    // edge-stop testing "is this the same surface" instead of "is this
    // pixel close in depth", which is the question it is actually for.
    c1[0] = (float)g_aoBlurSharp / (spacing > 1.0f ? spacing : 1.0f);
    c1[1] = spacing;
    c1[2] = c1[3] = 0.0f;
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 220, c0, 1);
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 221, c1, 1);
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

// Explicit engine-state restore - the state block replacement (v25h).
// The bisect proved CreateStateBlock(ALL)+Apply is NOT identity on this
// device (level 5: an empty capture/Apply bracket, nothing in between,
// still broke the newest-loaded model and derailed env-map matrices), so
// the AO passes restore exactly what they touch, from the engine-state
// shadows the mod's own hooks maintain (15_msaa.c). Cause confirmed from
// the device's BehaviorFlags (0x44 = HARDWARE_VERTEXPROCESSING |
// MULTITHREADED, no PUREDEVICE): the game drives D3D from several threads,
// so Apply reverts whatever the ASSET LOADER uploaded between capture and
// restore - see the full note in 15_msaa.c. Restoring only what we touched
// is what makes injection safe next to a loader thread.
//
// Deliberately NOT restored: sampler states on s12/s13 (nothing hooks
// SetSamplerState; the recon showed materials sampling s0-s2 and s14, so
// our point/clamp settings on 12/13 have no observed consumer), and PS
// constants c220-c222 (engine shaders use low registers; ours were moved
// up there so there is nothing of the engine's to restore).
static void AoRestoreEngineState(IDirect3DDevice9 *dev)
{
    g_origSetTexture(dev, 12, (IDirect3DBaseTexture9 *)g_esTex[12]);
    g_origSetTexture(dev, 13, (IDirect3DBaseTexture9 *)g_esTex[13]);
    AoSetPs(dev, (IDirect3DPixelShader9 *)g_curPsObj);
    if (g_origSetVertexShader)
        g_origSetVertexShader(dev, (IDirect3DVertexShader9 *)g_esVs);
    if (g_esDeclIsFvf) {
        if (g_origSetFVF) g_origSetFVF(dev, g_esFvf);
    } else if (g_esDecl && g_origSetVertexDecl) {
        g_origSetVertexDecl(dev, (IDirect3DVertexDeclaration9 *)g_esDecl);
    }
    // DrawPrimitiveUP leaves stream 0 pointing at the runtime's internal
    // buffer; put the engine's binding back (NULL is a valid restore).
    if (g_origSetStreamSource)
        g_origSetStreamSource(dev, 0, (IDirect3DVertexBuffer9 *)g_esStreamVb,
                              g_esStreamOffset, g_esStreamStride);
    g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, EsRs(D3DRS_ALPHABLENDENABLE, FALSE));
    g_origSetRenderState(dev, D3DRS_BLENDOP, EsRs(D3DRS_BLENDOP, D3DBLENDOP_ADD));
    g_origSetRenderState(dev, D3DRS_SRCBLEND, EsRs(D3DRS_SRCBLEND, D3DBLEND_ONE));
    g_origSetRenderState(dev, D3DRS_DESTBLEND, EsRs(D3DRS_DESTBLEND, D3DBLEND_ZERO));
    g_origSetRenderState(dev, D3DRS_SEPARATEALPHABLENDENABLE, EsRs(D3DRS_SEPARATEALPHABLENDENABLE, FALSE));
    g_origSetRenderState(dev, D3DRS_BLENDOPALPHA, EsRs(D3DRS_BLENDOPALPHA, D3DBLENDOP_ADD));
    g_origSetRenderState(dev, D3DRS_SRCBLENDALPHA, EsRs(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE));
    g_origSetRenderState(dev, D3DRS_DESTBLENDALPHA, EsRs(D3DRS_DESTBLENDALPHA, D3DBLEND_ZERO));
    g_origSetRenderState(dev, D3DRS_ZENABLE, EsRs(D3DRS_ZENABLE, D3DZB_TRUE));
    g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, EsRs(D3DRS_ZWRITEENABLE, TRUE));
    g_origSetRenderState(dev, D3DRS_CULLMODE, EsRs(D3DRS_CULLMODE, D3DCULL_CCW));
    g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, EsRs(D3DRS_ALPHATESTENABLE, FALSE));
    g_origSetRenderState(dev, D3DRS_FOGENABLE, EsRs(D3DRS_FOGENABLE, FALSE));
    g_origSetRenderState(dev, D3DRS_STENCILENABLE, EsRs(D3DRS_STENCILENABLE, FALSE));
    g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, EsRs(D3DRS_SCISSORTESTENABLE, FALSE));
    g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, EsRs(D3DRS_COLORWRITEENABLE, 0x0F));
    // Viewport last: the caller restored RT0 just before this, which reset
    // the viewport to full-target - also the correct fallback when the
    // engine has never called SetViewport at all.
    if (g_esVpKnown) g_origSetViewport(dev, &g_esVp);
}

// Runs at the s14 bind, before the engine's consumers. dev-state discipline:
// explicit save/restore via AoRestoreEngineState above - NO state block.
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
    // Bisect level (diagnostic, see 01): peels stages off the END of the
    // pipeline so the run can name which draw's side effect blackens the
    // newest-loaded model. Logged on change so sessions self-document.
    LONG bis = g_aoBisect;
    {
        static LONG lastBis = 0;
        if (bis != lastBis) {
            char l[96];
            lastBis = bis;
            sprintf(l, "[ssao] bisect level %ld (%s)", bis,
                    bis == 0 ? "full pipeline" :
                    bis == 1 ? "no composite write" :
                    bis == 2 ? "+ no snapshot copy" :
                    bis == 3 ? "+ no blur draws" :
                    bis == 4 ? "setup only, no draws" :
                    bis == 5 ? "state block bracket only" : "NOTHING (control)");
            LogLine(l);
            LogFlushNow();
        }
    }
    {
        static LONG lastFlat = 0;
        if (g_aoFlatTest != lastFlat) {
            char l[64];
            lastFlat = g_aoFlatTest;
            sprintf(l, "[ssao] flat-write test: %s (%ld%%)",
                    g_aoFlatTest ? "ON" : "off", g_aoFlatTest);
            LogLine(l);
            LogFlushNow();
        }
    }
    // Level 6: the control - SsaoApply contributes literally nothing, so a
    // frame here must be indistinguishable from AO off. Black at 6 means the
    // bug is not in this function at all.
    if (bis >= 6) return;
    // Level 5: the state block bracket alone - capture and immediately
    // restore, no other call in between. Isolates CreateStateBlock/Apply
    // from everything level 4 still does (retargets, binds, shader sets).
    if (bis == 5) {
        __try {
            IDirect3DStateBlock9 *sb5 = NULL;
            if (SUCCEEDED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb5)) && sb5) {
                IDirect3DStateBlock9_Apply(sb5);
                IDirect3DStateBlock9_Release(sb5);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }
    if (g_aoPsState[est] == 0 || g_aoBlurState == 0 || g_aoCombineState == 0)
        AoEnsureShaders(dev, est);
    if (g_aoPsState[est] != 1) return;
    if (!g_aoDepthTex) return;

    // The RT path is now used for EVERY mode, not just blurred ones: the
    // floor clamp needs the engine's own buffer readable, which means our
    // AO has to live in a texture and the destination has to be written by
    // a shader rather than by blend state. Debug bands still skip the blur
    // (they are a pipeline diagnostic, and smoothing them hides exactly
    // what they exist to show).
    int useRt = (g_aoCombineState == 1) ? 1 : 0;
    int useBlur = (g_aoBlur && !g_aoDebug && g_aoBlurState == 1) ? 1 : 0;

    IDirect3DSurface9 *dstSurf = NULL, *oldRt = NULL;
    IDirect3DSurface9 *surfA = NULL, *surfB = NULL;
    IDirect3DSurface9 *oldMrt[3] = { NULL, NULL, NULL };
    __try {
        D3DSURFACE_DESC d;
        if (FAILED(g_origGetRenderTarget(dev, 0, &oldRt)) || !oldRt) goto done;
        // MRT slots 1..3 (v25d). State blocks do NOT capture render targets -
        // we always knew that for RT0 and restore it by hand - and D3D9
        // requires every bound target to share dimensions, so pointing RT0 at
        // our own surfaces silently drops whatever was in 1..3. Anything the
        // engine renders afterwards that expected a second output then writes
        // into nowhere, which is exactly the shape of "one object renders
        // pure black only when AO is on" (2026-08-16 menu shield report:
        // the dumped composite is a ~0.65 multiplier over that object, which
        // cannot blacken anything, so the AO term is not what breaks it).
        // Save, unbind for our passes, restore after.
        {
            int i;
            for (i = 1; i <= 3; i++) {
                IDirect3DSurface9 *s = NULL;
                if (SUCCEEDED(g_origGetRenderTarget(dev, (DWORD)i, &s)) && s) {
                    oldMrt[i - 1] = s;
                    g_origSetRT(dev, (DWORD)i, NULL);
                }
            }
            if (!g_aoMrtLogged) {
                g_aoMrtLogged = 1;
                if (oldMrt[0] || oldMrt[1] || oldMrt[2]) {
                    char l[160];
                    sprintf(l, "[ssao] MRT live at injection: rt1=%p rt2=%p rt3=%p"
                               " - saved and restored around our passes",
                            (void *)oldMrt[0], (void *)oldMrt[1], (void *)oldMrt[2]);
                    LogLine(l);
                } else {
                    LogLine("[ssao] MRT slots 1-3 empty at injection (single render target)");
                }
            }
        }
        if (raw) {
            // Current RT0 (the backbuffer during DRAW_MENU) is the target.
            dstSurf = oldRt;
            if (FAILED(IDirect3DSurface9_GetDesc(dstSurf, &d))) goto done;
        } else {
            if (FAILED(IDirect3DTexture9_GetSurfaceLevel(
                    (IDirect3DTexture9 *)tex, 0, &dstSurf)) || !dstSurf) goto done;
            if (FAILED(IDirect3DSurface9_GetDesc(dstSurf, &d))) goto done;
        }
        if (useRt) {
            if (raw) {
                // The raw view rides the RT pair the normal path owns (it
                // only draws while AoEnable is on, so the pair exists at
                // composite size). Creating a second pair at backbuffer
                // size would thrash recreation every frame under SSAA.
                if (!g_aoRtA || !g_aoRtB) useRt = 0;
            } else {
                if (!AoEnsureRts(dev, d.Width, d.Height)) useRt = 0;
            }
        }
        if (useRt) {
            if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_aoRtA, 0, &surfA)) || !surfA)
                useRt = 0;
            else if (FAILED(IDirect3DTexture9_GetSurfaceLevel(g_aoRtB, 0, &surfB)) || !surfB)
                useRt = 0;
        }
        if (!useRt) useBlur = 0;

        // States shared by every pass. Through g_orig*: these methods are
        // now hooked for the engine-state shadow, and our own traffic must
        // stay invisible to it (the shadows hold engine truth only).
        if (g_origSetVertexShader) g_origSetVertexShader(dev, NULL);
        else IDirect3DDevice9_SetVertexShader(dev, NULL);
        if (g_origSetFVF) g_origSetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
        else IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);
        g_origSetRenderState(dev, D3DRS_ZENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);

        if (useRt) {
            UINT rw = (UINT)g_aoRtW, rh = (UINT)g_aoRtH;
            // Pass 1: estimator -> RT A, opaque. Raw mode writes aoBase
            // (mode 3), debug writes the bands, normal writes the [0.5..1]
            // term (mode 0) - the mapping is linear, so blurring the term
            // equals blurring ao.
            if (!AoTarget(dev, surfA, rw, rh)) goto done;
            AoSetPs(dev, g_aoPs[est]);
            AoBindTex(dev, 12, (IDirect3DBaseTexture9 *)g_aoDepthTex);
            AoBlendOpaque(dev, 0x0F);
            AoSetEstimatorConsts(dev, rw, rh,
                                 raw ? 3.0f : (g_aoDebug ? 1.0f : 0.0f), est);
            if (bis < 4) AoDrawFsQuad(dev, rw, rh);

            // A-trous levels: each is a separable H then V with the spacing
            // doubled, ping-ponging A->B->A. Unlike v25b the vertical pass
            // ALWAYS lands back in A: the destination is now written by the
            // combine pass, which needs the finished AO in a texture it can
            // sample alongside the engine's own buffer.
            if (useBlur && bis < 3) {
                AoSetPs(dev, g_aoBlurPs);
                AoBindTex(dev, 13, (IDirect3DBaseTexture9 *)g_aoDepthTex);
                {
                    LONG passes = g_aoBlurPasses;
                    if (passes < 1) passes = 1;
                    if (passes > 4) passes = 4;
                    float base = (float)g_aoBlurStep100 / 100.0f;
                    for (LONG p = 0; p < passes; p++) {
                        float spacing = base * (float)(1 << p);
                        if (!AoTarget(dev, surfB, rw, rh)) goto done;
                        AoBindTex(dev, 12, (IDirect3DBaseTexture9 *)g_aoRtA);
                        AoBlendOpaque(dev, 0x0F);
                        AoSetBlurConsts(dev, rw, rh, 1.0f, 0.0f, spacing);
                        AoDrawFsQuad(dev, rw, rh);
                        if (!AoTarget(dev, surfA, rw, rh)) goto done;
                        AoBindTex(dev, 12, (IDirect3DBaseTexture9 *)g_aoRtB);
                        AoBlendOpaque(dev, 0x0F);
                        AoSetBlurConsts(dev, rw, rh, 0.0f, 1.0f, spacing);
                        AoDrawFsQuad(dev, rw, rh);
                    }
                }
            }

            // Final pass. Passthrough for the raw view (onto the backbuffer,
            // all channels) and for the debug bands (into the composite, RGB
            // only). Otherwise the combine: snapshot the engine's own buffer
            // into RT B, then write eng*ao clamped to its floor. Alpha is
            // never written in either composite case - the write mask, not
            // blend factors, is what protects the sun-shadow mask now.
            AoSetPs(dev, g_aoCombinePs);
            {
                float k0[4];
                k0[0] = g_aoRespectFloor ? 1.0f : 0.0f;
                k0[1] = (raw || g_aoDebug) ? 1.0f : 0.0f;
                k0[2] = (float)g_aoFlatTest / 100.0f;   // 0 = off
                k0[3] = 0.0f;
                if (!raw && !g_aoDebug && bis < 2) {
                    // RT B is still bound at s0 from the last blur pass, and
                    // it is about to be a StretchRect DESTINATION - a texture
                    // that is simultaneously a sampler source and a copy
                    // target is exactly the hazard D3D9 leaves undefined.
                    // Bind the finished AO (A) first, which displaces it.
                    AoBindTex(dev, 12, (IDirect3DBaseTexture9 *)g_aoRtA);
                    // Self-interference rule: HookedStretchRect is the SSAA
                    // present-path probe; our snapshot must not feed it.
                    if (!g_origStretchRect ||
                        FAILED(g_origStretchRect(dev, dstSurf, NULL, surfB, NULL, D3DTEXF_NONE))) {
                        // No snapshot means no floor clamp is possible; fall
                        // back to the stacking multiply rather than drawing
                        // an un-combined AO term over the engine's buffer.
                        if (bis >= 1) goto drew;
                        if (!AoTarget(dev, dstSurf, d.Width, d.Height)) goto done;
                        AoBindTex(dev, 12, (IDirect3DBaseTexture9 *)g_aoRtA);
                        AoSetPs(dev, g_aoCombinePs);
                        k0[1] = 1.0f;
                        IDirect3DDevice9_SetPixelShaderConstantF(dev, 220, k0, 1);
                        AoBlendMultiply(dev);
                        AoDrawFsQuad(dev, d.Width, d.Height);
                        goto drew;
                    }
                    AoBindTex(dev, 13, (IDirect3DBaseTexture9 *)g_aoRtB);
                }
                if (bis < 1) {
                    if (!AoTarget(dev, dstSurf, d.Width, d.Height)) goto done;
                    AoBindTex(dev, 12, (IDirect3DBaseTexture9 *)g_aoRtA);
                    AoBlendOpaque(dev, raw ? 0x0F : 0x07);
                    IDirect3DDevice9_SetPixelShaderConstantF(dev, 220, k0, 1);
                    AoDrawFsQuad(dev, d.Width, d.Height);
                }
            }
        drew:;
        } else {
            // Single-pass direct path (AoBlur=0, debug bands, or blur infra
            // unavailable) - the v24 behavior, unchanged.
            if (!AoTarget(dev, dstSurf, d.Width, d.Height)) goto done;
            AoSetPs(dev, g_aoPs[est]);
            AoBindTex(dev, 12, (IDirect3DBaseTexture9 *)g_aoDepthTex);
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
            sprintf(l, "[ssao] first draw: est=%s path=%s floor=%s",
                    est ? "HBAO" : "Alchemy",
                    !useRt ? "LEGACY direct multiply (no combine shader)"
                           : (useBlur ? "estimator->atrous->combine"
                                      : (g_aoDebug ? "DEBUG bands" : "estimator->combine")),
                    g_aoRespectFloor ? "clamped to engine 0.5" : "unclamped");
            LogLine(l);
        }
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (oldRt) { g_origSetRT(dev, 0, oldRt); }
    // MRT slots after RT0 (they must match its dimensions to bind at all).
    {
        int i;
        for (i = 1; i <= 3; i++) {
            if (oldMrt[i - 1]) {
                g_origSetRT(dev, (DWORD)i, oldMrt[i - 1]);
                IDirect3DSurface9_Release(oldMrt[i - 1]);
            }
        }
    }
    AoRestoreEngineState(dev);
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
