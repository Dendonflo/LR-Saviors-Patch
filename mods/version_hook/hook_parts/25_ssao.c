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
// Injection: at the s14 bind, render one fullscreen quad into the composite
// with an SSAO pixel shader sampling the depth texture, blended with
// D3DBLENDOP_MIN. RGB becomes min(engineShadow, ourAO) - the two darkenings
// merge without stacking - and since our alpha outputs 1.0, min(1, A) leaves
// the sun mask BIT-EXACT. No channel masks, no ping-pong, one draw.
//
// Estimator: Alchemy/SAO-style spiral (McGuire et al.) - the best fit for
// depth-only ps_3_0: view-space position from linear depth, normal from
// ddx/ddy, 12 spiral taps with per-pixel interleaved-gradient rotation.
// Deliberately isolated in one HLSL function so an HBAO horizon-march can
// replace it later without touching any plumbing.
//
// The shader is compiled AT RUNTIME with D3DXCompileShader from d3dx9_43.dll
// - which the GAME imports, so it is guaranteed present and loaded. The HLSL
// lives in this string: nothing is loaded from disk, and the tunables are
// live ini values rather than recompiles.

#if ENABLE_AO_SSAO

// ---- tunables (config keys in 08, declared in 01 for TU order) ------------
// AoEnable      0/1      master switch (menu toggle)
// AoDebug       0/1      draw raw AO opaquely instead of MIN-blending -
//                        the only sane way to tune radius/strength by eye
// AoStrengthPct 0..100   how much of the [0.5..1] envelope AO may use
// AoRadius100   world-units radius x100 (engine units - tuned by eye)
// AoProj100     projection scale x100 (cot(fovY/2)); wrong values show as
//                        AO that stretches with screen position in debug view

static const char *g_ssaoHlsl =
"sampler2D depthTex : register(s0);\n"
"float4 cParam0 : register(c0);\n"   // x=texelW y=texelH z=radius w=strength
"float4 cParam1 : register(c1);\n"   // x=projX  y=projY  z=bias   w=intensity
"float3 ViewPos(float2 uv) {\n"
"    float z = tex2Dlod(depthTex, float4(uv, 0, 0)).r;\n"
"    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);\n"
"    return float3(ndc.x * z / cParam1.x, ndc.y * z / cParam1.y, z);\n"
"}\n"
"float4 main(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR {\n"
"    float3 P = ViewPos(uv);\n"
"    if (P.z > 1500.0) return float4(1, 1, 1, 1);\n"   // sky/far (far plane ~2000)
// cross(ddx, ddy), NOT (ddy, ddx): view space is x-right/y-up/z-into-screen
// and screen v runs DOWN, so the other order points normals AWAY from the
// camera - every dot(v,N) clamps to zero and AO is white everywhere. That
// was v24's entire failure; depth units were correct all along (measured
// 4..2000 world units, [aodepth] 2026-08-15).
"    float3 N = normalize(cross(ddx(P), ddy(P)));\n"
"    float ign = frac(52.9829189 * frac(dot(vpos, float2(0.06711056, 0.00583715))));\n"
"    float ca = cos(ign * 6.2831853), sa = sin(ign * 6.2831853);\n"
"    float occ = 0.0;\n"
"    float rPix = cParam0.z * cParam1.y / P.z;\n"      // world radius -> uv
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
"    float ao = saturate(1.0 - cParam1.w * occ / 12.0);\n"
"    ao = 1.0 - cParam0.w * (1.0 - ao);\n"             // strength envelope
"    float term = 0.5 + 0.5 * ao;\n"                   // map into [0.5..1]
"    return float4(term, term, term, 1.0);\n"          // alpha 1: MIN keeps sun mask
"}\n";

typedef struct ID3DXBuffer ID3DXBuffer;   // vtable slots used: 3 GetBufferPointer, 4 GetBufferSize
typedef HRESULT (WINAPI *PFN_D3DXCompileShader)(
    const char *, UINT, const void *, const void *, const char *, const char *,
    DWORD, ID3DXBuffer **, ID3DXBuffer **, void *);

static IDirect3DPixelShader9 *g_ssaoPs = NULL;
static LONG g_ssaoCompileState = 0;   // 0 not tried, 1 ok, -1 failed
static volatile LONG g_ssaoDraws = 0;

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

static void SsaoCompile(IDirect3DDevice9 *dev)
{
    g_ssaoCompileState = -1;   // pessimistic until every step lands
    HMODULE hDx = GetModuleHandleA("d3dx9_43.dll");
    if (!hDx) { LogLine("[ssao] d3dx9_43.dll not loaded - cannot compile"); return; }
    PFN_D3DXCompileShader compile =
        (PFN_D3DXCompileShader)GetProcAddress(hDx, "D3DXCompileShader");
    if (!compile) { LogLine("[ssao] D3DXCompileShader not found"); return; }

    ID3DXBuffer *code = NULL, *errs = NULL;
    HRESULT hr = compile(g_ssaoHlsl, (UINT)strlen(g_ssaoHlsl), NULL, NULL,
                         "main", "ps_3_0", 0, &code, &errs, NULL);
    if (FAILED(hr) || !code) {
        char l[320];
        sprintf(l, "[ssao] compile FAILED hr=0x%08lX: %.200s",
                (unsigned long)hr, errs ? (const char *)BufPtr(errs) : "(no error text)");
        LogLine(l);
        if (errs) BufRelease(errs);
        if (code) BufRelease(code);
        return;
    }
    hr = IDirect3DDevice9_CreatePixelShader(dev, (const DWORD *)BufPtr(code), &g_ssaoPs);
    BufRelease(code);
    if (errs) BufRelease(errs);
    if (FAILED(hr) || !g_ssaoPs) {
        char l[128];
        sprintf(l, "[ssao] CreatePixelShader FAILED hr=0x%08lX", (unsigned long)hr);
        LogLine(l);
        return;
    }
    g_ssaoCompileState = 1;
    LogLine("[ssao] pixel shader compiled and created (ps_3_0, Alchemy spiral, 12 taps)");
}

// Runs at the s14 bind, before the engine's consumers. dev-state discipline
// identical to the tint probe: D3DSBT_ALL block plus hand-restored RT0.
static void SsaoApply(IDirect3DDevice9 *dev, IDirect3DBaseTexture9 *tex)
{
    static LONG lastFrame = -1;
    LONG fr = g_msFrameSeq;
    if (fr == lastFrame) return;
    lastFrame = fr;

    if (g_ssaoCompileState == 0) SsaoCompile(dev);
    if (g_ssaoCompileState != 1) return;
    if (!g_aoDepthTex) return;

    IDirect3DSurface9 *surf = NULL, *oldRt = NULL;
    IDirect3DStateBlock9 *sb = NULL;
    __try {
        D3DSURFACE_DESC d;
        if (FAILED(IDirect3DTexture9_GetSurfaceLevel(
                (IDirect3DTexture9 *)tex, 0, &surf)) || !surf) goto done;
        if (FAILED(IDirect3DSurface9_GetDesc(surf, &d))) goto done;
        if (FAILED(IDirect3DDevice9_CreateStateBlock(dev, D3DSBT_ALL, &sb)) || !sb) goto done;
        if (FAILED(g_origGetRenderTarget(dev, 0, &oldRt)) || !oldRt) goto done;
        if (FAILED(g_origSetRT(dev, 0, surf))) goto done;

        {
            D3DVIEWPORT9 vp;
            vp.X = 0; vp.Y = 0;
            vp.Width = d.Width; vp.Height = d.Height;
            vp.MinZ = 0.0f; vp.MaxZ = 1.0f;
            g_origSetViewport(dev, &vp);
        }
        // Depth texture at s0, point-sampled, clamped.
        g_origSetTexture(dev, 0, (IDirect3DBaseTexture9 *)g_aoDepthTex);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        IDirect3DDevice9_SetSamplerState(dev, 0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);

        IDirect3DDevice9_SetVertexShader(dev, NULL);
        IDirect3DDevice9_SetPixelShader(dev, g_ssaoPs);
        IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_TEX1);

        if (g_aoDebug) {
            // Opaque replace: the raw AO term fills the buffer so the whole
            // screen SHOWS it (materials multiply it in) - the tuning view.
            g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, FALSE);
        } else {
            g_origSetRenderState(dev, D3DRS_ALPHABLENDENABLE, TRUE);
            g_origSetRenderState(dev, D3DRS_BLENDOP, D3DBLENDOP_MIN);
            g_origSetRenderState(dev, D3DRS_SRCBLEND, D3DBLEND_ONE);
            g_origSetRenderState(dev, D3DRS_DESTBLEND, D3DBLEND_ONE);
        }
        g_origSetRenderState(dev, D3DRS_ZENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_ZWRITEENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
        g_origSetRenderState(dev, D3DRS_ALPHATESTENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_FOGENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_STENCILENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_SCISSORTESTENABLE, FALSE);
        g_origSetRenderState(dev, D3DRS_COLORWRITEENABLE, 0x0F);

        {
            float c0[4], c1[4];
            c0[0] = 1.0f / (float)d.Width;
            c0[1] = 1.0f / (float)d.Height;
            c0[2] = (float)g_aoRadius100 / 100.0f;
            c0[3] = (float)g_aoStrengthPct / 100.0f;
            c1[0] = ((float)g_aoProj100 / 100.0f) * ((float)d.Height / (float)d.Width);
            c1[1] = (float)g_aoProj100 / 100.0f;
            c1[2] = 0.02f;    // depth-proportional bias (self-occlusion guard)
            c1[3] = 1.6f;     // estimator intensity, folded with strength
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 0, c0, 1);
            IDirect3DDevice9_SetPixelShaderConstantF(dev, 1, c1, 1);
        }
        {
            // Half-texel offset: D3D9 maps texels to pixel CENTRES.
            struct { float x, y, z, w, u, v; } q[4];
            float W = (float)d.Width, H = (float)d.Height;
            for (int k = 0; k < 4; k++) {
                q[k].x = ((k & 1) ? W : 0.0f) - 0.5f;
                q[k].y = ((k & 2) ? H : 0.0f) - 0.5f;
                q[k].z = 0.0f; q[k].w = 1.0f;
                q[k].u = (k & 1) ? 1.0f : 0.0f;
                q[k].v = (k & 2) ? 1.0f : 0.0f;
            }
            IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLESTRIP, 2, q, sizeof(q[0]));
        }
        InterlockedIncrement(&g_ssaoDraws);
        if (g_ssaoDraws == 1)
            LogLine(g_aoDebug ? "[ssao] first draw (DEBUG view - raw AO visible)"
                              : "[ssao] first draw (MIN-blended into shadow term)");
    done:;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (oldRt) { g_origSetRT(dev, 0, oldRt); IDirect3DSurface9_Release(oldRt); }
    if (sb) { IDirect3DStateBlock9_Apply(sb); IDirect3DStateBlock9_Release(sb); }
    if (surf) IDirect3DSurface9_Release(surf);
}

// Device Reset invalidates the shader? No - pixel shaders live in
// D3DPOOL-independent storage and survive Reset. The state block does not,
// but it is created and released within one call. Nothing to release here
// except on device destruction, where the process is going down anyway.

#endif // ENABLE_AO_SSAO
