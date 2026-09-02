#include "render.h"
#include "clipboard.h"
#include "context_switch.h"
#include "game/common.h"
#include "imgui/imgui.h"
#include "imgui_fonts/arial_bold.h"
#include "imrw.h"
#include "input_system.h"
#include "keyboard.h"
#include "screen.h"
#include "touch_handler.h"
#include "utils/android.h"
#include <cmath>
#include <string>

#define ARGB_TO_IMGUI(color) IM_COL32(((color) & 0x00FF0000) >> 16, ((color) & 0x0000FF00) >> 8, (color) & 0x000000FF, ((color) & 0xFF000000) >> 24)
namespace {
enum font_flags {
  FONT_FLAG_NONE = 0,
  FONT_FLAG_OUTLINE = (1 << 16),
  FONT_FLAG_VALID = (1 << 17)
};
static constexpr std::int32_t FONT_SIZE_MASK = 0xFFFF;

ImGuiContext* g_render_context = nullptr;
ImFont* g_render_font = nullptr;
float g_dpi_scale = 1.f;

ImGuiContext* g_overlay_context = nullptr;
touch_handler g_overlay_touch { };
ImGuiIO* g_keyboard_io = nullptr;

bool are_hex_chars(const char* s, std::size_t size)
{
  bool result = true;
  for (std::size_t i = 0; i < size; ++i) {
    char c = s[i];
    if (c >= '0' && c <= '9')
      continue;
    char lower = c | 0x20; // ASCII hack to convert to lower case
    if (lower >= 'a' && lower <= 'f')
      continue;
    result = false;
    break;
  }
  return result;
}

std::string strip_color_tags(std::string_view text)
{
  std::string copy { };
  copy.reserve(text.size());

  std::size_t last_slice_start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    std::size_t left = text.size() - i;
    if (text[i] == '{' && left > 7 && text[i + 7] == '}' && are_hex_chars(&text[i + 1], 6)) {
      copy += text.substr(last_slice_start, i - last_slice_start);
      if (left > 8) {
        last_slice_start = i + 8;
      } else {
        last_slice_start = std::string_view::npos;
      }
      i += 7;
    } else if (text[i] == '{' && left > 9 && text[i + 9] == '}' && are_hex_chars(&text[i + 1], 8)) {
      copy += text.substr(last_slice_start, i - last_slice_start);
      if (left > 10) {
        last_slice_start = i + 10;
      } else {
        last_slice_start = std::string_view::npos;
      }
      i += 9;
    }
  }

  if (last_slice_start != std::string_view::npos) {
    copy += text.substr(last_slice_start, text.size() - last_slice_start);
  }
  return copy;
}

void render_outline_text(ImVec2 pos, float font_size, ImU32 col, bool outline, const char* text_begin, const char* text_end = nullptr)
{
  const float size = std::round(1.f * g_dpi_scale);

  ImU32 outline_col = col & IM_COL32_A_MASK;
  if (outline) {
    // Left
    pos.x -= size;
    ImGui::GetBackgroundDrawList()->AddText(g_render_font, font_size, pos, outline_col, text_begin, text_end);
    // Right
    pos.x += 2.f * size;
    ImGui::GetBackgroundDrawList()->AddText(g_render_font, font_size, pos, outline_col, text_begin, text_end);
    // Top
    pos.x -= size;
    pos.y -= size;
    ImGui::GetBackgroundDrawList()->AddText(g_render_font, font_size, pos, outline_col, text_begin, text_end);
    // Bottom
    pos.y += 2.f * size;
    ImGui::GetBackgroundDrawList()->AddText(g_render_font, font_size, pos, outline_col, text_begin, text_end);
    // Restore
    pos.y -= size;
  }

  ImGui::GetBackgroundDrawList()->AddText(g_render_font, font_size, pos, col, text_begin, text_end);
}

bool process_inline_hex_color(const char* start, const char* end, ImU32& color)
{
  const auto cnt = static_cast<std::size_t>(end - start);

  char hex[9];
  strncpy(hex, start, cnt);
  hex[cnt] = 0;

  ImU32 hex_color = 0x0;
  if (sscanf(hex, "%x", &hex_color) > 0) {
    if (cnt == 6) {
      color = ARGB_TO_IMGUI(hex_color) | (color & IM_COL32_A_MASK);
    } else if (cnt == 8) {
      color = ARGB_TO_IMGUI(hex_color);
    }
    return true;
  }

  return false;
}

void render_text_with_color_tags(ImVec2 pos, float font_size, ImU32 default_col, bool outline, const char* text_begin, const char* text_end)
{
  const char* text_cur = text_begin;
  if (!text_end)
    text_end = text_begin + std::strlen(text_begin);

  ImVec2 cur = pos;
  ImU32 col = default_col;
  while (*text_cur) {
    if (text_cur[0] == '\n') {
      if (text_cur != text_begin) {
        render_outline_text(cur, font_size, col, outline, text_begin, text_cur);
      }
      cur.x = pos.x;
      cur.y += g_render_font->CalcTextSizeA(font_size, FLT_MAX, -1.f, text_begin, text_cur).y;
      text_begin = text_cur + 1;
    } else if (text_cur[0] == '{' && text_cur + 7 < text_end && text_cur[7] == '}') {
      if (text_cur != text_begin) {
        render_outline_text(cur, font_size, col, outline, text_begin, text_cur);
        cur.x += g_render_font->CalcTextSizeA(font_size, FLT_MAX, -1.f, text_begin, text_cur).x;
      }

      if (process_inline_hex_color(text_cur + 1, text_cur + 7, col)) {
        text_cur += 7;
      }
      text_begin = text_cur + 1;
    } else if (text_cur[0] == '{' && text_cur + 9 < text_end && text_cur[9] == '}') {
      if (text_cur != text_begin) {
        render_outline_text(cur, font_size, col, outline, text_begin, text_cur);
        cur.x += g_render_font->CalcTextSizeA(font_size, FLT_MAX, -1.f, text_begin, text_cur).x;
      }

      if (process_inline_hex_color(text_cur + 1, text_cur + 9, col)) {
        text_cur += 9;
      }
      text_begin = text_cur + 1;
    }

    text_cur++;
  }

  if (text_cur != text_begin)
    render_outline_text(cur, font_size, col, outline, text_begin, text_cur);
}
}

float gui::render::get_dpi_scale()
{
  return g_dpi_scale;
}

void gui::render::init()
{
  g_dpi_scale = std::max(1.f, andutils::jni::get_dpi() / 200.f);

  g_render_context = ImGui::CreateContext();
  {
    imgui_context_switch sw { g_render_context };
    imrw::init();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;

    ImFontConfig cfg { };
    cfg.FontDataOwnedByAtlas = false;
    g_render_font = io.Fonts->AddFontFromMemoryTTF(
        imgui_fonts::arial_bold(), imgui_fonts::arial_bold_size(), 72.f,
        &cfg, io.Fonts->GetGlyphRangesCyrillic());
    imrw::create_font();
  }

  g_overlay_context = ImGui::CreateContext();
  {
    imgui_context_switch sw { g_overlay_context };
    imrw::init();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.MouseDoubleClickMaxDist = g_dpi_scale * 40.f;
    io.MouseDoubleClickTime = 0.5f;
    init_keyboard(&io);

    ImFontConfig cfg { };
    cfg.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(
        imgui_fonts::arial_bold(), imgui_fonts::arial_bold_size(), 40.f,
        &cfg, io.Fonts->GetGlyphRangesCyrillic());
    imrw::create_font();

    g_overlay_touch.init(&ImGui::GetIO(), ImGui::GetCurrentContext());
    g_overlay_touch.enable_touches(true);
    g_overlay_touch.enable_multitouch(true);
  }
}

void gui::render::new_frame()
{
  if (!g_render_context) {
    gui::render::init();
  }
  imgui_context_switch sw { g_render_context };

  imrw::new_frame();
  ImGuiIO& io = ImGui::GetIO();
  if (io.DisplaySize.x != screen::size.x || io.DisplaySize.y != screen::size.y) {
    screen::update_size(io.DisplaySize);
  }
  ImGui::NewFrame();
}

void gui::render::end_frame()
{
  imgui_context_switch sw { g_render_context };
  input_system::end_frame();
  ImGui::EndFrame();
  ImGui::Render();
}

void gui::render::render()
{
  imgui_context_switch sw { g_render_context };
  if (!ImGui::GetCurrentContext() || !ImGui::GetDrawData())
    return;

  imrw::render_draw_data(ImGui::GetDrawData());
}

#define WATERMARK_TEXT "Powered by MonetLoader " PROJECT_VERSION "\nBuild Date: " __DATE__ "\nOfficial Telegram: t.me/MonetLoader"
#define WATERMARK_SIZE 40.f
void gui::render::render_overlay()
{
  if (!g_overlay_context) {
    gui::render::init();
  }
  imgui_context_switch sw { g_overlay_context };

  imrw::new_frame();
  ImGui::NewFrame();

  // Debug posisi window ImGui aktif (gunakan internal header imgui_internal.h jika diperlukan, 
  // atau cetak ukuran DisplaySize io untuk membandingkan skala sentuhan x: 301, y: 917)
  ImGuiIO& io = ImGui::GetIO();
  static int frame_counter = 0;
  if (++frame_counter % 120 == 0) { // Cetak setiap 120 frame sekali agar terhindar dari spam/crash log
    logger::log<logger::LV_SYSTEM, false>(
      nullptr, 
      "[ImGui Display] Size: ({}, {}), MousePos: ({}, {})", 
      io.DisplaySize.x, io.DisplaySize.y, io.MousePos.x, io.MousePos.y
    );
  }

  keyboard::instance().render(g_overlay_touch.active_pointers_count() > 1);
  if (game::IsPaused()) {
    ImGui::GetForegroundDrawList()->AddText(ImVec2 { 8.f, 8.f }, IM_COL32_BLACK, WATERMARK_TEXT);
    ImGui::GetForegroundDrawList()->AddText(ImVec2 { 12.f, 8.f }, IM_COL32_BLACK, WATERMARK_TEXT);
    ImGui::GetForegroundDrawList()->AddText(ImVec2 { 8.f, 12.f }, IM_COL32_BLACK, WATERMARK_TEXT);
    ImGui::GetForegroundDrawList()->AddText(ImVec2 { 12.f, 12.f }, IM_COL32_BLACK, WATERMARK_TEXT);
    ImGui::GetForegroundDrawList()->AddText(ImVec2 { 10.f, 10.f }, IM_COL32_WHITE, WATERMARK_TEXT);
  }

  input_system::end_frame();
  ImGui::EndFrame();
  g_overlay_touch.end_frame();

  ImGui::Render();
  imrw::render_draw_data(ImGui::GetDrawData());
}

void gui::render::deinit()
{
  imgui_context_switch sw { g_render_context };
  imrw::shutdown();
  ImGui::DestroyContext(g_render_context);
  input_system::destroyed_context();
  g_render_context = nullptr;
}

void gui::render::init_keyboard(void* io_void)
{
  auto* io = static_cast<ImGuiIO*>(io_void);

  io->GetClipboardTextFn = [](void*) {
    thread_local static std::string cached_value;
    cached_value = clipboard::get();
    return cached_value.c_str();
  };
  io->SetClipboardTextFn = [](void*, const char* text) {
    clipboard::set(text);
  };

  io->KeyMap[ImGuiKey_Backspace] = ImGuiKey_Backspace;
  io->KeyMap[ImGuiKey_Enter] = ImGuiKey_Enter;
  io->KeyMap[ImGuiKey_A] = ImGuiKey_A;
  io->KeyMap[ImGuiKey_C] = ImGuiKey_C;
  io->KeyMap[ImGuiKey_V] = ImGuiKey_V;
  io->KeyMap[ImGuiKey_X] = ImGuiKey_X;
  io->KeyMap[ImGuiKey_Y] = ImGuiKey_Y;
  io->KeyMap[ImGuiKey_Z] = ImGuiKey_Z;
}

void gui::render::show_keyboard(void* io_void, void* style)
{
  auto* io = static_cast<ImGuiIO*>(io_void);
  if (!g_keyboard_io) {
    keyboard::instance().show(io, static_cast<ImGuiStyle*>(style));
  }
  g_keyboard_io = io;
}

void gui::render::hide_keyboard(void* io_void)
{
  auto* io = static_cast<ImGuiIO*>(io_void);
  if (g_keyboard_io == io) {
    g_keyboard_io = nullptr;
    keyboard::instance().hide();
  }
}

bool gui::render::handle_touch(int type, int num, int x, int y)
{
  touch_handler::touch_data data;
  data.type = static_cast<touch_handler::touch_type>(type);
  data.num = num;
  data.x = x;
  data.y = y;

  return g_overlay_touch.handle(data);
}

void gui::render::draw_line(float x1, float y1, float x2, float y2, float width, std::uint32_t color)
{
  imgui_context_switch sw { g_render_context };
  color = ARGB_TO_IMGUI(color);
  ImGui::GetBackgroundDrawList()->AddLine({ x1, y1 }, { x2, y2 }, color, width);
}

void gui::render::draw_box(float x, float y, float w, float h, std::uint32_t color)
{
  imgui_context_switch sw { g_render_context };
  color = ARGB_TO_IMGUI(color);
  ImGui::GetBackgroundDrawList()->AddRectFilled({ x, y }, { x + w, y + h }, color);
}

void gui::render::draw_box_with_border(float x, float y, float w, float h, std::uint32_t color, float bsize, std::uint32_t bcolor)
{
  imgui_context_switch sw { g_render_context };
  color = ARGB_TO_IMGUI(color);
  bcolor = ARGB_TO_IMGUI(bcolor);
  ImGui::GetBackgroundDrawList()->AddRectFilled({ x, y }, { x + w, y + h }, color);
  ImGui::GetBackgroundDrawList()->AddRect({ x, y }, { x + w, y + h }, bcolor, 0.f, ImDrawCornerFlags_All, bsize);
}

void gui::render::draw_polygon(float x, float y, float w, float h, int corners, float rotation, std::uint32_t color)
{
  color = ARGB_TO_IMGUI(color);
  if ((color & IM_COL32_A_MASK) == 0 || corners <= 2)
    return;

  imgui_context_switch sw { g_render_context };
  const float w_scale = w / h;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  const int vtx_1 = dl->VtxBuffer.size();

  const auto a_min = static_cast<float>(rotation * (M_PI / 180.0));
  const float a_max = (static_cast<float>(M_PI) * 2.0f) * ((float)corners - 1.0f) / (float)corners;
  dl->PathArcTo({ x + 0.5f * h, y + 0.5f * h }, 0.5f * h, a_min, a_min + a_max, corners - 1);
  dl->PathFillConvex(color);

  const int vtx_2 = dl->VtxBuffer.size();
  for (int i = vtx_1; i < vtx_2; ++i) {
    float dx = dl->VtxBuffer[i].pos.x - x;
    dl->VtxBuffer[i].pos.x = x + (dx * w_scale);
  }
}

void gui::render::draw_image(void* texture, float x, float y, float w, float h, float rotation, std::uint32_t color)
{
  color = ARGB_TO_IMGUI(color);
  if ((color & IM_COL32_A_MASK) == 0)
    return;

  auto angle = static_cast<float>(rotation * (M_PI / 180.0));
  float cos_a = std::cos(angle);
  float sin_a = std::sin(angle);
  ImVec2 center = { x + w * 0.5f, y + h * 0.5f };
  auto rotated = [&center, cos_a, sin_a](const ImVec2& v) {
    return ImVec2(center.x + v.x * cos_a - v.y * sin_a, center.y + v.x * sin_a + v.y * cos_a);
  };
  ImVec2 pos[4] = {
    rotated(ImVec2(-w * 0.5f, -h * 0.5f)),
    rotated(ImVec2(w * 0.5f, -h * 0.5f)),
    rotated(ImVec2(w * 0.5f, h * 0.5f)),
    rotated(ImVec2(-w * 0.5f, h * 0.5f))
  };
  ImVec2 uvs[4] = {
    ImVec2(0.0f, 0.0f),
    ImVec2(1.0f, 0.0f),
    ImVec2(1.0f, 1.0f),
    ImVec2(0.0f, 1.0f)
  };

  ImGui::GetBackgroundDrawList()->AddImageQuad(texture,
      pos[0], pos[1], pos[2], pos[3],
      uvs[0], uvs[1], uvs[2], uvs[3],
      color);
}

std::int32_t gui::render::get_font(int size, bool outline)
{
  return (outline ? FONT_FLAG_OUTLINE : FONT_FLAG_NONE) | FONT_FLAG_VALID | (static_cast<int>(static_cast<float>(size) * g_dpi_scale) & FONT_SIZE_MASK);
}

float gui::render::get_text_length(std::int32_t handle, std::string_view text, bool ignore_color_tags)
{
  if (!(handle & FONT_FLAG_VALID)) {
    return 0.f;
  }
  imgui_context_switch sw { g_render_context };
  float font_size { static_cast<float>(handle & FONT_SIZE_MASK) };
  if (ignore_color_tags) {
    return g_render_font->CalcTextSizeA(font_size, FLT_MAX, -1.f, text.data(), text.data() + text.size()).x;
  }

  std::string stripped = strip_color_tags(text);
  return g_render_font->CalcTextSizeA(font_size, FLT_MAX, -1.f, stripped.data(), stripped.data() + stripped.size()).x;
}

int gui::render::get_font_height(std::int32_t handle)
{
  if (!(handle & FONT_FLAG_VALID)) {
    return 0;
  }
  return handle & FONT_SIZE_MASK;
}

void gui::render::draw_font(std::int32_t handle, std::string_view text, float x, float y, std::uint32_t color, bool ignore_color_tags)
{
  if (!(handle & FONT_FLAG_VALID)) {
    return;
  }
  imgui_context_switch sw { g_render_context };

  bool outline = handle & FONT_FLAG_OUTLINE;
  float font_size { static_cast<float>(handle & FONT_SIZE_MASK) };
  color = ARGB_TO_IMGUI(color);

  if (ignore_color_tags) {
    return render_outline_text({ x, y }, font_size, color, outline, text.data(), text.data() + text.size());
  }

  render_text_with_color_tags({ x, y }, font_size, color, outline, text.data(), text.data() + text.size());
}
