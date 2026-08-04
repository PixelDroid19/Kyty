#include "Emulator/Host/HostInput.h"

#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_stdinc.h"
#include "SDL_touch.h"

#include <algorithm>

namespace Kyty::Emulator::Host {

namespace {

[[nodiscard]] SDL_Event* NativeEvent(void* event)
{
	return static_cast<SDL_Event*>(event);
}

[[nodiscard]] WindowEvent TranslateWindowEvent(Uint8 event)
{
	switch (event)
	{
		case SDL_WINDOWEVENT_SHOWN: return WindowEvent::Shown;
		case SDL_WINDOWEVENT_HIDDEN: return WindowEvent::Hidden;
		case SDL_WINDOWEVENT_EXPOSED: return WindowEvent::Exposed;
		case SDL_WINDOWEVENT_MOVED: return WindowEvent::Moved;
		case SDL_WINDOWEVENT_RESIZED: return WindowEvent::Resized;
		case SDL_WINDOWEVENT_SIZE_CHANGED: return WindowEvent::SizeChanged;
		case SDL_WINDOWEVENT_MINIMIZED: return WindowEvent::Minimized;
		case SDL_WINDOWEVENT_MAXIMIZED: return WindowEvent::Maximized;
		case SDL_WINDOWEVENT_RESTORED: return WindowEvent::Restored;
		case SDL_WINDOWEVENT_ENTER: return WindowEvent::Enter;
		case SDL_WINDOWEVENT_LEAVE: return WindowEvent::Leave;
		case SDL_WINDOWEVENT_FOCUS_GAINED: return WindowEvent::FocusGained;
		case SDL_WINDOWEVENT_FOCUS_LOST: return WindowEvent::FocusLost;
		case SDL_WINDOWEVENT_CLOSE: return WindowEvent::Close;
		default: return WindowEvent::Unknown;
	}
}

[[nodiscard]] ControllerButton TranslateControllerButton(Uint8 button)
{
	switch (button)
	{
		case SDL_CONTROLLER_BUTTON_A: return ControllerButton::A;
		case SDL_CONTROLLER_BUTTON_B: return ControllerButton::B;
		case SDL_CONTROLLER_BUTTON_X: return ControllerButton::X;
		case SDL_CONTROLLER_BUTTON_Y: return ControllerButton::Y;
		case SDL_CONTROLLER_BUTTON_BACK: return ControllerButton::Back;
		case SDL_CONTROLLER_BUTTON_GUIDE: return ControllerButton::Guide;
		case SDL_CONTROLLER_BUTTON_START: return ControllerButton::Start;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return ControllerButton::LeftStick;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return ControllerButton::RightStick;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return ControllerButton::LeftShoulder;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return ControllerButton::RightShoulder;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return ControllerButton::DpadUp;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return ControllerButton::DpadDown;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return ControllerButton::DpadLeft;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return ControllerButton::DpadRight;
		default: return ControllerButton::Invalid;
	}
}

[[nodiscard]] ControllerAxis TranslateControllerAxis(Uint8 axis)
{
	switch (axis)
	{
		case SDL_CONTROLLER_AXIS_LEFTX: return ControllerAxis::LeftX;
		case SDL_CONTROLLER_AXIS_LEFTY: return ControllerAxis::LeftY;
		case SDL_CONTROLLER_AXIS_RIGHTX: return ControllerAxis::RightX;
		case SDL_CONTROLLER_AXIS_RIGHTY: return ControllerAxis::RightY;
		case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return ControllerAxis::TriggerLeft;
		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return ControllerAxis::TriggerRight;
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
		case SDL_QUIT: event->type = InputEventType::Quit; break;
		case SDL_APP_TERMINATING: event->type = InputEventType::Terminate; break;
		case SDL_APP_LOWMEMORY: event->type = InputEventType::LowMemory; break;
		case SDL_APP_WILLENTERBACKGROUND: event->type = InputEventType::WillEnterBackground; break;
		case SDL_APP_DIDENTERBACKGROUND: event->type = InputEventType::DidEnterBackground; break;
		case SDL_APP_WILLENTERFOREGROUND: event->type = InputEventType::WillEnterForeground; break;
		case SDL_APP_DIDENTERFOREGROUND: event->type = InputEventType::DidEnterForeground; break;

		case SDL_KEYDOWN:
		case SDL_KEYUP:
			event->type                     = InputEventType::Keyboard;
			event->keyboard.down            = native.type == SDL_KEYDOWN;
			event->keyboard.up              = native.type == SDL_KEYUP;
			event->keyboard.pressed         = native.key.state == SDL_PRESSED;
			event->keyboard.released        = native.key.state == SDL_RELEASED;
			event->keyboard.repeat          = native.key.repeat != 0u;
			event->keyboard.scan_code       = native.key.keysym.scancode;
			event->keyboard.key_code        = native.key.keysym.sym;
			event->keyboard.mod             = native.key.keysym.mod;
			break;

		case SDL_WINDOWEVENT:
			event->type             = InputEventType::Window;
			event->window.window_id = native.window.windowID;
			event->window.event     = TranslateWindowEvent(native.window.event);
			event->window.data1     = native.window.data1;
			event->window.data2     = native.window.data2;
			break;

		case SDL_DISPLAYEVENT:
			event->type                    = InputEventType::Display;
			event->display.display         = native.display.display;
			event->display.orientation_value = native.display.data1;
			event->display.native_orientation = native.display.event == SDL_DISPLAYEVENT_ORIENTATION;
			event->display.orientation      = TranslateDisplayOrientation(native.display.data1);
			break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
			event->type                  = InputEventType::MouseButton;
			event->mouse.down            = native.button.type == SDL_MOUSEBUTTONDOWN;
			event->mouse.up              = native.button.type == SDL_MOUSEBUTTONUP;
			event->mouse.left            = native.button.button == SDL_BUTTON_LEFT;
			event->mouse.middle          = native.button.button == SDL_BUTTON_MIDDLE;
			event->mouse.right           = native.button.button == SDL_BUTTON_RIGHT;
			event->mouse.x1              = native.button.button == SDL_BUTTON_X1;
			event->mouse.x2              = native.button.button == SDL_BUTTON_X2;
			event->mouse.touch           = native.button.which == SDL_TOUCH_MOUSEID;
			event->mouse.pressed         = native.button.state == SDL_PRESSED;
			event->mouse.released        = native.button.state == SDL_RELEASED;
			event->mouse.num_of_clicks   = native.button.clicks;
			event->mouse.x               = native.button.x;
			event->mouse.y               = native.button.y;
			break;

		case SDL_MOUSEWHEEL:
			event->type                = InputEventType::MouseWheel;
			event->mouse.touch         = native.wheel.which == SDL_TOUCH_MOUSEID;
			event->mouse.wheel         = true;
			event->mouse.x             = native.wheel.x;
			event->mouse.y             = native.wheel.y;
			break;

		case SDL_MOUSEMOTION:
			event->type                = InputEventType::MouseMotion;
			event->mouse.left          = (native.motion.state & SDL_BUTTON_LMASK) != 0u;
			event->mouse.middle        = (native.motion.state & SDL_BUTTON_MMASK) != 0u;
			event->mouse.right         = (native.motion.state & SDL_BUTTON_RMASK) != 0u;
			event->mouse.x1            = (native.motion.state & SDL_BUTTON_X1MASK) != 0u;
			event->mouse.x2            = (native.motion.state & SDL_BUTTON_X2MASK) != 0u;
			event->mouse.touch         = native.motion.which == SDL_TOUCH_MOUSEID;
			event->mouse.x             = native.motion.x;
			event->mouse.y             = native.motion.y;
			event->mouse.motion        = true;
			event->mouse.motion_x      = native.motion.xrel;
			event->mouse.motion_y      = native.motion.yrel;
			break;

		case SDL_FINGERMOTION:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP:
			event->type                 = InputEventType::Finger;
			event->finger.down          = native.tfinger.type == SDL_FINGERDOWN;
			event->finger.up            = native.tfinger.type == SDL_FINGERUP;
			event->finger.motion        = native.tfinger.type == SDL_FINGERMOTION;
			event->finger.finger_id     = static_cast<int>(native.tfinger.fingerId);
			event->finger.touch_id      = static_cast<int>(native.tfinger.touchId);
			event->finger.x             = native.tfinger.x;
			event->finger.y             = native.tfinger.y;
			event->finger.dx            = native.tfinger.dx;
			event->finger.dy            = native.tfinger.dy;
			event->finger.pressure      = native.tfinger.pressure;
			break;

		case SDL_CONTROLLERAXISMOTION:
			event->type                   = InputEventType::ControllerAxis;
			event->controller.id          = native.caxis.which;
			event->controller.axis_id     = TranslateControllerAxis(native.caxis.axis);
			event->controller.axis_value  = native.caxis.value;
			event->controller.axis        = true;
			break;

		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
			event->type                   = InputEventType::ControllerButton;
			event->controller.id          = native.cbutton.which;
			event->controller.button      = TranslateControllerButton(native.cbutton.button);
			event->controller.down        = native.cbutton.type == SDL_CONTROLLERBUTTONDOWN;
			event->controller.up          = native.cbutton.type == SDL_CONTROLLERBUTTONUP;
			event->controller.pressed     = native.cbutton.state == SDL_PRESSED;
			event->controller.released    = native.cbutton.state == SDL_RELEASED;
			break;

		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
		case SDL_CONTROLLERDEVICEREMAPPED:
			event->type                   = InputEventType::ControllerDevice;
			event->controller.id          = native.cdevice.which;
			event->controller.added       = native.cdevice.type == SDL_CONTROLLERDEVICEADDED;
			event->controller.removed     = native.cdevice.type == SDL_CONTROLLERDEVICEREMOVED;
			event->controller.remapped    = native.cdevice.type == SDL_CONTROLLERDEVICEREMAPPED;
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
	return new HostInput;
}

int HostInput::Poll()
{
	m_event = {};
	const int result = SDL_PollEvent(NativeEvent(m_native_event));
	if (result != 0)
	{
		TranslateEvent(*NativeEvent(m_native_event), &m_event);
	}
	return result;
}

int HostInput::Wait()
{
	m_event = {};
	const int result = SDL_WaitEvent(NativeEvent(m_native_event));
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
	return SDL_GetEventState(SDL_DISPLAYEVENT) == SDL_ENABLE;
}

bool HostInput::EnterPressed() const
{
	int       count    = 0;
	const auto* keyboard = SDL_GetKeyboardState(&count);
	if (keyboard == nullptr || count <= SDL_SCANCODE_KP_ENTER)
	{
		return false;
	}
	return keyboard[SDL_SCANCODE_RETURN] != 0 || keyboard[SDL_SCANCODE_KP_ENTER] != 0;
}

bool HostInput::OpenController(int device_index, int* instance_id)
{
	if (instance_id == nullptr)
	{
		return false;
	}
	*instance_id = 0;
	SDL_GameController* controller = SDL_GameControllerOpen(device_index);
	if (controller == nullptr)
	{
		return false;
	}
	SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
	if (joystick == nullptr)
	{
		SDL_GameControllerClose(controller);
		return false;
	}
	*instance_id = SDL_JoystickInstanceID(joystick);
	if (*instance_id < 0)
	{
		SDL_GameControllerClose(controller);
		return false;
	}
	return true;
}

void HostInput::CloseController(int instance_id)
{
	if (SDL_GameController* controller = SDL_GameControllerFromInstanceID(instance_id); controller != nullptr)
	{
		SDL_GameControllerClose(controller);
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

bool HostInput::HasAltModifier(uint16_t modifiers)
{
	return (modifiers & KMOD_ALT) != 0;
}

KeyboardAction HostInput::ClassifyKeyboardAction(int key_code)
{
	switch (key_code)
	{
		case SDLK_RETURN:
		case SDLK_SPACE:
		case SDLK_z: return KeyboardAction::Cross;
		case SDLK_ESCAPE:
		case SDLK_x: return KeyboardAction::Circle;
		case SDLK_c: return KeyboardAction::Square;
		case SDLK_v: return KeyboardAction::Triangle;
		case SDLK_UP: return KeyboardAction::Up;
		case SDLK_DOWN: return KeyboardAction::Down;
		case SDLK_LEFT: return KeyboardAction::Left;
		case SDLK_RIGHT: return KeyboardAction::Right;
		case SDLK_q: return KeyboardAction::L1;
		case SDLK_e: return KeyboardAction::R1;
		case SDLK_TAB:
		case SDLK_BACKSPACE: return KeyboardAction::Options;
		default: return KeyboardAction::None;
	}
}

} // namespace Kyty::Emulator::Host
