// fh2 - world detail ("mundo" means "world"): grass, objects, reflections, shadows and crowd, each from 0.25x to 4x
// of the Xbox 360's, and all but objects also off (0).
//
//   --fh2_mundo_grama=2 --fh2_mundo_objetos=1.5 --fh2_mundo_reflexos=1 --fh2_mundo_sombras=2 --fh2_mundo_multidao=0.5
//
// Grass is the procedural vegetation, the bushes and tufts of the open world: each plant fades out between two
// distances its vertex shader gets in FadeValues (52.5 to 97.5 m, 17.5 to 62.5 m), from the game's code, not from a
// file. The native renderer scales them in each draw's constants, and with 0 draws none
// (nfsmw_nativo_dibujos.cpp, EntradaShader::fh2_fade_vegetacion). The emulated renderer keeps the game's.
//
// The rest are read by the open world from media\tracks\Anthem\TrackSettings.xml, a plain XML file the
// game does not check (renderscenarios.zip, by contrast, is a "secure file" whose size the game verifies). At
// start, before the game runs, the original is read through the VFS (a folder or the disc image alike), the chosen
// factors are applied and the result is written to <cache or user folder>/fh2_mundo/TrackSettings.xml, mounted
// read-only as \Device\Fh2Mundo and served in place of the original (VirtualFileSystem::RegisterFileOverride).
// With those factors at 1 nothing is mounted and the game reads its own file. Its Grass* keys are not used: they
// drive shell grass, which the open world has off (GrassMaxNumShells 0); 0.25x and 4x changed nothing on screen.
//
// What each factor scales:
//   objects     screenAreaDistances mainScene (350) and depth (60): the level of detail of the scene
//   reflections the dynamic cubemap: FarDistance (200 m), MeshLimit (300), MeshSizeThreshold (divided),
//               screenAreaDistances cubeMap and mirror. Off: the cubemap is not drawn at all (DynamicCubemap 0;
//               +11 % on the Moto G9 Play in the festival, the car paint turns flat)
//   shadows     ShadowEnds InGameShadowEnd (202 m) and TrackSegCamShadowEnd (replays). Off: no car casts a projected
//               shadow, and the launcher hands the native renderer a 0 % shadow map (nfsmw_nativo_sombras_escala),
//               which is what takes the sun's shadows away: an InGameShadowEnd of 1 m left them on screen in both
//               renderers
//   crowd       CrowdSize (1024), Crowds lod0-lod3 (5/20/50/125 m) and SpriteDistance (60 m). Off (0) is the native
//               renderer's: it skips the crowd's draws (PROC_CHARACTER_* vertex shaders; CrowdSize 0 still left
//               people in the festival), with the file at 0.25x for either renderer

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/runtime.h>

REXCVAR_DEFINE_DOUBLE(fh2_mundo_grama, 1.0, "FH2",
                      "World detail: how far bushes and tufts are drawn, 0 (none) to 4 times the Xbox 360's (native "
                      "renderer)")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(fh2_mundo_objetos, 1.0, "FH2",
                      "World detail: how far objects keep their detail, 0.25 to 4 times the Xbox 360's")
    .range(0.25, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(fh2_mundo_reflexos, 1.0, "FH2",
                      "World detail: what the car reflections show, 0 (no live reflections) to 4 times the Xbox 360's")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(fh2_mundo_sombras, 1.0, "FH2", "World detail: how far shadows reach, 0 (none) to 4 times the Xbox 360's")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(fh2_mundo_multidao, 1.0, "FH2",
                      "World detail: how many people and how far, 0 (none, native renderer) to 4 times the Xbox 360's")
    .range(0.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace {

constexpr const char* kGamePath = "game:\\media\\tracks\\Anthem\\TrackSettings.xml";
constexpr const char* kMountPath = "\\Device\\Fh2Mundo";
constexpr const char* kFileName = "TrackSettings.xml";

bool ReadGuestFile(rex::filesystem::VirtualFileSystem* vfs, const char* path, std::string& out) {
  rex::filesystem::Entry* entry = vfs->ResolvePath(path);
  if (!entry) {
    return false;
  }
  rex::filesystem::File* file = nullptr;
  if (XFAILED(entry->Open(rex::filesystem::FileAccess::kFileReadData, &file)) || !file) {
    return false;
  }
  out.resize(entry->size());
  size_t read = 0;
  const rex::X_STATUS status =
      file->ReadSync(std::span<uint8_t>(reinterpret_cast<uint8_t*>(out.data()), out.size()), 0, &read);
  file->Destroy();
  out.resize(read);
  return XSUCCEEDED(status) && read > 0;
}

// The attribute `attr` of the first <tag ...> element, rewritten by `change`; false when it is not there.
template <typename Change>
bool EditAttribute(std::string& xml, const char* tag, const char* attr, Change change) {
  const std::regex pattern(std::string("(<") + tag + "\\b[^>]*?\\b" + attr + "=\")([^\"]*)(\")");
  std::smatch match;
  if (!std::regex_search(xml, match, pattern)) {
    REXLOG_WARN("[mundo] {} {} not found in TrackSettings.xml", tag, attr);
    return false;
  }
  const std::string value = change(match[2].str());
  xml.replace(match.position(2), match.length(2), value);
  return true;
}

std::string Decimal(double value) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.6f", value);
  return text;
}

void ScaleFloat(std::string& xml, const char* tag, const char* attr, double factor) {
  EditAttribute(xml, tag, attr, [&](const std::string& v) { return Decimal(std::strtod(v.c_str(), nullptr) * factor); });
}

void ScaleInteger(std::string& xml, const char* tag, const char* attr, double factor) {
  EditAttribute(xml, tag, attr, [&](const std::string& v) {
    const long scaled = std::lround(std::strtod(v.c_str(), nullptr) * factor);
    return std::to_string(std::max(1L, scaled));
  });
}

void Set(std::string& xml, const char* tag, const char* attr, const char* value) {
  EditAttribute(xml, tag, attr, [&](const std::string&) { return std::string(value); });
}

void ApplyFactors(std::string& xml, double objects, double reflections, double shadows, double crowd) {
  ScaleFloat(xml, "screenAreaDistances", "mainScene", objects);
  ScaleFloat(xml, "screenAreaDistances", "depth", objects);

  if (reflections <= 0) {
    Set(xml, "Performance", "DynamicCubemap", "0");
  } else {
    ScaleFloat(xml, "Cubemap", "FarDistance", reflections);
    ScaleInteger(xml, "Cubemap", "MeshLimit", reflections);
    ScaleFloat(xml, "Cubemap", "MeshSizeThreshold", 1.0 / reflections);
    ScaleFloat(xml, "screenAreaDistances", "cubeMap", reflections);
    ScaleFloat(xml, "screenAreaDistances", "mirror", reflections);
  }

  if (shadows <= 0) {
    Set(xml, "Performance", "NonFocusCarProjectedShadows", "0");
    Set(xml, "Performance", "FocusCarProjectedShadows", "0");
  } else {
    ScaleFloat(xml, "ShadowEnds", "InGameShadowEnd", shadows);
    ScaleFloat(xml, "ShadowEnds", "TrackSegCamShadowEnd", shadows);
  }

  ScaleInteger(xml, "Performance", "CrowdSize", crowd);
  for (const char* attr : {"lod0", "lod1", "lod2", "lod3", "SpriteDistance"}) {
    ScaleFloat(xml, "Crowds", attr, crowd);
  }
}

}  // namespace

// Called from OnPostSetup (fh2_app.h): the VFS is mounted and the game has not started.
void Fh2MundoAplicar(rex::Runtime* runtime) {
  const double objects = REXCVAR_GET(fh2_mundo_objetos);
  const double reflections = REXCVAR_GET(fh2_mundo_reflexos);
  const double shadows = REXCVAR_GET(fh2_mundo_sombras);
  // Off is the native renderer's (it skips the crowd's draws); the file gets the lowest factor.
  const double crowd = std::max(0.25, REXCVAR_GET(fh2_mundo_multidao));
  if (!runtime || (objects == 1 && reflections == 1 && shadows == 1 && crowd == 1)) {
    return;
  }
  rex::filesystem::VirtualFileSystem* vfs = runtime->file_system();
  std::string xml;
  if (!vfs || !ReadGuestFile(vfs, kGamePath, xml)) {
    REXLOG_ERROR("[mundo] {} could not be read: the game keeps its own detail", kGamePath);
    return;
  }
  ApplyFactors(xml, objects, reflections, shadows, crowd);

  const std::filesystem::path base = !runtime->cache_root().empty() ? runtime->cache_root() : runtime->user_data_root();
  const std::filesystem::path folder = base / "fh2_mundo";
  std::error_code ec;
  std::filesystem::create_directories(folder, ec);
  {
    std::ofstream out(folder / kFileName, std::ios::binary | std::ios::trunc);
    out.write(xml.data(), std::streamsize(xml.size()));
    if (!out) {
      REXLOG_ERROR("[mundo] {} could not be written: the game keeps its own detail", (folder / kFileName).string());
      return;
    }
  }
  auto device = std::make_unique<rex::filesystem::HostPathDevice>(kMountPath, folder, true);
  if (!device->Initialize() || !vfs->RegisterDevice(std::move(device))) {
    REXLOG_ERROR("[mundo] {} could not be mounted: the game keeps its own detail", folder.string());
    return;
  }
  vfs->RegisterFileOverride(kGamePath, std::string(kMountPath) + "\\" + kFileName);
  REXLOG_INFO("[mundo] objects {}x, reflections {}x, shadows {}x, crowd {}x (TrackSettings.xml from {})", objects,
              reflections, shadows, crowd, folder.string());
}
