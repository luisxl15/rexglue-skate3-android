/**
 * @file        ui/overlay/touch_controls_overlay.cpp
 * @brief       On-screen touch controls driving an SDL virtual gamepad.
 *              See touch_controls_overlay.h.
 */
#include <rex/ui/overlay/touch_controls_overlay.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>

#include <imgui.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_joystick.h>
#include <SDL3/SDL_touch.h>
#include <SDL3/SDL_init.h>

#include <rex/cvar.h>
#include <rex/logging.h>

// PNG decoding for the button glyphs. stb_image is already vendored in the
// tree; only the PNG decoder is compiled in, and no other translation unit
// defines the implementation (Tracy, the copy's owner, is off in the builds
// that draw touch controls).
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_FAILURE_STRINGS
#include "../../../thirdparty/tracy/profiler/src/stb_image.h"

namespace rex::ui {

namespace {

struct Finger {
  uint64_t id;
  ImVec2 pos;  // in display pixels
};

constexpr int16_t kAxisMax = 32767;

// Standard SDL gamepad button / axis indices, valid because the virtual
// joystick is created with the full standard gamepad layout.
// On-screen button artwork. The glyph set ships in the APK under
// assets/buttons/<layout>/<fill>/<color>/<name>.png, so these three pick a
// directory rather than naming files: the overlay builds the path from them and
// reloads its 16 glyphs when any of them changes.
REXCVAR_DEFINE_STRING(touch_button_layout, "xbox", "UI/Touch",
                      "On-screen button glyphs: xbox (A/B/X/Y, bumpers) or "
                      "ps (Cross/Circle/Square/Triangle, L1/R1)")
    .allowed({"xbox", "ps"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(touch_button_fill, "solid", "UI/Touch",
                      "On-screen button style: outline, solid (glyph filled, "
                      "ring outlined) or full (whole button filled)")
    .allowed({"outline", "solid", "full"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(touch_button_color, "white", "UI/Touch",
                      "On-screen button colour: black or white")
    .allowed({"black", "white"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// ---- Button glyph artwork -------------------------------------------------
// One PNG per control, shipped in the APK under
// assets/buttons/<layout>/<fill>/<color>/<name>.png. On Android SDL resolves a
// relative path straight into the APK's assets, so no unpacking is needed.
enum Glyph {
  kGlyphA, kGlyphB, kGlyphX, kGlyphY,
  kGlyphLb, kGlyphRb, kGlyphLt, kGlyphRt,
  kGlyphDUp, kGlyphDDown, kGlyphDLeft, kGlyphDRight,
  kGlyphStart, kGlyphBack, kGlyphLStick, kGlyphRStick,
  kGlyphCount
};

constexpr std::array<const char*, kGlyphCount> kGlyphNames = {
    "a", "b", "x", "y", "lb", "rb", "lt", "rt",
    "dpad_up", "dpad_down", "dpad_left", "dpad_right",
    "start", "back", "lstick", "rstick"};

// Textures are built straight through the ImmediateDrawer, the same way the
// font atlas is: an ImmediateTexture pointer IS the ImTextureID a draw list
// takes. The earlier attempt registered ImTextureData in ImGui's own texture
// list instead, and the create request was re-honoured every frame - 16
// 256x256 RGBA uploads per frame, which blacked the game out.
struct GlyphSet {
  std::string key;              // "<layout>/<fill>/<color>", empty = not loaded
  ImmediateDrawer* owner = nullptr;  // textures die with the drawer that made them
  std::array<std::unique_ptr<ImmediateTexture>, kGlyphCount> textures{};
};
GlyphSet g_glyphs;

std::unique_ptr<ImmediateTexture> LoadGlyph(ImmediateDrawer* drawer,
                                            const std::string& dir,
                                            const char* name) {
  // Relative paths resolve into the APK's assets on Android.
  const std::string path = "buttons/" + dir + "/" + name + ".png";
  size_t size = 0;
  void* file = SDL_LoadFile(path.c_str(), &size);
  if (file == nullptr) {
    return nullptr;
  }
  int w = 0, h = 0, channels = 0;
  stbi_uc* pixels = stbi_load_from_memory(static_cast<const stbi_uc*>(file),
                                          int(size), &w, &h, &channels, 4);
  SDL_free(file);
  if (pixels == nullptr || w <= 0 || h <= 0) {
    if (pixels != nullptr) {
      stbi_image_free(pixels);
    }
    return nullptr;
  }
  auto texture = drawer->CreateTexture(uint32_t(w), uint32_t(h),
                                       ImmediateTextureFilter::kLinear, true,
                                       reinterpret_cast<const uint8_t*>(pixels));
  stbi_image_free(pixels);
  return texture;
}

// Loads the set the cvars select, and reloads when they change or when the
// drawer is replaced (presenter recreation invalidates every texture).
void EnsureGlyphSet(ImmediateDrawer* drawer) {
  if (drawer == nullptr) {
    return;
  }
  std::string key = std::string(REXCVAR_GET(touch_button_layout)) + "/" +
                    REXCVAR_GET(touch_button_fill) + "/" +
                    REXCVAR_GET(touch_button_color);
  if (key == g_glyphs.key && drawer == g_glyphs.owner) {
    return;
  }
  for (auto& texture : g_glyphs.textures) {
    texture.reset();
  }
  g_glyphs.key = key;
  g_glyphs.owner = drawer;
  uint32_t loaded = 0;
  for (uint32_t i = 0; i < kGlyphCount; ++i) {
    g_glyphs.textures[i] = LoadGlyph(drawer, key, kGlyphNames[i]);
    loaded += g_glyphs.textures[i] != nullptr ? 1u : 0u;
  }
  REXLOG_INFO("Touch controls: loaded {}/{} button glyphs from buttons/{}",
              loaded, uint32_t(kGlyphCount), key);
}

bool GlyphReady(Glyph g) {
  return g_glyphs.textures[g] != nullptr;
}

constexpr int B_A = SDL_GAMEPAD_BUTTON_SOUTH;
constexpr int B_B = SDL_GAMEPAD_BUTTON_EAST;
constexpr int B_X = SDL_GAMEPAD_BUTTON_WEST;
constexpr int B_Y = SDL_GAMEPAD_BUTTON_NORTH;
constexpr int B_BACK = SDL_GAMEPAD_BUTTON_BACK;
constexpr int B_START = SDL_GAMEPAD_BUTTON_START;
constexpr int B_LB = SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
constexpr int B_RB = SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
constexpr int B_DUP = SDL_GAMEPAD_BUTTON_DPAD_UP;
constexpr int B_DDOWN = SDL_GAMEPAD_BUTTON_DPAD_DOWN;
constexpr int B_DLEFT = SDL_GAMEPAD_BUTTON_DPAD_LEFT;
constexpr int B_DRIGHT = SDL_GAMEPAD_BUTTON_DPAD_RIGHT;

bool InCircle(const ImVec2& p, const ImVec2& c, float r) {
  const float dx = p.x - c.x, dy = p.y - c.y;
  return dx * dx + dy * dy <= r * r;
}
bool InRect(const ImVec2& p, const ImVec2& mn, const ImVec2& mx) {
  return p.x >= mn.x && p.x <= mx.x && p.y >= mn.y && p.y <= mx.y;
}
bool AnyInCircle(const std::vector<Finger>& fs, const ImVec2& c, float r) {
  for (const auto& f : fs)
    if (InCircle(f.pos, c, r)) return true;
  return false;
}
bool AnyInRect(const std::vector<Finger>& fs, const ImVec2& mn, const ImVec2& mx) {
  for (const auto& f : fs)
    if (InRect(f.pos, mn, mx)) return true;
  return false;
}

// Round button. Draws the glyph artwork when it is loaded and falls back to the
// labelled circle otherwise (missing file, or the first frames before ImGui has
// created the texture). The hit test is the same either way, so artwork never
// changes where the controls respond.
bool DrawButton(ImDrawList* dl, const std::vector<Finger>& fs, ImVec2 c, float r,
                const char* label, ImU32 col, Glyph glyph = kGlyphCount) {
  const bool pressed = AnyInCircle(fs, c, r * 1.25f);
  if (glyph != kGlyphCount && GlyphReady(glyph)) {
    // Square inscribed around the touch radius; pressing brightens it and nudges
    // the size, which reads as a press without moving the hit area.
    const float s = r * (pressed ? 1.18f : 1.10f);
    const uint32_t alpha = pressed ? 255u : 205u;
    const ImTextureID id =
        reinterpret_cast<ImTextureID>(g_glyphs.textures[glyph].get());
    dl->AddImage(ImTextureRef(id), ImVec2(c.x - s, c.y - s),
                 ImVec2(c.x + s, c.y + s), ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, alpha));
    return pressed;
  }
  const int a = pressed ? 235 : 120;
  dl->AddCircleFilled(c, r, IM_COL32(20, 20, 24, a));
  dl->AddCircle(c, r, (col & 0x00FFFFFF) | (uint32_t(pressed ? 255 : 180) << 24), 0, 2.5f);
  const ImVec2 ts = ImGui::CalcTextSize(label);
  dl->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
              (col & 0x00FFFFFF) | (uint32_t(pressed ? 255 : 210) << 24), label);
  return pressed;
}

// Analog stick. Owns a finger across frames; sets outX/outY in [-1,1].
void UpdateStick(const std::vector<Finger>& fs, ImVec2 base, float radius,
                 uint64_t& owner, float& out_x, float& out_y, ImDrawList* dl) {
  const Finger* f = nullptr;
  if (owner) {
    for (const auto& g : fs)
      if (g.id == owner) { f = &g; break; }
    if (!f) owner = 0;  // finger lifted
  }
  if (!owner) {
    for (const auto& g : fs)
      if (InCircle(g.pos, base, radius)) { owner = g.id; f = &g; break; }
  }

  ImVec2 knob = base;
  out_x = out_y = 0.0f;
  if (f) {
    float dx = f->pos.x - base.x, dy = f->pos.y - base.y;
    float mag = std::sqrt(dx * dx + dy * dy);
    if (mag > radius && mag > 0.0f) {
      dx = dx / mag * radius;
      dy = dy / mag * radius;
    }
    out_x = dx / radius;
    out_y = dy / radius;
    knob = ImVec2(base.x + dx, base.y + dy);
  }
  dl->AddCircle(base, radius, IM_COL32(200, 200, 210, owner ? 200 : 110), 0, 2.5f);
  dl->AddCircleFilled(knob, radius * 0.42f, IM_COL32(230, 230, 240, owner ? 220 : 130));
}

int16_t ToAxis(float v) {
  return static_cast<int16_t>(std::clamp(v, -1.0f, 1.0f) * kAxisMax);
}

}  // namespace

TouchControlsOverlayDialog::TouchControlsOverlayDialog(ImGuiDrawer* imgui_drawer)
    : ImGuiDialog(imgui_drawer) {
  if (!SDL_WasInit(SDL_INIT_JOYSTICK)) {
    SDL_InitSubSystem(SDL_INIT_JOYSTICK);
  }

  SDL_VirtualJoystickDesc desc;
  SDL_INIT_INTERFACE(&desc);
  desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
  desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
  desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
  desc.button_mask =
      (1u << B_A) | (1u << B_B) | (1u << B_X) | (1u << B_Y) | (1u << B_BACK) |
      (1u << SDL_GAMEPAD_BUTTON_GUIDE) | (1u << B_START) |
      (1u << SDL_GAMEPAD_BUTTON_LEFT_STICK) | (1u << SDL_GAMEPAD_BUTTON_RIGHT_STICK) |
      (1u << B_LB) | (1u << B_RB) | (1u << B_DUP) | (1u << B_DDOWN) | (1u << B_DLEFT) |
      (1u << B_DRIGHT);
  desc.axis_mask = (1u << SDL_GAMEPAD_AXIS_LEFTX) | (1u << SDL_GAMEPAD_AXIS_LEFTY) |
                   (1u << SDL_GAMEPAD_AXIS_RIGHTX) | (1u << SDL_GAMEPAD_AXIS_RIGHTY) |
                   (1u << SDL_GAMEPAD_AXIS_LEFT_TRIGGER) |
                   (1u << SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
  desc.name = "Skate 3 Touch Controls";

  virtual_joystick_id_ = SDL_AttachVirtualJoystick(&desc);
  if (virtual_joystick_id_) {
    virtual_joystick_ = SDL_OpenJoystick(virtual_joystick_id_);
    REXLOG_INFO("[core] Touch controls: attached virtual gamepad (id={})",
                virtual_joystick_id_);
  } else {
    REXLOG_ERROR("[core] Touch controls: SDL_AttachVirtualJoystick failed: {}",
                 SDL_GetError());
  }
}

TouchControlsOverlayDialog::~TouchControlsOverlayDialog() {
  if (virtual_joystick_) {
    SDL_CloseJoystick(virtual_joystick_);
  }
  if (virtual_joystick_id_) {
    SDL_DetachVirtualJoystick(virtual_joystick_id_);
  }
}

void TouchControlsOverlayDialog::OnDraw(ImGuiIO& io) {
  const float w = io.DisplaySize.x;
  const float h = io.DisplaySize.y;
  if (w <= 0.0f || h <= 0.0f) return;

  // Collect all active fingers across all touch devices, in display pixels.
  std::vector<Finger> fs;
  int nd = 0;
  if (SDL_TouchID* devs = SDL_GetTouchDevices(&nd)) {
    for (int d = 0; d < nd; ++d) {
      int nf = 0;
      if (SDL_Finger** fingers = SDL_GetTouchFingers(devs[d], &nf)) {
        for (int i = 0; i < nf; ++i) {
          fs.push_back({static_cast<uint64_t>(fingers[i]->id),
                        ImVec2(fingers[i]->x * w, fingers[i]->y * h)});
        }
        SDL_free(fingers);
      }
    }
    SDL_free(devs);
  }

  ImDrawList* dl = ImGui::GetForegroundDrawList();

  // Picks up layout/fill/colour changes made in the settings screen (all three
  // are hot-reload cvars); a no-op once the set matches.
  EnsureGlyphSet(imgui_drawer() ? imgui_drawer()->immediate_drawer() : nullptr);

  const float u = std::min(w, h);  // unit for sizing
  const float stick_r = 0.16f * h;
  const float face_r = 0.052f * h;
  const float small_r = 0.06f * h;

  // Analog sticks.
  float lx, ly, rx, ry;
  UpdateStick(fs, ImVec2(0.16f * w, 0.72f * h), stick_r, left_stick_finger_, lx, ly, dl);
  UpdateStick(fs, ImVec2(0.84f * w, 0.72f * h), stick_r, right_stick_finger_, rx, ry, dl);

  // Face buttons (diamond), right-upper.
  const ImVec2 fc(0.86f * w, 0.30f * h);
  const bool y = DrawButton(dl, fs, ImVec2(fc.x, fc.y - face_r * 1.4f), face_r, "Y",
                            IM_COL32(240, 220, 60, 255), kGlyphY);
  const bool a = DrawButton(dl, fs, ImVec2(fc.x, fc.y + face_r * 1.4f), face_r, "A",
                            IM_COL32(90, 220, 90, 255), kGlyphA);
  const bool x = DrawButton(dl, fs, ImVec2(fc.x - face_r * 1.4f, fc.y), face_r, "X",
                            IM_COL32(80, 150, 240, 255), kGlyphX);
  const bool b = DrawButton(dl, fs, ImVec2(fc.x + face_r * 1.4f, fc.y), face_r, "B",
                            IM_COL32(230, 80, 80, 255), kGlyphB);

  // D-pad (four buttons), left-upper.
  const ImVec2 dc(0.14f * w, 0.30f * h);
  const float dr = 0.048f * h;
  const bool du = DrawButton(dl, fs, ImVec2(dc.x, dc.y - dr * 1.5f), dr, "^",
                             IM_COL32(220, 220, 230, 255), kGlyphDUp);
  const bool dd = DrawButton(dl, fs, ImVec2(dc.x, dc.y + dr * 1.5f), dr, "v",
                             IM_COL32(220, 220, 230, 255), kGlyphDDown);
  const bool dlft = DrawButton(dl, fs, ImVec2(dc.x - dr * 1.5f, dc.y), dr, "<",
                               IM_COL32(220, 220, 230, 255), kGlyphDLeft);
  const bool drgt = DrawButton(dl, fs, ImVec2(dc.x + dr * 1.5f, dc.y), dr, ">",
                               IM_COL32(220, 220, 230, 255), kGlyphDRight);

  // Shoulders / triggers (top corners).
  const ImU32 gray = IM_COL32(210, 210, 220, 255);
  const bool lb = DrawButton(dl, fs, ImVec2(0.10f * w, 0.10f * h), small_r, "LB", gray, kGlyphLb);
  const bool lt = DrawButton(dl, fs, ImVec2(0.24f * w, 0.12f * h), small_r, "LT", gray, kGlyphLt);
  const bool rb = DrawButton(dl, fs, ImVec2(0.90f * w, 0.10f * h), small_r, "RB", gray, kGlyphRb);
  const bool rt = DrawButton(dl, fs, ImVec2(0.76f * w, 0.12f * h), small_r, "RT", gray, kGlyphRt);

  // Start / Back (top center).
  const bool start = DrawButton(dl, fs, ImVec2(0.56f * w, 0.07f * h), small_r * 0.8f,
                                "=", gray, kGlyphStart);
  const bool back = DrawButton(dl, fs, ImVec2(0.44f * w, 0.07f * h), small_r * 0.8f,
                               "<<", gray, kGlyphBack);

  // ESC: opens the Skate 3 settings screen (graphics options). This is the one
  // control that is NOT a pad button - it pushes real keyboard events so every
  // Escape keybind behaves as it does on desktop, including backing out of the
  // settings screen level by level. Placed away from Start/Back so it is not hit
  // by accident. Only the press/release EDGES are sent: a held finger would
  // otherwise toggle the screen open and shut on every frame.
  const bool esc = DrawButton(dl, fs, ImVec2(0.32f * w, 0.07f * h), small_r * 0.8f,
                              "ESC", IM_COL32(255, 190, 90, 255));
  if (esc != esc_pressed_) {
    esc_pressed_ = esc;
    SDL_Window* focus = SDL_GetKeyboardFocus();
    if (!focus) {
      // Android never assigns keyboard focus (there is no keyboard): fall back
      // to the single application window, which is what the event router
      // matches on.
      int window_count = 0;
      SDL_Window* const* windows = SDL_GetWindows(&window_count);
      if (windows && window_count > 0) {
        focus = windows[0];
      }
    }
    if (focus) {
      SDL_Event ev{};
      ev.key.type = esc ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      ev.key.windowID = SDL_GetWindowID(focus);  // timestamp 0: SDL stamps it
      ev.key.scancode = SDL_SCANCODE_ESCAPE;
      ev.key.key = SDLK_ESCAPE;
      ev.key.mod = SDL_KMOD_NONE;
      ev.key.down = esc;
      ev.key.repeat = false;
      SDL_PushEvent(&ev);
    }
  }

  if (!virtual_joystick_) return;

  SDL_SetJoystickVirtualAxis(virtual_joystick_, SDL_GAMEPAD_AXIS_LEFTX, ToAxis(lx));
  SDL_SetJoystickVirtualAxis(virtual_joystick_, SDL_GAMEPAD_AXIS_LEFTY, ToAxis(ly));
  SDL_SetJoystickVirtualAxis(virtual_joystick_, SDL_GAMEPAD_AXIS_RIGHTX, ToAxis(rx));
  SDL_SetJoystickVirtualAxis(virtual_joystick_, SDL_GAMEPAD_AXIS_RIGHTY, ToAxis(ry));
  SDL_SetJoystickVirtualAxis(virtual_joystick_, SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
                             lt ? kAxisMax : 0);
  SDL_SetJoystickVirtualAxis(virtual_joystick_, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
                             rt ? kAxisMax : 0);

  SDL_SetJoystickVirtualButton(virtual_joystick_, B_A, a);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_B, b);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_X, x);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_Y, y);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_LB, lb);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_RB, rb);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_START, start);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_BACK, back);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_DUP, du);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_DDOWN, dd);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_DLEFT, dlft);
  SDL_SetJoystickVirtualButton(virtual_joystick_, B_DRIGHT, drgt);
}

}  // namespace rex::ui
