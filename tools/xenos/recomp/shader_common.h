#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// NFSMW: constants arrive through a dynamic UBO (a constant bank on Maxwell) instead of being read
// through a 64-bit pointer. High bit so as not to clash with the UNLEASHED_RECOMP ones.
#define SPEC_CONSTANT_CONSTANTES_UBO    (1 << 8)
// The tfetch offset is scaled with 1/size taken from the shared constants instead of querying the
// texture for its size. A size query is another texture unit operation, and the library has 160 of
// them for 435 samples.
#define SPEC_CONSTANT_INV_TAMANO_TEX    (1 << 9)
// Cheap PCF. The shaders that sample the shadow map use a 3x3 pattern at half-texel offsets: nine
// samples per pixel. In p_000101 (the smoke and wheel spray, which are full-screen rectangles) there
// are eleven, and that single draw is 21 % of the scene. With this bit the eight outer offsets are
// set to zero: the nine samples become identical, DXC merges them into one and the shadow goes from
// 3x3 filtered to a single texel. Edge smoothness is lost.
#define SPEC_CONSTANT_PCF_BARATO        (1 << 15)
/*
 * The alpha test function, specialized (bits 16-18).
 *
 * g_AlphaFunction used to come from the shared constants, that is, at run time, so alphaTestValue
 * was a 7-case switch: ~8 branches in every pixel shader that does alpha testing. In p_000131 (the
 * distant world, 45 % of the scene's fragments) that was 12 branches for 1 sample and 14 operations.
 * And it was not needed at all: the app already knows the function when it creates the pipeline (it
 * comes from RB_COLORCONTROL, which is already in the key), so with three more bits the compiler
 * keeps the one comparison that applies and drops the other six. The output is bit-identical.
 */
/*
 * The radial blur of the final composition.
 *
 * p_000139 is the race VisualTreatment: one full-screen quad, exactly 1280x720, no overdraw, with
 * twelve samples. It takes 2.8-3.2 ms real, 72-80 % of all post-processing.
 *
 * Of those twelve, seven are offset DIFFUSEMAP taps (plus one HEIGHTMAP tap that only feeds the
 * factor): the radial speed blur. They are blended with `r5*factor + r3`, where r3 is the center tap,
 * so with factor 0 the output is exactly the center tap and the eight samples are dead: DXC and the
 * driver remove them when specializing.
 *
 * The edge blur when accelerating and with NOS is lost. The image is sharper.
 */
#define SPEC_CONSTANT_SIN_DESENFOQUE    (1 << 19)
/*
 * The world shadow map as the minimum of two textures (nfsmw_nativo_sombra_minimo).
 *
 * The game resolves the same 1600x1600 map twice: without cars (sampled by the car body) and with the
 * cars drawn on top (sampled by the world). With this option the renderer draws the cars onto a
 * render target cleared to 1.0, without copying the world underneath, and the shaders that sample the
 * map with cars take the minimum of the two textures: with the game's LESS/LEQUAL depth test,
 * min(world, cars) is exactly what drawing the cars on top produces. The second texture arrives in the
 * 3D index word of the same register (the shadow map is 2D: that word is unused). With the bit off the
 * code is that of tfetch2DSombra.
 */
#define SPEC_CONSTANT_SOMBRA_MINIMO     (1 << 23)
/*
 * The app knows the library has tfetch2DSombraMin because this constant appears in the SPIR-V of the
 * shaders that use it (OpConstant) and nowhere else. The specialization constant never reaches this
 * value (bits 24-30).
 */
#define NFSMW_MARCA_SOMBRA_MINIMO       0x5E3B1A84u

// FH2: pixel shaders use all 256 float constants (c254 and c255 too); NFSMW stopped at 224.
#define FH2_CONSTANTES_PS         256
// FH2: where the textures of each fetch constant slot are in the shared constants. Pixel shader samplers (slots
// 0-15) keep NFSMW's layout, which the native renderer fills: the descriptor index of each texture dimension (2D, 3D,
// cube) at dim * 64 + slot * 4, the sampler's at 192 + slot * 4 and 1/size at 360 + slot * 8. Vertex shader samplers
// (slots 16-19: register + 16) are FH2's own, after NFS Carbon's high input remaps (488-511).
#define FH2_BYTES_TEXTURA(SLOT, DIM)  ((SLOT) < 16 ? (DIM) * 64 + (SLOT) * 4 : 512 + (DIM) * 16 + ((SLOT) - 16) * 4)
#define FH2_BYTES_SAMPLER(SLOT)       ((SLOT) < 16 ? 192 + (SLOT) * 4 : 560 + ((SLOT) - 16) * 4)
#define FH2_BYTES_INV_TAMANO(SLOT)    ((SLOT) < 16 ? 360 + (SLOT) * 8 : 576 + ((SLOT) - 16) * 8)
// FH2: the vertex fetches without a declared element (instance data, vertex fetch constants 88-95): per constant,
// 16 bytes: where its buffer starts in g_Fh2Buscas (in dwords), its size in dwords and its xenos::Endian.
#define FH2_COMPARTIDA_BUSCAS     640
// FH2: the 32 loop constants as the game set them (xenos: count in bits 0-7, start 8-15, step 16-23), for shaders
// without literal ones.
#define FH2_COMPARTIDA_BUCLES     768
// FH2: per declared vertex element (ordinal 0-31 in the container), as the D3D patched its fetch: word 0 the vertex fetch
// constant (bits 0-6), xenos::VertexFormat (8-13), flags as fh2BuscaVertices takes them (16-23) and the exponent
// adjust (24-29, signed); word 1 the stride in dwords (0-7) and the offset in dwords (8-31, signed).
#define FH2_COMPARTIDA_ELEMENTOS  896
#define FH2_COMPARTIDA_BYTES      1152
#define SPEC_CONSTANT_ALPHA_FUNC_SHIFT  16
#define SPEC_CONSTANT_ALPHA_FUNC_MASK   (7 << 16)

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#if !defined(__cplusplus) || defined(__INTELLISENSE__)

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

// NFSMW: the same blocks of the upload buffer, also as dynamic UBOs in set 4. With
// -fvk-use-dx-layout each float4 takes 16 contiguous bytes, so any 4-byte word of the shared block is
// a component: v[B / 16][(B % 16) / 4], and asuint reads it without changing a bit.
struct NfsmwBloqueVs { float4 v[256]; };
struct NfsmwBloquePs { float4 v[FH2_CONSTANTES_PS]; };
struct NfsmwBloqueCompartidas { float4 v[FH2_COMPARTIDA_BYTES / 16]; };  // FH2: was 23
[[vk::binding(0, 4)]] ConstantBuffer<NfsmwBloqueVs> g_UboVertex;
[[vk::binding(1, 4)]] ConstantBuffer<NfsmwBloquePs> g_UboPixel;
[[vk::binding(2, 4)]] ConstantBuffer<NfsmwBloqueCompartidas> g_UboCompartidas;
// FH2: constants only through the set 4 UBOs, as the native renderer reads them by default
// (nfsmw_nativo_constantes_ubo). Without the 64-bit pointer path the SPIR-V is ~40 % smaller and needs neither
// shaderInt64 nor buffer device address (Vulkan 1.1 drivers, the Moto G9 Play's).
#define NFSMW_UBO true
#define NFSMW_COMPARTIDA_UINT(B)  asuint(g_UboCompartidas.v[(B) / 16][((B) % 16) / 4])
#define NFSMW_COMPARTIDA_FLOAT(B) g_UboCompartidas.v[(B) / 16][((B) % 16) / 4]
#define FH2_BUCLE(ID) (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(FH2_COMPARTIDA_BUCLES + (ID) * 4) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + FH2_COMPARTIDA_BUCLES + (ID) * 4))

#define g_Booleans                 (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(256) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 256))
#define g_SwappedTexcoords         (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(260) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 260))
#define g_HalfPixelOffset          (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(264), NFSMW_COMPARTIDA_FLOAT(268)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 264))
#define g_AlphaThreshold           (NFSMW_UBO ? NFSMW_COMPARTIDA_FLOAT(272) : vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 272))
// NFSMW: alpha test function (RB_COLORCONTROL.alpha_func): 0 never, 1 <, 2 ==, 3 <=,
// 4 >, 5 !=, 6 >=, 7 always.
#define g_AlphaFunction            (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(276) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 276))
// NFSMW: position to host clip space, like ndc_scale/ndc_offset in the
// emulation (graphics/util/draw.cpp). (1, 1) and (0, 0) for normal draws;
// with Xenos clipping disabled it converts from pixels to NDC.
#define g_NdcScale                 (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(280), NFSMW_COMPARTIDA_FLOAT(284)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 280))
#define g_NdcOffset                (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT(288), NFSMW_COMPARTIDA_FLOAT(292)) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 288))
// NFSMW: where each component of the vertex input at that location comes from
// (D3D patches the fetch swizzle according to the declaration). 0xFFF = as is.
// FH2: locations 16-21 (its extra positions) at 488-511, as NFS Carbon's renderer writes them.
#define FH2_BYTES_REMAPEO(LOC)     ((LOC) < 16 ? 296 + (LOC) * 4 : 488 + ((LOC) - 16) * 4)
#define g_InputRemap(LOC)          (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT(FH2_BYTES_REMAPEO(LOC)) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + FH2_BYTES_REMAPEO(LOC)))

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;
// FH2: vertex inputs in k_10_11_11, which Vulkan has no format for (FH2 uses it for positions and texture
// coordinates). The renderer binds them as R32_SFLOAT, which only carries the 32 bits, and sets bit L (decode) and
// 16 + L (signed) for vertex input location L (0-15) in this constant (the pipeline key's word "relleno").
[[vk::constant_id(1)]] const uint g_Fh2Entradas1011 = 0;

#define g_SpecConstants() g_SpecConstants

// FH2: a vertex input that may come in k_10_11_11 (see g_Fh2Entradas1011): its 32 bits back
// and decoded as the SDK's translator does (x 11 bits, y 11, z 10 from the low bits; normalized, signed clamped to
// -1; the missing w is 0).
float4 fh2Entrada(float4 valor, uint ubicacion)
{
    if (((g_Fh2Entradas1011 >> ubicacion) & 1) == 0)
        return valor;
    const uint w = asuint(valor.x);
    float3 r;
    if (((g_Fh2Entradas1011 >> (16 + ubicacion)) & 1) != 0)
    {
        const int3 x = int3(int(w << 21) >> 21, int(w << 10) >> 21, int(w) >> 22);
        r = max(float3(x) / float3(1023.0, 1023.0, 511.0), -1.0);
    }
    else
    {
        r = float3(uint3(w & 0x7FFu, (w >> 11) & 0x7FFu, w >> 22)) / float3(2047.0, 2047.0, 1023.0);
    }
    return float4(r, 0.0);
}

// FH2: the buffers of the vertex fetch constants that the vertex shaders read without a declaration, copied by the
// renderer as they are in guest memory (the GPU's byte order is applied here, from the fetch constant).
[[vk::binding(3, 4)]] ByteAddressBuffer g_Fh2Buscas;

uint fh2PalabraBusca(uint constante, int palabra)
{
    const uint b = FH2_COMPARTIDA_BUSCAS + constante * 16;
    const uint inicio = NFSMW_COMPARTIDA_UINT(b), tamano = NFSMW_COMPARTIDA_UINT(b + 4),
               endian = NFSMW_COMPARTIDA_UINT(b + 8);
    if (palabra < 0 || uint(palabra) >= tamano)
        return 0;
    uint w = g_Fh2Buscas.Load((inicio + uint(palabra)) * 4);
    if (endian == 1)
        w = ((w & 0x00FF00FFu) << 8) | ((w >> 8) & 0x00FF00FFu);
    else if (endian == 2)
        w = (w << 24) | ((w & 0xFF00u) << 8) | ((w >> 8) & 0xFF00u) | (w >> 24);
    else if (endian == 3)
        w = (w << 16) | (w >> 16);
    return w;
}

float4 fh2BuscaVertices(uint constante, int direccion, uint formato, uint banderas, int ajusteExp);

// A declared vertex element read directly (fetches indexed by bone or instance indices): its constant and stride come
// from the element of the full fetch (base), its format, flags, offset and exponent adjust from its own (mio).
float4 fh2BuscaElemento(uint base, uint mio, int indice)
{
    const uint w0b = NFSMW_COMPARTIDA_UINT(FH2_COMPARTIDA_ELEMENTOS + base * 8);
    const uint w1b = NFSMW_COMPARTIDA_UINT(FH2_COMPARTIDA_ELEMENTOS + base * 8 + 4);
    const uint w0m = NFSMW_COMPARTIDA_UINT(FH2_COMPARTIDA_ELEMENTOS + mio * 8);
    const uint w1m = NFSMW_COMPARTIDA_UINT(FH2_COMPARTIDA_ELEMENTOS + mio * 8 + 4);
    const uint constante = w0b & 0x7Fu;
    if (constante < 88 || constante > 95)
        return 0.0;
    return fh2BuscaVertices(constante - 88, indice * int(w1b & 0xFFu) + (int(w1m) >> 8), (w0m >> 8) & 0x3Fu,
                            (w0m >> 16) & 0xFFu, int(w0m << 2) >> 26);
}

// One vertex fetch read directly, decoded like the SDK's translator (spirv_translator_fetch.cpp): packed components
// from the low bits, normalized (signed: clamped to -1, or without zero) unless integer, times 2^exp_adjust, the
// components the format lacks are 0. formato is a xenos::VertexFormat; banderas: 1 signed, 2 integer, 4 signed
// repeating fraction without zero.
float4 fh2BuscaVertices(uint constante, int direccion, uint formato, uint banderas, int ajusteExp)
{
    const bool conSigno = (banderas & 1) != 0, entero = (banderas & 2) != 0, sinCero = (banderas & 4) != 0;
    uint4 anchos = 0, desplazamientos = 0, palabras = 0;
    float4 resultado = 0.0;
    bool empaquetado = true;
    switch (formato)
    {
    case 6: anchos = uint4(8, 8, 8, 8); desplazamientos = uint4(0, 8, 16, 24); break;
    case 7: anchos = uint4(10, 10, 10, 2); desplazamientos = uint4(0, 10, 20, 30); break;
    case 16: anchos = uint4(11, 11, 10, 0); desplazamientos = uint4(0, 11, 22, 0); break;
    case 17: anchos = uint4(10, 11, 11, 0); desplazamientos = uint4(0, 10, 21, 0); break;
    case 25: anchos = uint4(16, 16, 0, 0); desplazamientos = uint4(0, 16, 0, 0); break;
    case 26: anchos = uint4(16, 16, 16, 16); desplazamientos = uint4(0, 16, 0, 16); palabras = uint4(0, 0, 1, 1); break;
    default: empaquetado = false; break;
    }
    if (empaquetado)
    {
        const uint w0 = fh2PalabraBusca(constante, direccion), w1 = fh2PalabraBusca(constante, direccion + 1);
        [unroll] for (uint i = 0; i < 4; i++)
        {
            if (anchos[i] == 0)
                continue;
            const uint w = palabras[i] ? w1 : w0;
            float v;
            if (conSigno)
            {
                const int x = int(w << (32 - desplazamientos[i] - anchos[i])) >> (32 - anchos[i]);
                v = float(x);
                if (!entero)
                {
                    const float escala = float((1u << (anchos[i] - 1)) - 1) + (sinCero ? 0.5 : 0.0);
                    v = sinCero ? (v + 0.5) / escala : max(v / escala, -1.0);
                }
            }
            else
            {
                v = float((w >> desplazamientos[i]) & ((1u << anchos[i]) - 1));
                if (!entero)
                    v /= float((1u << anchos[i]) - 1);
            }
            resultado[i] = v;
        }
    }
    else if (formato == 31 || formato == 32)
    {
        const uint w0 = fh2PalabraBusca(constante, direccion);
        resultado.xy = f16tof32(uint2(w0 & 0xFFFF, w0 >> 16));
        if (formato == 32)
        {
            const uint w1 = fh2PalabraBusca(constante, direccion + 1);
            resultado.zw = f16tof32(uint2(w1 & 0xFFFF, w1 >> 16));
        }
    }
    else
    {
        const uint n = (formato == 33 || formato == 36) ? 1 : (formato == 34 || formato == 37) ? 2 :
                       (formato == 57) ? 3 : 4;
        const bool flotante = formato == 36 || formato == 37 || formato == 38 || formato == 57;
        [unroll] for (uint i = 0; i < 4; i++)
        {
            if (i >= n)
                continue;
            const uint w = fh2PalabraBusca(constante, direccion + int(i));
            if (flotante)
                resultado[i] = asfloat(w);
            else if (conSigno)
                resultado[i] = entero ? float(int(w)) : max(float(int(w)) / 2147483647.0, -1.0);
            else
                resultado[i] = entero ? float(w) : float(w) / 4294967295.0;
        }
    }
    return resultado * exp2(float(ajusteExp));
}

#else

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c16.x); \
    uint g_SwappedTexcoords : packoffset(c16.y); \
    float2 g_HalfPixelOffset : packoffset(c16.z); \
    float g_AlphaThreshold : packoffset(c17.x); \
    uint g_AlphaFunction : packoffset(c17.y); \
    float2 g_NdcScale : packoffset(c17.z); \
    float2 g_NdcOffset : packoffset(c18.x);

uint g_SpecConstants();

#define g_InputRemap(LOC) 0xFFF

#endif

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

#ifdef FH2_SOMBREADOR_VERTICES
// FH2: in vertex shaders every sample below is at level 0 (no implicit level outside pixel shaders).
#define Sample(SAMPLER, COORD) SampleLevel(SAMPLER, COORD, 0)
#endif

// FH2: a 2D texture coordinate in texels (tfetch with unnormalized coordinates) to 0-1.
float2 fh2Normalizar(uint resourceDescriptorIndex, float2 texCoord, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TAMANO_TEX)
        return texCoord * invSize;
    return texCoord / float2(getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]));
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    // With the bit set, the size comes from a constant and the texture does not have to be queried. It
    // is the same computation with the same number: the renderer writes 1/size of the host image. With a
    // ternary, DXC evaluates both branches and the size query stays: an if with [branch] is needed for
    // the dead branch to disappear when the pipeline is specialized.
    float2 desplazamiento;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TAMANO_TEX)
        desplazamiento = offset * invSize;
    else
        desplazamiento = offset / getTexture2DDimensions(texture);
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + desplazamiento);
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

// FH2_RECOMP: the translator passes 1/size to every 2D fetch (NFSMW_RECOMP), getWeights2D included, which NFSMW
// never used and FH2's shadow filter does. Without SPEC_CONSTANT_INV_TAMANO_TEX the size comes from the texture.
float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    float2 tamano;
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_INV_TAMANO_TEX)
        tamano = 1.0 / invSize;
    else
        tamano = getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]);
    return select(isnan(texCoord), 0.0, frac(texCoord * tamano + offset - 0.5));
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

// Shadow map sampling. The translator emits tfetch2D for everything; the library rewrite step
// changes the calls whose sampler is SHADOWMAP_SAMPLER to this function, since those are the only
// ones meant to be made cheaper. With the bit set, all the 3x3 samples land on the same texel and
// the compiler keeps only one.
float4 tfetch2DSombra(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_PCF_BARATO)
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, float2(0.0, 0.0), invSize);
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
}

// The shadow map with the minimum of its pair (nfsmw_nativo_sombra_minimo). The library step changes
// all shadow map calls to this function and passes it the 3D index of that same register, which is
// where the app puts the pair: the world map (or the texture itself, which gives the same texel, while
// the app is checking). Both are sampled the same way (same point sampler, same coordinates and
// offsets), so the minimum is per texel. The first comparison is the library marker: it is never
// true and the driver removes it when specializing.
float4 tfetch2DSombraMin(uint resourceDescriptorIndex, uint parejaDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float2 invSize)
{
    [branch] if (g_SpecConstants() == NFSMW_MARCA_SOMBRA_MINIMO)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 valor = tfetch2DSombra(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize);
    [branch] if (g_SpecConstants() & SPEC_CONSTANT_SOMBRA_MINIMO)
        valor = min(valor, tfetch2DSombra(parejaDescriptorIndex, samplerDescriptorIndex, texCoord, offset, invSize));
    return valor;
}

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
    
    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h0y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h0y) / float2(dimensions))) +
        g1(fy) * (g0x * texture.Sample(samplerState, float2(px + h0x, py + h1y) / float2(dimensions)) +
            g1x * texture.Sample(samplerState, float2(px + h1x, py + h1y) / float2(dimensions)));

    return r;
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

// NFSMW: 3 bits per component: 0-3 = data component, 4 = 0, 5 = 1, 7 = unchanged.
float4 remapInput(float4 value, uint code)
{
    if (code == 0xFFF)
        return value;

    float4 result = value;
    for (uint i = 0; i < 4; i++)
    {
        uint source = (code >> (i * 3)) & 7;
        if (source < 4)
            result[i] = value[source];
        else if (source == 4)
            result[i] = 0.0;
        else if (source == 5)
            result[i] = 1.0;
    }
    return result;
}

// NFSMW: Xenos alpha test with its comparison function. Positive if the pixel passes.
float alphaTestValue(float alpha)
{
    bool pass = true;
    switch ((g_SpecConstants() & SPEC_CONSTANT_ALPHA_FUNC_MASK) >> SPEC_CONSTANT_ALPHA_FUNC_SHIFT)
    {
    case 0: pass = false; break;
    case 1: pass = alpha < g_AlphaThreshold; break;
    case 2: pass = alpha == g_AlphaThreshold; break;
    case 3: pass = alpha <= g_AlphaThreshold; break;
    case 4: pass = alpha > g_AlphaThreshold; break;
    case 5: pass = alpha != g_AlphaThreshold; break;
    case 6: pass = alpha >= g_AlphaThreshold; break;
    default: break;
    }
    return pass ? 1.0 : -1.0;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;
    
    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
