/**
 * @file        rex/ui/overlay/touch_controls_overlay.h
 *
 * @brief       On-screen touch controls for Android.
 *
 * Renders a virtual gamepad (two sticks, face buttons, d-pad, shoulders,
 * triggers, start/back) with ImGui and drives an SDL virtual gamepad from the
 * touch input, so the guest sees a standard controller through the normal SDL
 * input driver. Adapted from the UnleashedRecomp-Android touch overlay, but
 * injected through an SDL virtual joystick instead of a game-specific pad state.
 */
#pragma once

#include <cstdint>

#include <rex/ui/imgui_dialog.h>

struct SDL_Joystick;

namespace rex::ui {

class TouchControlsOverlayDialog : public ImGuiDialog {
 public:
  explicit TouchControlsOverlayDialog(ImGuiDrawer* imgui_drawer);
  ~TouchControlsOverlayDialog();

  bool WantsContinuousRepaint() const override { return true; }

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  uint32_t virtual_joystick_id_ = 0;   // SDL_JoystickID (0 = not attached)
  SDL_Joystick* virtual_joystick_ = nullptr;

  // Fingers currently owning the analog sticks (0 = none; SDL finger ids are
  // never 0). Tracked across frames so a drag keeps controlling its stick even
  // when it leaves the stick zone.
  uint64_t left_stick_finger_ = 0;
  uint64_t right_stick_finger_ = 0;

  // "ESC" button: pushes real SDL keyboard events rather than a pad button, so
  // it drives whatever is bound to Escape (the Skate 3 settings screen) exactly
  // like a keyboard would. Held across frames to emit one down/up edge pair per
  // press instead of a key-down every frame the finger rests on it.
  bool esc_pressed_ = false;
};

}  // namespace rex::ui
