#include "Emulator/Host/InputProfile.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/JsonReader.h"
#include "Kyty/Core/String.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>

namespace Kyty::Emulator::Host {

namespace {

constexpr const char* PROFILE_SCHEMA = "kyty_input_profile_v1";
// A complete profile is a few kilobytes; anything far larger is not a profile.
constexpr uintmax_t PROFILE_MAX_BYTES = 256 * 1024;

constexpr std::array<const char*, PAD_CONTROL_COUNT> CONTROL_NAMES = {
    "cross",
    "circle",
    "square",
    "triangle",
    "up",
    "down",
    "left",
    "right",
    "l1",
    "r1",
    "l2",
    "r2",
    "l3",
    "r3",
    "options",
    "touchpad",
    "left-stick-left",
    "left-stick-right",
    "left-stick-up",
    "left-stick-down",
    "right-stick-left",
    "right-stick-right",
    "right-stick-up",
    "right-stick-down",
};

struct MouseButtonName
{
	const char* name;
	int         code;
};

constexpr std::array<MouseButtonName, 5> MOUSE_BUTTON_NAMES = {{
    {"Mouse Left", MOUSE_BUTTON_LEFT},
    {"Mouse Middle", MOUSE_BUTTON_MIDDLE},
    {"Mouse Right", MOUSE_BUTTON_RIGHT},
    {"Mouse X1", MOUSE_BUTTON_X1},
    {"Mouse X2", MOUSE_BUTTON_X2},
}};

// Kept identical to the launcher's "Default" profile (docs/input-profiles.md).
constexpr const char* DEFAULT_PROFILE = R"({
  "schema": "kyty_input_profile_v1",
  "name": "Default",
  "keyboard": {
    "cross": ["Z", "Space", "Return"], "circle": ["X"], "square": ["C"], "triangle": ["V"],
    "up": ["Up"], "down": ["Down"], "left": ["Left"], "right": ["Right"],
    "l1": ["Q"], "r1": ["E"], "l2": ["1", "Mouse Right"], "r2": ["3", "Mouse Left"],
    "l3": ["F"], "r3": ["H"], "options": ["Tab", "Backspace"], "touchpad": ["T"],
    "left-stick-left": ["A"], "left-stick-right": ["D"], "left-stick-up": ["W"], "left-stick-down": ["S"],
    "right-stick-left": ["J"], "right-stick-right": ["L"], "right-stick-up": ["I"], "right-stick-down": ["K"]
  },
  "mouse": { "look": true, "sensitivity": 1.0, "invert_y": false },
  "gamepad": {
    "device": "",
    "buttons": {
      "cross": ["a"], "circle": ["b"], "square": ["x"], "triangle": ["y"],
      "up": ["dpup"], "down": ["dpdown"], "left": ["dpleft"], "right": ["dpright"],
      "l1": ["leftshoulder"], "r1": ["rightshoulder"], "l2": [], "r2": [],
      "l3": ["leftstick"], "r3": ["rightstick"], "options": ["start"], "touchpad": ["back", "touchpad"]
    },
    "left_stick": { "deadzone": 0.10, "invert_x": false, "invert_y": false },
    "right_stick": { "deadzone": 0.10, "invert_x": false, "invert_y": false },
    "trigger_deadzone": 0.0
  }
})";

struct JsonDeleter
{
	void operator()(const Core::Json* json) const { delete json; }
};

std::string Utf8(const String& s)
{
	return s.utf8_str().GetData();
}

// Every member of `object` must be one of `keys`, appear once, and every key must be present.
template <size_t N>
std::string CheckMembers(const Core::Json* object, const std::array<const char*, N>& keys, const std::string& path)
{
	if (!object->IsObject())
	{
		return path + " must be an object";
	}
	std::array<bool, N> seen {};
	for (const Core::Json* member: object->ToArray())
	{
		const std::string name  = Utf8(member->GetName());
		const auto*       found = std::find_if(keys.begin(), keys.end(), [&](const char* key) { return name == key; });
		if (found == keys.end())
		{
			return path + "." + name + " is not a known field";
		}
		auto index = static_cast<size_t>(std::distance(keys.begin(), found));
		if (seen[index])
		{
			return path + "." + name + " appears twice";
		}
		seen[index] = true;
	}
	for (size_t i = 0; i < N; ++i)
	{
		if (!seen[i])
		{
			return path + "." + keys[i] + " is missing";
		}
	}
	return {};
}

const Core::Json* Member(const Core::Json* object, const char* key)
{
	for (const Core::Json* member: object->ToArray())
	{
		if (Utf8(member->GetName()) == key)
		{
			return member;
		}
	}
	return nullptr;
}

std::string ReadBool(const Core::Json* object, const char* key, const std::string& path, bool* out)
{
	const Core::Json* value = Member(object, key);
	if (!value->IsBool())
	{
		return path + "." + key + " must be true or false";
	}
	*out = value->ToBool() != 0;
	return {};
}

std::string ReadRange(const Core::Json* object, const char* key, double min, double max, const std::string& path, float* out)
{
	const Core::Json* value = Member(object, key);
	if (!value->IsNumber() || value->ToFloat() < min || value->ToFloat() > max)
	{
		char range[64] = {};
		std::snprintf(range, sizeof(range), " must be a number from %g to %g", min, max);
		return path + "." + key + range;
	}
	*out = static_cast<float>(value->ToFloat());
	return {};
}

int KeyboardInputFromName(const std::string& name)
{
	for (const auto& mouse: MOUSE_BUTTON_NAMES)
	{
		if (name == mouse.name)
		{
			return mouse.code;
		}
	}
	return HostInput::ScanCodeFromName(name.c_str());
}

std::string ReadKeyboard(const Core::Json* keyboard, InputProfile* profile)
{
	if (auto error = CheckMembers(keyboard, CONTROL_NAMES, "keyboard"); !error.empty())
	{
		return error;
	}
	for (size_t control = 0; control < PAD_CONTROL_COUNT; ++control)
	{
		const std::string path  = std::string("keyboard.") + CONTROL_NAMES[control];
		const Core::Json* list  = Member(keyboard, CONTROL_NAMES[control]);
		auto&             codes = profile->keyboard[control];
		if (!list->IsArray())
		{
			return path + " must be a list of key names";
		}
		for (const Core::Json* item: list->ToArray())
		{
			const std::string name = item->IsString() ? Utf8(item->ToString()) : std::string();
			const int         code = KeyboardInputFromName(name);
			if (code == 0)
			{
				return path + ": '" + name + "' is not a key name";
			}
			if (code > 0 && HostInput::IsReservedScanCode(code))
			{
				return path + ": '" + name + "' is reserved for a host command";
			}
			codes.push_back(code);
		}
	}
	return {};
}

std::string ReadGamepadButtons(const Core::Json* buttons, InputProfile* profile)
{
	std::array<const char*, PAD_BUTTON_CONTROL_COUNT> button_controls {};
	std::copy_n(CONTROL_NAMES.begin(), PAD_BUTTON_CONTROL_COUNT, button_controls.begin());
	if (auto error = CheckMembers(buttons, button_controls, "gamepad.buttons"); !error.empty())
	{
		return error;
	}
	for (size_t control = 0; control < PAD_BUTTON_CONTROL_COUNT; ++control)
	{
		const std::string path = std::string("gamepad.buttons.") + CONTROL_NAMES[control];
		const Core::Json* list = Member(buttons, CONTROL_NAMES[control]);
		if (!list->IsArray())
		{
			return path + " must be a list of gamepad button names";
		}
		for (const Core::Json* item: list->ToArray())
		{
			const std::string      name   = item->IsString() ? Utf8(item->ToString()) : std::string();
			const ControllerButton button = HostInput::ControllerButtonFromName(name.c_str());
			if (button == ControllerButton::Invalid)
			{
				return path + ": '" + name + "' is not a gamepad button name";
			}
			profile->gamepad_buttons[control].push_back(button);
		}
	}
	return {};
}

std::string ReadStick(const Core::Json* stick, const std::string& path, StickTuning* out)
{
	if (auto error = CheckMembers(stick, std::array<const char*, 3> {"deadzone", "invert_x", "invert_y"}, path); !error.empty())
	{
		return error;
	}
	if (auto error = ReadRange(stick, "deadzone", 0.0, 0.9, path, &out->deadzone); !error.empty())
	{
		return error;
	}
	if (auto error = ReadBool(stick, "invert_x", path, &out->invert_x); !error.empty())
	{
		return error;
	}
	return ReadBool(stick, "invert_y", path, &out->invert_y);
}

std::string ReadMouse(const Core::Json* mouse, InputProfile* profile)
{
	if (auto error = CheckMembers(mouse, std::array<const char*, 3> {"look", "sensitivity", "invert_y"}, "mouse"); !error.empty())
	{
		return error;
	}
	if (auto error = ReadBool(mouse, "look", "mouse", &profile->mouse_look); !error.empty())
	{
		return error;
	}
	if (auto error = ReadRange(mouse, "sensitivity", 0.1, 10.0, "mouse", &profile->mouse_sensitivity); !error.empty())
	{
		return error;
	}
	return ReadBool(mouse, "invert_y", "mouse", &profile->mouse_invert_y);
}

std::string ReadGamepad(const Core::Json* gamepad, InputProfile* profile)
{
	if (auto error = CheckMembers(
	        gamepad, std::array<const char*, 5> {"device", "buttons", "left_stick", "right_stick", "trigger_deadzone"}, "gamepad");
	    !error.empty())
	{
		return error;
	}
	const Core::Json* device = Member(gamepad, "device");
	if (!device->IsString())
	{
		return "gamepad.device must be a string";
	}
	profile->gamepad_device = Utf8(device->ToString());
	if (auto error = ReadGamepadButtons(Member(gamepad, "buttons"), profile); !error.empty())
	{
		return error;
	}
	if (auto error = ReadStick(Member(gamepad, "left_stick"), "gamepad.left_stick", &profile->left_stick); !error.empty())
	{
		return error;
	}
	if (auto error = ReadStick(Member(gamepad, "right_stick"), "gamepad.right_stick", &profile->right_stick); !error.empty())
	{
		return error;
	}
	return ReadRange(gamepad, "trigger_deadzone", 0.0, 0.9, "gamepad", &profile->trigger_deadzone);
}

std::string ReadProfile(const Core::Json* root, InputProfile* profile)
{
	if (auto error = CheckMembers(root, std::array<const char*, 5> {"schema", "name", "keyboard", "mouse", "gamepad"}, "profile");
	    !error.empty())
	{
		return error;
	}
	const Core::Json* schema = Member(root, "schema");
	if (!schema->IsString() || Utf8(schema->ToString()) != PROFILE_SCHEMA)
	{
		return std::string("profile.schema must be '") + PROFILE_SCHEMA + "'";
	}
	const Core::Json* name = Member(root, "name");
	if (!name->IsString() || name->ToString().IsEmpty())
	{
		return "profile.name must be a non-empty string";
	}
	profile->name = Utf8(name->ToString());
	if (auto error = ReadKeyboard(Member(root, "keyboard"), profile); !error.empty())
	{
		return error;
	}
	if (auto error = ReadMouse(Member(root, "mouse"), profile); !error.empty())
	{
		return error;
	}
	return ReadGamepad(Member(root, "gamepad"), profile);
}

} // namespace

const char* PadControlName(PadControl control)
{
	return CONTROL_NAMES[static_cast<size_t>(control)];
}

std::string ParseInputProfile(const std::string& text, InputProfile* profile)
{
	const std::unique_ptr<const Core::Json, JsonDeleter> root(Core::Json::Create(String::FromUtf8(text.c_str())));
	if (root == nullptr)
	{
		return "not valid JSON: " + Utf8(Core::Json::GetError());
	}
	InputProfile parsed;
	if (auto error = ReadProfile(root.get(), &parsed); !error.empty())
	{
		return error;
	}
	*profile = std::move(parsed);
	return {};
}

InputProfile DefaultInputProfile()
{
	InputProfile profile;
	const auto   error = ParseInputProfile(DEFAULT_PROFILE, &profile);
	EXIT_IF(!error.empty());
	return profile;
}

std::string LoadInputProfile(const std::string& path, InputProfile* profile)
{
	std::error_code ec;
	const auto      size = std::filesystem::file_size(path, ec);
	if (ec)
	{
		return "cannot read '" + path + "': " + ec.message();
	}
	if (size > PROFILE_MAX_BYTES)
	{
		return "'" + path + "' is larger than " + std::to_string(PROFILE_MAX_BYTES) + " bytes";
	}
	std::ifstream file(path, std::ios::binary);
	std::string   text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
	if (!file && !file.eof())
	{
		return "cannot read '" + path + "'";
	}
	return ParseInputProfile(text, profile);
}

bool ControllerGuidMatches(const std::string& a, const std::string& b)
{
	if (a.size() != b.size())
	{
		return false;
	}
	for (size_t i = 0; i < a.size(); ++i)
	{
		// Bytes 2-3 (hex digits 4-7) hold a CRC of the reported name, which differs between drivers for one model.
		if ((i < 4 || i >= 8) && std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
		{
			return false;
		}
	}
	return true;
}

} // namespace Kyty::Emulator::Host
