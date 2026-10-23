#include "Emulator/Host/HostInput.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace Kyty::Emulator::Host {

namespace {

[[nodiscard]] SDL_Event* NativeEvent(void* event)
{
	return static_cast<SDL_Event*>(event);
}

[[nodiscard]] WindowEvent TranslateWindowEvent(Uint32 type)
{
	switch (type)
	{
		case SDL_EVENT_WINDOW_SHOWN: return WindowEvent::Shown;
		case SDL_EVENT_WINDOW_HIDDEN: return WindowEvent::Hidden;
		case SDL_EVENT_WINDOW_EXPOSED: return WindowEvent::Exposed;
		case SDL_EVENT_WINDOW_MOVED: return WindowEvent::Moved;
		case SDL_EVENT_WINDOW_RESIZED: return WindowEvent::Resized;
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: return WindowEvent::SizeChanged;
		case SDL_EVENT_WINDOW_MINIMIZED: return WindowEvent::Minimized;
		case SDL_EVENT_WINDOW_MAXIMIZED: return WindowEvent::Maximized;
		case SDL_EVENT_WINDOW_RESTORED: return WindowEvent::Restored;
		case SDL_EVENT_WINDOW_MOUSE_ENTER: return WindowEvent::Enter;
		case SDL_EVENT_WINDOW_MOUSE_LEAVE: return WindowEvent::Leave;
		case SDL_EVENT_WINDOW_FOCUS_GAINED: return WindowEvent::FocusGained;
		case SDL_EVENT_WINDOW_FOCUS_LOST: return WindowEvent::FocusLost;
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED: return WindowEvent::Close;
		default: return WindowEvent::Unknown;
	}
}

[[nodiscard]] ControllerButton TranslateControllerButton(Uint8 button)
{
	switch (button)
	{
		case SDL_GAMEPAD_BUTTON_SOUTH: return ControllerButton::A;
		case SDL_GAMEPAD_BUTTON_EAST: return ControllerButton::B;
		case SDL_GAMEPAD_BUTTON_WEST: return ControllerButton::X;
		case SDL_GAMEPAD_BUTTON_NORTH: return ControllerButton::Y;
		case SDL_GAMEPAD_BUTTON_BACK: return ControllerButton::Back;
		case SDL_GAMEPAD_BUTTON_GUIDE: return ControllerButton::Guide;
		case SDL_GAMEPAD_BUTTON_START: return ControllerButton::Start;
		case SDL_GAMEPAD_BUTTON_LEFT_STICK: return ControllerButton::LeftStick;
		case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return ControllerButton::RightStick;
		case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ControllerButton::LeftShoulder;
		case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ControllerButton::RightShoulder;
		case SDL_GAMEPAD_BUTTON_DPAD_UP: return ControllerButton::DpadUp;
		case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ControllerButton::DpadDown;
		case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ControllerButton::DpadLeft;
		case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ControllerButton::DpadRight;
		default: return ControllerButton::Invalid;
	}
}

[[nodiscard]] ControllerAxis TranslateControllerAxis(Uint8 axis)
{
	switch (axis)
	{
		case SDL_GAMEPAD_AXIS_LEFTX: return ControllerAxis::LeftX;
		case SDL_GAMEPAD_AXIS_LEFTY: return ControllerAxis::LeftY;
		case SDL_GAMEPAD_AXIS_RIGHTX: return ControllerAxis::RightX;
		case SDL_GAMEPAD_AXIS_RIGHTY: return ControllerAxis::RightY;
		case SDL_GAMEPAD_AXIS_LEFT_TRIGGER: return ControllerAxis::TriggerLeft;
		case SDL_GAMEPAD_AXIS_RIGHT_TRIGGER: return ControllerAxis::TriggerRight;
		default: return ControllerAxis::Invalid;
	}
}

[[nodiscard]] DisplayOrientation TranslateDisplayOrientation(Sint32 orientation)
{
	switch (orientation)
	{
		case SDL_ORIENTATION_LANDSCAPE: return DisplayOrientation::Landscape;
		case SDL_ORIENTATION_LANDSCAPE_FLIPPED: return DisplayOrientation::LandscapeFlipped;
		case SDL_ORIENTATION_PORTRAIT: return DisplayOrientation::Portrait;
		case SDL_ORIENTATION_PORTRAIT_FLIPPED: return DisplayOrientation::PortraitFlipped;
		default: return DisplayOrientation::Unknown;
	}
}

void TranslateEvent(const SDL_Event& native, InputEvent* event)
{
	if (event == nullptr)
	{
		return;
	}

	switch (native.type)
	{
		case SDL_EVENT_QUIT: event->type = InputEventType::Quit; break;
		case SDL_EVENT_TERMINATING: event->type = InputEventType::Terminate; break;
		case SDL_EVENT_LOW_MEMORY: event->type = InputEventType::LowMemory; break;
		case SDL_EVENT_WILL_ENTER_BACKGROUND: event->type = InputEventType::WillEnterBackground; break;
		case SDL_EVENT_DID_ENTER_BACKGROUND: event->type = InputEventType::DidEnterBackground; break;
		case SDL_EVENT_WILL_ENTER_FOREGROUND: event->type = InputEventType::WillEnterForeground; break;
		case SDL_EVENT_DID_ENTER_FOREGROUND: event->type = InputEventType::DidEnterForeground; break;

		case SDL_EVENT_KEY_DOWN:
		case SDL_EVENT_KEY_UP:
			event->type                     = InputEventType::Keyboard;
			event->keyboard.down            = native.type == SDL_EVENT_KEY_DOWN;
			event->keyboard.up              = native.type == SDL_EVENT_KEY_UP;
			event->keyboard.pressed         = native.key.down;
			event->keyboard.released        = !native.key.down;
			event->keyboard.repeat          = native.key.repeat;
			event->keyboard.scan_code       = native.key.scancode;
			event->keyboard.key_code        = static_cast<int>(native.key.key);
			event->keyboard.mod             = native.key.mod;
			break;

		case SDL_EVENT_WINDOW_SHOWN:
		case SDL_EVENT_WINDOW_HIDDEN:
		case SDL_EVENT_WINDOW_EXPOSED:
		case SDL_EVENT_WINDOW_MOVED:
		case SDL_EVENT_WINDOW_RESIZED:
		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		case SDL_EVENT_WINDOW_MINIMIZED:
		case SDL_EVENT_WINDOW_MAXIMIZED:
		case SDL_EVENT_WINDOW_RESTORED:
		case SDL_EVENT_WINDOW_MOUSE_ENTER:
		case SDL_EVENT_WINDOW_MOUSE_LEAVE:
		case SDL_EVENT_WINDOW_FOCUS_GAINED:
		case SDL_EVENT_WINDOW_FOCUS_LOST:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			event->type             = InputEventType::Window;
			event->window.window_id = native.window.windowID;
			event->window.event     = TranslateWindowEvent(native.type);
			event->window.data1     = native.window.data1;
			event->window.data2     = native.window.data2;
			break;

		case SDL_EVENT_DISPLAY_ORIENTATION:
			event->type                    = InputEventType::Display;
			event->display.display         = native.display.displayID;
			event->display.orientation_value = native.display.data1;
			event->display.native_orientation = true;
			event->display.orientation      = TranslateDisplayOrientation(native.display.data1);
			break;

		case SDL_EVENT_MOUSE_BUTTON_DOWN:
		case SDL_EVENT_MOUSE_BUTTON_UP:
			event->type                  = InputEventType::MouseButton;
			event->mouse.down            = native.button.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
			event->mouse.up              = native.button.type == SDL_EVENT_MOUSE_BUTTON_UP;
			event->mouse.left            = native.button.button == SDL_BUTTON_LEFT;
			event->mouse.middle          = native.button.button == SDL_BUTTON_MIDDLE;
			event->mouse.right           = native.button.button == SDL_BUTTON_RIGHT;
			event->mouse.x1              = native.button.button == SDL_BUTTON_X1;
			event->mouse.x2              = native.button.button == SDL_BUTTON_X2;
			event->mouse.touch           = native.button.which == SDL_TOUCH_MOUSEID;
			event->mouse.pressed         = native.button.down;
			event->mouse.released        = !native.button.down;
			event->mouse.num_of_clicks   = native.button.clicks;
			event->mouse.x               = static_cast<int>(native.button.x);
			event->mouse.y               = static_cast<int>(native.button.y);
			break;

		case SDL_EVENT_MOUSE_WHEEL:
			event->type                = InputEventType::MouseWheel;
			event->mouse.touch         = native.wheel.which == SDL_TOUCH_MOUSEID;
			event->mouse.wheel         = true;
			event->mouse.x             = native.wheel.integer_x;
			event->mouse.y             = native.wheel.integer_y;
			break;

		case SDL_EVENT_MOUSE_MOTION:
			event->type                = InputEventType::MouseMotion;
			event->mouse.left          = (native.motion.state & SDL_BUTTON_LMASK) != 0u;
			event->mouse.middle        = (native.motion.state & SDL_BUTTON_MMASK) != 0u;
			event->mouse.right         = (native.motion.state & SDL_BUTTON_RMASK) != 0u;
			event->mouse.x1            = (native.motion.state & SDL_BUTTON_X1MASK) != 0u;
			event->mouse.x2            = (native.motion.state & SDL_BUTTON_X2MASK) != 0u;
			event->mouse.touch         = native.motion.which == SDL_TOUCH_MOUSEID;
			event->mouse.x             = static_cast<int>(native.motion.x);
			event->mouse.y             = static_cast<int>(native.motion.y);
			event->mouse.motion        = true;
			event->mouse.motion_x      = static_cast<int>(native.motion.xrel);
			event->mouse.motion_y      = static_cast<int>(native.motion.yrel);
			break;

		case SDL_EVENT_FINGER_MOTION:
		case SDL_EVENT_FINGER_DOWN:
		case SDL_EVENT_FINGER_UP:
			event->type                 = InputEventType::Finger;
			event->finger.down          = native.tfinger.type == SDL_EVENT_FINGER_DOWN;
			event->finger.up            = native.tfinger.type == SDL_EVENT_FINGER_UP;
			event->finger.motion        = native.tfinger.type == SDL_EVENT_FINGER_MOTION;
			event->finger.finger_id     = static_cast<int>(native.tfinger.fingerID);
			event->finger.touch_id      = static_cast<int>(native.tfinger.touchID);
			event->finger.x             = native.tfinger.x;
			event->finger.y             = native.tfinger.y;
			event->finger.dx            = native.tfinger.dx;
			event->finger.dy            = native.tfinger.dy;
			event->finger.pressure      = native.tfinger.pressure;
			break;

		case SDL_EVENT_GAMEPAD_AXIS_MOTION:
			event->type                   = InputEventType::ControllerAxis;
			event->controller.id          = static_cast<int>(native.gaxis.which);
			event->controller.axis_id     = TranslateControllerAxis(native.gaxis.axis);
			event->controller.axis_value  = native.gaxis.value;
			event->controller.axis        = true;
			break;

		case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
		case SDL_EVENT_GAMEPAD_BUTTON_UP:
			event->type                   = InputEventType::ControllerButton;
			event->controller.id          = static_cast<int>(native.gbutton.which);
			event->controller.button      = TranslateControllerButton(native.gbutton.button);
			event->controller.down        = native.gbutton.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
			event->controller.up          = native.gbutton.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
			event->controller.pressed     = native.gbutton.down;
			event->controller.released    = !native.gbutton.down;
			break;

		case SDL_EVENT_GAMEPAD_ADDED:
		case SDL_EVENT_GAMEPAD_REMOVED:
		case SDL_EVENT_GAMEPAD_REMAPPED:
			event->type                   = InputEventType::ControllerDevice;
			event->controller.id          = static_cast<int>(native.gdevice.which);
			event->controller.added       = native.gdevice.type == SDL_EVENT_GAMEPAD_ADDED;
			event->controller.removed     = native.gdevice.type == SDL_EVENT_GAMEPAD_REMOVED;
			event->controller.remapped    = native.gdevice.type == SDL_EVENT_GAMEPAD_REMAPPED;
			break;

		default: break;
	}
}

} // namespace

HostInput::HostInput(): m_native_event(new SDL_Event) {}

HostInput::~HostInput()
{
	delete NativeEvent(m_native_event);
	m_native_event = nullptr;
}

HostInput* HostInput::Create()
{
	// Key codes are the unshifted code points of the host layout, which is what
	// KeyboardTextCharacter expects; no Latin or number-row substitution.
	SDL_SetHint(SDL_HINT_KEYCODE_OPTIONS, "none");
	return new HostInput;
}

int HostInput::Poll()
{
	m_event = {};
	const int result = SDL_PollEvent(NativeEvent(m_native_event)) ? 1 : 0;
	if (result != 0)
	{
		TranslateEvent(*NativeEvent(m_native_event), &m_event);
	}
	return result;
}

int HostInput::Wait()
{
	m_event = {};
	const int result = SDL_WaitEvent(NativeEvent(m_native_event)) ? 1 : 0;
	if (result != 0)
	{
		TranslateEvent(*NativeEvent(m_native_event), &m_event);
	}
	return result;
}

const InputEvent* HostInput::GetEvent() const
{
	return &m_event;
}

const void* HostInput::GetNativeEvent() const
{
	return m_native_event;
}

bool HostInput::DisplayEventsEnabled() const
{
	return SDL_EventEnabled(SDL_EVENT_DISPLAY_ORIENTATION);
}

bool HostInput::EnterPressed() const
{
	int       count    = 0;
	const auto* keyboard = SDL_GetKeyboardState(&count);
	if (keyboard == nullptr || count <= SDL_SCANCODE_KP_ENTER)
	{
		return false;
	}
	return keyboard[SDL_SCANCODE_RETURN] || keyboard[SDL_SCANCODE_KP_ENTER];
}

bool HostInput::OpenController(int device_id, int* instance_id)
{
	if (instance_id == nullptr)
	{
		return false;
	}
	*instance_id = 0;
	if (device_id <= 0 || SDL_OpenGamepad(static_cast<SDL_JoystickID>(device_id)) == nullptr)
	{
		return false;
	}
	*instance_id = device_id;
	return true;
}

void HostInput::CloseController(int instance_id)
{
	if (SDL_Gamepad* gamepad = SDL_GetGamepadFromID(static_cast<SDL_JoystickID>(instance_id)); gamepad != nullptr)
	{
		SDL_CloseGamepad(gamepad);
	}
}

const char* HostInput::LastError() const
{
	return SDL_GetError();
}

int HostInput::NormalizeAxis(ControllerAxis axis, int value)
{
	const bool trigger = axis == ControllerAxis::TriggerLeft || axis == ControllerAxis::TriggerRight;
	const int  min     = trigger ? 0 : -32768;
	const int  max     = 32767;
	const int  clamped = std::clamp(value, min, max);
	const int  scaled  = (255 * (clamped - min)) / (max - min);
	return std::clamp(scaled, 0, 255);
}

bool HostInput::IsEscapeKey(int key_code)
{
	return key_code == SDLK_ESCAPE;
}

bool HostInput::IsPauseKey(int key_code)
{
	return key_code == SDLK_F9;
}

bool HostInput::IsF11Key(int key_code)
{
	return key_code == SDLK_F11;
}

bool HostInput::IsEnterKey(int key_code)
{
	return key_code == SDLK_RETURN || key_code == SDLK_KP_ENTER;
}

bool HostInput::IsF1Key(int key_code)
{
	return key_code == SDLK_F1;
}

bool HostInput::IsTextEntryToggleKey(int key_code)
{
	return key_code == SDLK_F2;
}

uint16_t HostInput::KeyboardHidUsage(int scan_code)
{
	// SDL scancodes are the USB HID keyboard usages (usage page 0x07).
	return (scan_code >= SDL_SCANCODE_A && scan_code <= SDL_SCANCODE_RGUI) ? static_cast<uint16_t>(scan_code) : 0;
}

uint16_t HostInput::KeyboardTextCharacter(int key_code, uint16_t modifiers)
{
	// The SDL keycode of a printable key is the code point its unshifted position types
	// under the host layout; keypad and function keys carry the scancode mask instead.
	if (key_code < 0x20 || key_code == SDLK_DELETE || key_code > 0xffff || (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) != 0)
	{
		return 0;
	}
	const bool shift  = (modifiers & SDL_KMOD_SHIFT) != 0;
	const bool upper  = shift != ((modifiers & SDL_KMOD_CAPS) != 0);
	const bool letter = (key_code >= 'a' && key_code <= 'z') || (key_code >= 0xe0 && key_code <= 0xfe && key_code != 0xf7);
	if (letter)
	{
		return static_cast<uint16_t>(upper ? key_code - 0x20 : key_code);
	}
	return shift ? 0 : static_cast<uint16_t>(key_code);
}

bool HostInput::HasAltModifier(uint16_t modifiers)
{
	return (modifiers & SDL_KMOD_ALT) != 0;
}

KeyboardAction HostInput::ClassifyKeyboardAction(int key_code)
{
	switch (key_code)
	{
		case SDLK_RETURN:
		case SDLK_SPACE:
		case SDLK_Z: return KeyboardAction::Cross;
		case SDLK_ESCAPE:
		case SDLK_X: return KeyboardAction::Circle;
		case SDLK_C: return KeyboardAction::Square;
		case SDLK_V: return KeyboardAction::Triangle;
		case SDLK_UP: return KeyboardAction::Up;
		case SDLK_DOWN: return KeyboardAction::Down;
		case SDLK_LEFT: return KeyboardAction::Left;
		case SDLK_RIGHT: return KeyboardAction::Right;
		case SDLK_Q: return KeyboardAction::L1;
		case SDLK_E: return KeyboardAction::R1;
		case SDLK_TAB:
		case SDLK_BACKSPACE: return KeyboardAction::Options;
		default: return KeyboardAction::None;
	}
}

} // namespace Kyty::Emulator::Host
