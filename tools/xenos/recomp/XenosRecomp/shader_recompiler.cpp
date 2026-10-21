#include "shader_recompiler.h"
#include "shader_common.h"
#include <cctype>
#include <stdexcept>

static constexpr char SWIZZLES[] = 
{ 
    'x',
    'y', 
    'z', 
    'w', 
    '0', 
    '1',
    '_',
    '_'
};

static constexpr const char* USAGE_TYPES[] =
{
    "float4", // POSITION
    "float4", // BLENDWEIGHT
#ifdef FH2_RECOMP
    "float4", // BLENDINDICES: the native renderer's FH2/NFS Carbon path gives them as floats (USCALED for bytes)
#else
    "uint4", // BLENDINDICES
#endif
#ifdef NFSMW_RECOMP
    "float4", // NORMAL: NFSMW stores them as 16-bit integers or as float
#else
    "uint4", // NORMAL
#endif
    "float4", // PSIZE
    "float4", // TEXCOORD
#ifdef NFSMW_RECOMP
    "float4", // TANGENT
    "float4", // BINORMAL
#else
    "uint4", // TANGENT
    "uint4", // BINORMAL
#endif
    "float4", // TESSFACTOR
    "float4", // POSITIONT
    "float4", // COLOR
    "float4", // FOG
    "float4", // DEPTH
    "float4", // SAMPLE
};

static constexpr const char* USAGE_VARIABLES[] =
{
    "Position",
    "BlendWeight",
    "BlendIndices",
    "Normal",
    "PointSize",
    "TexCoord",
    "Tangent",
    "Binormal",
    "TessFactor",
    "PositionT",
    "Color",
    "Fog",
    "Depth",
    "Sample"
};

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "NORMAL",
    "PSIZE",
    "TEXCOORD",
    "TANGENT",
    "BINORMAL",
    "TESSFACTOR",
    "POSITIONT",
    "COLOR",
    "FOG",
    "DEPTH",
    "SAMPLE"
};

struct DeclUsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

// NOTE: These are specialized Vulkan locations for Unleashed Recompiled. Change as necessary. Likely not going to work with other games.
static constexpr DeclUsageLocation USAGE_LOCATIONS[] =
{
    { DeclUsage::Position, 0, 0 },
    { DeclUsage::Normal, 0, 1 },
    { DeclUsage::Tangent, 0, 2 },
    { DeclUsage::Binormal, 0, 3 },
    { DeclUsage::TexCoord, 0, 4 },
    { DeclUsage::TexCoord, 1, 5 },
    { DeclUsage::TexCoord, 2, 6 },
    { DeclUsage::TexCoord, 3, 7 },
    { DeclUsage::Color, 0, 8 },
    { DeclUsage::BlendIndices, 0, 9 },
    { DeclUsage::BlendWeight, 0, 10 },
#ifdef FH2_RECOMP
    // FH2's vertex declarations use NORMAL1 (34 shaders), never COLOR1, which also has location 11.
    { DeclUsage::Normal, 1, 11 },
#endif
    { DeclUsage::Color, 1, 11 },
    { DeclUsage::TexCoord, 4, 12 },
    { DeclUsage::TexCoord, 5, 13 },
    { DeclUsage::TexCoord, 6, 14 },
    { DeclUsage::TexCoord, 7, 15 },
    { DeclUsage::Position, 1, 15 },
#ifdef FH2_RECOMP
    // FH2's executable has vertex shaders with POSITION1-4 in several streams (as NFS Carbon's NFSC_RECOMP does).
    { DeclUsage::Position, 2, 16 },
    { DeclUsage::Position, 3, 17 },
    { DeclUsage::Position, 4, 18 },
    { DeclUsage::Position, 5, 19 },
    { DeclUsage::Position, 6, 20 },
#endif
};

static constexpr std::pair<DeclUsage, size_t> INTERPOLATORS[] =
{
    { DeclUsage::TexCoord, 0 },
    { DeclUsage::TexCoord, 1 },
    { DeclUsage::TexCoord, 2 },
    { DeclUsage::TexCoord, 3 },
    { DeclUsage::TexCoord, 4 },
    { DeclUsage::TexCoord, 5 },
    { DeclUsage::TexCoord, 6 },
    { DeclUsage::TexCoord, 7 },
    { DeclUsage::TexCoord, 8 },
    { DeclUsage::TexCoord, 9 },
    { DeclUsage::TexCoord, 10 },
    { DeclUsage::TexCoord, 11 },
    { DeclUsage::TexCoord, 12 },
    { DeclUsage::TexCoord, 13 },
    { DeclUsage::TexCoord, 14 },
    { DeclUsage::TexCoord, 15 },
    { DeclUsage::Color, 0 }
    , { DeclUsage::Color, 1 }
#ifdef FH2_RECOMP
    // FH2's shaders also pass NORMAL0, TANGENT0, BINORMAL0 and POSITION1, never COLOR2..5.
    , { DeclUsage::Normal, 0 }
    , { DeclUsage::Tangent, 0 }
    , { DeclUsage::Binormal, 0 }
    , { DeclUsage::Position, 1 }
#endif
#if defined(NFSMW_RECOMP) && !defined(FH2_RECOMP)
    // NFSMW declares and consumes COLOR2..5 in the original 2005 shaders.
    , { DeclUsage::Color, 2 }
    , { DeclUsage::Color, 3 }
    , { DeclUsage::Color, 4 }
    , { DeclUsage::Color, 5 }
#endif
};

static constexpr std::string_view TEXTURE_DIMENSIONS[] = 
{
    "2D",
    "3D", 
    "Cube" 
};

static FetchDestinationSwizzle getDestSwizzle(uint32_t dstSwizzle, uint32_t index)
{
    return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
}

void ShaderRecompiler::printDstSwizzle(uint32_t dstSwizzle, bool operand)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
            out += SWIZZLES[operand ? uint32_t(swizzle) : i];
    }
}

void ShaderRecompiler::printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle == FetchDestinationSwizzle::Zero)
        {
            indent();
            println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
        }
        else if (swizzle == FetchDestinationSwizzle::One)
        {
            indent();
            println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
        }
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
    if (instr.isPredicated)
        abrirPredicado(instr.predicateCondition);
    else
        cerrarPredicado();
    const size_t marcaPredicado = out.size();

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    auto findResult = vertexElements.find(address);
#ifdef FH2_RECOMP
    // A declared fetch indexed by something else than the vertex index (another register, or r0 once that component
    // was overwritten: bone indices, instance indices) reads its buffer directly with the parameters the D3D patched
    // in (fh2BuscaElemento: the renderer writes them per element from the patched microcode).
    bool fh2Manual;
    if (instr.isMiniFetch)
        fh2Manual = fh2UltimaBuscaManual;
    else
    {
        fh2Manual = findResult != vertexElements.end() &&
            (instr.srcRegister != 0 || ((fh2R0Escrito >> (instr.srcSwizzle & 3)) & 1) != 0);
        fh2UltimaBuscaManual = fh2Manual;
    }
    if (instr.dstRegister == 0)
    {
        for (uint32_t c = 0; c < 4; c++)
            if (((instr.dstSwizzle >> (c * 3)) & 7) != 7)
                fh2R0Escrito |= 1u << c;
    }
    if (findResult != vertexElements.end() && fh2Manual)
    {
        const std::string destino = out.substr(marcaPredicado);
        out.resize(marcaPredicado);
        const uint32_t ordinal = fh2OrdinalElemento.count(address) ? fh2OrdinalElemento[address] : 0;
        if (!instr.isMiniFetch)
        {
            fh2UltimoElementoCompleto = int32_t(ordinal);
            indent();
            println("fh2BuscaIndice = int(floor(r{}.{}{}));", instr.srcRegister, SWIZZLES[instr.srcSwizzle & 3],
                instr.isIndexRounded ? " + 0.5" : "");
        }
        // The components the container's fetch writes, filled through the swizzle the D3D patched in for this
        // element (fh2SwizzleElemento): the container's own swizzle does not know the declaration, and for D3DCOLOR
        // elements (red and blue swapped) or reordered halves it gave the wrong components (grass tufts flat and
        // brown, their sizes swapped).
        std::string mascara;
        for (size_t i = 0; i < 4; i++)
            if (getDestSwizzle(instr.dstSwizzle, i) != FetchDestinationSwizzle::Keep)
                mascara += SWIZZLES[i];
        if (!mascara.empty())
        {
            indent();
            println("r{}.{} = fh2SwizzleElemento(fh2BuscaElemento({}, {}, fh2BuscaIndice), {}).{};", instr.dstRegister,
                mascara, fh2UltimoElementoCompleto < 0 ? ordinal : uint32_t(fh2UltimoElementoCompleto), ordinal,
                ordinal, mascara);
        }
        (void)destino;
        cerrarSiEscribePredicado(marcaPredicado);
        return;
    }
    if (findResult == vertexElements.end())
    {
        // FH2: a fetch the vertex declaration does not describe reads its buffer directly (shader_common.h,
        // fh2BuscaVertices), as the GPU does: a full fetch computes the address from its fetch constant and index
        // register, a mini fetch reuses the last one.
        // The destination ("rN.xyz = ") is already written: the address lines go before it.
        const std::string destino = out.substr(marcaPredicado);
        out.resize(marcaPredicado);
        if (!instr.isMiniFetch)
        {
            const uint32_t constante = instr.constIndex * 3 + instr.constIndexSelect;
            if (constante < 88 || constante > 95)
                throw std::runtime_error("FETCH de vertices sin elemento declarado fuera de las constantes 88-95");
            fh2BuscasManuales |= 1u << (constante - 88);
            indent();
            println("fh2BuscaConstante = {};", constante - 88);
            indent();
            println("fh2BuscaDireccion = int(floor(r{}.{}{})) * {};", instr.srcRegister,
                SWIZZLES[instr.srcSwizzle & 3], instr.isIndexRounded ? " + 0.5" : "", uint32_t(instr.stride));
        }
        out += destino;
        println("fh2BuscaVertices(fh2BuscaConstante, fh2BuscaDireccion + {}, {}, {}, {}).{};",
            int32_t(instr.offset), uint32_t(instr.format),
            (instr.formatCompAll ? 1u : 0u) | (instr.numFormatAll ? 2u : 0u) | (instr.signedRfModeAll ? 4u : 0u),
            int32_t(instr.expAdjust), [&]
            {
                std::string swizzle;
                std::swap(swizzle, out);
                printDstSwizzle(instr.dstSwizzle, true);
                std::swap(swizzle, out);
                return swizzle;
            }());
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        cerrarSiEscribePredicado(marcaPredicado);
        return;
    }
#else
    if (findResult == vertexElements.end())
        throw std::runtime_error("FETCH de vertices sin elemento declarado");
#endif

#ifdef NFSMW_RECOMP
    // NFSMW: D3D patches the fetch swizzle according to the vertex declaration; the
    // native renderer passes the mapping through g_InputRemap.
    int32_t remapLocation = -1;
    for (auto& usageLocation : USAGE_LOCATIONS)
    {
        if (usageLocation.usage == findResult->second.usage && usageLocation.usageIndex == findResult->second.usageIndex)
        {
            remapLocation = int32_t(usageLocation.location);
            break;
        }
    }
    if (remapLocation >= 0)
        out += "remapInput(float4(";
#endif

    switch (findResult->second.usage)
    {
    // NFSMW declares normals, tangents and binormals as float4 (USAGE_TYPES): the
    // native renderer supplies them already normalized through the Vulkan format.
#ifndef NFSMW_RECOMP
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        specConstantsMask |= SPEC_CONSTANT_R11G11B10_NORMAL;
        print("tfetchR11G11B10(");
        break;
#endif

    case DeclUsage::TexCoord:
        print("tfetchTexcoord(g_SwappedTexcoords, ");
        break;
    }

#ifdef FH2_RECOMP
    // FH2: every float input at locations 0-15 goes through fh2Entrada (shader_common.h): it may come in k_10_11_11.
    {
        if (remapLocation >= 0 && remapLocation < 16)
            print("fh2Entrada(i{}{}, {})", USAGE_VARIABLES[uint32_t(findResult->second.usage)],
                uint32_t(findResult->second.usageIndex), remapLocation);
        else
            print("i{}{}", USAGE_VARIABLES[uint32_t(findResult->second.usage)], uint32_t(findResult->second.usageIndex));
    }
#else
    print("i{}{}", USAGE_VARIABLES[uint32_t(findResult->second.usage)], uint32_t(findResult->second.usageIndex));
#endif

    switch (findResult->second.usage)
    {
#ifndef NFSMW_RECOMP
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
        out += ')';
        break;
#endif

    case DeclUsage::TexCoord:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }

#ifdef NFSMW_RECOMP
    if (remapLocation >= 0)
        print("), g_InputRemap({}))", remapLocation);
#endif

    out += '.';
    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    cerrarSiEscribePredicado(marcaPredicado);
}

void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, bool bicubic)
{
#ifdef FH2_RECOMP
    // FH2: getGradients too (its vector UI fonts compute the antialiasing width from screen derivatives).
    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights &&
        instr.opcode != FetchOpcode::GetTextureGradients)
        return;
#else
    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)
        return;
#endif

    if (instr.isPredicated)
        abrirPredicado(instr.predCondition);
    else
        cerrarPredicado();
    const size_t marcaPredicado = out.size();

    auto printSrcRegister = [&](size_t componentCount)
        {
            print("r{}.", instr.srcRegister);

            for (size_t i = 0; i < componentCount; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
        };

#ifdef FH2_RECOMP
    if (instr.opcode == FetchOpcode::GetTextureGradients)
    {
        // As Xenia (spirv_translator_fetch.cpp): (ddx(src.x), ddy(src.x), ddx(src.y), ddy(src.y)), coarse derivatives;
        // no texture is read. Zero in a vertex shader.
        indent();
        print("r{}.", instr.dstRegister);
        printDstSwizzle(instr.dstSwizzle, false);
        out += " = fh2Gradientes(";
        printSrcRegister(2);
        out += ").";
        printDstSwizzle(instr.dstSwizzle, true);
        out += ";\n";
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        cerrarSiEscribePredicado(marcaPredicado);
        return;
    }
#endif

    std::string constName;
    const char* constNamePtr = nullptr;
#ifdef UNLEASHED_RECOMP
    bool subtractFromOne = false;
#endif

#ifdef FH2_RECOMP
    // FH2: a vertex shader's samplers are keyed by slot 16-19; its microcode names them 0-3 (patched to 16-19 by D3D
    // at run time) or already 16-19.
    auto findResult = samplers.find(instr.constIndex);
    if (findResult == samplers.end() && !isPixelShader && instr.constIndex < 16)
        findResult = samplers.find(instr.constIndex + 16);
#else
    auto findResult = samplers.find(instr.constIndex);
#endif
    if (findResult != samplers.end())
    {
        constNamePtr = findResult->second;

    #ifdef UNLEASHED_RECOMP
        subtractFromOne = hasMtxPrevInvViewProjection && strcmp(constNamePtr, "sampZBuffer") == 0;
    #endif
    }
    else
    {
        constName = fmt::format("s{}", instr.constIndex);
        constNamePtr = constName.c_str();
    }

#ifdef UNLEASHED_RECOMP
    if (instr.constIndex == 0 && instr.dimension == TextureDimension::Texture2D)
    {
        indent();
        print("pixelCoord = getPixelCoord({}_Texture2DDescriptorIndex, ", constNamePtr);
        printSrcRegister(2);
        out += ");\n";
    }
#endif

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";
    switch (instr.opcode)
    {
    case FetchOpcode::TextureFetch:
    {
    #ifdef UNLEASHED_RECOMP
        if (subtractFromOne)
            out += "1.0 - ";
    #endif

    #ifdef FH2_RECOMP
        out += "(tfetch";  // times 2^exp_adjust (closed below)
    #else
        out += "tfetch";
    #endif
        break;
    }
    case FetchOpcode::GetTextureWeights:
    {
        out += "getWeights";
        break;
    }
    }

    std::string_view dimension;
    uint32_t componentCount = 0;

    switch (instr.dimension)
    {
    case TextureDimension::Texture1D:
        dimension = "1D";
        componentCount = 1;
        break;
    case TextureDimension::Texture2D:
        dimension = "2D";
        componentCount = 2;
        break;
    case TextureDimension::Texture3D:
        dimension = "3D";
        componentCount = 3;
        break;
    case TextureDimension::TextureCube:
        dimension = "Cube";
        componentCount = 3;
        break;
    }

    out += dimension;

#ifdef UNLEASHED_RECOMP
    if (bicubic)
        out += "Bicubic";
#endif

    print("({0}_Texture{1}DescriptorIndex, {0}_SamplerDescriptorIndex, ", constNamePtr, dimension);
#ifdef FH2_RECOMP
    // Unnormalized coordinates (texels): normalized here with the texture's size (FH2's blur passes use them).
    if (instr.texCoordDenorm && instr.dimension == TextureDimension::Texture2D)
    {
        print("fh2Normalizar({0}_Texture2DDescriptorIndex, ", constNamePtr);
        printSrcRegister(componentCount);
        print(", {}_InvTamano)", constNamePtr);
    }
    else
#endif
    printSrcRegister(componentCount);

    switch (instr.dimension)
    {
    case TextureDimension::Texture2D:
        // The last argument is 1/size of the slot, so the texture does not have to be queried.
        print(", float2({}, {}), {}_InvTamano", instr.offsetX * 0.5f, instr.offsetY * 0.5f, constNamePtr);
        break;
    case TextureDimension::TextureCube:
        out += ", cubeMapData";
        break;
    }

#ifdef FH2_RECOMP
    if (instr.opcode == FetchOpcode::TextureFetch)
        print(") * {}_ExpAjuste).", constNamePtr);
    else
#endif
    out += ").";

    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    cerrarSiEscribePredicado(marcaPredicado);
}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
    if (instr.isPredicated)
        abrirPredicado(instr.predicateCondition);
    else
        cerrarPredicado();
    const size_t marcaPredicado = out.size();

    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    auto op = [&](size_t operand)
        {
            size_t reg = 0;
            size_t swizzle = 0;
            bool select = true;
            bool negate = false;
            bool abs = false;

            switch (operand)
            {
            case SCALAR_CONSTANT_0:
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = false;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            case SCALAR_CONSTANT_1:
                reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                swizzle = instr.src3Swizzle;
                select = true;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            default:
                switch (operand)
                {
                case VECTOR_0:
                    reg = instr.src1Register;
                    swizzle = instr.src1Swizzle;
                    select = instr.src1Select;
                    negate = instr.src1Negate;
                    break;
                case VECTOR_1:
                    reg = instr.src2Register;
                    swizzle = instr.src2Swizzle;
                    select = instr.src2Select;
                    negate = instr.src2Negate;
                    break;
                case VECTOR_2:
                case SCALAR_0:
                case SCALAR_1:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = instr.src3Select;
                    negate = instr.src3Negate;
                    break;
                }

                if (select)
                {
                    abs = (reg & 0x80) != 0;
                    reg &= 0x3F;
                }
                else
                {
                    abs = instr.absConstants;
                }

                break;
            }

            std::string regFormatted;

            if (select)
            {
                regFormatted = fmt::format("r{}", reg);
            }
            else
            {
                // The bits correspond to the first constant and the following ones,
                // not to the whole instruction. Temporary operands do not count.
                const bool relativa = operand == VECTOR_0 ? instr.const0Relative :
                    operand == VECTOR_1 ? (instr.src1Select ? instr.const0Relative : instr.const1Relative) :
                    (instr.src1Select && instr.src2Select ? instr.const0Relative : instr.const1Relative);
                auto findResult = float4Constants.find(reg);
                if (findResult != float4Constants.end())
                {
                    const char* constantName = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                    if (findResult->second->registerCount > 1)
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (hasMtxProjection && strcmp(constantName, "g_MtxProjection") == 0)
                        {
                            regFormatted = fmt::format("(iterationIndex == 0 ? mtxProjectionReverseZ[{0}] : mtxProjection[{0}])",
                                reg - findResult->second->registerIndex);
                        }
                        else
                    #endif
                        {
                            regFormatted = fmt::format("{}({}{})", constantName,
                                reg - findResult->second->registerIndex, relativa ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                        }
                    }
                    else
                    {
                        if (relativa)
                            throw std::runtime_error("direccionamiento relativo de constante escalar pendiente");
                        regFormatted = constantName;
                    }
                }
                else
                {
                    if (relativa)
                        throw std::runtime_error("direccionamiento relativo sin constante declarada pendiente");
                #ifdef FH2_RECOMP
                    // FH2: a register without a name nor a literal value is read from the constants the game set.
                    if (!fh2ConstantesLiterales.count(reg))
                    {
                        const char* nombreShader = isPixelShader ? "Pixel" : "Vertex";
                        regFormatted = fmt::format("(NFSMW_UBO ? g_Ubo{}.v[{}] : vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + {}, 0x10))",
                            nombreShader, reg, nombreShader, reg * 16);
                    }
                    else
                #endif
                    regFormatted = fmt::format("c{}", reg);
                }
            }

            std::string result;

            if (negate)
                result += '-';

            if (abs)
                result += "abs(";

            result += regFormatted;
            result += '.';

            switch (operand)
            {
            case VECTOR_0:
            case VECTOR_1:
            case VECTOR_2:
            {
                uint32_t mask;

                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Dp2Add:
                    mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                    break;

                case AluVectorOpcode::Dp3:
                    mask = 0b111;
                    break;

                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Max4:
                    mask = 0b1111;
                    break;

                default:
                    mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                    break;
                }

                for (size_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 0x1)
                        result += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                }

                break;
            }

            case SCALAR_0:
            case SCALAR_CONSTANT_0:
                result += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                break;

            case SCALAR_1:
            case SCALAR_CONSTANT_1:
                result += SWIZZLES[swizzle & 0x3];
                break;
            }

            if (abs)
                result += ")";

            return result;
        };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::KillEq:
        indent();
        println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGt:
        indent();
        println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillGe:
        indent();
        println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    
    case AluVectorOpcode::KillNe:
        indent();
        println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0), op(VECTOR_1));
        break;
    }

    bool closeIfBracket = false;

    std::string_view exportRegister;
    if (instr.exportData)
    {
        if (isPixelShader)
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
                exportRegister = "oC0";
                break;        
            case ExportRegister::PSColor1:
                exportRegister = "oC1";
                break;        
            case ExportRegister::PSColor2:
                exportRegister = "oC2";
                break;            
            case ExportRegister::PSColor3:
                exportRegister = "oC3";
                break;           
            case ExportRegister::PSDepth:
                exportRegister = "oDepth";
                break;
            }
        }
        else
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::VSPosition:
                exportRegister = "oPos";

            #ifdef UNLEASHED_RECOMP
                if (hasMtxProjection)
                {
                    indent();
                    out += "if ((g_SpecConstants() & SPEC_CONSTANT_REVERSE_Z) == 0 || iterationIndex == 0)\n";
                    indent();
                    out += "{\n";
                    ++indentation;

                    closeIfBracket = true;
                }
            #endif

                break;

            default:
            {
                auto findResult = interpolators.find(instr.vectorDest);
                if (findResult == interpolators.end())
                    throw std::runtime_error("exportacion de vertices sin interpolador declarado");
                exportRegister = findResult->second;
                break;
            }
            }
        }
    }

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        indent();
        print("p0 = {} == 0.0 && {} ", op(VECTOR_0), op(VECTOR_1));

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::SetpEqPush:
            out += "==";
            break;
        case AluVectorOpcode::SetpNePush:
            out += "!=";
            break;
        case AluVectorOpcode::SetpGtPush:
            out += ">";
            break;
        case AluVectorOpcode::SetpGePush:
            out += ">=";
            break;
        }

        out += " 0.0;\n";
    }
    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
        println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0));
    }

#ifdef FH2_RECOMP
    // r0 (the vertex index of a vertex shader) overwritten: later fetches indexed by it read their buffer directly.
    if (!instr.exportData)
    {
        if (instr.vectorDest == 0)
            fh2R0Escrito |= instr.vectorWriteMask;
        if (instr.scalarDest == 0)
            fh2R0Escrito |= instr.scalarWriteMask;
    }
#endif
    uint32_t vectorWriteMask = instr.vectorWriteMask;
    if (instr.exportData)
        vectorWriteMask &= ~instr.scalarWriteMask;

    if (vectorWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.vectorDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ";

        if (instr.vectorSaturate)
            out += "saturate(";

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Add:
            print("{} + {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Mul:
            print("{} * {}", op(VECTOR_0), op(VECTOR_1));
            break;

        /*
         * The Xenos MOV is a max with itself.
         *
         * The Xenos has no copy instruction: the assembler writes it as MAX dst, src, src
         * (and MAXs for the scalar channel). The translator emitted it as is, so the library
         * had 452 `max(a, a)` in the pixel shaders and 482 in the vertex shaders: almost a
         * thousand operations that are a copy. max(a, a) == a exactly (also with NaN and with
         * both zeros, which is the only thing that sets IEEE min/max apart), so the operand is
         * emitted directly.
         *
         * The `a0 = ...` of MaxA/MaxAs/MaxAsf is not touched: it is handled separately and
         * stays the same.
         */
        case AluVectorOpcode::Max:
        case AluVectorOpcode::MaxA:
        case AluVectorOpcode::Min:
        {
            std::string a = op(VECTOR_0);
            std::string b = op(VECTOR_1);
            if (a == b)
                out += a;
            else
                print("{}({}, {})", instr.vectorOpcode == AluVectorOpcode::Min ? "min" : "max", a, b);
            break;
        }

        case AluVectorOpcode::Seq:
            print("{} == {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sgt:
            print("{} > {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sge:
            print("{} >= {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Sne:
            print("{} != {}", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Frc:
            print("frac({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Trunc:
            print("trunc({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Floor:
            print("floor({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::Mad:
            print("{} * {} + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndEq:
            print("select({} == 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGe:
            print("select({} >= 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::CndGt:
            print("select({} > 0.0, {}, {})", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Dp3:
            print("dot({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dp2Add:
            print("dot({}, {}) + {}", op(VECTOR_0), op(VECTOR_1), op(VECTOR_2));
            break;

        case AluVectorOpcode::Cube:
        #ifdef FH2_RECOMP
            // The direction is operand 0 read as (z, _, x, y), with its swizzle and modifiers, as the emulated
            // translator does (spirv_translator_alu.cpp, kCube). Unleashed's shaders always pass v.zzxy, so taking
            // the register's xyz worked there; FH2's car paint passes r6.xxzy (the direction is r6.zyx), and about
            // 240 of its cube instructions use other swizzles: with xyz they reflected the wrong side (the sky and
            // the sun where the trees are), and the paint came out washed out.
            print("cube(({}).zwxx, cubeMapData)", op(VECTOR_0));
        #else
            print("cube(r{}, cubeMapData)", instr.src1Register);
        #endif
            break;

        case AluVectorOpcode::Max4:
            print("max4({})", op(VECTOR_0));
            break;

        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
            print("p0 ? 0.0 : {} + 1.0", op(VECTOR_0));
            break;

        case AluVectorOpcode::KillEq:
            print("any({} == {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGt:
            print("any({} > {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillGe:
            print("any({} >= {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::KillNe:
            print("any({} != {})", op(VECTOR_0), op(VECTOR_1));
            break;

        case AluVectorOpcode::Dst:
            print("dst({}, {})", op(VECTOR_0), op(VECTOR_1));
            break;
        }

        if (instr.vectorSaturate)
            out += ')';

        out += ";\n";
    }

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            indent();
            out += "p0 = ";

            switch (instr.scalarOpcode)
            {
            case AluScalarOpcode::SetpEq:
                print("{} == 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpNe:
                print("{} != 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGt:
                print("{} > 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpGe:
                print("{} >= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpInv:
                print("{} == 1.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpPop:
                print("{} - 1.0 <= 0.0", op(SCALAR_0));
                break;

            case AluScalarOpcode::SetpClr:
                out += "false";
                break;

            case AluScalarOpcode::SetpRstr:
                print("{} == 0.0", op(SCALAR_0));
                break;
            }

            out += ";\n";
        }

        indent();
        out += "ps = ";
        if (instr.scalarSaturate)
            out += "saturate(";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::Adds:
            print("{} + {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::AddsPrev:
            print("{} + ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::Muls:
            print("{} * {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::MulsPrev:
        case AluScalarOpcode::MulsPrev2:
            print("{} * ps", op(SCALAR_0));
            break;

        // Same as above, for the scalar channel: MAXs dst, src, src is the Xenos MOV.
        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
        case AluScalarOpcode::Mins:
        {
            std::string a = op(SCALAR_0);
            std::string b = op(SCALAR_1);
            if (a == b)
                out += a;
            else
                print("{}({}, {})", instr.scalarOpcode == AluScalarOpcode::Mins ? "min" : "max", a, b);
            break;
        }

        case AluScalarOpcode::Seqs:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sgts:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sges:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Snes:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Frcs:
            print("frac({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Truncs:
            print("trunc({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Floors:
            print("floor({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Exp:
            print("exp2({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
            print("clamp(log2({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcpf:
        case AluScalarOpcode::Rcp:
            print("clamp(rcp({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsqf:
        case AluScalarOpcode::Rsq:
            print("clamp(rsqrt({}), FLT_MIN, FLT_MAX)", op(SCALAR_0));
            break;

        case AluScalarOpcode::Subs:
            print("{} - {}", op(SCALAR_0), op(SCALAR_1));
            break;

        case AluScalarOpcode::SubsPrev:
            print("{} - ps", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out += "p0 ? 0.0 : 1.0";
            break;

        case AluScalarOpcode::SetpInv:
#ifdef NFSMW_RECOMP
            // Xenos: with src == 1 the counter goes back to 0 (Xenia's ucode.h, kSetpInv). Without that
            // case the counter stayed at 1 and later p0 blocks did not run.
            print("p0 ? 0.0 : ({0} == 0.0 ? 1.0 : {0})", op(SCALAR_0));
#else
            print("{0} == 0.0 ? 1.0 : {0}", op(SCALAR_0));
#endif
            break;

        case AluScalarOpcode::SetpPop:
            print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0));
            break;

        case AluScalarOpcode::SetpClr:
            out += "FLT_MAX";
            break;

        case AluScalarOpcode::SetpRstr:
            print("p0 ? 0.0 : {}", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsEq:
            print("{} == 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGt:
            print("{} > 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsGe:
            print("{} >= 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsNe:
            print("{} != 0.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::KillsOne:
            print("{} == 1.0", op(SCALAR_0));
            break;

        case AluScalarOpcode::Sqrt:
            print("sqrt({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            print("{} * {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            print("{} + {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            print("{} - {}", op(SCALAR_CONSTANT_0), op(SCALAR_CONSTANT_1));
            break;

        case AluScalarOpcode::Sin:
            print("sin({})", op(SCALAR_0));
            break;

        case AluScalarOpcode::Cos:
            print("cos({})", op(SCALAR_0));
            break;
        }

        if (instr.scalarSaturate)
            out += ')';

        out += ";\n";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::MaxAs:
            indent();
            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0));
            break;     
        case AluScalarOpcode::MaxAsf:
            indent();
            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0));
            break;
        }
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;

    if (scalarWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            out += '.';
        }
        else
        {
            print("r{}.", instr.scalarDest);
        }

        for (size_t i = 0; i < 4; i++)
        {
            if ((scalarWriteMask >> i) & 0x1)
                out += SWIZZLES[i];
        }

        out += " = ps;\n";
    }

    if (instr.exportData)
    {
        uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;

        for (size_t i = 0; i < 4; i++)
        {
            uint32_t mask = 1 << i;
            if (zeroMask & mask)
            {
                indent();
                println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
            }
            else if (oneMask & mask)
            {
                indent();
                println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
            }
        }
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        indent();
        out += "clip(ps != 0.0 ? -1 : 1);\n";
    }

    if (closeIfBracket)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    cerrarSiEscribePredicado(marcaPredicado);
}

#ifdef FH2_RECOMP
// FH2 samples 1D textures (as NFS Carbon does, NFSC_RECOMP in nfsmw-android): they are bound as 2D images of height
// 1, so a 1D fetch is a 2D one at the middle row.
static void fh2Textura1D(std::string& out, const std::string& nombre)
{
    out += fmt::format("#define {0}_Texture1DDescriptorIndex {0}_Texture2DDescriptorIndex\n", nombre);
    out += "#ifndef tfetch1D\n"
           "#define tfetch1D(RES, SAMP, COORD) tfetch2D(RES, SAMP, float2(COORD, 0.5), float2(0.0, 0.0), float2(0.0, 0.0))\n"
           "#endif\n";
}
#endif

// The iteration count of loop constant id: its literal (i<id>.x) or, without one, the value the game set (FH2: the
// shared constants' loop area, FH2_COMPARTIDA_BUCLES; the count is the low byte of the Xenos loop constant).
std::string ShaderRecompiler::fh2Bucle(uint32_t id)
{
#ifdef FH2_RECOMP
    if (!fh2BuclesLiterales.count(id))
        return fmt::format("int(FH2_BUCLE({}) & 0xFF)", id);
#endif
    return fmt::format("i{}.x", id);
}

void ShaderRecompiler::recompile(const uint8_t* shaderData, const std::string_view& include)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);

    /*
     * Most Wanted (2005) has 0x102A0E00; Unleashed (2008), 0x102A1100.
     * Both are accepted: only the two high bytes are compared.
     */
    assert((shaderContainer->flags & 0xFFFF0000) == 0x102A0000);
#ifndef FH2_RECOMP
    assert(shaderContainer->constantTableOffset != NULL);
#endif

#ifdef FH2_RECOMP
    // FH2's vertex shaders sample textures: shader_common.h samples at level 0 there (Vulkan allows the implicit
    // level only in pixel and compute shaders).
    if ((shaderContainer->flags & 0x1) != 0)
        out += "#define FH2_SOMBREADOR_VERTICES\n";
#endif
    out += include;
    out += '\n';

    isPixelShader = (shaderContainer->flags & 0x1) == 0;

#ifdef FH2_RECOMP
    // FH2's executable has containers without a constant table (the D3D library's own shaders): an empty one.
    static const ConstantTableContainer tablaVacia{};
    const auto constantTableContainer = shaderContainer->constantTableOffset
        ? reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset)
        : &tablaVacia;
#else
    const auto constantTableContainer = reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset);
#endif
    constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);

    out += "#ifdef __spirv__\n\n";

#ifdef UNLEASHED_RECOMP
    bool isMetaInstancer = false;
    bool hasIndexCount = false;
#endif

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

    #ifdef UNLEASHED_RECOMP
        if (!isPixelShader)
        {
            if (strcmp(constantName, "g_MtxProjection") == 0)
                hasMtxProjection = true;
            else if (strcmp(constantName, "g_InstanceTypes") == 0)
                isMetaInstancer = true;
            else if (strcmp(constantName, "g_IndexCount") == 0)
                hasIndexCount = true;
        }
        else
        {
            if (strcmp(constantName, "g_MtxPrevInvViewProjection") == 0)
                hasMtxPrevInvViewProjection = true;
        }
    #endif

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? FH2_CONSTANTES_PS : 256) - constantInfo->registerIndex;

                // NFSMW: dynamic UBO or pointer, depending on SPEC_CONSTANT_CONSTANTES_UBO.
                println("#define {}(INDEX) select((INDEX) < {}, (NFSMW_UBO ? g_Ubo{}.v[{} + min(INDEX, {})] : vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + ({} + min(INDEX, {})) * 16, 0x10)), 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1,
                    shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} (NFSMW_UBO ? g_Ubo{}.v[{}] : vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + {}, 0x10))",
                    constantName, shaderName, constantInfo->registerIndex.get(), shaderName, constantInfo->registerIndex * 16);
            }
            
            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);

            break;
        }

        case RegisterSet::Sampler:
        {
        #ifdef FH2_RECOMP
            // FH2: the slot is the fetch constant the microcode reads: pixel shader samplers 0-15, vertex shader
            // samplers 16-19 (register + 16; FH2's vertex shaders sample cube maps). Pixel shader ones keep NFSMW's
            // layout, vertex shader ones have their own area (FH2_BYTES_* in shader_common.h), so the two kinds of
            // a draw never share a word.
            {
                const uint32_t slot = constantInfo->registerIndex + (isPixelShader ? 0 : 16);
                for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
                {
                    const uint32_t b = FH2_BYTES_TEXTURA(slot, uint32_t(j));
                    println("#define {}_Texture{}DescriptorIndex (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT({}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {}))",
                        constantName, TEXTURE_DIMENSIONS[j], b, b);
                }
                const uint32_t bs = FH2_BYTES_SAMPLER(slot);
                println("#define {}_SamplerDescriptorIndex (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT({}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {}))",
                    constantName, bs, bs);
                const uint32_t bi = FH2_BYTES_INV_TAMANO(slot);
                println("#define {}_InvTamano (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT({}), NFSMW_COMPARTIDA_FLOAT({})) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + {}))",
                    constantName, bi, bi + 4, bi);
                println("#define {}_ExpAjuste exp2(float(int(NFSMW_COMPARTIDA_UINT(FH2_COMPARTIDA_EXP + {}))))", constantName,
                    slot * 4);
                fh2Textura1D(out, constantName);
                samplers.emplace(slot, constantName);
                break;
            }
        #endif
            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("#define {}_Texture{}DescriptorIndex (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT({}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {}))",
                    constantName, TEXTURE_DIMENSIONS[j], j * 64 + constantInfo->registerIndex * 4, j * 64 + constantInfo->registerIndex * 4);
            }

            println("#define {}_SamplerDescriptorIndex (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT({}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {}))",
                constantName, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4);

            // 1/size of the host image of that slot, which the renderer writes at byte
            // 360 + slot * 8 of the shared constants (right after g_InputRemap).
            {
                const uint32_t invBase = 360 + constantInfo->registerIndex * 8;
                println("#define {}_InvTamano (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT({}), NFSMW_COMPARTIDA_FLOAT({})) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + {}))",
                    constantName, invBase, invBase + 4, invBase);
            }

            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }

        }
    }

#ifdef FH2_RECOMP
    // FH2: fetches whose slot has no sampler in the constant table (the D3D library's own shaders have no table) are
    // named s<slot>; they read the same per-slot words as the named ones.
    // The name is the microcode's constant index; a vertex shader's n (0-3) and n + 16 both read slot n + 16 (its own
    // area).
    for (uint32_t n = 0; n < (isPixelShader ? 16u : 20u); n++)
    {
        if (!isPixelShader && n >= 4 && n < 16)
            continue;
        const uint32_t slot = isPixelShader || n >= 16 ? n : n + 16;
        if (samplers.count(slot))
            continue;  // the fetch takes the table's name
        for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
        {
            const uint32_t b = FH2_BYTES_TEXTURA(slot, uint32_t(j));
            println("#define s{}_Texture{}DescriptorIndex (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT({}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {}))",
                n, TEXTURE_DIMENSIONS[j], b, b);
        }
        const uint32_t bs = FH2_BYTES_SAMPLER(slot);
        println("#define s{}_SamplerDescriptorIndex (NFSMW_UBO ? NFSMW_COMPARTIDA_UINT({}) : vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {}))",
            n, bs, bs);
        const uint32_t bi = FH2_BYTES_INV_TAMANO(slot);
        println("#define s{}_InvTamano (NFSMW_UBO ? float2(NFSMW_COMPARTIDA_FLOAT({}), NFSMW_COMPARTIDA_FLOAT({})) : vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + {}))",
            n, bi, bi + 4, bi);
        println("#define s{}_ExpAjuste exp2(float(int(NFSMW_COMPARTIDA_UINT(FH2_COMPARTIDA_EXP + {}))))", n, slot * 4);
        fh2Textura1D(out, fmt::format("s{}", n));
    }
#endif

    out += "\n#else\n\n";

    println("cbuffer {}ShaderConstants : register(b{}, space4)", isPixelShader ? "Pixel" : "Vertex", isPixelShader ? 1 : 0);
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Float4)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            print("\tfloat4 {}", constantName);

            if (constantInfo->registerCount > 1)
                print("[{}]", constantInfo->registerCount.get());

            println(" : packoffset(c{});", constantInfo->registerIndex.get());

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? FH2_CONSTANTES_PS : 256) - constantInfo->registerIndex;
                println("#define {0}(INDEX) select((INDEX) < {1}, {0}[min(INDEX, {2})], 0.0)", constantName, tailCount, tailCount - 1);
            }
        }
    }

    out += "};\n\n";

    out += "cbuffer SharedConstants : register(b2, space4)\n";
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Sampler)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                    constantName, TEXTURE_DIMENSIONS[j], j * 4 + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
            }

            println("\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});",
                constantName, 4 * std::size(TEXTURE_DIMENSIONS) + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
        }
    }

    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Bool)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);
#ifdef FH2_RECOMP
            // The control flow's boolean address (pixel shaders' from 128), read with FH2_BOOLEANO: all 256 reach
            // the shaders (shader_common.h, FH2_COMPARTIDA_BOOLEANOS).
            println("\t#define {} {}", constantName, constantInfo->registerIndex + (isPixelShader ? 128 : 0));
#else
            println("\t#define {} (1 << {})", constantName, constantInfo->registerIndex + (isPixelShader ? 16 : 0));
#endif
            // In NFSMW CTAB numbers b0.. and CF uses 128.. for pixels.
            // The g_Booleans packing keeps its 16 bits per stage.
#ifdef NFSMW_RECOMP
            boolConstants.emplace(constantInfo->registerIndex + (isPixelShader ? 128 : 0), constantName);
#else
            boolConstants.emplace(constantInfo->registerIndex, constantName);
#endif
        }
    }

    out += '\n';

    const auto shader = reinterpret_cast<const Shader*>(shaderData + shaderContainer->shaderOffset);

    out += "#ifndef __spirv__\n";

    if (isPixelShader)
        out += "[shader(\"pixel\")]\n";
    else
        out += "[shader(\"vertex\")]\n";

    out += "#endif\n";

    out += "void main(\n";

    if (isPixelShader)
    {
        out += "\tin float4 iPos : SV_Position,\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\tin float4 i{0}{1} : {2}{1},", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);

        out += "#ifdef __spirv__\n";
        out += "\tin bool iFace : SV_IsFrontFace\n";
        out += "#else\n";
        out += "\tin uint iFace : SV_IsFrontFace\n";
        out += "#endif\n";

        auto pixelShader = reinterpret_cast<const PixelShader*>(shader);
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += ",\n\tout float4 oC0 : SV_Target0";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += ",\n\tout float4 oC1 : SV_Target1";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += ",\n\tout float4 oC2 : SV_Target2";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += ",\n\tout float4 oC3 : SV_Target3";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += ",\n\tout float oDepth : SV_Depth";
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

        #ifdef FH2_RECOMP
            // The same element declared again (another stream of the same data): one input, every fetch address
            // maps to it.
            bool repetido = false;
            for (auto& [direccion, elemento] : vertexElements)
                repetido |= elemento.usage == vertexElement.usage && elemento.usageIndex == vertexElement.usageIndex;
            if (repetido)
            {
                vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
                fh2OrdinalElemento.emplace(uint32_t(vertexElement.address), i);
                continue;
            }
        #endif

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

        #ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
        #endif

            out += '\t';

            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    print("[[vk::location({})]] ", usageLocation.location);
                    break;
                }
            }

            println("in {0} i{1}{2} : {3}{2},", usageType, USAGE_VARIABLES[uint32_t(vertexElement.usage)],
                uint32_t(vertexElement.usageIndex), USAGE_SEMANTICS[uint32_t(vertexElement.usage)]);

            vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
        #ifdef FH2_RECOMP
            fh2OrdinalElemento.emplace(uint32_t(vertexElement.address), i);
        #endif
        }

    #ifdef UNLEASHED_RECOMP
        if (hasIndexCount)
        {
            out += "\tin uint iVertexId : SV_VertexID,\n";
            out += "\tin uint iInstanceId : SV_InstanceID,\n";
        }
    #endif
    #ifdef FH2_RECOMP
        // The Xenos puts the vertex index in r0.x (FH2's grass and billboards compute their corners from it).
        out += "\tin uint iVertexId : SV_VertexID,\n";
    #endif

        out += "\tout float4 oPos : SV_Position";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print(",\n\tout float4 o{0}{1} : {2}{1}", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);
    #ifdef FH2_RECOMP
        // Point lists (the night sky's stars): Vulkan wants PointSize written; the renderer gives the diameter.
        // (DXC only takes the PointSize builtin inside a struct: shader_common.h, Fh2Punto.)
        out += ",\n\tout Fh2Punto oPunto";
    #endif
    }

    out += ")\n";
    out += "{\n";

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
    {
        specConstantsMask |= SPEC_CONSTANT_REVERSE_Z;

        out += "\toPos = 0.0;\n";

        out += "\tfloat4x4 mtxProjection = float4x4(g_MtxProjection(0), g_MtxProjection(1), g_MtxProjection(2), g_MtxProjection(3));\n";
        out += "\tfloat4x4 mtxProjectionReverseZ = mul(mtxProjection, float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1));\n";

        out += "\t[unroll] for (int iterationIndex = 0; iterationIndex < 2; iterationIndex++)\n";
        out += "\t{\n";
    }
#endif

    if (shaderContainer->definitionTableOffset != NULL)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            auto value = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + definition->physicalOffset);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
                println("\tfloat4 c{} = asfloat(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());
            #ifdef FH2_RECOMP
                fh2ConstantesLiterales.insert(definition->registerIndex + i - (isPixelShader ? 256 : 0));
            #endif

                value += 4;
            }
            definitions += 2;
        }
        ++definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Int4Definition*>(definitions);
            for (uint16_t i = 0; i < definition->count; i++)
            {
                /*
                 * Named: GCC does not accept the anonymous local union member
                 * that MSVC and clang accept with -fms-extensions.
                 */
                union Bytes4
                {
                    uint32_t value;
                    struct { int8_t x, y, z, w; } b;
                } u;

                u.value = definition->values[i].get();

                println("\tint4 i{} = int4({}, {}, {}, {});",
                    (definition->registerIndex - 8992) / 4 + i, u.b.x, u.b.y, u.b.z, u.b.w);
            #ifdef FH2_RECOMP
                fh2BuclesLiterales.insert((definition->registerIndex - 8992) / 4 + i);
            #endif
            }
            definitions += 2;
            definitions += definition->count;
        }

        out += "\n";
    }

    bool printedRegisters[32]{};

    uint32_t interpolatorCount = (shader->interpolatorInfo >> 5) & 0x1F;

    for (uint32_t i = 0; i < interpolatorCount; i++)
    {
        union
        {
            Interpolator interpolator;
            uint32_t value;
        };
    
        if (isPixelShader)
        {
            value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
            println("\tfloat4 r{} = i{}{};", uint32_t(interpolator.reg), USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex));
            printedRegisters[interpolator.reg] = true;
        }
        else
        {
            auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
            interpolators.emplace(i, fmt::format("o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex)));
        }
    }

    if (!isPixelShader)
    {
    #ifdef UNLEASHED_RECOMP
        if (!hasMtxProjection)
            out += "\toPos = 0.0;\n";
    #endif

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\to{}{} = 0.0;", USAGE_VARIABLES[uint32_t(usage)], usageIndex);
    #ifdef FH2_RECOMP
        out += "\toPunto.tam = FH2_TAM_PUNTO;\n";
    #endif

        out += "\n";
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (!printedRegisters[i])
        {
            print("\tfloat4 r{} = ", i);
            if (isPixelShader && i == ((shader->fieldC >> 8) & 0xFF))
            {
#ifdef FH2_RECOMP
                // In guest pixels: the pass may be drawn smaller (fh2_nativo_escala).
                out += "float4((iPos.xy * FH2_INV_ESCALA - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
#else
                out += "float4((iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
#endif
            }
        #ifdef UNLEASHED_RECOMP
            else if (!isPixelShader && hasIndexCount && i == 0)
            {
                out += "float4(iVertexId + g_IndexCount.x * iInstanceId, 0.0, 0.0, 0.0);\n";
            }
        #endif
        #ifdef FH2_RECOMP
            else if (!isPixelShader && i == 0)
            {
                out += "float4(float(int(iVertexId) - FH2_BASE_VERTICE), 0.0, 0.0, 0.0);\n";
            }
        #endif
            else
            {
                out += "0.0;\n";
            }
        }
    }

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";
    if (isPixelShader)
    {
#ifdef UNLEASHED_RECOMP
        out += "\tfloat2 pixelCoord = 0.0;\n";
#endif
        out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";
    }
#ifdef FH2_RECOMP
    else
    {
        // FH2's vertex shaders also sample cube maps (cube + tfetchCube), and fetch instance data directly.
        out += "\tCubeMapData cubeMapData = (CubeMapData)0;\n";
        out += "\tuint fh2BuscaConstante = 0;\n";
        out += "\tint fh2BuscaDireccion = 0;\n";
        out += "\tint fh2BuscaIndice = 0;\n";
    }
#endif

    const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + shader->physicalOffset);

    /*
     * Named, with references: GCC does not accept the anonymous struct inside a local
     * union that MSVC and clang accept. The references leave the rest of the body
     * untouched, which uses code0..code3 eighteen times.
     */
    union ControlFlowWords
    {
        ControlFlowInstruction controlFlow[2];
        struct { uint32_t code0, code1, code2, code3; } w;
    } cfu;
    uint32_t& code0 = cfu.w.code0;
    uint32_t& code1 = cfu.w.code1;
    uint32_t& code2 = cfu.w.code2;
    uint32_t& code3 = cfu.w.code3;

    auto controlFlowCode = code;
    uint32_t instrAddress = 0;
    uint32_t instrSize = shader->size;
    bool simpleControlFlow = true;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : cfu.controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional || cfInstr.condJmp.direction)
                    simpleControlFlow = false;
                else
                    ++ifEndLabels[cfInstr.condJmp.address];

                break;
            }
            }

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    controlFlowCode = code;
    instrAddress = 0;
    uint32_t pc = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : cfu.controlFlow)
        {
            if (!simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
            }

            ++pc;

            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;
            bool shouldReturn = false;
            bool shouldCloseCurlyBracket = false;
            /*
             * If this CF opens its own `if (p0)` (CondExecPred), its condition. It serves
             * two purposes: reusing the identical block that was just closed, and leaving its
             * own brace recorded so the next one can reuse it. See reabrirCierre().
             */
            int condicionPredicadoCf = -1;

            // Control flow boundary. An open predicated block does not cross this point.
            cerrarPredicado();

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::ExecEnd);
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd);
                {
                    // The condition belongs to the whole EXEC block, including its
                    // return. Per-instruction ALU predication does not replace it.
                    const auto booleano = boolConstants.find(cfInstr.condExec.boolAddress);
                    if (booleano == boolConstants.end())
                        throw std::runtime_error("EXEC condicional sin constante booleana declarada");
                    indent();
#ifdef FH2_RECOMP
                    println("if (FH2_BOOLEANO({}) {}= 0)", booleano->second, cfInstr.condExec.condition ? "!" : "=");
#else
                    println("if ((g_Booleans & {}) {}= 0)", booleano->second, cfInstr.condExec.condition ? "!" : "=");
#endif
                    indent();
                    out += "{\n";
                    ++indentation;
                    shouldCloseCurlyBracket = true;
                }
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
                condicionPredicadoCf = cfInstr.condExecPred.condition ? 1 : 0;
                /*
                 * The previous block was this same `if (p0)` and nothing was emitted between
                 * the two: its brace is deleted and this EXEC continues inside. This way runs of
                 * CondExecPred with the same predicate (the nine PCF taps, the six car lights)
                 * end up in one basic block instead of three or four.
                 */
                if (!reabrirCierre(condicionPredicadoCf))
                {
                    indent();
                    println("if ({}p0)", condicionPredicadoCf ? "" : "!");
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                shouldCloseCurlyBracket = true;
                // This `if` governs the whole block, so the predicated instructions inside
                // do not have to test it again. See predGarantizado_.
                predGarantizado_ = condicionPredicadoCf;
                break;

            case ControlFlowOpcode::LoopStart:
                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    print("[unroll] ");
                #endif
                    println("for (aL = 0; aL < {}; aL++)", fh2Bucle(uint32_t(cfInstr.loopStart.loopId)));
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                else 
                {
                    out += "\t\t\taL = 0;\n";
                }
                break;

            case ControlFlowOpcode::LoopEnd:
                if (simpleControlFlow)
                {
                    --indentation;
                    indent();
                    out += "}\n";
                }
                else
                {
                    out += "\t\t\t++aL;\n";
                    println("\t\t\tif (aL < {})", fh2Bucle(uint32_t(cfInstr.loopEnd.loopId)));
                    out += "\t\t\t{\n";
                    println("\t\t\t\tpc = {};", uint32_t(cfInstr.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                }
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional)
                {
                    assert(!simpleControlFlow);
                    println("\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                    out += "\t\t\tcontinue;\n";
                }
                else
                {
                    indent();
                    if (cfInstr.condJmp.isPredicated)
                    {
                        println("if ({}p0)", cfInstr.condJmp.condition ^ simpleControlFlow ? "" : "!");
                    }
                    else
                    {
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
#ifdef FH2_RECOMP
                            println("if (FH2_BOOLEANO({}) {}= 0)", findResult->second, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
#else
                            println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
#endif
                        else
                            println("if (b{} {}= 0)", uint32_t(cfInstr.condJmp.boolAddress), cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                    }

                    if (simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }
            }

            auto instructionCode = code + address * 3;
            
            for (uint32_t i = 0; i < count; i++)
            {
                /*
                 * Named, for the same reason as the other two: GCC does not accept
                 * the anonymous struct inside a local union.
                 */
                union InstrWords
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct { uint32_t code0, code1, code2; } w;
                } iu;
                VertexFetchInstruction& vertexFetch = iu.vertexFetch;
                TextureFetchInstruction& textureFetch = iu.textureFetch;
                AluInstruction& alu = iu.alu;

                iu.w.code0 = instructionCode[0];
                iu.w.code1 = instructionCode[1];
                iu.w.code2 = instructionCode[2];
            
                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        recompile(vertexFetch, address + i);
                    }
                    else
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (textureFetch.constIndex == 10) // g_GISampler
                        {
                            specConstantsMask |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                            indent();
                            out += "if (g_SpecConstants() & SPEC_CONSTANT_BICUBIC_GI_FILTER)";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, true);
                            --indentation;

                            indent();
                            out += "}";
                            indent();
                            out += "else";
                            indent();
                            out += '{';

                            ++indentation;
                            recompile(textureFetch, false);
                            --indentation;

                            indent();
                            out += '}';
                        }
                        else
                    #endif
                        {
                            recompile(textureFetch, false);
                        }
                    }
                }
                else
                {
                    recompile(alu);
                }
            
                sequence >>= 2;
                instructionCode += 3;
            }

            cerrarPredicado();  // end of the EXEC instructions

            if (shouldReturn)
            {
                if (isPixelShader)
                {
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TEST;

                    indent();
                    out += "[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)";
                    indent();
                    out += '{';

                    indent();
                #ifdef NFSMW_RECOMP
                    // NFSMW: the 8 comparison functions (alphaTestValue, shader_common.h).
                    out += "\tclip(alphaTestValue(oC0.w, iPos.xy));\n";
                #else
                    out += "\tclip(oC0.w - g_AlphaThreshold);\n";
                #endif

                    indent();
                    out += "}";

                #ifdef UNLEASHED_RECOMP
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;

                    indent();
                    out += "else if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TO_COVERAGE)";
                    indent();
                    out += '{';

                    indent();
                    out += "\toC0.w *= 1.0 + computeMipLevel(pixelCoord) * 0.25;\n";
                    indent();
                    out += "\toC0.w = 0.5 + (oC0.w - g_AlphaThreshold) / max(fwidth(oC0.w), 1e-6);\n";

                    indent();
                    out += '}';
                #endif
                }
                else
                {
                #ifdef UNLEASHED_RECOMP
                    if (!hasMtxProjection)
                #endif
                    {
                    #ifdef NFSMW_RECOMP
                        // To host clip space (ndc_scale/ndc_offset from
                        // draw.cpp): draws in pixels with clipping disabled.
                        out += "\toPos.xy = oPos.xy * g_NdcScale + g_NdcOffset * oPos.w;\n";
                    #endif
                        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
                    }
                }

                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    if (hasMtxProjection)
                    {
                        out += "continue;\n";
                    }
                    else
                #endif
                    {
                        out += "return;\n";
                    }
                }
                else
                {
                    out += "\t\t\tbreak;\n";
                }
            }

            if (shouldCloseCurlyBracket)
            {
                --indentation;
                const size_t marcaLlaveCf = out.size();
                indent();
                out += "}\n";
                /*
                 * It is recorded in case the next CondExecPred is identical. Not with a
                 * `return` inside, since the block does not continue; and anotarCierreCf() also
                 * refuses if something wrote p0 in here (predGarantizado_ was set to -1): the
                 * next block has to test the new p0.
                 */
                anotarCierreCf(marcaLlaveCf, shouldReturn ? -1 : condicionPredicadoCf);
            }
            predGarantizado_ = -1;  // end of the EXEC block: it no longer guarantees anything
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
        out += "\t}\n";

    if (!isPixelShader && hasMtxProjection)
        out += "\toPos.xy += g_HalfPixelOffset * oPos.w;\n";
#endif

    out += "}";

#ifdef FH2_RECOMP
    // A vertex input whose fetches all read their buffer directly is not read: it leaves the signature, so its
    // location is not an input of the SPIR-V and the renderer does not bind it.
    if (!isPixelShader)
    {
        const size_t inicioMain = out.find("void main(");
        const size_t cuerpo = inicioMain == std::string::npos ? std::string::npos : out.find("\n{\n", inicioMain);
        if (cuerpo != std::string::npos)
        {
            std::string firma = out.substr(inicioMain, cuerpo - inicioMain);
            const std::string_view resto(out.data() + cuerpo, out.size() - cuerpo);
            std::string nueva;
            size_t pos = 0;
            while (pos < firma.size())
            {
                size_t fin = firma.find('\n', pos);
                if (fin == std::string::npos) fin = firma.size();
                const std::string linea = firma.substr(pos, fin - pos);
                bool quitar = false;
                const size_t in = linea.find(" in ");
                if (in != std::string::npos && linea.find("SV_") == std::string::npos)
                {
                    const size_t nombre = linea.find(" i", in + 3);
                    const size_t dos = linea.find(" :", nombre);
                    if (nombre != std::string::npos && dos != std::string::npos)
                    {
                        const std::string variable = linea.substr(nombre + 1, dos - nombre - 1);
                        bool usada = false;
                        for (size_t q = resto.find(variable); q != std::string_view::npos; q = resto.find(variable, q + 1))
                        {
                            const char sig = q + variable.size() < resto.size() ? resto[q + variable.size()] : ' ';
                            if (!std::isalnum(static_cast<unsigned char>(sig)) && sig != '_')
                            {
                                usada = true;
                                break;
                            }
                        }
                        quitar = !usada;
                    }
                }
                if (!quitar)
                {
                    nueva += linea;
                    if (fin < firma.size()) nueva += '\n';
                }
                pos = fin + 1;
            }
            out = out.substr(0, inicioMain) + nueva + out.substr(cuerpo);
        }
    }
#endif
}
