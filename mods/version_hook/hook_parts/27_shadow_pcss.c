// ---- PCSS: replacement for the screen-space shadow projection shader ------
// The engine's soft shadows are ps_C7978054 (identified 2026-09-14 with the
// debug panel's identify walk, confirmed by disassembly): a fullscreen pass
// in DRAW_MULTI_SAMPLE_SHADOW at half res that, per pixel, reconstructs the
// view-space position from the linear depth buffer, picks a cascade, projects
// into the 1:2 stacked shadow atlas and takes EIGHT compare taps whose
// offsets/weights the CPU computes (FUN_00a525c0) and uploads in c10-c19,
// jittered per pixel by a 64x64 noise texture. A depth-aware 4-tap blur
// (ps_BE7317DB) and the half->full upsample (ps_7D468E35) follow and are
// left alone.
//
// Eight jittered taps is why wider kernels read as scatter rather than blur:
// the taps stop overlapping and the downstream blur cannot hide it. This
// substitutes a denser kernel with a PCSS blocker search - contact-hardening
// penumbrae sized from the caster distance - and honours the ORIGINAL's
// input contract exactly (same samplers, same c0-c9 constants, same output
// convention: visibility in oC0.w, 1 = lit), so nothing else changes.
//
// Constant contract of ps_C7978054, from its disassembly:
//   c0.y  cascade split (compared against -v0.z*depth)   c0.w  far cutoff
//         (the -c0.xyyw swizzle: first cut read .x/.y and picked the wrong
//         cascade - shadows slid with the camera and lost resolution)
//   c1.xy 1 / half-res buffer size
//   c2..c5   near cascade matrix rows x,y,z,w    c6..c9   far cascade rows
//   c10,c15  8 tap weights   c11-c14,c16-c19  8 tap offsets   (ignored here)
//   s0 linear depth   s1 shadow atlas (R32F, res x 2res)   s2 64x64 noise
//   v0.xyz view ray (position = depth * v0), vPos pixel
// Ours: c40, c41 - uploaded at every substitution bind, see PcssBind.
//
// Penumbra maths is done in WORLD units so it is independent of the shadow
// resolution and of which cascade the pixel landed in: |row_x.xyz| is atlas
// uv per world unit, |row_z.xyz| is depth units per world unit.
#if ENABLE_SHADOW_PCSS

#define PCSS_ORIG_HASH 0xC7978054u

// Tunables (g_shadowPcss, g_pcss*) are defined in 03_render_state.c so the
// config table can take their addresses; see there for units.

// g_pcssOrigObj (the engine's ps_C7978054 object, latest), g_pcssState
// (0 not tried, 1 ok, -1 failed) and g_pcssBinds live in 03_render_state.c.
static IDirect3DPixelShader9 *g_pcssPs = NULL;
static LONG g_pcssLoggedRes = 0;

static const char *g_pcssHlsl =
"float4 cSplit   : register(c0);\n"
"float4 cInvSize : register(c1);\n"
"float4 cAx : register(c2); float4 cAy : register(c3); float4 cAz : register(c4); float4 cAw : register(c5);\n"
"float4 cBx : register(c6); float4 cBy : register(c7); float4 cBz : register(c8); float4 cBw : register(c9);\n"
"float4 cP0 : register(c40);   // xy atlas texel uv, z light size (tan), w min radius (texels)\n"
"float4 cP1 : register(c41);   // x max radius (texels), y search radius (texels), z bias, w unused\n"
"sampler2D sDepth : register(s0);\n"
"sampler2D sAtlas : register(s1);\n"
"sampler2D sNoise : register(s2);\n"
"\n"
"static const float2 POISSON[PCSS_TAPS] = {\n"
"  float2(-0.613392, 0.617481), float2( 0.170019,-0.040254), float2(-0.299417, 0.791925), float2( 0.645680, 0.493210),\n"
"  float2(-0.651784, 0.717887), float2( 0.421003, 0.027070), float2(-0.817194,-0.271096), float2(-0.705374,-0.668203),\n"
"  float2( 0.977050,-0.108615), float2( 0.063326, 0.142369), float2( 0.203528, 0.214331), float2(-0.667531, 0.326090),\n"
"  float2(-0.098422,-0.295755), float2(-0.885922, 0.215369), float2( 0.566637, 0.605213), float2( 0.039766,-0.396100),\n"
"  float2( 0.751946, 0.453352), float2( 0.078707,-0.715323), float2(-0.075838,-0.529344), float2( 0.724479,-0.580798),\n"
"  float2( 0.222999,-0.215125), float2(-0.467574,-0.405438), float2(-0.248268,-0.814753), float2( 0.354411,-0.887570),\n"
"  float2( 0.175817, 0.382366), float2( 0.487472,-0.063082), float2(-0.084078, 0.898312), float2( 0.488876,-0.783441),\n"
"  float2( 0.470016, 0.217933), float2(-0.696890,-0.549791), float2(-0.149693, 0.605762), float2( 0.034211, 0.979980)\n"
"};\n"
"\n"
"float4 main(float3 v0 : TEXCOORD0, float2 vPos : VPOS) : COLOR\n"
"{\n"
"    float2 pix = floor(vPos) + 0.5;\n"
"    float2 uvD = pix * cInvSize.xy;\n"
"    float2 nz  = tex2D(sNoise, pix * (1.0 / 64.0)).zy * 2.0 - 1.0;\n"
"    float  depth = tex2D(sDepth, uvD).x;\n"
"    float4 P = float4(depth * v0, 1.0);\n"
"    // mad r0.zw, v0.z, -r1.x, -c0.xyyw : split is c0.Y, far cutoff c0.W\n"
"    float2 t = v0.z * -depth - cSplit.yw;\n"
"    if (t.y >= 0.0) return float4(1, 1, 1, 1);          // beyond shadow range: lit\n"
"    bool farC = t.x >= 0.0;\n"
"    float4 rx = farC ? cBx : cAx;\n"
"    float4 ry = farC ? cBy : cAy;\n"
"    float4 rz = farC ? cBz : cAz;\n"
"    float4 rw = farC ? cBw : cAw;\n"
"    float  w  = dot(rw, P);\n"
"    float2 uvL = float2(dot(rx, P), dot(ry, P)) / w;\n"
"    float  zL  = dot(rz, P);\n"
"\n"
"    // World-unit conversion for this cascade.\n"
"    float uvPerWorld    = length(rx.xyz) / abs(w);\n"
"    float depthPerWorld = max(length(rz.xyz), 1e-6);\n"
"    float2 texel = cP0.xy;\n"
"    // The cascade owns one vertical half of the 1:2 atlas; never sample the\n"
"    // other one. One-texel margin keeps bilinear-free point taps inside.\n"
"    float band = (uvL.y < 0.5) ? 0.0 : 0.5;\n"
"    float2 lo = float2(texel.x, band + texel.y);\n"
"    float2 hi = float2(1.0 - texel.x, band + 0.5 - texel.y);\n"
"    // The engine shader never clamps: a pixel that projects outside the\n"
"    // atlas gets the sampler's edge behaviour, which reads as lit. Match\n"
"    // that instead of dragging the edge texel across the whole area.\n"
"    if (uvL.x < 0.0 || uvL.x > 1.0 || uvL.y < 0.0 || uvL.y > 1.0) return float4(1, 1, 1, 1);\n"
"\n"
"    // Per-pixel rotation from the engine's own noise texture (its taps are\n"
"    // jittered the same way, so the downstream blur expects this).\n"
"    float2 n = normalize(nz + 1e-4);\n"
"    float2x2 rot = float2x2(n.x, -n.y, n.y, n.x);\n"
"\n"
"    float bias = cP1.z;\n"
"    // ---- blocker search ------------------------------------------------\n"
"    float2 rSearch = texel * cP1.y;\n"
"    // Classic PCSS: average depth of the blockers found. Two alternatives\n"
"    // were tried on 2026-09-14 for the case where a character's shadow\n"
"    // meets a canopy's (the average softens the character edge there):\n"
"    // nearest-blocker sharpened the canopy in a halo one search radius\n"
"    // wide around the character, and min-over-max(penumbra, distance)\n"
"    // removed the halo but read as too soft overall, even in open sun. The\n"
"    // user reverted to this; the intersection softening is accepted.\n"
"    float blockerSum = 0.0; float blockers = 0.0;\n"
"    [unroll] for (int i = 0; i < PCSS_SEARCH; i++) {\n"
"        float2 o = mul(rot, POISSON[i]) * rSearch;\n"
"        float zB = tex2D(sAtlas, clamp(uvL + o, lo, hi)).x;\n"
"        float isB = (zL - bias - zB >= 0.0) ? 1.0 : 0.0;\n"
"        blockerSum += zB * isB; blockers += isB;\n"
"    }\n"
"    if (blockers < 0.5) return float4(1, 1, 1, 1);      // nothing in front: lit\n"
"    float zB = blockerSum / blockers;\n"
"\n"
"    // ---- penumbra from caster distance, world units -> atlas texels -----\n"
"    float worldGap = max(zL - zB, 0.0) / depthPerWorld;\n"
"    float penUV    = worldGap * cP0.z * uvPerWorld;\n"
"    float rTex     = clamp(penUV / max(texel.x, 1e-7), cP0.w, cP1.x);\n"
"    float2 rPcf    = texel * rTex;\n"
"\n"
"    // ---- PCF ------------------------------------------------------------\n"
"    float shadow = 0.0;\n"
"    [unroll] for (int j = 0; j < PCSS_TAPS; j++) {\n"
"        float2 o = mul(rot, POISSON[j]) * rPcf;\n"
"        float zS = tex2D(sAtlas, clamp(uvL + o, lo, hi)).x;\n"
"        shadow += (zL - bias - zS >= 0.0) ? 1.0 : 0.0;\n"
"    }\n"
"    float vis = 1.0 - shadow / PCSS_TAPS;\n"
"    return float4(1, 1, 1, vis);\n"
"}\n";

static void PcssEnsure(IDirect3DDevice9 *dev)
{
    if (g_pcssState != 0) return;
    AoHlslMacro defs[3];
    defs[0].Name = "PCSS_TAPS";   defs[0].Definition = "32";
    defs[1].Name = "PCSS_SEARCH"; defs[1].Definition = "16";
    defs[2].Name = NULL;          defs[2].Definition = NULL;
    g_pcssPs = AoCompilePs(dev, g_pcssHlsl, defs, "PCSS shadow projection (32 PCF / 16 search)");
    g_pcssState = g_pcssPs ? 1 : -1;
}

// Called from HookedSetPixelShader before any other substitution. Returns 1
// with *hr set when the bind was replaced.
static int PcssBind(void *devv, void *pShader, HRESULT *hr)
{
    IDirect3DDevice9 *dev = (IDirect3DDevice9 *)devv;
    if (!g_shadowPcss || !pShader || pShader != g_pcssOrigObj) return 0;
    PcssEnsure(dev);
    if (!g_pcssPs) return 0;

    // Resolution-independent tunables: radii are given in 2048-map texels
    // and scaled to the live map size, so the same ini reads the same at
    // every ShadowMapRes (the lesson of ShadowFilterPct's first cut).
    float res = 2048.0f;
    if (g_mainModBase) {
        __try {
            DWORD settings = *(DWORD *)(g_mainModBase + SHADOW_SETTINGS_PTR_RVA);
            DWORD r = settings ? *(DWORD *)(settings + 0x24) : 0;
            if (r >= 256 && r <= 16384) res = (float)r;
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    float scale = res / 2048.0f;
    float c[8];
    c[0] = 1.0f / res;                              // atlas texel u (res wide)
    c[1] = 1.0f / (2.0f * res);                     // atlas texel v (2*res tall)
    c[2] = g_pcssLightSize / 1000.0f;               // tan(half angle)
    c[3] = (g_pcssMinRadius / 10.0f) * scale;       // min PCF radius, texels
    c[4] = (float)g_pcssMaxRadius * scale;          // max PCF radius, texels
    // Blocker search must reach at least as far as the widest penumbra the
    // PCF can produce, or pixels just outside a wide penumbra find no
    // caster, are declared lit, and the shadow keeps a hard rim one search
    // radius out while its interior lightens (Jungle tree shadows,
    // 2026-09-14). The slider is therefore a FLOOR; the max radius wins.
    {
        float search = (float)g_pcssSearchRadius;
        if (search < (float)g_pcssMaxRadius) search = (float)g_pcssMaxRadius;
        c[5] = search * scale;                      // blocker search radius, texels
    }
    c[6] = g_pcssBias / 100000.0f;                  // depth bias
    c[7] = 0.0f;
    IDirect3DDevice9_SetPixelShaderConstantF(dev, 40, c, 2);
    if ((LONG)res != g_pcssLoggedRes) {
        char l[200];
        g_pcssLoggedRes = (LONG)res;
        sprintf(l, "[pcss] active: map=%ld light=%.3f radius %.1f..%.1f texels search=%.1f bias=%g",
                (LONG)res, c[2], c[3], c[4], c[5], c[6]);
        LogLine(l);
    }
    InterlockedIncrement(&g_pcssBinds);
    *hr = g_origSetPixelShader(dev, g_pcssPs);
    return 1;
}

// Called from HookedCreatePixelShader for every created shader.
static void PcssNoteCreated(DWORD hash, void *obj)
{
    if (hash == PCSS_ORIG_HASH) {
        g_pcssOrigObj = obj;
        LogLine("[pcss] engine shadow projection shader ps_C7978054 created - substitution armed");
    }
}
#endif  // ENABLE_SHADOW_PCSS
