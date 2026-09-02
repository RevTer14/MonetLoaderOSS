#include "compat.h"
#include "file_manager.h"
#include "logger.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/ostreamwrapper.h"
#include "rapidjson/prettywriter.h"
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace {
void set_default_profile(rapidjson::Document& doc)
{
  doc["gtasa_name"].SetString("libGTASA.so", doc.GetAllocator());
  doc["compat_scripts"].SetArray();

#if INTPTR_MAX == INT64_MAX
  doc["profile_name"].SetString("default GTA:2.1 SAMP:SCH-1.3", doc.GetAllocator());
  doc["samp_name"].SetString("libsamp.so", doc.GetAllocator());
  doc["receiveignorerpc_pattern"].SetString(
    "FFC301D1E80B00FDFDFB01A9FD630091FB1700F9FA6703A9F85F04A9F65705A9F44F06A9080040F9F30300AA082D40F900013FD6",
    doc.GetAllocator());
  doc["cnetgame_ctor_pattern"].SetString(
    "FF0304D1FD7B0BA9FDC30291FA670CA9F85F0DA9F6570EA9F44F0FA9F40304AA",
    doc.GetAllocator());
  doc["rakclientinterface_netgame_offset"] = 536;
  doc["use_samp_touch_workaround"] = true;
  doc["nveventinsertnewest_offset"] = 3362884;
#else
  doc["profile_name"].SetString("default GTA:2.0 SAMP:ARZ-15.0.8", doc.GetAllocator());
  doc["samp_name"].SetString("libsamp.so", doc.GetAllocator());
  doc["receiveignorerpc_pattern"].SetString(
      "F0B503AF2DE900????B004460068C16A20468847",
      doc.GetAllocator());
  doc["cnetgame_ctor_pattern"].SetString(
      "F0B503AF2DE9000788B00D46????9146????0446002079447A44",
      doc.GetAllocator());
  doc["rakclientinterface_netgame_offset"] = 528;
  doc["use_samp_touch_workaround"] = true;
  doc["nveventinsertnewest_offset"] = 0x27C4F0;
#endif
}
}

void compat::init()
{
  std::filesystem::path path = file_manager::public_external_storage / "monetloader/compat/profile.json";

  // set default profile

  rapidjson::Document doc {};
  set_default_profile(doc);

  // try to load user profile

  if (!std::filesystem::exists(path)) {
    // if profile does not exists, write default one
    std::filesystem::create_directories(path.parent_path());

    std::ofstream f { path };
    if (f) {
      rapidjson::OStreamWrapper f_wrap { f };
      rapidjson::PrettyWriter<rapidjson::OStreamWrapper> out { f_wrap };
      doc.Accept(out);
    }

    f.clear();
    f.close();
  } else {
    // load user profile

    std::ifstream f { path };
    if (f) {
      rapidjson::IStreamWrapper f_wrap { f };
      rapidjson::ParseResult result = doc.ParseStream(f_wrap);
      if (result.Code() != rapidjson::kParseErrorNone) {
        logger::log<logger::LV_ERROR, false>(nullptr,
            "{}: {} (at: {})",
            "failed to parse compat profile file",
            rapidjson::GetParseError_En(result.Code()),
            result.Offset());
      }
    } else {
      logger::log<logger::LV_ERROR, false>(nullptr,
          "{}", "failed to open compat profile file, using default");
    }

    f.clear();
    f.close();
  }

  // validate profile

  const char* s_gtasa_name = "gtasa_name";
  const char* s_compat_scripts = "compat_scripts";
  const char* s_profile_name = "profile_name";
  const char* s_samp_name = "samp_name";
  const char* s_receiveignorerpc_pattern = "receiveignorerpc_pattern";
  const char* s_cnetgame_ctor_pattern = "cnetgame_ctor_pattern";
  const char* s_rakclientinterface_netgame_offset = "rakclientinterface_netgame_offset";
  const char* s_use_samp_touch_workaround = "use_samp_touch_workaround";
  const char* s_nveventinsertnewest_offset = "nveventinsertnewest_offset";

  if (!doc.HasMember(s_gtasa_name) || !doc[s_gtasa_name].IsString()
      || !doc.HasMember(s_compat_scripts) || !doc[s_compat_scripts].IsArray()
      || !doc.HasMember(s_profile_name) || !doc[s_profile_name].IsString()
      || !doc.HasMember(s_samp_name) || !doc[s_samp_name].IsString()
      || !doc.HasMember(s_receiveignorerpc_pattern) || !doc[s_receiveignorerpc_pattern].IsString()
      || !doc.HasMember(s_cnetgame_ctor_pattern) || !doc[s_cnetgame_ctor_pattern].IsString()
      || !doc.HasMember(s_rakclientinterface_netgame_offset) || !doc[s_rakclientinterface_netgame_offset].IsNumber()
      || !doc.HasMember(s_use_samp_touch_workaround) || !doc[s_use_samp_touch_workaround].IsBool()
      || !doc.HasMember(s_nveventinsertnewest_offset) || !doc[s_nveventinsertnewest_offset].IsNumber()) {
    logger::log<logger::LV_ERROR, false>(nullptr,
        "{}", "invalid compat profile, using default");
    set_default_profile(doc);
  }

  // parse profile

  gtasa_name = doc[s_gtasa_name].GetString();

  compat_scripts.clear();
  for (auto& i : doc[s_compat_scripts].GetArray()) {
    if (i.IsString()) {
      compat_scripts.push_back(i.GetString());
    }
  }

  profile_name = doc[s_profile_name].GetString();
  samp_name = doc[s_samp_name].GetString();
  receiveignorerpc_pattern = doc[s_receiveignorerpc_pattern].GetString();
  cnetgame_ctor_pattern = doc[s_cnetgame_ctor_pattern].GetString();
  rakclientinterface_netgame_offset = static_cast<std::uintptr_t>(doc[s_rakclientinterface_netgame_offset].GetDouble());
  use_samp_touch_workaround = doc[s_use_samp_touch_workaround].GetBool();
  nveventinsertnewest_offset = static_cast<std::uintptr_t>(doc[s_nveventinsertnewest_offset].GetDouble());

  logger::log<logger::LV_SYSTEM, false>(nullptr, "{}: {}",
      "loaded compat profile",
      profile_name);
}
