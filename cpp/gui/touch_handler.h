#pragma once
#include "context_switch.h"
#include "imgui/imgui.h"
#include <cstddef>

namespace ImGui {
void UpdateHoveredWindowAndCaptureFlags();
}

class touch_handler {
  public:
  enum touch_type {
    POP = 1,
    PUSH = 2,
    MOVE = 3
  };

  struct touch_data {
    touch_type type;
    int num;
    int x;
    int y;
  };

  void init(ImGuiIO* p_io, ImGuiContext* p_ctx)
  {
    io = p_io;
    ctx = p_ctx;
  }

  bool handle(const touch_data& data)
  {
    if (!touches_enabled || data.num >= IM_ARRAYSIZE(pointers)) {
      return false;
    }
    if (!is_num_valid(data.type, data.num)) {
      return false;
    }

    if (pointers[data.num]) {
      consume(data);
      if (data.type == touch_type::POP) {
        reset(data);
      }
      return true;
    }

    ImVec2 old_mouse_pos = io->MousePos;
    bool was_down = io->MouseDown[0];
    bool was_clicked = io->MouseClicked[0];
    io->MousePos = ImVec2 { static_cast<float>(data.x), static_cast<float>(data.y) };
    if (active_pointers > 0) {
      io->MouseDown[0] = false;
      io->MouseClicked[0] = false;
    }

    bool want_capture;
    {
      imgui_context_switch sw { ctx };
      ImGui::UpdateHoveredWindowAndCaptureFlags();
      want_capture = io->WantCaptureMouse;
    }

    if (active_pointers == 0) {
      consume(data);
    } else {
      io->MousePos = old_mouse_pos;
      io->MouseDown[0] = was_down;
      io->MouseClicked[0] = was_clicked;
      imgui_context_switch sw { ctx };
      ImGui::UpdateHoveredWindowAndCaptureFlags();
    }

    if (want_capture && (multitouch || active_pointers == 0)) {
      if (active_pointers > 0) {
        io->MouseDown[0] = false; // register a release, then push this pointer
        delayed_push = true;
        delayed_push_pos = { static_cast<float>(data.x), static_cast<float>(data.y) };
      }
      pointers[data.num] = true;
      ++active_pointers;
      return true;
    }

    if (active_pointers == 0) {
      need_clear_mouse_pos = true;
      need_clear_mouse_down = true;
    }
    return false;
  }

  void end_frame();

  void enable_touches(bool enable)
  {
    touches_enabled = enable;
  }

  // useful only for "push-only" windows, because imgui only supports a single pointer
  void enable_multitouch(bool enable)
  {
    multitouch = enable;
  }

  std::size_t active_pointers_count()
  {
    return active_pointers;
  }

  private:
  bool is_num_valid(touch_type type, int num_check)
  {
    if (num_check < 0 || num_check >= IM_ARRAYSIZE(pointers)) return false;

    if (type == touch_type::PUSH) {
      // Izinkan PUSH jika pointer belum aktif, dan berikan kelonggaran untuk indeks 0 atau saat active_pointers tidak akurat
      return !pointers[num_check];
    }

    // Untuk MOVE atau POP, pastikan pointer tersebut memang tercatat aktif
    return pointers[num_check];
  }

  void reset(const touch_data& data)
  {
    pointers[data.num] = false;
    --active_pointers;
  }

  void consume(const touch_data& data)
  {
    switch (data.type) {
    case touch_type::POP: {
      io->MouseDown[0] = false;
      need_clear_mouse_pos = true;
      break;
    }
    case touch_type::PUSH: {
      io->MouseDown[0] = true;
      io->MousePosPrev = io->MousePos = { static_cast<float>(data.x), static_cast<float>(data.y) };
      break;
    }
    case touch_type::MOVE: {
      io->MousePos = { static_cast<float>(data.x), static_cast<float>(data.y) };
      break;
    }
    }
  }

  ImGuiIO* io { nullptr };
  ImGuiContext* ctx { nullptr };
  std::size_t active_pointers { 0 };
  bool pointers[10] {};
  bool touches_enabled { false };
  bool multitouch { false };
  bool need_clear_mouse_pos { false };
  bool need_clear_mouse_down { false };
  bool delayed_push { false };
  ImVec2 delayed_push_pos {};
};
