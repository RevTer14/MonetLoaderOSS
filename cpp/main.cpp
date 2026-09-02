#include "aml_stuff.h"
#include "bass_manager.h"
#include "compat.h"
#include "file_manager.h"
#include "game/CEntity.h"
#include "game/RenderWare.h"
#include "game/ScriptCommands.h"
#include "game/audiostream_sync.h"
#include "game/common.h"
#include "game/customgxt.h"
#include "game/netinfo.h"
#include "game/rakhook.h"
#include "gui/render.h"
#include "gui/touch_handler.h"
#include "hook/monethook.h"
#include "lib_manager.h"
#include "logger.h"
#include "offsets.h"
#include "raknet/RakClientInterface.h"
#include "script/modules/mimgui/renderer.h"
#include "script/script_manager.h"
#include "utils/android.h"
#include <chrono>
#include <dlfcn.h>
#include <jni.h>
#include <spdlog/spdlog.h>
#include <thread>

namespace {
#if SAMP_MODE >= 1
// ------ NetInfo ------
bool g_can_process_netinfo = false;

monethook::hook<bool(void*)> o_crunningscript_process;
bool h_crunningscript_process(void* self)
{
  g_can_process_netinfo = true;
  bool result = o_crunningscript_process(self);
  g_can_process_netinfo = false;
  return result;
}

monethook::plt_hook<void(CEntity*)> o_cworld_add;
void h_cworld_add(CEntity* entity)
{
  o_cworld_add(entity);
  game::netinfo::add(entity); // If entity is added after player or vehicle RPC add, it is them
}

monethook::plt_hook<void(CEntity*)> o_cworld_remove;
void h_cworld_remove(CEntity* entity)
{
  game::netinfo::remove(entity); // If entity is removed after player or vehicle RPC remove, it is them
  o_cworld_remove(entity);
}
#endif

// ------ Touch handling ------
struct touch_info {
  int type;
  int num;
  int x, y;
};
std::mutex g_touch_queue_lock;
std::deque<touch_info> g_touch_queue;

void submit_touch(int type, int num, int x, int y);

monethook::hook<void(int, int, int, int)> o_and_touchevent;
void h_and_touchevent(int type, int num, int x, int y)
{
  if (lua::script_manager::initialized()) {
    submit_touch(type == 4 ? touch_handler::touch_type::POP : type, num, x, y);
  } else {
    o_and_touchevent(type, num, x, y);
  }
}

#if SAMP_MODE >= 1
struct NvVec2 {
  float x, y;

  bool operator==(const NvVec2& rhs) const
  {
    return x == rhs.x && y == rhs.y;
  }
  bool operator!=(const NvVec2& rhs) const
  {
    return !(*this == rhs);
  }
};
struct NvEvent {
  int type; // 4 - multitouch
  int mt_flags; // 0xFF00 - pointers, 0x00FF - action
  NvVec2 posn[4]; // 3rd and 4th fingers don't really belong here, but they belong according to Arizona layout
};
monethook::hook<void(NvEvent*)> o_nveventinsertnewest;
void h_nveventinsertnewest(NvEvent* ev)
{
  static NvVec2 pointers_posns[4];
  if (!ev || ev->type != 4) {
    return o_nveventinsertnewest(ev);
  }

  int action = ev->mt_flags & 0x00FF;
  if (action == 4) { // cancel
    action = touch_handler::touch_type::POP;
  }
  int num = (ev->mt_flags & 0xFF00) >> 8;

  if (action != touch_handler::touch_type::MOVE) {
    submit_touch(action, num, static_cast<int>(ev->posn[num].x), static_cast<int>(ev->posn[num].y));
  }

  for (int i = 0; i < 4; ++i) {
    if (ev->posn[i] != pointers_posns[i]) {
      if (action != touch_handler::touch_type::MOVE && i == num) {
        pointers_posns[i] = ev->posn[i];
        continue;
      }

      submit_touch(touch_handler::touch_type::MOVE, i, static_cast<int>(ev->posn[i].x), static_cast<int>(ev->posn[i].y));
      pointers_posns[i] = ev->posn[i];
    }
  }
}
#endif

bool on_touch(int type, int num, int x, int y)
{
  if (gui::render::handle_touch(type, num, x, y)) {
    return true;
  }
  if (lua::mimgui::handle_touch(type, num, x, y)) {
    return true;
  }

  bool consumed = false;
  lua::script_manager::for_each_event_handler("onTouch",
      [&consumed, type, num, x, y](sol::protected_function fn) -> std::optional<sol::error> {
        auto pfr = fn(type, num, x, y);
        if (!pfr.valid()) {
          sol::error e = pfr;
          return e;
        }

        auto t_bool = pfr.get<std::optional<bool>>();
        if (t_bool.has_value() && *t_bool == false) {
          consumed = true;
        }
        return std::nullopt;
      });

  return consumed;
}

void submit_touch(int type, int num, int x, int y)
{
  std::lock_guard<std::mutex> lock { g_touch_queue_lock };
  g_touch_queue.emplace_back(type, num, x, y);
}

void process_touch()
{
  std::lock_guard<std::mutex> lock { g_touch_queue_lock };

  for (auto& i : g_touch_queue) {
    if (!on_touch(i.type, i.num, i.x, i.y)) {
      using and_touchevent_t = void (*)(int, int, int, int);
      if (o_and_touchevent.applied())
        o_and_touchevent(i.type, i.num, i.x, i.y);
      else
        (*reinterpret_cast<and_touchevent_t*>(lib_manager::got_and_touchevent))(i.type, i.num, i.x, i.y);
    }
  }

  g_touch_queue.clear();
}

// ------ Script Processing ------
bool g_had_run_lua;
bool g_game_restarted;

monethook::plt_hook<void()> o_cgame_process;
void h_cgame_process()
{
  g_had_run_lua = false;
  process_touch();
  game::audiostream_sync::process();
  rakhook::process(); // Safe to call even before RakHook is initialized since it only checks queue which is filled after init
  game::netinfo::process();
  o_cgame_process();
  if (!g_had_run_lua) { // Scripts are not processed in pause
    if (g_game_restarted) {
      lua::script_manager::on_game_restart();
      g_game_restarted = false;
    }

    lua::script_manager::run(game::IsPaused());
  }
}

monethook::plt_hook<void()> o_cthescripts_init;
void h_cthescripts_init()
{
  g_game_restarted = true; // Scripts are being reinitialized
  o_cthescripts_init();
}

monethook::plt_hook<void()> o_cthescripts_process;
void h_cthescripts_process()
{
  static bool once = false;

  o_cthescripts_process();
  if (!once) {
#ifdef NEW_INPUT_SYSTEM
    {
      andutils::java_getter java { };
      andutils::jni::input_init(java.env());
    }
#endif

    game::scripting::InitSafeProcessing();
    lua::script_manager::init();
    g_game_restarted = false;
    once = true;
  } else {
    if (g_game_restarted) {
      lua::script_manager::on_game_restart();
      g_game_restarted = false;
    }
    lua::script_manager::run(game::IsPaused());
  }
  g_had_run_lua = true;
}

monethook::plt_hook<void()> o_cgame_shutdown;
void h_cgame_shutdown()
{
  lua::script_manager::shutdown();
  o_cgame_shutdown();
}

// ------ Render ------
bool g_has_to_do_render;

monethook::plt_hook<void()> o_chid_flushqueuedtext;
void h_chid_flushqueuedtext()
{
  o_chid_flushqueuedtext();
  g_has_to_do_render = true; // We need to render in CameraEndUpdate to render after Arizona
}

monethook::hook<rw::RwBool(void*, void*, std::int32_t)> o_opengl_cameraendupdate;
rw::RwBool h_opengl_cameraendupdate(void* out, void* camera_in, std::int32_t in)
{
  if (g_has_to_do_render) {
    lua::script_manager::for_each_event_handler("onD3DPresent", [](sol::protected_function fn) -> std::optional<sol::error> {
      auto pfr = fn();
      if (!pfr.valid()) {
        sol::error e = pfr;
        return e;
      }
      return std::nullopt;
    });

    gui::render::render(); // Render only at end of Idle
    lua::mimgui::render();
    gui::render::render_overlay();
    g_has_to_do_render = false;
  }
  return o_opengl_cameraendupdate(out, camera_in, in);
}

// ------ SA-MP ------
#if SAMP_MODE >= 1
monethook::hook<void*(char*, const char*, int, const char*, const char*)> o_netgame_ctor;
void* h_netgame_ctor(char* self, const char* address, int port, const char* nickname, const char* password)
{
  void* result = o_netgame_ctor(self, address, port, nickname, password);
  rakhook::init(
      *reinterpret_cast<RakClientInterface**>(self + compat::rakclientinterface_netgame_offset),
      lib_manager::samp_receive_ignore);
  game::netinfo::address = address;
  game::netinfo::port = static_cast<std::uint16_t>(port);
  return result;
}
#endif

// ------ Init ------
void do_init()
{
  void* handle = dlopen(compat::gtasa_name.c_str(), RTLD_LAZY);
  monethook::plt_scanner scanner { lib_manager::gtasa_base, lib_manager::gtasa_path.c_str() };
  if (!scanner.ok()) {
    dlclose(handle);
    return;
  }

  // ------ Hook addresses ------
  auto cgame_process = scanner.sym("_ZN5CGame7ProcessEv");
  auto cthescripts_init = scanner.sym("_ZN11CTheScripts4InitEv");
  auto cthescripts_process = scanner.sym("_ZN11CTheScripts7ProcessEv");
  auto cgame_shutdown = scanner.sym("_ZN5CGame8ShutdownEv");
  auto chid_flushqueuedtext = scanner.sym("_ZN4CHID15FlushQueuedTextEv");
  auto opengl_cameraendupdate = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z24_rwOpenGLCameraEndUpdatePvS_i"));

#if SAMP_MODE >= 1
  auto crunningscript_process = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN14CRunningScript7ProcessEv"));
  auto cworld_add = scanner.sym("_ZN6CWorld3AddEP7CEntity");
  auto cworld_remove = scanner.sym("_ZN6CWorld6RemoveEP7CEntity");
#endif

  // ------ GOT addresses ------
  lib_manager::got_and_touchevent = reinterpret_cast<std::uintptr_t>(
      dlsym(handle, "_Z14AND_TouchEventiiii"));

  // ------ Common addresses ------
  lib_manager::process_one_command = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN14CRunningScript17ProcessOneCommandEv"));
  lib_manager::ctouchinterface_istouched = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN15CTouchInterface9IsTouchedENS_9WidgetIDsEP9CVector2Di"));
  lib_manager::ascii_to_gxt_char = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z14AsciiToGxtCharPKcPt"));
  lib_manager::gxt_char_to_ascii = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z14GxtCharToAsciiPth"));
  lib_manager::add_big_message = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN9CMessages13AddBigMessageEPtjt"));
  lib_manager::add_message = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN9CMessages10AddMessageEPKcPtjtb"));
  lib_manager::add_message_jumpq = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN9CMessages15AddMessageJumpQEPKcPtjtb"));
  lib_manager::process_line_of_sight = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbb"));
  lib_manager::calc_screen_coors = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN7CSprite15CalcScreenCoorsERK5RwV3dPS0_PfS4_bb"));

  // ------ Render addresses ------
  lib_manager::set_scissor = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN7CWidget10SetScissorER5CRect"));
  lib_manager::rw_render_state_get = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z16RwRenderStateGet13RwRenderStatePv"));
  lib_manager::rw_render_state_set = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z16RwRenderStateSet13RwRenderStatePv"));
  lib_manager::rwim2d_render_indexed_primitive = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z35RwIm2DRenderIndexedPrimitive_BUGFIX15RwPrimitiveTypeP14RwOpenGLVertexiPti"));
  lib_manager::rwim2d_get_near_screen_z = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z20RwIm2DGetNearScreenZv"));
  lib_manager::rw_raster_destroy = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z15RwRasterDestroyP8RwRaster"));
  lib_manager::rw_raster_create = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z14RwRasterCreateiiii"));
  lib_manager::rw_raster_set_from_image = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z20RwRasterSetFromImageP8RwRasterP7RwImage"));
  lib_manager::rw_raster_lock = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z12RwRasterLockP8RwRasterhi"));
  lib_manager::rw_raster_unlock = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z14RwRasterUnlockP8RwRaster"));
  lib_manager::rw_image_create = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z13RwImageCreateiii"));
  lib_manager::rw_image_allocate_pixels = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z21RwImageAllocatePixelsP7RwImage"));
  lib_manager::rw_image_find_raster_format = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z23RwImageFindRasterFormatP7RwImageiPiS1_S1_S1_"));
  lib_manager::rw_image_destroy = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z14RwImageDestroyP7RwImage"));
  lib_manager::rt_png_image_read = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_Z14RtPNGImageReadPKc"));

  // ------ Variable addresses ------
  lib_manager::ped_pool = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN6CPools11ms_pPedPoolE"));
  lib_manager::vehicle_pool = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN6CPools15ms_pVehiclePoolE"));
  lib_manager::object_pool = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN6CPools14ms_pObjectPoolE"));
  lib_manager::radar_traces = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN6CRadar13ms_RadarTraceE"));
  lib_manager::mobile_menu = reinterpret_cast<std::uintptr_t>(dlsym(handle, "gMobileMenu"));
  lib_manager::pause_1 = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN6CTimer11m_UserPauseE"));
  lib_manager::pause_2 = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN6CTimer11m_CodePauseE"));
  lib_manager::rsglobal = reinterpret_cast<std::uintptr_t>(dlsym(handle, "RsGlobal"));
  lib_manager::thetext = reinterpret_cast<std::uintptr_t>(dlsym(handle, "TheText"));
  lib_manager::thecamera = reinterpret_cast<std::uintptr_t>(dlsym(handle, "TheCamera"));
  lib_manager::recip_near_clip = reinterpret_cast<std::uintptr_t>(dlsym(handle, "_ZN9CSprite2d13RecipNearClipE"));
  lib_manager::pads = reinterpret_cast<std::uintptr_t>(dlsym(handle, "Pads"));

  customgxt::init(scanner);
  dlclose(handle);

  // ------ Hooking ------
  o_cgame_process = { cgame_process, h_cgame_process };
  o_cgame_process.apply();
  o_cthescripts_init = { cthescripts_init, h_cthescripts_init };
  o_cthescripts_init.apply();
  o_cthescripts_process = { cthescripts_process, h_cthescripts_process };
  o_cthescripts_process.apply();
  o_cgame_shutdown = { cgame_shutdown, h_cgame_shutdown };
  o_cgame_shutdown.apply();
  o_chid_flushqueuedtext = { chid_flushqueuedtext, h_chid_flushqueuedtext };
  o_chid_flushqueuedtext.apply();
  o_opengl_cameraendupdate = { opengl_cameraendupdate, h_opengl_cameraendupdate };
  o_opengl_cameraendupdate.apply();
#if SAMP_MODE == 0
  o_and_touchevent = { lib_manager::got_and_touchevent, h_and_touchevent };
  o_and_touchevent.apply();
#endif

#if SAMP_MODE >= 1
  auto scan_netgame = monethook::scanner::pattern_scan(
      lib_manager::samp_path,
      compat::cnetgame_ctor_pattern);
  auto scan_raknet = monethook::scanner::pattern_scan(
      lib_manager::samp_path,
      compat::receiveignorerpc_pattern);

  if (
#if SAMP_MODE == 1
      lib_manager::samp_base != 0 &&
#endif
      scan_netgame.first == monethook::scanner::result::success && scan_raknet.first == monethook::scanner::result::success) {
    // SA-MP addresses are still valid!
#ifdef __thumb__
    scan_netgame.second |= 1;
    scan_raknet.second |= 1;
#endif

    o_crunningscript_process = { crunningscript_process, h_crunningscript_process };
    o_crunningscript_process.apply();
    o_cworld_add = { cworld_add, h_cworld_add };
    o_cworld_add.apply();
    o_cworld_remove = { cworld_remove, h_cworld_remove };
    o_cworld_remove.apply();
    o_netgame_ctor = { scan_netgame.second, h_netgame_ctor };
    o_netgame_ctor.apply();

    if (compat::use_samp_touch_workaround) {
      o_nveventinsertnewest = { lib_manager::gtasa_base + (compat::nveventinsertnewest_offset), h_nveventinsertnewest };
      o_nveventinsertnewest.apply();
    } else {
      o_and_touchevent = { lib_manager::got_and_touchevent, h_and_touchevent };
      o_and_touchevent.apply();
    }

    lib_manager::samp_receive_ignore = scan_raknet.second;
  } else {
#if SAMP_MODE == 2
    // Abort if SA-MP is invalid.
    exit(0);
#elif SAMP_MODE == 1
    lib_manager::samp_base = 0;

    o_and_touchevent = { lib_manager::got_and_touchevent, h_and_touchevent };
    o_and_touchevent.apply();
#endif
  }
#endif
}

bool check_libs_loaded()
{
  monethook::library_map::snapshot();

  std::string suffix_gtasa = "/" + compat::gtasa_name;
  // Some random invalid unicode characters
  std::string suffix_samp = compat::samp_name.empty() ? "\xFF\xFF\xFF\xFF" : "/" + compat::samp_name;

  for (auto& i : monethook::library_map::get()) {
    if (i.first.find(suffix_gtasa) != std::string::npos) {
      lib_manager::gtasa_base = i.second.start;
      lib_manager::gtasa_path = i.first;
#if SAMP_MODE >= 1
    } else if (i.first.find(suffix_samp) != std::string::npos) {
      lib_manager::samp_base = i.second.start;
      lib_manager::samp_path = i.first;
    }
#else
      break;
    }
#endif
  }

  if (lib_manager::gtasa_base != 0
#if SAMP_MODE == 2
      && lib_manager::samp_base != 0
#endif
  ) {
    void* handle = dlopen(compat::gtasa_name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!handle) {
      return false;
    }
    dlclose(handle);

#if SAMP_MODE == 1
    if (lib_manager::samp_base != 0) {
#endif
#if SAMP_MODE >= 1
      handle = dlopen(compat::samp_name.c_str(), RTLD_LAZY | RTLD_NOLOAD);
      if (!handle) {
        return false;
      }
      dlclose(handle);
#endif
#if SAMP_MODE == 1
    }
#endif

    return true;
  }
  return false;
}

void init_thread()
{
  while (!check_libs_loaded()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  do_init();
}

void do_preinit()
{
  andutils::init();
  file_manager::init();
  bass_manager::init();
  logger::init();
  compat::init();

  if (!check_libs_loaded())
    std::thread { init_thread }.detach(); // Do async loading
  else
    do_init(); // Do sync loading
}
}

extern "C" JNIEXPORT JNICALL jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
  JNIEnv* env;
  vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  andutils::jni::init_jni(env);
  do_preinit();
  return JNI_VERSION_1_6;
}

// ------ AML compat ------
AML_MYMOD(com.monetloader.aml, MonetLoader, PROJECT_VERSION_MAJOR, PROJECT_VERSION_MINOR, PROJECT_VERSION_PATCH, The MonetLoader Team)
AML_NEEDGAME(com.rockstargames.gtasa)

AML_ENTRYPOINT()
{
  andutils::jni::init_jni(amlInterface->GetJNIEnvironment());
  do_preinit();
}
