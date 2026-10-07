/*
 * NFSMW: WMV3 decoder (VC-1 main profile) for the game's cutscenes.
 *
 * WMV3 is enabled along with everything it selects in configure: vc1_decoder_select = "blockdsp
 * h263_decoder h264qpel intrax8 mpegvideo vc1dsp", and in turn h263_decoder, mpegvideo, intrax8,
 * vc1dsp, me_cmp, mpeg_er and error_resilience. config.h includes it after each platform's header.
 */
#undef CONFIG_WMV3_DECODER
#define CONFIG_WMV3_DECODER 1
#undef CONFIG_VC1_DECODER
#define CONFIG_VC1_DECODER 1
#undef CONFIG_H263_DECODER
#define CONFIG_H263_DECODER 1
#undef CONFIG_H263_PARSER
#define CONFIG_H263_PARSER 1
#undef CONFIG_H263DSP
#define CONFIG_H263DSP 1
#undef CONFIG_MPEGVIDEO
#define CONFIG_MPEGVIDEO 1
#undef CONFIG_QPELDSP
#define CONFIG_QPELDSP 1
#undef CONFIG_BLOCKDSP
#define CONFIG_BLOCKDSP 1
#undef CONFIG_H264QPEL
#define CONFIG_H264QPEL 1
#undef CONFIG_INTRAX8
#define CONFIG_INTRAX8 1
#undef CONFIG_VC1DSP
#define CONFIG_VC1DSP 1
#undef CONFIG_H264CHROMA
#define CONFIG_H264CHROMA 1
#undef CONFIG_HPELDSP
#define CONFIG_HPELDSP 1
#undef CONFIG_IDCTDSP
#define CONFIG_IDCTDSP 1
#undef CONFIG_ME_CMP
#define CONFIG_ME_CMP 1
#undef CONFIG_MPEG_ER
#define CONFIG_MPEG_ER 1
#undef CONFIG_ERROR_RESILIENCE
#define CONFIG_ERROR_RESILIENCE 1
#undef CONFIG_VIDEODSP
#define CONFIG_VIDEODSP 1
#undef CONFIG_PIXBLOCKDSP
#define CONFIG_PIXBLOCKDSP 1
#undef CONFIG_STARTCODE
#define CONFIG_STARTCODE 1
#undef CONFIG_FDCTDSP
#define CONFIG_FDCTDSP 1
#undef CONFIG_WMV2DSP
#define CONFIG_WMV2DSP 1
