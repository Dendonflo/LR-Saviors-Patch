// ---- Texture filtering: the D3D half --------------------------------------
//
// Split from 08d_texfilter.c for one mechanical reason: d3d9.h does not enter
// this translation unit until 12_locks_staging.c, so anything naming a D3D
// type has to sit after it - the same constraint that put the tentative defs
// at the top of 03_render_state.c. 08d keeps the state, the counters and the
// report, which 09/10/11 need and which name no D3D type; this file keeps the
// hook. READ 08D'S HEADER BLOCK FIRST: the reasoning for the whole feature,
// and the disassembly it rests on, live there.
//
// SELF-INTERFERENCE. This hook is a state machine over the engine's sampler
// state, so the mod's OWN sampler writes must not enter it - our UI blit
// deliberately samples POINT/CLAMP and an "upgrade" to anisotropic would
// soften it, and our AO passes would teach the census filter values the
// engine never asked for. 25_ssao.c and 26_ingame_ui.c go through
// ModSetSamplerState below, which is the same rule every other hooked slot in
// this mod already follows.

typedef HRESULT (STDMETHODCALLTYPE *PFN_SetSamplerState)(
    IDirect3DDevice9 *, DWORD, D3DSAMPLERSTATETYPE, DWORD);
static PFN_SetSamplerState g_origSetSamplerState = NULL;

// The form the mod's own render passes must use. Falls back to the vtable
// when the slot was never hooked - which is then the original entry anyway,
// so this is not a second code path so much as the same one reached
// differently. Same shape as AoSetPs in 25_ssao.c.
static void ModSetSamplerState(IDirect3DDevice9 *dev, DWORD stage,
                               D3DSAMPLERSTATETYPE type, DWORD value)
{
    if (g_origSetSamplerState) g_origSetSamplerState(dev, stage, type, value);
    else IDirect3DDevice9_SetSamplerState(dev, stage, type, value);
}

static int TfAnisoBucket(DWORD v)
{
    switch (v) {
    case 1:  return 0;
    case 2:  return 1;
    case 4:  return 2;
    case 8:  return 3;
    case 16: return 4;
    default: return 5;
    }
}

// Clamp a requested level to something the device will accept. Caps are read
// once, lazily, from the first device that reaches the hook - GetDeviceCaps is
// an unhooked slot, so calling it through the macro form is safe here.
static LONG TfClampLevel(IDirect3DDevice9 *dev, LONG want)
{
    LONG cap = g_tfCapAniso;
    if (!cap && dev) {
        D3DCAPS9 caps;
        memset(&caps, 0, sizeof(caps));
        if (SUCCEEDED(IDirect3DDevice9_GetDeviceCaps(dev, &caps))) {
            cap = (LONG)caps.MaxAnisotropy;
            if (cap < 1) cap = 1;
            InterlockedExchange(&g_tfCapAniso, cap);
            char l[192];
            sprintf(l, "[texfilter] device caps: MaxAnisotropy=%ld minAniso=%d magAniso=%d mipLinear=%d",
                    cap,
                    (caps.TextureFilterCaps & D3DPTFILTERCAPS_MINFANISOTROPIC) ? 1 : 0,
                    (caps.TextureFilterCaps & D3DPTFILTERCAPS_MAGFANISOTROPIC) ? 1 : 0,
                    (caps.TextureFilterCaps & D3DPTFILTERCAPS_MIPFLINEAR) ? 1 : 0);
            LogLine(l);
        }
    }
    if (cap > 0 && want > cap) want = cap;
    if (want < 1) want = 1;
    return want;
}

// See the qualifying-rule paragraph in 08d's header block.
static int TfStageQualifies(DWORD s)
{
    DWORD mip = g_tfEngKnown[s][D3DSAMP_MIPFILTER]
                    ? g_tfEng[s][D3DSAMP_MIPFILTER] : (DWORD)D3DTEXF_NONE;
    return mip == D3DTEXF_POINT || mip == D3DTEXF_LINEAR;
}

// Put MAXANISOTROPY on a stage the engine may never set it on itself. Guarded
// by the device shadow, so a qualifying stage costs one extra D3D call when it
// first qualifies and none afterwards.
static void TfPushAniso(IDirect3DDevice9 *dev, DWORD s, DWORD lvl)
{
    if (g_tfDevAniso[s] == lvl) return;
    g_tfDevAniso[s] = lvl;
    g_origSetSamplerState(dev, s, D3DSAMP_MAXANISOTROPY, lvl);
    g_tfAnisoSet++;
}

static HRESULT STDMETHODCALLTYPE HookedSetSamplerState(
    IDirect3DDevice9 *This, DWORD Sampler, D3DSAMPLERSTATETYPE Type, DWORD Value)
{
    LONG lvl;

    if (Sampler >= TF_STAGES || (DWORD)Type >= TF_STATES)
        return g_origSetSamplerState(This, Sampler, Type, Value);

    // Census BEFORE the rewrite, always. It records what the ENGINE asked
    // for, which is the question it exists to answer, and letting our own
    // rewrite feed back into it would make the recon confirm itself.
    //
    // PLAIN increments, not Interlocked, and that is deliberate on this one
    // path. Every other counter in this mod is atomic because it costs
    // nothing where it sits; here the hook runs several thousand times a
    // frame, and two locked read-modify-writes per call is real frame time
    // spent on a DIAGNOSTIC. A lost update under a race costs one tick of a
    // histogram nobody reads to single-digit precision. The two BITMASKS
    // still need the atomic form - a lost bit there is a wrong answer, not
    // an imprecise one - so they keep it, behind a test that skips the
    // locked op once the bit is already set, which after the first frame is
    // always.
    LONG sbit = (LONG)(1UL << Sampler);
    g_tfEng[Sampler][Type] = Value;
    g_tfEngKnown[Sampler][Type] = 1;
    g_tfCalls++;
    if (!(g_tfStagesSeen & sbit)) InterlockedOr(&g_tfStagesSeen, sbit);
    switch (Type) {
    case D3DSAMP_MINFILTER:
        if (Value < 8) g_tfMinHist[Value]++;
        break;
    case D3DSAMP_MAGFILTER:
        if (Value < 8) g_tfMagHist[Value]++;
        break;
    case D3DSAMP_MIPFILTER:
        if (Value < 8) g_tfMipHist[Value]++;
        if ((Value == D3DTEXF_POINT || Value == D3DTEXF_LINEAR) && !(g_tfMipStages & sbit))
            InterlockedOr(&g_tfMipStages, sbit);
        break;
    case D3DSAMP_MAXANISOTROPY:
        g_tfAnisoHist[TfAnisoBucket(Value)]++;
        break;
    case D3DSAMP_MIPMAPLODBIAS:
        // Float bit pattern, so it has to be decoded before it means anything.
        // A POSITIVE bias picks a blurrier mip than the pixel footprint calls
        // for - "mushy textures" that no filtering setting can fix - while a
        // negative one sharpens and buys shimmer. The engine does both, so
        // the sides are counted separately and the range is kept.
        if (Value != 0) {
            float bias, cur;
            memcpy(&bias, &Value, sizeof(bias));
            g_tfBiasWrites++;
            g_tfBiasLast = (LONG)Value;
            if (bias > 0.0f) {
                g_tfBiasPos++;
                if (!(g_tfBiasPosStages & sbit)) InterlockedOr(&g_tfBiasPosStages, sbit);
            } else {
                g_tfBiasNeg++;
            }
            memcpy(&cur, &g_tfBiasMinBits, sizeof(cur));
            if (bias < cur) g_tfBiasMinBits = (LONG)Value;
            memcpy(&cur, &g_tfBiasMaxBits, sizeof(cur));
            if (bias > cur) g_tfBiasMaxBits = (LONG)Value;
        }
        break;
    case D3DSAMP_MAXMIPLEVEL:
        // Non-zero = the engine is refusing to use the sharpest mip levels,
        // the other way to get "blurry at distance" with a perfect texture.
        if (Value != 0) g_tfMaxLevelNZ++;
        break;
    default:
        break;
    }

    lvl = g_anisoLevel;
    // MipBiasMode belongs in this gate too. Leaving it out would have made the
    // bias clamp silently inert for anyone running AnisoLevel=0 - exactly the
    // A/B baseline someone testing the clamp is most likely to be sitting on.
    if (lvl <= 0 && !g_forceTrilinear && !g_mipBiasMode)
        return g_origSetSamplerState(This, Sampler, Type, Value);
    lvl = TfClampLevel(This, lvl);

    switch (Type) {
    case D3DSAMP_MIPMAPLODBIAS:
        // A FLOOR on the negative side only - see g_mipBiasMode. Real-time
        // with no re-push machinery: the engine rewrites this state well over
        // a million times a session, so a menu change takes effect within a
        // frame in both directions, which is what makes it an A/B instrument
        // rather than a restart-required setting.
        if (g_mipBiasMode > 0) {
            float bias, floorv;
            memcpy(&bias, &Value, sizeof(bias));
            floorv = (g_mipBiasMode == 1) ? 0.0f
                   : (g_mipBiasMode == 2) ? -0.5f : -1.0f;
            if (bias < floorv) {
                memcpy(&Value, &floorv, sizeof(Value));
                g_tfBiasClamped++;
            }
        }
        break;
    case D3DSAMP_MIPFILTER:
        // Trilinear. Do this FIRST: it can make the stage qualify, and the
        // re-evaluation below depends on the new value being in the shadow.
        if (g_forceTrilinear && Value == D3DTEXF_POINT) {
            Value = D3DTEXF_LINEAR;
            g_tfEng[Sampler][Type] = Value;   // keep the shadow = the device
            g_tfMipUp++;
        }
        // Mipmapping just came on for this stage, so a MINFILTER the engine
        // set EARLIER was judged against "no mips" and declined. Re-emit it.
        // This is the ordering hazard the whole design has to survive:
        // sampler state arrives one field at a time, in no guaranteed order.
        if (lvl >= 2 && (Value == D3DTEXF_POINT || Value == D3DTEXF_LINEAR)) {
            DWORD mn = g_tfEngKnown[Sampler][D3DSAMP_MINFILTER]
                           ? g_tfEng[Sampler][D3DSAMP_MINFILTER] : (DWORD)D3DTEXF_POINT;
            if (mn == D3DTEXF_LINEAR || mn == D3DTEXF_ANISOTROPIC) {
                HRESULT mh = g_origSetSamplerState(This, Sampler, Type, Value);
                g_origSetSamplerState(This, Sampler, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
                TfPushAniso(This, Sampler, (DWORD)lvl);
                g_tfMinUp++;
                return mh;
            }
        }
        break;

    case D3DSAMP_MINFILTER:
        if (!TfStageQualifies(Sampler)) break;
        if (lvl >= 2 && (Value == D3DTEXF_LINEAR || Value == D3DTEXF_ANISOTROPIC)) {
            HRESULT h;
            Value = D3DTEXF_ANISOTROPIC;
            g_tfMinUp++;
            // The companion write. MAXANISOTROPY defaults to 1, so an
            // anisotropic MINFILTER without it is a more expensive way to get
            // exactly the filtering we started with.
            h = g_origSetSamplerState(This, Sampler, Type, Value);
            TfPushAniso(This, Sampler, (DWORD)lvl);
            return h;
        }
        // Level 1 = "Off": undo any anisotropy the engine asks for itself, so
        // Off really is off rather than "whatever the engine wanted".
        if (lvl == 1 && Value == D3DTEXF_ANISOTROPIC) Value = D3DTEXF_LINEAR;
        break;

    case D3DSAMP_MAXANISOTROPY:
        // Unconditional on a qualifying stage - NOT "only when the engine
        // asked for more than 1". The engine's own value is 1 whenever the
        // player's setting is Standard, and a level the mod's menu is showing
        // as 16x must not depend on a vanilla setting whose menu entries we
        // just deleted.
        if (lvl >= 1 && TfStageQualifies(Sampler) && (DWORD)lvl != Value) {
            Value = (DWORD)lvl;
            g_tfAnisoSet++;
        }
        g_tfDevAniso[Sampler] = Value;
        break;

    default:
        break;
    }
    return g_origSetSamplerState(This, Sampler, Type, Value);
}

// Sampler state returns to the API defaults across a device Reset, so every
// shadow this feature keeps is a lie afterwards - and the one that matters is
// g_tfDevAniso, whose whole job is to suppress a redundant D3D call. Stale, it
// suppresses a NECESSARY one and anisotropy silently stops applying after any
// resolution change. Same class of bug as the AO latches (AoReconReset).
static void TexFilterDeviceReset(void)
{
    memset(g_tfEng, 0, sizeof(g_tfEng));
    memset(g_tfEngKnown, 0, sizeof(g_tfEngKnown));
    memset(g_tfDevAniso, 0, sizeof(g_tfDevAniso));
    InterlockedExchange(&g_tfRepush, 1);
}

// Consumed from EndScene: end of frame is the right moment, because whatever
// this pushes survives into the next frame's draws until the engine itself
// overwrites it - which is precisely the case it exists for (see
// TexFilterMarkDirty in 08d).
static void TexFilterFrameTick(IDirect3DDevice9 *dev)
{
    LONG lvl;
    DWORD s;
    if (!InterlockedExchange(&g_tfRepush, 0)) return;
    lvl = g_anisoLevel;
    if (lvl <= 0 || !dev || !g_origSetSamplerState) return;
    lvl = TfClampLevel(dev, lvl);
    for (s = 0; s < TF_STAGES; s++) {
        if (!TfStageQualifies(s)) continue;
        g_tfDevAniso[s] = (DWORD)lvl;
        g_origSetSamplerState(dev, s, D3DSAMP_MAXANISOTROPY, (DWORD)lvl);
        if (lvl >= 2) {
            DWORD mn = g_tfEngKnown[s][D3DSAMP_MINFILTER]
                           ? g_tfEng[s][D3DSAMP_MINFILTER] : (DWORD)D3DTEXF_POINT;
            if (mn == D3DTEXF_LINEAR || mn == D3DTEXF_ANISOTROPIC)
                g_origSetSamplerState(dev, s, D3DSAMP_MINFILTER, D3DTEXF_ANISOTROPIC);
        }
    }
}

// Mip-chain census hook point, called from HookedCreateTexture. Deliberately
// narrow: render targets, depth surfaces and dynamic textures have no mip
// chains by design, and anything under 64x64 is a UI/lookup asset where a
// single level is correct. What is left is surface textures, where a single
// level IS the defect.
static void TexFilterNoteTexture(UINT Width, UINT Height, UINT Levels,
                                 DWORD Usage, D3DFORMAT Format, DWORD ra)
{
    UINT big;
    int bucket, dxt, fromGame;
    if (Usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC)) return;
    if (Width < 64 || Height < 64) return;
    InterlockedIncrement(&g_tfTexTotal);
    if (Levels != 1) { InterlockedIncrement(&g_tfTexFull); return; }
    InterlockedIncrement(&g_tfTexSingle);
    // Bucket on the LARGER edge: a 1024x64 strip is a big texture wearing an
    // awkward shape, and it is the large dimension that decides whether a
    // missing mip chain will be visible.
    big = Width > Height ? Width : Height;
    bucket = big >= 1024 ? 4 : (big >= 512 ? 3 : (big >= 256 ? 2 : (big >= 128 ? 1 : 0)));
    InterlockedIncrement(&g_tfTexSingleBucket[bucket]);
    // Compressed = authored art, i.e. a world surface. Uncompressed at these
    // sizes is overwhelmingly UI, lookup tables and other mods' uploads, all
    // of which are CORRECTLY single-level. This is the split that decides
    // whether the count means anything.
    dxt = (Format == D3DFMT_DXT1 || Format == D3DFMT_DXT2 || Format == D3DFMT_DXT3 ||
           Format == D3DFMT_DXT4 || Format == D3DFMT_DXT5);
    if (dxt) InterlockedIncrement(&g_tfTexSingleDxt);
    else     InterlockedIncrement(&g_tfTexSingleRaw);

    // Creator attribution. The game loading its own art with no mip chain is
    // a defect we could fix; another module's texture is none of our business
    // and must not be counted as evidence either way.
    fromGame = (g_mainModBase && ra >= g_mainModBase && ra < g_mainModBase + g_mainModSize);
    if (fromGame) InterlockedIncrement(&g_tfTexSingleGame);
    else          InterlockedIncrement(&g_tfTexSingleForeign);

    // A handful of worked examples alongside the counts. Capped hard: this is
    // for identifying WHAT the population is, and eight of them do that as
    // well as eight hundred would.
    //
    // The >300 gate is the 2026-08-19 correction. The first eight examples all
    // came from the title screen and were all UI-shaped - 512x64, 1024x64,
    // 660x76 - which says what the MENU loads and nothing about the world.
    // Sampling after the first 300 textures puts the examples in area loading
    // instead, so the shapes describe the population the question is actually
    // about, and any future log answers it without a special run.
    if (dxt && big >= 512 && g_tfTexTotal > 300 &&
        InterlockedIncrement(&g_tfTexDetail) <= 8) {
        char who[160], l[256];
        DescribeAddr(ra, who);
        sprintf(l, "[texfilter] 1-level DXT %ux%u fmt=%d usage=0x%lX created by %s",
                Width, Height, (int)Format, (unsigned long)Usage, who);
        LogLine(l);
    }
}
