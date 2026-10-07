/**
******************************************************************************
* Xenia : Xbox 360 Emulator Research Project                                 *
******************************************************************************
* Copyright 2021 Ben Vanik. All rights reserved.                             *
* Released under the BSD license - see LICENSE in the root for more details. *
******************************************************************************
*
* @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
*/

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>
#include <vector>

#include <rex/audio/xma/context.h>
#include <rex/audio/xma/decoder.h>
#include <rex/audio/xma/helpers.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/memory/ring_buffer.h>
#include <rex/platform.h>
#include <rex/stream.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>

extern "C" {
#if REX_COMPILER_MSVC
#pragma warning(push)
#pragma warning(disable : 4101 4244 5033)
#endif
#include "libavcodec/avcodec.h"
#include "libavutil/error.h"
#if REX_COMPILER_MSVC
#pragma warning(pop)
#endif
}  // extern "C"

// Credits for most of this code goes to:
// https://github.com/koolkdev/libertyv/blob/master/libav_wrapper/xma2dec.c

REXCVAR_DEFINE_INT32(audio_volcado_xma_s, 0, "Audio",
                     "Diagnostico: segundos de cada contexto XMA que se guardan en "
                     "xma_<contexto>_tramas/salida_<hz>.wav junto al ejecutable; 0 = nada");
REXCVAR_DEFINE_INT32(audio_volcado_xma_desde_s, 0, "Audio",
                     "Diagnostico: segundos desde el primer audio XMA antes de empezar a volcar");
REXCVAR_DEFINE_INT32(audio_volcado_xma_contextos, 12, "Audio",
                     "Diagnostico: contextos XMA que se vuelcan como mucho");
REXCVAR_DEFINE_INT32(audio_volcado_xma_mb, 32, "Audio",
                     "Diagnostico: megas como mucho de audio XMA acumulado sin escribir");
REXCVAR_DEFINE_INT32(audio_volcado_xma_min_s, 2, "Audio",
                     "Diagnostico: segundos minimos de un sonido XMA que se guardan cuando termina");

namespace {

// Robotic audio diagnostic: 16-bit big-endian PCM of an XMA context, kept in memory until the
// requested seconds are collected, then written as a little-endian WAV in one go.
struct VolcadoXma {
  uint32_t frecuencia = 0;
  uint32_t canales = 0;
  std::vector<int16_t> muestras;
  bool escrito = false;
  uint32_t secuencia = 0;  // segment number in the file name
};

std::mutex volcado_xma_mutex;
std::map<std::pair<uint32_t, int>, VolcadoXma> volcados_xma;  // (contexto, 0 tramas / 1 salida)
uint32_t ficheros_volcados_xma = 0;
uint32_t secuencia_volcados_xma = 0;
uint32_t anotados_xma = 0;
size_t muestras_acumuladas_xma = 0;  // total of the dumps not yet written
// Contexts currently accumulating (at most audio_volcado_xma_contextos): with all of them at once
// the memory cap filled up and none got written.
std::set<uint32_t> contextos_volcados_xma;
bool volcado_xma_con_reloj = false;
std::chrono::steady_clock::time_point volcado_xma_inicio;

// Written on a separate thread: on the Switch SD the file can take a while, and the caller holds
// the context lock on the game's audio thread.
void EscribirVolcadoXmaEnHilo(uint32_t contexto, int tipo, const VolcadoXma& v) {
  const std::string numero = std::to_string(contexto);
  const auto ruta = rex::filesystem::GetExecutableFolder() /
                    ("xma_" + std::string(numero.size() < 3 ? 3 - numero.size() : 0, '0') + numero + "_" +
                     std::to_string(v.secuencia) + (tipo ? "_salida_" : "_tramas_") +
                     std::to_string(v.frecuencia) + ".wav");
  std::ofstream fichero(ruta, std::ios::binary | std::ios::trunc);
  if (!fichero) {
    return;
  }
  const uint32_t bytes = uint32_t(v.muestras.size() * 2);
  const auto u32 = [&](uint32_t x) { fichero.write(reinterpret_cast<const char*>(&x), 4); };
  const auto u16 = [&](uint16_t x) { fichero.write(reinterpret_cast<const char*>(&x), 2); };
  fichero.write("RIFF", 4);
  u32(36 + bytes);
  fichero.write("WAVEfmt ", 8);
  u32(16);
  u16(1);  // PCM
  u16(uint16_t(v.canales));
  u32(v.frecuencia);
  u32(v.frecuencia * v.canales * 2);
  u16(uint16_t(v.canales * 2));
  u16(16);
  fichero.write("data", 4);
  u32(bytes);
  fichero.write(reinterpret_cast<const char*>(v.muestras.data()), bytes);
  REXAPU_INFO("XMA: volcado {} del contexto {}: {} muestras a {} Hz, {} canales",
              tipo ? "de salida" : "de tramas", contexto, v.muestras.size() / v.canales,
              v.frecuencia, v.canales);
}

// Dumps are written on a single thread that lives until the process ends: on the Switch SD a file
// can take seconds and the game's audio thread must not wait for that. There is no thread per file:
// on the Switch, std::thread::detach() threw std::system_error (the writer thread had already
// finished) and the game closed. If the thread cannot be created, the file is written immediately.
class EscritorVolcados {
 public:
  static void Encolar(std::function<void()> trabajo) {
    static EscritorVolcados* const escritor = Crear();  // never destroyed
    if (!escritor) {
      trabajo();
      return;
    }
    {
      std::lock_guard<std::mutex> cerrojo(escritor->mutex_);
      escritor->cola_.push_back(std::move(trabajo));
    }
    escritor->aviso_.notify_one();
  }

 private:
  EscritorVolcados() : hilo_([this]() { Bucle(); }) {}

  static EscritorVolcados* Crear() {
    try {
      return new EscritorVolcados();
    } catch (const std::system_error&) {
      return nullptr;
    }
  }

  void Bucle() {
    for (;;) {
      std::function<void()> trabajo;
      {
        std::unique_lock<std::mutex> cerrojo(mutex_);
        aviso_.wait(cerrojo, [this]() { return !cola_.empty(); });
        trabajo = std::move(cola_.front());
        cola_.pop_front();
      }
      trabajo();
    }
  }

  std::mutex mutex_;
  std::condition_variable aviso_;
  std::deque<std::function<void()>> cola_;
  std::thread hilo_;  // last: it starts with the other members already constructed
};

void EscribirVolcadoXma(uint32_t contexto, int tipo, VolcadoXma& v) {
  VolcadoXma copia;
  copia.frecuencia = v.frecuencia;
  copia.canales = v.canales;
  copia.secuencia = v.secuencia;
  copia.muestras = std::move(v.muestras);
  EscritorVolcados::Encolar([contexto, tipo, copia = std::move(copia)]() {
    EscribirVolcadoXmaEnHilo(contexto, tipo, copia);
  });
}

void VolcarXma(uint32_t contexto, int tipo, uint32_t frecuencia, uint32_t canales, const uint8_t* be,
               size_t bytes) {
  const int32_t segundos = REXCVAR_GET(audio_volcado_xma_s);
  if (segundos <= 0 || !frecuencia || !canales) {
    return;
  }
  std::lock_guard<std::mutex> cerrojo(volcado_xma_mutex);
  if (!volcado_xma_con_reloj) {
    volcado_xma_con_reloj = true;
    volcado_xma_inicio = std::chrono::steady_clock::now();
  }
  if (std::chrono::steady_clock::now() - volcado_xma_inicio <
      std::chrono::seconds(std::max(REXCVAR_GET(audio_volcado_xma_desde_s), 0))) {
    return;
  }
  if (ficheros_volcados_xma >= 2 * uint32_t(std::max(REXCVAR_GET(audio_volcado_xma_contextos), 0))) {
    return;
  }
  if (const auto escrito = volcados_xma.find({contexto, tipo});
      escrito != volcados_xma.end() && escrito->second.escrito) {
    return;
  }
  if (!contextos_volcados_xma.count(contexto)) {
    if (contextos_volcados_xma.size() >=
        size_t(std::max(REXCVAR_GET(audio_volcado_xma_contextos), 0))) {
      return;
    }
    contextos_volcados_xma.insert(contexto);
  }
  VolcadoXma& v = volcados_xma[{contexto, tipo}];
  if (!v.muestras.empty() && (v.frecuencia != frecuencia || v.canales != canales)) {
    // A different format in the same context: start over.
    muestras_acumuladas_xma -= v.muestras.size();
    v.muestras = std::vector<int16_t>();
  }
  if (v.muestras.empty() && tipo == 0 && anotados_xma < 64) {
    ++anotados_xma;
    REXAPU_INFO("XMA: contexto {} con audio a {} Hz, {} canales", contexto, frecuencia, canales);
  }
  if (v.muestras.empty()) {
    v.secuencia = ++secuencia_volcados_xma;
  }
  v.frecuencia = frecuencia;
  v.canales = canales;
  const size_t tope = size_t(std::max(REXCVAR_GET(audio_volcado_xma_mb), 1)) * (1024 * 1024 / 2);
  if (muestras_acumuladas_xma + bytes / 2 > tope) {
    // It does not fit: this context starts over so the dump has no gap.
    muestras_acumuladas_xma -= v.muestras.size();
    v.muestras = std::vector<int16_t>();
    return;
  }
  muestras_acumuladas_xma += bytes / 2;
  for (size_t i = 0; i + 1 < bytes; i += 2) {
    v.muestras.push_back(int16_t(uint16_t((uint16_t(be[i]) << 8) | be[i + 1])));
  }
  if (v.muestras.size() >= size_t(segundos) * frecuencia * canales) {
    ++ficheros_volcados_xma;
    v.escrito = true;
    muestras_acumuladas_xma -= v.muestras.size();
    EscribirVolcadoXma(contexto, tipo, v);  // takes the samples
    v.muestras = std::vector<int16_t>();
    const auto otro = volcados_xma.find({contexto, 1 - tipo});
    if (otro == volcados_xma.end() || otro->second.escrito) {
      contextos_volcados_xma.erase(contexto);  // it has written its part: frees its slot
    }
  }
}

// Another sound in the context: what was accumulated is no longer continuous with what follows.
void ReiniciarVolcadoXma(uint32_t contexto) {
  if (REXCVAR_GET(audio_volcado_xma_s) <= 0) {
    return;
  }
  std::lock_guard<std::mutex> cerrojo(volcado_xma_mutex);
  for (int tipo = 0; tipo < 2; ++tipo) {
    const auto it = volcados_xma.find({contexto, tipo});
    if (it != volcados_xma.end() && !it->second.escrito) {
      VolcadoXma& v = it->second;
      muestras_acumuladas_xma -= v.muestras.size();
      // The sound has ended: if it is long enough it is saved as a segment.
      const size_t minimo = size_t(std::max(REXCVAR_GET(audio_volcado_xma_min_s), 1)) *
                            std::max<uint32_t>(v.frecuencia, 1) * std::max<uint32_t>(v.canales, 1);
      if (v.frecuencia && v.muestras.size() >= minimo &&
          ficheros_volcados_xma < 2 * uint32_t(std::max(REXCVAR_GET(audio_volcado_xma_contextos), 0))) {
        ++ficheros_volcados_xma;
        EscribirVolcadoXma(contexto, tipo, v);
      }
      volcados_xma.erase(it);
    }
  }
  contextos_volcados_xma.erase(contexto);  // the sound has ended: frees its slot
}

}  // namespace

namespace rex::audio {

using stream::BitStream;

const uint32_t XmaContext::kBitsPerPacketHeader;
const uint32_t XmaContext::kOutputMaxSizeBytes;

XmaContext::XmaContext()
    : work_completion_event_(rex::thread::Event::CreateAutoResetEvent(false)) {}

XmaContext::~XmaContext() {
  if (av_context_) {
    avcodec_free_context(&av_context_);
  }
  if (av_frame_) {
    av_frame_free(&av_frame_);
  }
}

int XmaContext::Setup(uint32_t id, memory::Memory* memory, uint32_t guest_ptr) {
  id_ = id;
  memory_ = memory;
  guest_ptr_ = guest_ptr;

  // Allocate ffmpeg stuff:
  av_packet_ = av_packet_alloc();
  assert_not_null(av_packet_);
  av_packet_->buf = av_buffer_alloc(128 * 1024);

  // find the XMA2 audio decoder
  av_codec_ = avcodec_find_decoder(AV_CODEC_ID_XMAFRAMES);
  if (!av_codec_) {
    REXAPU_ERROR("XmaContext {}: Codec not found", id);
    return 1;
  }

  av_context_ = avcodec_alloc_context3(av_codec_);
  if (!av_context_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate context", id);
    return 1;
  }

  // Initialize these to 0. They'll actually be set later.
  av_context_->channels = 0;
  av_context_->sample_rate = 0;

  av_frame_ = av_frame_alloc();
  if (!av_frame_) {
    REXAPU_ERROR("XmaContext {}: Couldn't allocate frame", id);
    return 1;
  }

  // FYI: We're purposely not opening the codec here. That is done later.
  return 0;
}

/*
 * NFSMW: silence watchdog, measurement only.
 *
 * In one recording, after braking in a corner, the engine acceleration sound takes ~2.5 s to come
 * back, coinciding with a zone load. If the voice that goes quiet is XMA, it shows here: every time
 * the game enables the context with an output buffer pending and the pass produces no PCM, a silence
 * starts (or continues); when it produces again, if the silence lasted 50 ms or more, a log line
 * gives the context, how long it lasted, how many empty passes there were and why (1 only flush
 * without subframes, 2 no input, 3 no progress, 4 error). Contexts that never sound again (end of a
 * sound) write nothing. It does not change anything that is decoded. At most 200 lines per run.
 */
void XmaContext::AnotarProduccion(bool produjo, uint8_t motivo, const XMA_CONTEXT_DATA& data) {
  static std::atomic<uint32_t> lineas{0};
  const auto ahora = std::chrono::steady_clock::now();
  if (!produjo) {
    if (!en_silencio_) {
      en_silencio_ = true;
      silencio_desde_ = ahora;
      silencio_pasadas_ = 0;
    }
    ++silencio_pasadas_;
    silencio_motivo_ = motivo;
    return;
  }
  if (!en_silencio_) {
    return;
  }
  en_silencio_ = false;
  const double ms = std::chrono::duration<double, std::milli>(ahora - silencio_desde_).count();
  if (ms >= 50.0 && lineas.fetch_add(1, std::memory_order_relaxed) < 200) {
    REXLOG_INFO("[xma] silencio: el contexto {} estuvo {:.0f} ms sin producir y vuelve a sonar ({} pasadas vacias, "
                "ultimo motivo {}: 1 solo vaciar, 2 sin entrada, 3 sin avance, 4 error; entradas {}{}, bufer actual {}, "
                "bucles {})",
                id_, ms, silencio_pasadas_, silencio_motivo_, uint32_t(data.input_buffer_0_valid),
                uint32_t(data.input_buffer_1_valid), uint32_t(data.current_buffer), uint32_t(data.loop_count));
  }
}

bool XmaContext::Work() {
  if (!is_allocated() || !is_enabled()) {
    return false;
  }

  std::lock_guard<std::mutex> lock(lock_);
  // The immediate work and the XMA thread may have seen the same notification.
  // Only one must consume it; Disable/Release also use this lock.
  if (!is_allocated() || !is_enabled()) {
    return false;
  }
  set_is_enabled(false);

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  const XMA_CONTEXT_DATA initial_data = data;

  if (!data.output_buffer_valid) {
    return true;
  }

  // Avoid modulo by zero and accesses outside the guest buffer.
  if (!data.output_buffer_block_count ||
      data.output_buffer_read_offset >= data.output_buffer_block_count ||
      data.output_buffer_write_offset >= data.output_buffer_block_count) {
    data.error_status = 4;
    data.output_buffer_valid = 0;
    StoreContextMerged(data, initial_data, context_ptr);
    return true;
  }

  memory::RingBuffer output_rb = PrepareOutputRingBuffer(&data);
  uint32_t output_bytes_written = 0;

  // Consume-only context: no input, just drain remaining subframes.
  if (data.IsConsumeOnlyContext()) {
    if (current_frame_remaining_subframes_ == 0) {
      return true;
    }
    // Flush all the PCM that fits, even if the packet size was set to zero.
    // Consume returns zero when it has to wait for free space.
    while (current_frame_remaining_subframes_) {
      const uint32_t written = Consume(&output_rb, &data);
      if (!written) {
        break;
      }
      output_bytes_written += written;
    }
    data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;
    if (output_bytes_written && output_rb.empty()) {
      data.output_buffer_valid = 0;
    }
    StoreContextMerged(data, initial_data, context_ptr);
    AnotarProduccion(output_bytes_written != 0, 1, data);  // Measurement only
    return true;
  }

  // Minimum free blocks needed before attempting a decode.
  // Use subframe_decode_count (clamped to 1) instead of full frame size.
  const uint32_t effective_sdc = std::max(static_cast<uint32_t>(1), data.subframe_decode_count);
  const int32_t minimum_subframe_decode_count =
      static_cast<int32_t>(effective_sdc) + data.output_buffer_padding;

  if (minimum_subframe_decode_count > remaining_subframe_blocks_in_output_buffer_) {
    StoreContextMerged(data, initial_data, context_ptr);
    AnotarProduccion(true, 0, data);  // output full, not silence
    return true;
  }

  uint8_t motivo_silencio = 0;  // Measurement only
  while (remaining_subframe_blocks_in_output_buffer_ >= minimum_subframe_decode_count) {
    const uint32_t previous_offset = data.input_buffer_read_offset;
    const uint32_t previous_buffer = data.current_buffer;
    const auto previous_valid = data.input_buffer_0_valid | (data.input_buffer_1_valid << 1);
    const uint32_t previous_loop_count = data.loop_count;
    const uint8_t previous_subframes = current_frame_remaining_subframes_;
    Decode(&data);
    const uint32_t written = Consume(&output_rb, &data);
    output_bytes_written += written;

    // The last packet can leave several decoded subframes. If this returns with
    // PCM pending, the consumer may wait for a full block without advancing the
    // read position or re-enabling this context.
    if ((!data.IsAnyInputBufferValid() && !current_frame_remaining_subframes_) ||
        data.error_status == 4) {
      motivo_silencio = data.error_status == 4 ? 4 : 2;
      break;
    }
    // A frame's continuation may be missing while the producer refills the other
    // buffer. Keep the state and release the lock until the next Enable, without
    // spinning here or turning an input wait into an error.
    if (!written && previous_offset == data.input_buffer_read_offset &&
        previous_buffer == data.current_buffer &&
        previous_valid == (data.input_buffer_0_valid | (data.input_buffer_1_valid << 1)) &&
        previous_loop_count == data.loop_count &&
        previous_subframes == current_frame_remaining_subframes_) {
      motivo_silencio = 3;
      break;
    }
  }

  data.output_buffer_write_offset = output_rb.write_offset() / kOutputBytesPerBlock;

  // Equal pointers mean full only if PCM was written this time.
  if (output_bytes_written && output_rb.empty()) {
    data.output_buffer_valid = 0;
  }

  StoreContextMerged(data, initial_data, context_ptr);
  AnotarProduccion(output_bytes_written != 0, motivo_silencio, data);  // Measurement only
  return true;
}

void XmaContext::Enable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(true);
}

bool XmaContext::Block(bool poll) {
  if (!lock_.try_lock()) {
    if (poll) {
      return false;
    }
    lock_.lock();
  }
  lock_.unlock();
  return true;
}

void XmaContext::Clear() {
  std::lock_guard<std::mutex> lock(lock_);
  REXAPU_NOISY_DEBUG("XmaContext: reset context {}", id());

  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  XMA_CONTEXT_DATA data(context_ptr);
  ClearLocked(&data);
  data.Store(context_ptr);
}

void XmaContext::ClearLocked(XMA_CONTEXT_DATA* data) {
  data->input_buffer_0_valid = 0;
  data->input_buffer_1_valid = 0;
  data->output_buffer_valid = 0;

  data->input_buffer_read_offset = kBitsPerPacketHeader;
  data->output_buffer_read_offset = 0;
  data->output_buffer_write_offset = 0;

  ResetDecoderState();
}

void XmaContext::ResetDecoderState() {
  ReiniciarVolcadoXma(id());  // diagnostico audio_volcado_xma_s
  // A freed or re-initialized context is a new logical stream, so the previous
  // wave's MDCT overlap-add tail must not survive into frame 0 of the next one.
  // avcodec_flush_buffers() cannot drop it: ff_xmaframes_decoder declares no
  // flush callback, so the call never reaches the code clearing channel[].out.
  // Invalidating the cached format makes PrepareDecoder reopen the codec on the
  // next decode, which does discard the history.
  if (av_context_) {
    av_context_->sample_rate = 0;
    av_context_->channels = 0;
  }
  raw_frame_.fill(0);
  en_silencio_ = false;  // a new sound does not inherit the previous one's silence
  current_frame_remaining_subframes_ = 0;
  loop_frame_output_limit_ = 0;
  loop_start_skip_pending_ = false;
}

void XmaContext::Disable() {
  std::lock_guard<std::mutex> lock(lock_);
  set_is_enabled(false);
}

void XmaContext::Release() {
  std::lock_guard<std::mutex> lock(lock_);
  assert_true(is_allocated());

  set_is_allocated(false);
  ResetDecoderState();
  auto context_ptr = memory()->TranslateVirtual(guest_ptr());
  std::memset(context_ptr, 0, sizeof(XMA_CONTEXT_DATA));
}

void XmaContext::SwapInputBuffer(XMA_CONTEXT_DATA* data) {
  if (data->current_buffer == 0) {
    data->input_buffer_0_valid = 0;
  } else {
    data->input_buffer_1_valid = 0;
  }
  data->current_buffer ^= 1;
  data->input_buffer_read_offset = kBitsPerPacketHeader;
}

void XmaContext::UpdateLoopStatus(XMA_CONTEXT_DATA* data) {
  if (data->loop_count == 0) {
    return;
  }

  const uint32_t loop_start = std::max(kBitsPerPacketHeader, data->loop_start);
  const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);

  if (data->input_buffer_read_offset != loop_end) {
    return;
  }

  data->input_buffer_read_offset = loop_start;
  loop_start_skip_pending_ = true;

  if (data->loop_count < 255) {
    data->loop_count--;
  }
}

int XmaContext::GetSampleRate(int id) {
  return kIdToSampleRate[std::min(id, 3)];
}

int16_t XmaContext::GetPacketNumber(size_t size, size_t bit_offset) {
  if (bit_offset < kBitsPerPacketHeader) {
    assert_always();
    return -1;
  }
  if (bit_offset >= (size << 3)) {
    assert_always();
    return -1;
  }
  size_t byte_offset = bit_offset >> 3;
  size_t packet_number = byte_offset / kBytesPerPacket;
  return static_cast<int16_t>(packet_number);
}

uint32_t XmaContext::GetCurrentInputBufferSize(XMA_CONTEXT_DATA* data) {
  return data->GetCurrentInputBufferPacketCount() * kBytesPerPacket;
}

uint8_t* XmaContext::GetCurrentInputBuffer(XMA_CONTEXT_DATA* data) {
  return memory()->TranslatePhysical(data->GetCurrentInputBufferAddress());
}

uint32_t XmaContext::GetAmountOfBitsToRead(uint32_t remaining_stream_bits, uint32_t frame_size) {
  return std::min(remaining_stream_bits, frame_size);
}

const uint8_t* XmaContext::GetNextPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index,
                                         uint32_t current_input_packet_count) {
  uint8_t buffer_index = data->current_buffer;
  if (next_packet_index >= current_input_packet_count) {
    buffer_index ^= 1;
    next_packet_index -= current_input_packet_count;
  }

  if (!data->IsInputBufferValid(buffer_index) ||
      next_packet_index >= data->GetInputBufferPacketCount(buffer_index)) {
    return nullptr;
  }

  const uint32_t next_buffer_address = data->GetInputBufferAddress(buffer_index);
  if (!next_buffer_address) {
    REXAPU_ERROR("XmaContext {}: Buffer marked valid but has null pointer!", id());
    return nullptr;
  }

  // The skip between packets may continue inside the second buffer.
  // Always reading its start mixes the continuation with another audio stream.
  return memory()->TranslatePhysical(next_buffer_address) + next_packet_index * kBytesPerPacket;
}

uint32_t XmaContext::GetNextPacketReadOffset(uint8_t* buffer, uint32_t next_packet_index,
                                             uint32_t current_input_packet_count) {
  while (next_packet_index < current_input_packet_count) {
    uint8_t* next_packet = buffer + (next_packet_index * kBytesPerPacket);
    const uint32_t packet_frame_offset = xma::GetPacketFrameOffset(next_packet);

    // The helper already includes the 32-bit header. Compare with the packet size,
    // not with its payload: a frame can start in the last bits and continue in the
    // next packet (Decode handles that split header).
    if (packet_frame_offset < kBitsPerPacket) {
      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;
    }
    next_packet_index++;
  }

  // 32 is a valid offset: the first frame of the first packet.
  return UINT32_MAX;
}

void XmaContext::AdvanceInputPacket(XMA_CONTEXT_DATA* data, uint32_t next_packet_index) {
  // At most the two available buffers are consumed. Keep pending skips when
  // crossing them and search after packets with no new frames.
  for (unsigned buffer = 0; buffer < 2; ++buffer) {
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
    const uint32_t count = data->GetCurrentInputBufferPacketCount();
    if (!data->GetCurrentInputBufferAddress()) {
      data->error_status = 4;
      return;
    }
    if (next_packet_index < count) {
      const uint32_t offset = GetNextPacketReadOffset(
          GetCurrentInputBuffer(data), next_packet_index, count);
      if (offset != UINT32_MAX) {
        data->input_buffer_read_offset = offset;
        return;
      }
      next_packet_index = 0;
    } else {
      next_packet_index -= count;
    }
    SwapInputBuffer(data);
  }
}

memory::RingBuffer XmaContext::PrepareOutputRingBuffer(XMA_CONTEXT_DATA* data) {
  const uint32_t output_capacity = data->output_buffer_block_count * kOutputBytesPerBlock;
  const uint32_t output_read_offset = data->output_buffer_read_offset * kOutputBytesPerBlock;
  const uint32_t output_write_offset = data->output_buffer_write_offset * kOutputBytesPerBlock;

  if (output_capacity > kOutputMaxSizeBytes) {
    REXAPU_WARN(
        "XmaContext {}: Output buffer exceeds expected size! "
        "(Actual: {} Max: {})",
        id(), output_capacity, kOutputMaxSizeBytes);
  }

  uint8_t* output_buffer = memory()->TranslatePhysical(data->output_buffer_ptr);

  memory::RingBuffer output_rb(output_buffer, output_capacity);
  output_rb.set_read_offset(output_read_offset);
  output_rb.set_write_offset(output_write_offset);
  remaining_subframe_blocks_in_output_buffer_ =
      static_cast<int32_t>(output_rb.write_count()) / kOutputBytesPerBlock;

  return output_rb;
}

kPacketInfo XmaContext::GetPacketInfo(uint8_t* packet, uint32_t frame_offset) {
  kPacketInfo packet_info = {};

  const uint32_t first_frame_offset = xma::GetPacketFrameOffset(packet);
  BitStream stream(packet, kBitsPerPacket);
  stream.SetOffset(first_frame_offset);

  if (frame_offset < first_frame_offset) {
    packet_info.current_frame_ = 0;
    packet_info.current_frame_size_ = first_frame_offset - frame_offset;
  }

  while (true) {
    if (stream.BitsRemaining() < kBitsPerFrameHeader) {
      // A header that starts here may continue in the next packet. Counting it
      // avoids marking the previous frame as the last one and skipping this one.
      // Decode will join both fragments to get its full size.
      if (stream.BitsRemaining()) {
        if (stream.offset_bits() == frame_offset) {
          packet_info.current_frame_ = packet_info.frame_count_;
        }
        packet_info.frame_count_++;
      }
      break;
    }

    const uint64_t frame_size = stream.Peek(kBitsPerFrameHeader);
    if (frame_size == 0 || frame_size == xma::kMaxFrameLength) {
      break;
    }

    if (stream.offset_bits() == frame_offset) {
      packet_info.current_frame_ = packet_info.frame_count_;
      packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
    }

    packet_info.frame_count_++;

    if (frame_size > stream.BitsRemaining()) {
      break;
    }

    stream.Advance(frame_size - 1);

    if (stream.Read(1) == 0) {
      break;
    }
  }

  if (xma::IsPacketXma2Type(packet)) {
    const uint8_t xma2_frame_count = xma::GetPacketFrameCount(packet);
    if (xma2_frame_count > packet_info.frame_count_) {
      if (packet_info.current_frame_size_ == 0) {
        packet_info.current_frame_ = packet_info.frame_count_;
      }
      packet_info.frame_count_ = xma2_frame_count;
    }
  }
  return packet_info;
}

void XmaContext::StoreContextMerged(const XMA_CONTEXT_DATA& data,
                                    const XMA_CONTEXT_DATA& initial_data, uint8_t* context_ptr) {
  XMA_CONTEXT_DATA fresh(context_ptr);

  fresh.loop_count = data.loop_count;
  fresh.output_buffer_write_offset = data.output_buffer_write_offset;
  if (initial_data.input_buffer_0_valid && !data.input_buffer_0_valid) {
    fresh.input_buffer_0_valid = 0;
  }
  if (initial_data.input_buffer_1_valid && !data.input_buffer_1_valid) {
    fresh.input_buffer_1_valid = 0;
  }

  if (initial_data.output_buffer_valid && !data.output_buffer_valid) {
    fresh.output_buffer_valid = 0;
  }

  fresh.input_buffer_read_offset = data.input_buffer_read_offset;
  fresh.error_status = data.error_status;
  fresh.current_buffer = data.current_buffer;
  fresh.output_buffer_read_offset = data.output_buffer_read_offset;

  fresh.Store(context_ptr);
}

uint32_t XmaContext::Consume(memory::RingBuffer* output_rb, const XMA_CONTEXT_DATA* data) {
  if (!current_frame_remaining_subframes_) {
    return 0;
  }

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    if (consumed >= loop_frame_output_limit_) {
      remaining_subframe_blocks_in_output_buffer_ -= data->output_buffer_padding;
      current_frame_remaining_subframes_ = 0;
      loop_frame_output_limit_ = 0;
      return 0;
    }
  }

  const uint8_t effective_sdc = std::max(static_cast<uint32_t>(1), data->subframe_decode_count);
  int8_t subframes_to_write = std::min(static_cast<int8_t>(current_frame_remaining_subframes_),
                                       static_cast<int8_t>(effective_sdc));

  if (loop_frame_output_limit_ > 0) {
    const uint8_t total_subframes = (kBytesPerFrameChannel / kOutputBytesPerBlock)
                                    << data->is_stereo;
    const uint8_t consumed = total_subframes - current_frame_remaining_subframes_;
    const int8_t remaining_until_limit = static_cast<int8_t>(loop_frame_output_limit_ - consumed);
    if (subframes_to_write > remaining_until_limit) {
      subframes_to_write = remaining_until_limit;
    }
  }

  // The no-input path also goes through here. Do not drop samples that do not
  // fit: keep them until the consumer frees space.
  if (remaining_subframe_blocks_in_output_buffer_ < subframes_to_write ||
      output_rb->write_count() < uint32_t(subframes_to_write) * kOutputBytesPerBlock) {
    return 0;
  }

  const int8_t raw_frame_read_offset =
      ((kBytesPerFrameChannel / kOutputBytesPerBlock) << data->is_stereo) -
      current_frame_remaining_subframes_;

  const uint32_t written = static_cast<uint32_t>(output_rb->Write(
      raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset),
      subframes_to_write * kOutputBytesPerBlock));
  // Diagnostic (audio_volcado_xma_s): the blocks as they reach the game's buffer.
  VolcarXma(id(), 1, GetSampleRate(data->sample_rate), data->is_stereo ? 2 : 1,
            raw_frame_.data() + (kOutputBytesPerBlock * raw_frame_read_offset), written);

  const int8_t headroom = (current_frame_remaining_subframes_ - subframes_to_write == 0)
                              ? data->output_buffer_padding
                              : 0;

  remaining_subframe_blocks_in_output_buffer_ -= subframes_to_write + headroom;
  current_frame_remaining_subframes_ -= subframes_to_write;
  return written;
}

int XmaContext::PrepareDecoder(int sample_rate, bool is_two_channel) {
  sample_rate = GetSampleRate(sample_rate);

  uint32_t channels = is_two_channel ? 2 : 1;
  if (av_context_->sample_rate != sample_rate ||
      av_context_->channels != static_cast<int>(channels)) {
    REXAPU_NOISY_DEBUG("XmaContext {}: Codec reinit: rate {} -> {}, channels {} -> {}", id(),
                       av_context_->sample_rate, sample_rate, av_context_->channels, channels);
    avcodec_free_context(&av_context_);
    av_context_ = avcodec_alloc_context3(av_codec_);

    av_context_->sample_rate = sample_rate;
    av_context_->channels = channels;
    av_context_->flags2 |= AV_CODEC_FLAG2_SKIP_MANUAL;

    if (avcodec_open2(av_context_, av_codec_, NULL) < 0) {
      REXAPU_ERROR("XmaContext: Failed to reopen FFmpeg context");
      return -1;
    }
    return 1;
  }
  return 0;
}

void XmaContext::PreparePacket(uint32_t frame_size, uint32_t frame_padding) {
  av_packet_->data = xma_frame_.data();
  av_packet_->size = static_cast<int>(1 + ((frame_padding + frame_size) / 8) +
                                      (((frame_padding + frame_size) % 8) ? 1 : 0));

  auto padding_end = av_packet_->size * 8 - (8 + frame_padding + frame_size);
  assert_true(padding_end < 8);
  xma_frame_[0] = ((frame_padding & 7) << 5) | ((padding_end & 7) << 2);
}

bool XmaContext::DecodePacket(AVCodecContext* av_context, const AVPacket* av_packet,
                              AVFrame* av_frame) {
  auto ret = avcodec_send_packet(av_context, av_packet);
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error sending packet for decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  ret = avcodec_receive_frame(av_context, av_frame);

  if (ret == AVERROR(EAGAIN)) {
    return false;
  }
  if (ret < 0) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, errbuf, sizeof(errbuf));
    REXAPU_ERROR("XmaContext {}: Error during decoding: {} ({})", id(), errbuf, ret);
    return false;
  }
  return true;
}

void XmaContext::Decode(XMA_CONTEXT_DATA* data) {
  SCOPE_profile_cpu_f("apu");

  if (!data->IsAnyInputBufferValid()) {
    return;
  }

  if (current_frame_remaining_subframes_ > 0) {
    return;
  }

  if (!data->IsCurrentInputBufferValid()) {
    SwapInputBuffer(data);
    if (!data->IsCurrentInputBufferValid()) {
      return;
    }
  }

  uint8_t* current_input_buffer = GetCurrentInputBuffer(data);

  input_buffer_.fill(0);

  // Loop-end frame: decode it here (output limited to loop_subframe_end),
  // jump to loop_start afterwards in the next-offset step.
  bool is_loop_end_frame = false;
  if (data->loop_count > 0) {
    const uint32_t loop_end = std::max(kBitsPerPacketHeader, data->loop_end);
    is_loop_end_frame = (data->input_buffer_read_offset == loop_end);
  }

  if (!data->output_buffer_block_count) {
    REXAPU_ERROR("XmaContext {}: Error - Received 0 for output_buffer_block_count!", id());
    return;
  }

  if (data->input_buffer_read_offset < kBitsPerPacketHeader) {
    data->input_buffer_read_offset = kBitsPerPacketHeader;
  }

  const uint32_t current_input_size = GetCurrentInputBufferSize(data);
  const uint32_t current_input_packet_count = current_input_size / kBytesPerPacket;

  const int16_t packet_index = GetPacketNumber(current_input_size, data->input_buffer_read_offset);

  if (packet_index == -1) {
    REXAPU_ERROR("XmaContext {}: Invalid packet index. Input read offset: {}", id(),
                 static_cast<uint32_t>(data->input_buffer_read_offset));
    // Work must publish the error and release the lock. Without this state, it
    // repeated Decode forever with the same offset outside the input.
    data->error_status = 4;
    return;
  }

  uint8_t* packet = current_input_buffer + (packet_index * kBytesPerPacket);
  const uint32_t packet_first_frame_offset = xma::GetPacketFrameOffset(packet);
  uint32_t relative_offset = data->input_buffer_read_offset % kBitsPerPacket;

  if (relative_offset < packet_first_frame_offset) {
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + packet_first_frame_offset;
    relative_offset = packet_first_frame_offset;
  }

  const uint8_t skip_count = xma::GetPacketSkipCount(packet);

  // Full packet skip (0xFF) -- no new frames begin in this packet.
  if (skip_count == 0xFF) {
    AdvanceInputPacket(data, packet_index + 1);
    return;
  }

  kPacketInfo packet_info = GetPacketInfo(packet, relative_offset);
  const uint32_t packet_to_skip = skip_count + 1;
  const uint32_t next_packet_index = packet_index + packet_to_skip;

  // Frame header split across packet boundary.
  if (packet_info.current_frame_size_ == 0) {
    const uint8_t* next_packet = GetNextPacket(data, next_packet_index, current_input_packet_count);
    if (!next_packet) {
      // A split header still belongs to the current buffer. Do not release it
      // before the producer publishes its continuation in the other buffer.
      if (kBitsPerPacket - relative_offset < kBitsPerFrameHeader &&
          next_packet_index >= current_input_packet_count &&
          !data->IsInputBufferValid(data->current_buffer ^ 1)) {
        return;
      }
      SwapInputBuffer(data);
      return;
    }
    std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);
    std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                kBytesPerPacketData);

    BitStream combined(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
    combined.SetOffset(relative_offset - kBitsPerPacketHeader);

    uint64_t frame_size = combined.Peek(kBitsPerFrameHeader);
    if (frame_size == xma::kMaxFrameLength) {
      data->error_status = 4;
      return;
    }
    packet_info.current_frame_size_ = static_cast<uint32_t>(frame_size);
  }

  BitStream stream(current_input_buffer, (packet_index + 1) * kBitsPerPacket);
  stream.SetOffset(data->input_buffer_read_offset);

  const uint64_t bits_to_copy = GetAmountOfBitsToRead(static_cast<uint32_t>(stream.BitsRemaining()),
                                                      packet_info.current_frame_size_);

  if (bits_to_copy == 0) {
    REXAPU_ERROR("XmaContext {}: There are no bits to copy!", id());
    SwapInputBuffer(data);
    return;
  }

  if (packet_info.isLastFrameInPacket()) {
    if (stream.BitsRemaining() < packet_info.current_frame_size_) {
      const uint8_t* next_packet =
          GetNextPacket(data, next_packet_index, current_input_packet_count);
      if (!next_packet) {
        if (next_packet_index >= current_input_packet_count &&
            !data->IsInputBufferValid(data->current_buffer ^ 1)) {
          return;
        }
        data->error_status = 4;
        return;
      }
      std::memcpy(input_buffer_.data() + kBytesPerPacketData, next_packet + kBytesPerPacketHeader,
                  kBytesPerPacketData);
    }
  }

  std::memcpy(input_buffer_.data(), packet + kBytesPerPacketHeader, kBytesPerPacketData);

  stream = BitStream(input_buffer_.data(), (kBitsPerPacket - kBitsPerPacketHeader) * 2);
  stream.SetOffset(relative_offset - kBitsPerPacketHeader);

  xma_frame_.fill(0);

  const uint32_t padding_start =
      static_cast<uint8_t>(stream.Copy(xma_frame_.data() + 1, packet_info.current_frame_size_));

  raw_frame_.fill(0);

  if (PrepareDecoder(data->sample_rate, bool(data->is_stereo)) < 0) {
    data->error_status = 4;
    return;
  }
  PreparePacket(packet_info.current_frame_size_, padding_start);
  const bool decoded = DecodePacket(av_context_, av_packet_, av_frame_);
  if (decoded) {
    ConvertFrame(reinterpret_cast<const uint8_t**>(&av_frame_->data), bool(data->is_stereo),
                 raw_frame_.data());
    // Diagnostic (audio_volcado_xma_s): the whole frame as it comes out of FFmpeg.
    VolcarXma(id(), 0, GetSampleRate(data->sample_rate), data->is_stereo ? 2 : 1, raw_frame_.data(),
              size_t(kBytesPerFrameChannel) << data->is_stereo);

    // NFSMW discards 384 samples at the start of each sound in sub_825E1CD0.
    // Deliver the decoder's whole block: shifting it by 192 samples and holding
    // its tail until the next frame also dropped a final block of 512. The
    // consumer ran out of input and waited forever.
    current_frame_remaining_subframes_ = 4 << data->is_stereo;
    loop_frame_output_limit_ =
        is_loop_end_frame ? static_cast<uint8_t>((data->loop_subframe_end + 1) << data->is_stereo)
                          : 0;
    const uint8_t decoded_start_skip =
        loop_start_skip_pending_ ? static_cast<uint8_t>(data->loop_subframe_skip << data->is_stereo)
                                 : 0;
    loop_start_skip_pending_ = false;
    current_frame_remaining_subframes_ -=
        std::min(decoded_start_skip, current_frame_remaining_subframes_);
  }

  // Compute where to go next.
  if (is_loop_end_frame) {
    UpdateLoopStatus(data);
    return;
  }

  if (!packet_info.isLastFrameInPacket()) {
    const uint32_t next_frame_offset =
        (data->input_buffer_read_offset + bits_to_copy) % kBitsPerPacket;
    data->input_buffer_read_offset = (packet_index * kBitsPerPacket) + next_frame_offset;
    return;
  }

  AdvanceInputPacket(data, next_packet_index);
}

void XmaContext::ConvertFrame(const uint8_t** samples, bool is_two_channel,
                              uint8_t* output_buffer) {
  // Loop through every sample, convert and drop it into the output array.
  // If more than one channel, we need to interleave the samples from each
  // channel next to each other. Always saturate because FFmpeg output is
  // not limited to [-1, 1] (for example 1.095 as seen in 5454082B).
  constexpr float scale = (1 << 15) - 1;
  auto out = reinterpret_cast<int16_t*>(output_buffer);

  // For testing of vectorized versions, stereo audio is common in 4D5307E6,
  // since the first menu frame; the intro cutscene also has more than 2
  // channels.
#if REX_ARCH_AMD64
  static_assert(kSamplesPerFrame % 8 == 0);
  const auto in_channel_0 = reinterpret_cast<const float*>(samples[0]);
  const __m128 scale_mm = _mm_set1_ps(scale);
  if (is_two_channel) {
    const auto in_channel_1 = reinterpret_cast<const float*>(samples[1]);
    const __m128i shufmask = _mm_set_epi8(14, 15, 6, 7, 12, 13, 4, 5, 10, 11, 2, 3, 8, 9, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 4) {
      // Load 8 samples, 4 for each channel.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_1[i]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Interleave channels and byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 4] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i * 2]), out_mm);
    }
  } else {
    const __m128i shufmask = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
    for (uint32_t i = 0; i < kSamplesPerFrame; i += 8) {
      // Load 8 samples, as [in_channel_0 + i * 4] and
      // [in_channel_0 + i * 4 + 16] movups.
      __m128 in_mm0 = _mm_loadu_ps(&in_channel_0[i]);
      __m128 in_mm1 = _mm_loadu_ps(&in_channel_0[i + 4]);
      // Rescale.
      in_mm0 = _mm_mul_ps(in_mm0, scale_mm);
      in_mm1 = _mm_mul_ps(in_mm1, scale_mm);
      // Cast to int32.
      __m128i out_mm0 = _mm_cvtps_epi32(in_mm0);
      __m128i out_mm1 = _mm_cvtps_epi32(in_mm1);
      // Saturated cast and pack to int16.
      __m128i out_mm = _mm_packs_epi32(out_mm0, out_mm1);
      // Byte swap.
      out_mm = _mm_shuffle_epi8(out_mm, shufmask);
      // Store, as [out + i * 2] movdqu.
      _mm_storeu_si128(reinterpret_cast<__m128i*>(&out[i]), out_mm);
    }
  }
#else
  uint32_t o = 0;
  for (uint32_t i = 0; i < kSamplesPerFrame; i++) {
    for (uint32_t j = 0; j <= uint32_t(is_two_channel); j++) {
      // Select the appropriate array based on the current channel.
      auto in = reinterpret_cast<const float*>(samples[j]);

      // Raw samples sometimes aren't within [-1, 1]
      float scaled_sample = rex::clamp_float(in[i], -1.0f, 1.0f) * scale;

      // Convert the sample and output it in big endian.
      auto sample = static_cast<int16_t>(scaled_sample);
      out[o++] = rex::byte_swap(sample);
    }
  }
#endif
}

}  // namespace rex::audio
