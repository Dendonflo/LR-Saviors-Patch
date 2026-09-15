// ---- FXAA removal ---------------------------------------------------------
// Identified offline from the 256 dumped shaders by constant fingerprint:
// ps_072C19AF is 952 bytes of ps_3_0 with THIRTEEN dependent taps of one
// sampler and the constant pair 1/8 + 1/16 - FXAA's EDGE_THRESHOLD and
// EDGE_THRESHOLD_MIN, in a shader far too small to be a material. (The
// runner-up, ps_90868A80, is its 1-tap Rec.601 luma prepass - harmless once
// the consumer is gone.)
//
// Removal is at BIND time, not creation: shader objects are immutable but the
// binding isn't, so recognising the object at creation and substituting a
// 1-instruction passthrough (tex2D of the same sampler/texcoord) at
// SetPixelShader makes the whole thing hot-togglable - and if the hash
// identification is WRONG, flipping the toggle shows exactly what the shader
// really was, in-place.
#include "passthrough_ps.h"
#include "tint_ps.h"
// The REAL FXAA shader, found by walking the identify list in game rather
// than by any static heuristic (all four of those failed). See the removal
// block in HookedSetPixelShader for the disassembly evidence.
#define FXAA_SHADER_HASH_REAL 0xA082B248u
// Known-identity shaders, recorded as they are confirmed:
//   ps_A082B248  FXAA (the post-process AA)
//   ps_1245A11B  colour correction / tonemap: 3D LUT + glare + dither
//   ps_DCD57A17  glyph outline filter (the HD GUI mod's lever)
//   ps_A26BF0E2  Lightning's hair
//   ps_8676670C  vegetation, 1-tap cutout - depth/prepass variant
//   ps_658CC589  vegetation/grass, also depth (tinting it blacks the ground)
// (Candidate hash table + labels are declared with the GUI block - the
// dropdown needs them and sits earlier in this file.)
typedef struct { void *obj; LONG cand; } PsKillObj;
#define PS_KILL_OBJ_MAX 64
static PsKillObj g_psKillObjs[PS_KILL_OBJ_MAX];
static volatile LONG g_psKillObjCount = 0;
static IDirect3DPixelShader9 *g_fxaaPassthrough = NULL;
static IDirect3DPixelShader9 *g_psTintObj = NULL;
static volatile LONG g_fxaaSubs = 0;

static DWORD PsFnv1a(const DWORD *pFunction)
{
    UINT bytes = ShaderTokenBytes(pFunction);
    if (bytes < 8 || bytes > 262144) return 0;
    DWORD h = 2166136261u;
    const unsigned char *b = (const unsigned char *)pFunction;
    for (UINT i = 0; i < bytes; i++) { h ^= b[i]; h *= 16777619u; }
    return h;
}

// Correct token walk. A COMMENT token (0xFFFE) carries its length in bits
// 16-30, NOT in bits 24-27 like an instruction - and every D3D9 shader opens
// with a CTAB comment holding the constant table. Walking that payload as
// code produced bogus opcode counts in the first version of this and in the
// offline scans built on the same mistake.
// Taps counted are TEXLD(66/0x42), TEXLDD(93/0x5D), TEXLDL(95/0x5F).
static UINT PsCountTaps(const DWORD *pFunction)
{
    UINT bytes = ShaderTokenBytes(pFunction);
    if (bytes < 8) return 0;
    UINT taps = 0, toks = bytes / 4, t = 1;
    while (t < toks) {
        DWORD tok = pFunction[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) {           // comment: skip its payload
            t += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        UINT op = tok & 0xFFFF;
        if (op == 0x42 || op == 0x5D || op == 0x5F) taps++;
        t += 1 + ((tok >> 24) & 0x0F);
    }
    return taps;
}

// ---- Alpha-to-coverage: synthesise a coverage-writing shader variant ------
// Vegetation, fences and barriers are alpha-TESTED cutouts. MSAA cannot
// smooth them (it samples geometry coverage, not the texture), and the vendor
// A2C render-state hack alone does nothing here for two reasons found by
// measurement: the engine never writes ALPHATESTENABLE (a2c counter stayed 0
// for whole sessions), and its cutout shaders end with
//
//     texld r0, v1, s0        ; r0.w = texture alpha
//     add  r1, r0.w, -c28.x   ; alpha - alphaTestThreshold
//     texkill r1              ; cutout, binary
//     ...
//     mov_pp oC0.w, c31.w     ; alpha OUT is a CONSTANT
//
// so even with A2C enabled there is no per-pixel alpha in the output for
// coverage to be derived from. The fix is to rewrite the bytecode:
//
//   1. before the texkill, stash the tested value (alpha - threshold)
//   2. drop the texkill - coverage 0 replaces it, and a zero-coverage pixel
//      writes neither colour nor depth, which is what the kill achieved
//   3. replace the constant alpha write with
//         mad_sat oC0.w, stashed, sharpness, 0.5
//      a ramp centred exactly where the alpha test used to flip, so the
//      silhouette lands in the same place but resolves across samples
//
// Done at CreatePixelShader for every shader matching the pattern, so the
// whole cutout family is covered without hand-authoring anything and without
// loading files from disk - the variants are synthesised in memory.
//
// D3D9 token encoding used below: instruction token has opcode in bits 0-15
// and length (DWORDs following) in bits 24-27; register tokens have bit 31
// set, register number in bits 0-10, and the register TYPE split across bits
// 28-30 (low 3) and 11-12 (high 2). Comment tokens (0xFFFE) carry their
// length in bits 16-30 - the trap that corrupted three earlier scans here.
#define D3DSIO_NOP_     0
#define D3DSIO_MOV_     1
#define D3DSIO_MAD_     4
#define D3DSIO_TEXKILL_ 65
#define D3DSIO_DEF_     81
#define D3DSPR_TEMP_     0
#define D3DSPR_CONST_    2
#define D3DSPR_COLOROUT_ 8

static DWORD RegTok(DWORD type, DWORD num)
{
    return 0x80000000u | (num & 0x7FF)
         | ((type & 7) << 28) | (((type >> 3) & 3) << 11);
}
static DWORD DstTok(DWORD type, DWORD num, DWORD mask, int saturate)
{
    return RegTok(type, num) | ((mask & 0xF) << 16) | (saturate ? 0x00100000u : 0u);
}
static DWORD SrcTok(DWORD type, DWORD num, DWORD swizzle)
{
    return RegTok(type, num) | ((swizzle & 0xFF) << 16);
}
// Swizzle byte: 2 bits per output component, replicated.
#define SWZ_XXXX 0x00
#define SWZ_YYYY 0x55
#define SWZ_WWWW 0xFF
#define MASK_W   0x8
#define MASK_ALL 0xF

// Returns 1 and fills out/outLen if the shader matched the cutout pattern.
static int BuildA2cVariant(const DWORD *src, DWORD *out, UINT outMax, UINT *outLen)
{
    UINT bytes = ShaderTokenBytes(src);
    if (bytes < 12) return 0;
    UINT toks = bytes / 4;

    // Pass 1: locate the first texkill, the last oC0.w write, the highest
    // temp register and the highest const register actually referenced.
    UINT killAt = 0, alphaOutAt = 0;
    DWORD killReg = 0;
    DWORD maxTemp = 0, maxConst = 0;
    int alphaOutIsConst = 0;   // oC0.w written by MOV from a CONSTANT?
    UINT t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT op = tok & 0xFFFF, len = (tok >> 24) & 0x0F;
        for (UINT k = 1; k <= len && t + k < toks; k++) {
            DWORD rt = src[t + k];
            if (!(rt & 0x80000000u)) continue;
            DWORD type = ((rt >> 28) & 7) | (((rt >> 11) & 3) << 3);
            DWORD num = rt & 0x7FF;
            if (type == D3DSPR_TEMP_ && num > maxTemp) maxTemp = num;
            if (type == D3DSPR_CONST_ && num > maxConst) maxConst = num;
        }
        if (op == D3DSIO_TEXKILL_ && !killAt && len >= 1) {
            killAt = t; killReg = src[t + 1];
        }
        if (len >= 1) {
            DWORD d = src[t + 1];
            if ((d & 0x80000000u)) {
                DWORD type = ((d >> 28) & 7) | (((d >> 11) & 3) << 3);
                if (type == D3DSPR_COLOROUT_ && (d & 0x7FF) == 0 && ((d >> 16) & MASK_W)) {
                    alphaOutAt = t;
                    // Is the alpha a CONSTANT? That is the opaque-cutout
                    // signature: the shader has no per-pixel alpha of its own
                    // and the silhouette is entirely the texkill's doing, so
                    // repurposing oC0.w for coverage costs nothing.
                    //
                    // If instead alpha is COMPUTED (from a texture or temp),
                    // the draw is alpha-BLENDED and that value is its blend
                    // factor - overwriting it destroys the transparency, which
                    // is exactly what happened to Lightning's hair.
                    alphaOutIsConst = 0;
                    if (op == D3DSIO_MOV_ && len >= 2) {
                        DWORD s0 = src[t + 2];
                        DWORD st = ((s0 >> 28) & 7) | (((s0 >> 11) & 3) << 3);
                        if (st == D3DSPR_CONST_) alphaOutIsConst = 1;
                    }
                }
            }
        }
        t += 1 + len;
    }
    // Not a cutout shader, or nothing to write coverage into.
    //
    // alphaOutIsConst is NOT used as a filter any more. It excluded every
    // shader that computes its alpha - which is most of them, and plausibly
    // the grass and barrier shaders that showed no effect at all. The real
    // question is whether the DRAW is blended, and that is a render state,
    // not a property of the bytecode: the bind path requires
    // ALPHABLENDENABLE to be FALSE, which keeps blended material (hair) safe
    // while letting opaque cutouts through however they compute alpha.
    if (!killAt || !alphaOutAt) return 0;
    (void)alphaOutIsConst;
    DWORD rFree = maxTemp + 1;
    DWORD cScale = maxConst + 1;
    if (rFree > 27 || cScale > 220) {           // no headroom; leave it alone
        InterlockedIncrement(&g_a2cSkipNoRoom);
        return 0;
    }

    // Pass 2: emit. Copy verbatim except at the three points of interest.
    UINT o = 0;
    if (outMax < toks + 16) return 0;
    out[o++] = src[0];                       // version token

    // Our own def goes first so it is valid wherever it is used. Register is
    // one past the highest the shader references, so it cannot collide with
    // anything the engine uploads for THIS shader.
    float sharp = (float)g_a2cSharpness;
    if (sharp < 1.0f) sharp = 1.0f;
    out[o++] = D3DSIO_DEF_ | (5u << 24);
    out[o++] = DstTok(D3DSPR_CONST_, cScale, MASK_ALL, 0);
    { float v0 = sharp, v1 = 0.5f, v2 = 0.0f, v3 = 1.0f;
      memcpy(&out[o + 0], &v0, 4); memcpy(&out[o + 1], &v1, 4);
      memcpy(&out[o + 2], &v2, 4); memcpy(&out[o + 3], &v3, 4); o += 4; }

    t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) {
            UINT clen = 1 + ((tok >> 16) & 0x7FFF);
            t += clen;                        // drop comments (CTAB etc.)
            continue;
        }
        UINT len = (tok >> 24) & 0x0F;
        if (t == killAt) {
            // Stash the tested value, then DROP the texkill. texkill's
            // operand is register-encoded; read component .x, which is what
            // the add wrote across all channels.
            DWORD type = ((killReg >> 28) & 7) | (((killReg >> 11) & 3) << 3);
            DWORD num = killReg & 0x7FF;
            out[o++] = D3DSIO_MOV_ | (2u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rFree, MASK_W, 0);
            out[o++] = SrcTok(type, num, SWZ_XXXX);
            t += 1 + len;
            continue;
        }
        if (t == alphaOutAt) {
            // Keep the original write, then OVERRIDE ONLY .w after it.
            // The first version REPLACED this instruction, which is correct
            // only when it writes alpha alone. Character shaders commonly end
            // with `mov oC0, rN` writing all four channels, so replacing it
            // discarded RGB entirely - that is what turned NPCs into black
            // silhouettes. Appending touches nothing but alpha.
            for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
            // coverage = saturate((alpha - threshold) * sharpness + 0.5)
            out[o++] = D3DSIO_MAD_ | (4u << 24);
            out[o++] = DstTok(D3DSPR_COLOROUT_, 0, MASK_W, 1);
            out[o++] = SrcTok(D3DSPR_TEMP_, rFree, SWZ_WWWW);
            out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_XXXX);
            out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_YYYY);   // the 0.5
            // DEBUG VISUALISE: also write the coverage into RGB. The variants
            // are provably bound (hundreds of thousands of times) yet nothing
            // changes on screen, which means either the rewritten shader is
            // not really executing or the vendor coverage state is inert.
            // Painting coverage as colour separates those two outright: if
            // foliage turns into a grey ramp, the shader IS running and the
            // A2C state is the dead part. Build-time flag, so it is set in the
            // ini before launch.
            if (g_a2cDebugVis) {
                out[o++] = D3DSIO_MAD_ | (4u << 24);
                out[o++] = DstTok(D3DSPR_COLOROUT_, 0, 0x7 /* xyz */, 1);
                out[o++] = SrcTok(D3DSPR_TEMP_, rFree, SWZ_WWWW);
                out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_XXXX);
                out[o++] = SrcTok(D3DSPR_CONST_, cScale, SWZ_YYYY);
            }
            t += 1 + len;
            continue;
        }
        for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
        t += 1 + len;
    }
    out[o++] = 0x0000FFFF;
    *outLen = o;
    return 1;
}

// ---- Foliage SSAA variant -------------------------------------------------
// Alpha-to-coverage is dead on this driver, but per-sample masking works
// (proven: restricting foliage to one of eight samples visibly changed the
// resolved image). So the cutout can be supersampled directly:
//
//   for each sample i:  MULTISAMPLEMASK = 1<<i
//                       cOff = sub-pixel offset of sample i
//                       draw
//
// with the shader sampling its alpha texture at that sub-pixel offset. Each
// pass evaluates the SAME binary cutout at a different position inside the
// pixel, and the MSAA resolve averages them - real supersampling of the alpha
// test, on foliage draws only.
//
// The offset is applied in TEXTURE space via screen-space derivatives, so it
// stays correct at any distance and needs no access to the projection matrix:
//
//   dsx rDx, uv        ; how much uv changes across one pixel horizontally
//   dsy rDy, uv
//   mad rUV, rDx, cOff.x, uv
//   mad rUV, rDy, cOff.y, rUV
//   texld dst, rUV, s  ; the original fetch, at the jittered position
//
// texkill is deliberately KEPT here - unlike the A2C rewrite, this wants the
// original binary cutout, just evaluated N times per pixel.
static int BuildSsaaVariant(const DWORD *src, DWORD *out, UINT outMax,
                            UINT *outLen, DWORD *offsetReg)
{
    UINT bytes = ShaderTokenBytes(src);
    if (bytes < 12) return 0;
    UINT toks = bytes / 4;

    UINT firstTexld = 0, killAt = 0;
    DWORD maxTemp = 0, maxConst = 0;
    UINT t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT op = tok & 0xFFFF, len = (tok >> 24) & 0x0F;
        for (UINT k = 1; k <= len && t + k < toks; k++) {
            DWORD rt = src[t + k];
            if (!(rt & 0x80000000u)) continue;
            DWORD type = ((rt >> 28) & 7) | (((rt >> 11) & 3) << 3);
            DWORD num = rt & 0x7FF;
            if (type == D3DSPR_TEMP_ && num > maxTemp) maxTemp = num;
            if (type == D3DSPR_CONST_ && num > maxConst) maxConst = num;
        }
        if (op == 0x42 && !firstTexld && len >= 3) firstTexld = t;
        if (op == D3DSIO_TEXKILL_ && !killAt) killAt = t;
        t += 1 + len;
    }
    // Only worth doing for an alpha-tested shader whose cutout comes from the
    // first fetch - which is the shape every foliage shader here has.
    if (!firstTexld || !killAt) return 0;
    DWORD rUV = maxTemp + 1, rDx = maxTemp + 2, rDy = maxTemp + 3;
    DWORD cOff = maxConst + 1;
    if (rDy > 27 || cOff > 220) return 0;
    if (outMax < toks + 24) return 0;

    UINT o = 0;
    out[o++] = src[0];
    t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT len = (tok >> 24) & 0x0F;
        if (t == firstTexld) {
            DWORD dst = src[t + 1], uv = src[t + 2], smp = src[t + 3];
            // Derivatives of the ORIGINAL uv, then offset along them.
            out[o++] = 0x5Bu | (2u << 24);                    // dsx
            out[o++] = DstTok(D3DSPR_TEMP_, rDx, MASK_ALL, 0);
            out[o++] = uv;
            out[o++] = 0x5Cu | (2u << 24);                    // dsy
            out[o++] = DstTok(D3DSPR_TEMP_, rDy, MASK_ALL, 0);
            out[o++] = uv;
            out[o++] = D3DSIO_MAD_ | (4u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rUV, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rDx, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cOff, SWZ_XXXX);
            out[o++] = uv;
            out[o++] = D3DSIO_MAD_ | (4u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rUV, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rDy, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cOff, SWZ_YYYY);
            out[o++] = SrcTok(D3DSPR_TEMP_, rUV, 0xE4);
            // The original fetch, now reading the jittered coordinate.
            out[o++] = tok;
            out[o++] = dst;
            out[o++] = SrcTok(D3DSPR_TEMP_, rUV, 0xE4);
            out[o++] = smp;
            t += 1 + len;
            continue;
        }
        for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
        t += 1 + len;
    }
    out[o++] = 0x0000FFFF;
    *outLen = o;
    *offsetReg = cOff;
    return 1;
}

// ---- Foliage FRINGE variant ----------------------------------------------
// Last credible in-engine route to smooth cutouts, and the standard one when
// alpha-to-coverage is unavailable (which it is: both vendor hacks are dead
// on this driver, proven with a coverage ramp wide enough to be unmissable).
//
// The engine's own draw stays exactly as it is and paints the solid interior.
// Then the SAME geometry is drawn again with ordinary alpha blending, using a
// shader that keeps ONLY the transition band around the alpha threshold and
// outputs a smooth alpha across it. The result is an antialiased edge built
// from blending, which no driver feature can refuse to honour.
//
//   v      = (alpha - threshold) * sharpness + 0.5     [unsaturated]
//   kill v < 0        -> outside the shape entirely
//   kill (1 - v) < 0  -> solid interior, already drawn by the engine's pass
//   oC0.w = saturate(v)                                -> the fringe ramp
//
// Both kills write all components: texkill tests x, y AND z, so leaving any
// of them holding garbage would discard pixels at random.
static int BuildFringeVariant(const DWORD *src, DWORD *out, UINT outMax, UINT *outLen)
{
    UINT bytes = ShaderTokenBytes(src);
    if (bytes < 12) return 0;
    UINT toks = bytes / 4;

    UINT killAt = 0, alphaOutAt = 0;
    DWORD killReg = 0, maxTemp = 0, maxConst = 0;
    UINT t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT op = tok & 0xFFFF, len = (tok >> 24) & 0x0F;
        for (UINT k = 1; k <= len && t + k < toks; k++) {
            DWORD rt = src[t + k];
            if (!(rt & 0x80000000u)) continue;
            DWORD ty = ((rt >> 28) & 7) | (((rt >> 11) & 3) << 3);
            DWORD nu = rt & 0x7FF;
            if (ty == D3DSPR_TEMP_ && nu > maxTemp) maxTemp = nu;
            if (ty == D3DSPR_CONST_ && nu > maxConst) maxConst = nu;
        }
        if (op == D3DSIO_TEXKILL_ && !killAt && len >= 1) { killAt = t; killReg = src[t + 1]; }
        if (len >= 1) {
            DWORD d = src[t + 1];
            if (d & 0x80000000u) {
                DWORD ty = ((d >> 28) & 7) | (((d >> 11) & 3) << 3);
                if (ty == D3DSPR_COLOROUT_ && (d & 0x7FF) == 0 && ((d >> 16) & MASK_W))
                    alphaOutAt = t;
            }
        }
        t += 1 + len;
    }
    if (!killAt || !alphaOutAt) return 0;
    DWORD rStash = maxTemp + 1, rA = maxTemp + 2, rB = maxTemp + 3,
          rT = maxTemp + 4, cS = maxConst + 1;
    if (rT > 27 || cS > 220) return 0;
    if (outMax < toks + 48) return 0;

    UINT o = 0;
    out[o++] = src[0];
    out[o++] = D3DSIO_DEF_ | (5u << 24);
    out[o++] = DstTok(D3DSPR_CONST_, cS, MASK_ALL, 0);
    // .x unused now (the fixed sharpness it held is what made this fail at
    // distance), .y = 0.5, .z = epsilon so a flat region cannot divide by
    // zero, .w = 1.0
    { float a = 0.0f, b = 0.5f, c = 1e-5f, d = 1.0f;
      memcpy(&out[o+0], &a, 4); memcpy(&out[o+1], &b, 4);
      memcpy(&out[o+2], &c, 4); memcpy(&out[o+3], &d, 4); o += 4; }

    t = 1;
    while (t < toks) {
        DWORD tok = src[t];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { t += 1 + ((tok >> 16) & 0x7FFF); continue; }
        UINT len = (tok >> 24) & 0x0F;
        if (t == killAt) {
            DWORD ty = ((killReg >> 28) & 7) | (((killReg >> 11) & 3) << 3);
            DWORD nu = killReg & 0x7FF;
            // v = alpha - threshold, replicated so texkill's x/y/z all agree.
            out[o++] = D3DSIO_MOV_ | (2u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rStash, MASK_ALL, 0);
            out[o++] = SrcTok(ty, nu, SWZ_XXXX);
            // fwidth(v) = |ddx(v)| + |ddy(v)|. This is the whole fix: it
            // scales with minification, so the coverage ramp stays ~1 pixel
            // wide at any distance. A FIXED band (what this used before)
            // collapses below a pixel exactly where foliage aliases worst.
            out[o++] = 0x5Bu | (2u << 24);                    // dsx
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rStash, 0xE4);
            out[o++] = 0x5Cu | (2u << 24);                    // dsy
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rStash, 0xE4);
            out[o++] = 0x23u | (2u << 24);                    // abs
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, 0xE4);
            out[o++] = 0x23u | (2u << 24);                    // abs
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rB, 0xE4);
            out[o++] = D3DSIO_ADD_ | (3u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, 0xE4);
            out[o++] = SrcTok(D3DSPR_TEMP_, rB, 0xE4);
            out[o++] = D3DSIO_ADD_ | (3u << 24);              // + epsilon
            out[o++] = DstTok(D3DSPR_TEMP_, rA, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cS, 0xAA);       // .zzzz
            out[o++] = 0x06u | (2u << 24);                    // rcp
            out[o++] = DstTok(D3DSPR_TEMP_, rA, 0x1 /* .x */, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, SWZ_XXXX);
            // t = v / fwidth + 0.5
            out[o++] = 0x05u | (3u << 24);                    // mul
            out[o++] = DstTok(D3DSPR_TEMP_, rT, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rStash, 0xE4);
            out[o++] = SrcTok(D3DSPR_TEMP_, rA, SWZ_XXXX);
            out[o++] = D3DSIO_ADD_ | (3u << 24);
            out[o++] = DstTok(D3DSPR_TEMP_, rT, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_TEMP_, rT, 0xE4);
            out[o++] = SrcTok(D3DSPR_CONST_, cS, SWZ_YYYY);
            out[o++] = D3DSIO_TEXKILL_ | (1u << 24);          // t < 0: outside
            out[o++] = DstTok(D3DSPR_TEMP_, rT, MASK_ALL, 0);
            out[o++] = D3DSIO_ADD_ | (3u << 24);              // 1 - t
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            out[o++] = SrcTok(D3DSPR_CONST_, cS, SWZ_WWWW);
            out[o++] = SrcTok(D3DSPR_TEMP_, rT, 0xE4) | 0x01000000u;
            out[o++] = D3DSIO_TEXKILL_ | (1u << 24);          // interior
            out[o++] = DstTok(D3DSPR_TEMP_, rB, MASK_ALL, 0);
            t += 1 + len;
            continue;
        }
        if (t == alphaOutAt) {
            for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
            // Blend alpha = the derivative-normalised coverage computed above.
            out[o++] = D3DSIO_MOV_ | (2u << 24);
            out[o++] = DstTok(D3DSPR_COLOROUT_, 0, MASK_W, 1);
            out[o++] = SrcTok(D3DSPR_TEMP_, rT, SWZ_XXXX);
            t += 1 + len;
            continue;
        }
        for (UINT k = 0; k <= len && t + k < toks; k++) out[o++] = src[t + k];
        t += 1 + len;
    }
    out[o++] = 0x0000FFFF;
    *outLen = o;
    return 1;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePixelShader(
    IDirect3DDevice9 *This, const DWORD *pFunction, IDirect3DPixelShader9 **ppShader)
{
    int isMain = ((LONG)GetCurrentThreadId() == g_mainThreadId);
    if (g_dumpShaders) DumpPixelShader(pFunction);
    unsigned __int64 t0 = __rdtsc();
    HRESULT hr = g_origCreatePS(This, pFunction, ppShader);
    if (g_cyclesPerUsec > 0.0) {
        LONG us = (LONG)((double)(__rdtsc() - t0) / g_cyclesPerUsec);
        InterlockedIncrement(&g_psCount);
        InterlockedExchangeAdd(&g_psSumUsec, us);
        if (us > g_psMaxUsec) g_psMaxUsec = us;
        if (isMain) InterlockedIncrement(&g_psMainCount);
    }
    // Recognise every kill-candidate shader by bytecode hash, whatever object
    // the engine wraps it in this session, and stage the substitutes on the
    // first sight of any.
    if (SUCCEEDED(hr) && ppShader && *ppShader && pFunction) {
        DWORD h = PsFnv1a(pFunction);
#if ENABLE_SHADOW_PCSS
        PcssNoteCreated(h, (void *)*ppShader);
#endif
        // The passthrough is needed by the FXAA toggle whether or not any
        // kill-list candidate was ever created, so build it on first sight of
        // the FXAA shader too.
        if (h == FXAA_SHADER_HASH_REAL) {
            if (!g_fxaaPassthrough)
                g_origCreatePS(This, (const DWORD *)g_psPassthrough, &g_fxaaPassthrough);
            char fl[144];
            sprintf(fl, "[fxaa] FXAA shader ps_%08X created; passthrough %s", h,
                    g_fxaaPassthrough ? "ready" : "FAILED");
            LogLine(fl);
        }
        // Alpha-to-coverage variant, synthesised now so the bind path is a
        // pointer lookup. Built unconditionally (cheap, once per shader) so
        // the feature can be toggled at runtime without a relaunch.
        // Gated on the feature flag AND on the allow-list. Building a variant
        // for every cutout shader meant thousands of rewrites per session
        // handed to the driver for shaders we never intended to change; now
        // only the confirmed foliage shaders are touched at all, which keeps
        // the blast radius equal to the feature's actual scope.
        // EITHER feature needs the variants built. The SSAA variant is
        // constructed in this same block, so gating the block on A2cEnable
        // alone meant turning A2C off (as the SSAA test did) silently built
        // nothing and the multi-draw path had no shader to bind - it reported
        // multiDraws=0 and looked exactly like the technique failing.
        int a2cWanted = 0;
#if ENABLE_CUTOUT_AA
        if (g_a2cEnable || g_ssaaFoliage || g_fringeFoliage) {
            for (size_t a = 0; a < A2C_ALLOW_COUNT; a++)
                if (g_a2cAllow[a] == h) { a2cWanted = 1; break; }
        }
#endif
        // Each variant is built INDEPENDENTLY. They used to be nested inside
        // the A2C variant's success branch, so a failure - or simply having
        // A2C switched off - silently produced no fringe and no SSAA shader
        // either, and the features reported "0 draws" exactly as though the
        // technique had failed. One slot, three optional variants.
        if (a2cWanted && g_a2cVariantCount < A2C_MAX) {
            LONG n = InterlockedIncrement(&g_a2cVariantCount) - 1;
            if (n >= A2C_MAX) {
                g_a2cVariantCount = A2C_MAX;
            } else {
                // STACK buffers, not static: several loader threads compile
                // shaders at once and a shared scratch would interleave.
                DWORD buf[4096];
                UINT len = 0, offReg = 0;
                IDirect3DPixelShader9 *sh = NULL;
                char l[192];

                g_a2cVariants[n].orig = (void *)*ppShader;
                g_a2cVariants[n].hash = h;
                g_a2cVariants[n].draws = 0;
                if (!g_psTintObj)
                    g_origCreatePS(This, (const DWORD *)g_psTint, &g_psTintObj);

                if (BuildA2cVariant(pFunction, buf, 4096, &len) &&
                    SUCCEEDED(g_origCreatePS(This, buf, &sh)) && sh) {
                    g_a2cVariants[n].variant = (void *)sh;
                } else if (InterlockedIncrement(&g_a2cBuildFails) <= 4) {
                    sprintf(l, "[a2c] variant build/create FAILED for ps_%08X", h);
                    LogLine(l);
                }
                sh = NULL; len = 0;
                if (BuildFringeVariant(pFunction, buf, 4096, &len) &&
                    SUCCEEDED(g_origCreatePS(This, buf, &sh)) && sh) {
                    g_a2cVariants[n].fringe = (void *)sh;
                    if (n < 6) {
                        sprintf(l, "[fringe] variant built for ps_%08X (%u tokens)", h, len);
                        LogLine(l);
                    }
                } else if (InterlockedIncrement(&g_fringeBuildFails) <= 4) {
                    sprintf(l, "[fringe] variant build/create FAILED for ps_%08X", h);
                    LogLine(l);
                }
                sh = NULL; len = 0;
                if (BuildSsaaVariant(pFunction, buf, 4096, &len, &offReg) &&
                    SUCCEEDED(g_origCreatePS(This, buf, &sh)) && sh) {
                    g_a2cVariants[n].ssaa = (void *)sh;
                    g_a2cVariants[n].ssaaOffReg = offReg;
                    g_ssaaOffsetReg = (LONG)offReg;
                } else if (InterlockedIncrement(&g_ssaaBuildFails) <= 4) {
                    sprintf(l, "[ssaa] variant build/create FAILED for ps_%08X", h);
                    LogLine(l);
                }
                if (n < 6) {
                    sprintf(l, "[a2c] slot #%ld for ps_%08X: a2c=%s fringe=%s ssaa=%s",
                            n + 1, h,
                            g_a2cVariants[n].variant ? "yes" : "no",
                            g_a2cVariants[n].fringe  ? "yes" : "no",
                            g_a2cVariants[n].ssaa    ? "yes" : "no");
                    LogLine(l);
                }
            }
        }
        // Identity map for draw attribution, every shader.
        // Reserve the slot atomically: shader creation runs on several
        // threads here (3000+ compiles at boot across worker threads), so
        // "read count, write, store count+1" hands two threads the same slot
        // and corrupts both the map and the pointer lookup.
        LONG mi = InterlockedIncrement(&g_psMapCount) - 1;
        if (mi < PS_MAP_MAX) {
            g_psMap[mi].obj = (void *)*ppShader;
            g_psMap[mi].hash = h;
            g_psMap[mi].taps = PsCountTaps(pFunction);
            g_psMap[mi].draws = 0;
            g_psMap[mi].prevDraws = 0;
            g_psMap[mi].recent = 0;
            LONG s = PsLookupSlot((void *)*ppShader);
            if (s >= 0) g_psLookup[s] = mi + 1;
        } else {
            g_psMapCount = PS_MAP_MAX;      // clamp, do not wrap
        }
        for (size_t ci = 0; ci < NUM_PS_KILL; ci++) {
            if (g_psKillCandidates[ci] != h) continue;
            LONG n = g_psKillObjCount;
            if (n < PS_KILL_OBJ_MAX) {
                g_psKillObjs[n].obj = (void *)*ppShader;
                g_psKillObjs[n].cand = (LONG)ci;
                g_psKillObjCount = n + 1;
            }
            if (!g_fxaaPassthrough)
                g_origCreatePS(This, (const DWORD *)g_psPassthrough, &g_fxaaPassthrough);
            if (!g_psTintObj)
                g_origCreatePS(This, (const DWORD *)g_psTint, &g_psTintObj);
            char l[160];
            sprintf(l, "[fxaa] kill candidate %u (ps_%08X) created: obj %p (%ld tracked)",
                    (unsigned)(ci + 1), h, (void *)*ppShader, g_psKillObjCount);
            LogLine(l);
            break;
        }
    }
    return hr;
}

// Bind-time substitution - one pointer compare per SetPixelShader when the
// toggle is off, a tiny loop when on.
typedef HRESULT (STDMETHODCALLTYPE *PFN_SetPixelShader)(
    IDirect3DDevice9 *, IDirect3DPixelShader9 *);
static PFN_SetPixelShader g_origSetPixelShader = NULL;

// Tentative definitions: the A2C bind path needs the MS colour surface and
// the SetRenderState original, both of which are defined with the MSAA block
// further down. Same pattern as the other early blocks in this file.
static IDirect3DSurface9 *g_msColour;
static volatile LONG g_msActive;
static HRESULT (STDMETHODCALLTYPE *g_origSetRenderState)(
    IDirect3DDevice9 *, D3DRENDERSTATETYPE, DWORD);

static HRESULT STDMETHODCALLTYPE HookedSetPixelShader(
    IDirect3DDevice9 *This, IDirect3DPixelShader9 *pShader)
{
    g_curPsObj = (void *)pShader;
    g_curPsIdx = PsIndexOf((void *)pShader);
#if ENABLE_CASCADE_WATCH
    CascadeWatchBind((void *)pShader);
#endif

    // ---- General shader identify ----------------------------------------
    // Deliberately NOT restricted to cutout candidates or to blending being
    // off: the previous version was, which is why stepping it never lit up
    // the grass - the grass shader was never in that set. This covers every
    // pixel shader the game binds, ranked by draws in the LAST WINDOW so the
    // list reflects what is on screen right now.
    // ---- FXAA off -------------------------------------------------------
    // ps_A082B248 IS the game's anti-aliasing pass, identified from its
    // disassembly: 9 taps of the scene sampler, 4 of them diagonal
    // neighbours, luma carried in alpha, local luma range -> edge strength
    // via pow(range, 0.25), edge DIRECTION from the diagonal luma
    // differences, normalised and clamped to +/-2 texels, then samples along
    // that direction and cmp-selects blended vs original. Textbook FXAA.
    //
    // It carries no texel-size literals - the offsets arrive at runtime in
    // c0 ($s_neighborhoodValue) - which is why every constant-fingerprint
    // search for it came back empty.
    //
    // Substituting the plain centre-tap passthrough removes the filter while
    // leaving the pass, its target and everything downstream intact.
    // psIdx is SNAPSHOTTED into a local before use. g_curPsIdx is a global
    // shared by every thread that binds shaders, so re-reading it between the
    // bounds check and the array index is a race: another thread setting it
    // to -1 in that gap indexes g_psMap[-1]. That is what crashed the game on
    // boot, during the ~3200 shader creations where binds are densest.
    LONG psIdx = g_curPsIdx;
    if (psIdx < 0 || psIdx >= g_psMapCount || psIdx >= PS_MAP_MAX) psIdx = -1;

#if ENABLE_SHADOW_PCSS
    {
        HRESULT phr;
        if (PcssBind((void *)This, (void *)pShader, &phr)) return phr;
    }
#endif
    if (g_fxaaOff && g_fxaaPassthrough && psIdx >= 0 &&
        g_psMap[psIdx].hash == FXAA_SHADER_HASH_REAL) {
        InterlockedIncrement(&g_fxaaSubs);
        return g_origSetPixelShader(This, g_fxaaPassthrough);
    }

    // Selection is the shader's CREATION index, which never changes for the
    // life of the process. Ranking by draw activity made entries swap places
    // while walking the list, which made it unusable; a stable list can be
    // stepped through quickly even if it is long.
    if (g_psIdentify > 0 && g_psTintObj && psIdx >= 0 &&
        psIdx == g_psIdentify - 1)
        return g_origSetPixelShader(This, g_psTintObj);

    // ---- Alpha-to-coverage bind path ------------------------------------
    // g_msActive, not merely "MSAA is on": the variant may ONLY be bound while
    // our multisampled colour target is the current render target. The first
    // version gated on the feature being enabled, so the rewritten shaders
    // were also used for the SHADOW pass - which renders into a plain
    // non-multisampled shadow map where coverage does nothing, while the
    // texkill the transform removes is the only thing making foliage shadows
    // see-through. Result: solid-block vegetation shadows.
    // The vendor A2C state is enabled only while a rewritten shader is
    // current and switched off immediately afterwards.
    if (g_msActive && pShader && !g_alphaBlendOn && g_a2cVariantCount) {
        LONG n = g_a2cVariantCount, i;
        if (n > A2C_MAX) n = A2C_MAX;
        for (i = 0; i < n; i++) {
            if (g_a2cVariants[i].orig != (void *)pShader) continue;
            InterlockedIncrement(&g_a2cVariants[i].draws);
            // (Identify now happens earlier in this function, over ALL
            // shaders rather than only cutout candidates - see g_psIdentify.)
            // Allow-list only. An unlisted shader is left completely alone,
            // which is why this can no longer damage NPCs or water.
            if (!g_a2cEnable || !g_gpuVendor || g_msaaSamples < 2) break;
            int allowed = 0;
            for (size_t a = 0; a < A2C_ALLOW_COUNT; a++)
                if (g_a2cAllow[a] && g_a2cAllow[a] == g_a2cVariants[i].hash) { allowed = 1; break; }
            if (!allowed) break;
            if (!g_a2cStateOn) {
                // Several drivers gate alpha-to-coverage on ALPHATESTENABLE
                // being TRUE - NVIDIA's ATOC documents it outright, and AMD's
                // A2M is reported to want it too. The engine never enables
                // alpha test (it cuts out with texkill instead), so the state
                // was FALSE for every one of the 329,954 substituted binds,
                // which would make the coverage hack silently inert. ALWAYS
                // as the compare function keeps it from rejecting anything.
                g_origSetRenderState(This, D3DRS_ALPHATESTENABLE, TRUE);
                g_origSetRenderState(This, D3DRS_ALPHAFUNC, D3DCMP_ALWAYS);
                g_origSetRenderState(This, D3DRS_MULTISAMPLEANTIALIAS, TRUE);
                // BOTH vendor dialects, unconditionally. The AMD 'A2M' hack
                // is from ~2007 and current drivers may simply ignore it;
                // some AMD drivers are reported to accept NVIDIA's 'ATOC'
                // spelling as well. Setting the wrong one is harmless - the
                // driver that does not recognise it treats the write as an
                // ordinary (and unused) render state - so trying both costs
                // nothing and removes a guess.
                g_origSetRenderState(This, D3DRS_POINTSIZE, MAKEFOURCC('A','2','M','1'));
                g_origSetRenderState(This, D3DRS_ADAPTIVETESS_Y, MAKEFOURCC('A','T','O','C'));
                // ---- MULTISAMPLEMASK feasibility probe ---------------------
                // Alpha-to-coverage is dead on this driver (both vendor
                // dialects ignored, proven with a coverage ramp so wide it
                // could not have been missed). The remaining route to smooth
                // cutouts is sample-masked jittered multi-draw: draw foliage
                // once per sample with the projection nudged by that sample's
                // sub-pixel offset, letting the MSAA resolve average them -
                // real supersampling of the alpha test, on foliage only.
                //
                // That is a lot of machinery to build on an assumption, and
                // the last assumption of this kind (the vendor A2C hack) cost
                // a day. So probe the mechanism first: restrict foliage to
                // ONE of the N samples. If the mask works, foliage renders at
                // 1/N coverage and comes out of the resolve obviously faint.
                // If it looks completely normal, per-sample control is not
                // available either and the approach is dead before it is
                // written.
                if (g_a2cMaskTest)
                    g_origSetRenderState(This, D3DRS_MULTISAMPLEMASK, 0x1);
                g_a2cStateOn = 1;
            }
            InterlockedIncrement(&g_a2cBinds);
            return g_origSetPixelShader(This,
                       (IDirect3DPixelShader9 *)g_a2cVariants[i].variant);
        }
    }
    // Count the case where a rewritten shader WAS current material but the
    // draw is blended - distinguishes "never matched" from "matched but
    // always rejected at bind", which are very different failures.
    if (g_a2cEnable && g_alphaBlendOn && pShader && g_a2cVariantCount) {
        LONG n = g_a2cVariantCount, i;
        if (n > A2C_MAX) n = A2C_MAX;
        for (i = 0; i < n; i++)
            if (g_a2cVariants[i].orig == (void *)pShader) {
                InterlockedIncrement(&g_a2cBlockedBlend);
                break;
            }
    }
    if (g_a2cStateOn) {
        g_origSetRenderState(This, D3DRS_POINTSIZE, MAKEFOURCC('A','2','M','0'));
        g_origSetRenderState(This, D3DRS_ADAPTIVETESS_Y, D3DFMT_UNKNOWN);
        if (g_a2cMaskTest)
            g_origSetRenderState(This, D3DRS_MULTISAMPLEMASK, 0xFFFFFFFF);
        // Put alpha test back where the engine had it. It never enables it,
        // so FALSE is the correct restore - but track it anyway rather than
        // assume, since leaving alpha test on would silently change every
        // draw that follows.
        g_origSetRenderState(This, D3DRS_ALPHATESTENABLE, g_alphaTestWasOn ? TRUE : FALSE);
        g_a2cStateOn = 0;
    }

    LONG pick = g_fxaaPick;
    if (pick >= 1 && pShader && g_fxaaPassthrough) {
        LONG n = g_psKillObjCount, i;
        if (n > PS_KILL_OBJ_MAX) n = PS_KILL_OBJ_MAX;
        for (i = 0; i < n; i++) {
            if (g_psKillObjs[i].obj == (void *)pShader &&
                g_psKillObjs[i].cand == pick - 1) {
                InterlockedIncrement(&g_fxaaSubs);
                // MsaaDebugClear doubles as the footprint instrument: solid
                // magenta instead of a passthrough shows exactly which pixels
                // this pass writes, which is how the real AA pass gets found.
                return g_origSetPixelShader(This,
                    (g_msaaDebugClear && g_psTintObj) ? g_psTintObj : g_fxaaPassthrough);
            }
        }
    }
    return g_origSetPixelShader(This, pShader);
}

