#include "Emulator/Controller.h"

#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Controller {

LIB_NAME("Pad", "Pad");

struct PadData
{
	uint32_t buttons;
	uint8_t  left_stick_x;
	uint8_t  left_stick_y;
	uint8_t  right_stick_x;
	uint8_t  right_stick_y;
	uint8_t  analog_buttons_l2;
	uint8_t  analog_buttons_r2;
	uint8_t  padding[2];
	float    orientation_x;
	float    orientation_y;
	float    orientation_z;
	float    orientation_w;
	float    acceleration_x;
	float    acceleration_y;
	float    acceleration_z;
	float    angular_velocity_x;
	float    angular_velocity_y;
	float    angular_velocity_z;
	uint8_t  touch_data_touch_num;
	uint8_t  touch_data_reserve[3];
	uint32_t touch_data_reserve1;
	uint16_t touch_data_touch0_x;
	uint16_t touch_data_touch0_y;
	uint8_t  touch_data_touch0_id;
	uint8_t  touch_data_touch0_reserve[3];
	uint16_t touch_data_touch1_x;
	uint16_t touch_data_touch1_y;
	uint8_t  touch_data_touch1_id;
	uint8_t  touch_data_touch1_reserve[3];
	bool     connected;
	uint64_t timestamp;
	uint32_t extension_unit_data_extension_unit_id;
	uint8_t  extension_unit_data_reserve[1];
	uint8_t  extension_unit_data_data_length;
	uint8_t  extension_unit_data_data[10];
	uint8_t  connected_count;
	uint8_t  reserve[2];
	uint8_t  device_unique_data_len;
	uint8_t  device_unique_data[12];
};

struct PadControllerInformation
{
	float    touch_pixel_density;
	uint16_t touch_resolution_x;
	uint16_t touch_resolution_y;
	uint8_t  stick_dead_zone_left;
	uint8_t  stick_dead_zone_right;
	uint8_t  connection_type;
	uint8_t  connected_count;
	bool     connected;
	int      device_class;
};

// ScePadExtControllerInformation — 0x40 bytes.
struct PadExtControllerInformation
{
	PadControllerInformation base;
	uint8_t                  device_class_ext;
	uint8_t                  connected_ext;
	uint8_t                  connection_type_ext;
	uint8_t                  reserved[0x40 - 0x1F];
};

// ScePadDeviceClassExtendedInformation — 0x14 bytes.
struct PadDeviceClassExtendedInformation
{
	int32_t device_class;
	uint8_t reserved[4];
	uint8_t class_data[12];
};

struct PadDeviceClassData
{
	int32_t device_class;
	bool    data_valid;
	uint8_t reserved[3];
	uint8_t class_data[16];
};

static_assert(sizeof(PadDeviceClassExtendedInformation) == 0x14);
static_assert(sizeof(PadDeviceClassData) == 0x18);

struct PadVibrationParam
{
	uint8_t large_motor;
	uint8_t small_motor;
};

struct ControllerState
{
	uint64_t time                                  = 0;
	uint32_t buttons                               = 0;
	int      axes[static_cast<int>(Axis::AxisMax)] = {128, 128, 128, 128, 0, 0};
};

class GameController
{
public:
	GameController()          = default;
	virtual ~GameController() = default;

	KYTY_CLASS_NO_COPY(GameController);

	void Connect(int id);
	void Disconnect(int id);
	void Button(int id, uint32_t button, bool down);
	void Axis(int id, Axis axis, int value);
	void GetConnectionInfo(bool* flag, int* count);
	void ReadState(ControllerState* state, bool* flag, int* count);
	int  ReadStates(ControllerState* states, int states_num, bool* flag, int* count);

private:
	static constexpr uint32_t STATES_MAX = 64;

	struct StatePrivate
	{
		bool obtained = false;
	};

	void                          CheckActive();
	[[nodiscard]] ControllerState GetLastState() const;
	void                          AddState(const ControllerState& state);

	Core::Mutex     m_mutex;
	Vector<int>     m_connected_ids;
	int             m_active_id       = -1;
	bool            m_connected       = false;
	int             m_connected_count = 0;
	ControllerState m_states[STATES_MAX];
	StatePrivate    m_private[STATES_MAX];
	ControllerState m_last_state;
	uint32_t        m_states_num  = 0;
	uint32_t        m_first_state = 0;
};

static GameController* g_controller = nullptr;

struct AgentPadOverlay
{
	enum class TapPhase: uint8_t
	{
		None,
		ReleaseBeforePress,
		Pressed,
		ReleaseAfterPress,
	};

	Core::Mutex mutex;
	uint32_t    buttons                                         = 0;
	uint8_t     axes[static_cast<int>(Axis::AxisMax)]           = {128, 128, 128, 128, 0, 0};
	bool        axis_set[static_cast<int>(Axis::AxisMax)]       = {};
	TapPhase    tap_phase                                       = TapPhase::None;
	uint32_t    tap_button                                      = 0;
	uint64_t    read_state_samples                               = 0;
	uint64_t    read_samples                                     = 0;
	uint64_t    delivered_taps                                   = 0;
};

static AgentPadOverlay g_agent_pad;

struct PadScriptEntry
{
	double   start_seconds = 0.0;
	double   end_seconds   = 0.0;
	uint32_t buttons       = 0;
	uint8_t  axis_mask     = 0;
	int8_t   left_x        = 0;
	int8_t   left_y        = 0;
	int8_t   right_x       = 0;
	int8_t   right_y       = 0;
};

struct PadScriptState
{
	uint32_t buttons   = 0;
	uint8_t  axis_mask = 0;
	int8_t   left_x    = 0;
	int8_t   left_y    = 0;
	int8_t   right_x   = 0;
	int8_t   right_y   = 0;

	bool operator==(const PadScriptState& other) const
	{
		return buttons == other.buttons && axis_mask == other.axis_mask && left_x == other.left_x && left_y == other.left_y &&
		       right_x == other.right_x && right_y == other.right_y;
	}
};

static std::string PadScriptTrim(std::string value)
{
	const auto not_space = [](unsigned char c) { return std::isspace(c) == 0; };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

static bool PadScriptParseNumber(const std::string& text, double* value)
{
	if (value == nullptr)
	{
		return false;
	}
	char* end = nullptr;
	const double parsed = std::strtod(text.c_str(), &end);
	if (end == text.c_str() || *end != '\0' || !std::isfinite(parsed) || parsed < 0.0)
	{
		return false;
	}
	*value = parsed;
	return true;
}

static bool PadScriptParseActions(const std::string& text, PadScriptEntry* entry)
{
	if (entry == nullptr)
	{
		return false;
	}

	std::size_t begin = 0;
	while (begin <= text.size())
	{
		const std::size_t end = text.find('+', begin);
		std::string       token = PadScriptTrim(text.substr(begin, end == std::string::npos ? end : end - begin));
		std::transform(token.begin(), token.end(), token.begin(), [](unsigned char c) {
			return static_cast<char>(std::tolower(c));
		});
		if (token.empty())
		{
			return false;
		}
		int8_t* axis_value = nullptr;
		uint8_t axis_bit   = 0;
		if (token == "left-stick-left" || token == "left_stick_left")
		{
			axis_value = &entry->left_x;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::LeftX);
			*axis_value = -1;
		} else if (token == "left-stick-right" || token == "left_stick_right")
		{
			axis_value = &entry->left_x;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::LeftX);
			*axis_value = 1;
		} else if (token == "left-stick-up" || token == "left_stick_up")
		{
			axis_value = &entry->left_y;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::LeftY);
			*axis_value = -1;
		} else if (token == "left-stick-down" || token == "left_stick_down")
		{
			axis_value = &entry->left_y;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::LeftY);
			*axis_value = 1;
		} else if (token == "right-stick-left" || token == "right_stick_left")
		{
			axis_value = &entry->right_x;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::RightX);
			*axis_value = -1;
		} else if (token == "right-stick-right" || token == "right_stick_right")
		{
			axis_value = &entry->right_x;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::RightX);
			*axis_value = 1;
		} else if (token == "right-stick-up" || token == "right_stick_up")
		{
			axis_value = &entry->right_y;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::RightY);
			*axis_value = -1;
		} else if (token == "right-stick-down" || token == "right_stick_down")
		{
			axis_value = &entry->right_y;
			axis_bit   = 1u << static_cast<uint8_t>(Axis::RightY);
			*axis_value = 1;
		}
		if (axis_value != nullptr)
		{
			if ((entry->axis_mask & axis_bit) != 0)
			{
				return false;
			}
			entry->axis_mask |= axis_bit;
		} else
		{
			uint32_t button = 0;
			if (!AgentPadButtonFromName(token.c_str(), &button))
			{
				return false;
			}
			entry->buttons |= button;
		}
		if (end == std::string::npos)
		{
			break;
		}
		begin = end + 1;
	}
	return entry->buttons != 0 || entry->axis_mask != 0;
}

static bool PadScriptParseText(const std::string& text, std::vector<PadScriptEntry>* entries, std::string* error)
{
	if (entries == nullptr || error == nullptr)
	{
		return false;
	}

	entries->clear();
	std::size_t line_start = 0;
	std::size_t line_number = 0;
	while (line_start <= text.size())
	{
		const std::size_t line_end = text.find('\n', line_start);
		std::string       line = text.substr(line_start, line_end == std::string::npos ? line_end : line_end - line_start);
		++line_number;
		if (const auto comment = line.find('#'); comment != std::string::npos)
		{
			line.resize(comment);
		}
		line = PadScriptTrim(line);
		if (!line.empty())
		{
			const std::size_t colon = line.find(':');
			if (colon == std::string::npos)
			{
				*error = "line " + std::to_string(line_number) + " has no ':' separator";
				return false;
			}

			std::string time_range = PadScriptTrim(line.substr(0, colon));
			std::string button_text = PadScriptTrim(line.substr(colon + 1));
			const std::size_t dash = time_range.find('-');
			double            start = 0.0;
			double            end   = 0.0;
			if (dash == std::string::npos)
			{
				if (!PadScriptParseNumber(time_range, &start))
				{
					*error = "line " + std::to_string(line_number) + " has an invalid start time";
					return false;
				}
				end = start + 0.300;
			} else
			{
				if (!PadScriptParseNumber(PadScriptTrim(time_range.substr(0, dash)), &start) ||
				    !PadScriptParseNumber(PadScriptTrim(time_range.substr(dash + 1)), &end) || end <= start)
				{
					*error = "line " + std::to_string(line_number) + " has an invalid time range";
					return false;
				}
			}
			if (end > 86400.0)
			{
				*error = "line " + std::to_string(line_number) + " exceeds the route time limit";
				return false;
			}

			PadScriptEntry entry {};
			entry.start_seconds = start;
			entry.end_seconds   = end;
			if (!PadScriptParseActions(button_text, &entry))
			{
				*error = "line " + std::to_string(line_number) + " has an unknown action";
				return false;
			}
			if (entries->size() >= 4096)
			{
				*error = "route contains too many entries";
				return false;
			}
			entries->push_back(entry);
		}
		if (line_end == std::string::npos)
		{
			break;
		}
		line_start = line_end + 1;
	}

	std::stable_sort(entries->begin(), entries->end(), [](const PadScriptEntry& lhs, const PadScriptEntry& rhs) {
		return lhs.start_seconds < rhs.start_seconds;
	});
	return true;
}

class PadScriptRuntime
{
public:
	bool Sample(PadScriptState* state)
	{
		if (state == nullptr)
		{
			return false;
		}
		std::lock_guard<std::mutex> lock(m_mutex);
		LoadLocked();
		if (!m_loaded)
		{
			return false;
		}

		const auto now = std::chrono::steady_clock::now();
		if (!m_started)
		{
			m_started = true;
			m_origin  = now;
		}
		const double elapsed = std::chrono::duration<double>(now - m_origin).count();
		PadScriptState value {};
		for (const auto& entry: m_entries)
		{
			if (elapsed >= entry.start_seconds && elapsed < entry.end_seconds)
			{
				value.buttons   |= entry.buttons;
				value.axis_mask |= entry.axis_mask;
				if ((entry.axis_mask & (1u << static_cast<uint8_t>(Axis::LeftX))) != 0)
				{
					value.left_x = entry.left_x;
				}
				if ((entry.axis_mask & (1u << static_cast<uint8_t>(Axis::LeftY))) != 0)
				{
					value.left_y = entry.left_y;
				}
				if ((entry.axis_mask & (1u << static_cast<uint8_t>(Axis::RightX))) != 0)
				{
					value.right_x = entry.right_x;
				}
				if ((entry.axis_mask & (1u << static_cast<uint8_t>(Axis::RightY))) != 0)
				{
					value.right_y = entry.right_y;
				}
			}
		}
		*state = value;
		if (std::getenv("KYTY_PAD_SCRIPT_LOG") != nullptr && !(value == m_last_state))
		{
			KYTY_LOG_DEBUG( "KYTY_PAD_SCRIPT state=0x%08" PRIx32 " axes=0x%02" PRIx8 " elapsed=%.3f\n", value.buttons,
			             value.axis_mask, elapsed);
			m_last_state = value;
		}
		return true;
	}

private:
	void LoadLocked()
	{
		if (m_initialized)
		{
			return;
		}
		m_initialized = true;

		const char* env = std::getenv("KYTY_PAD_SCRIPT");
		if (env == nullptr || env[0] == '\0')
		{
			return;
		}

		std::string source = env;
		if (source.front() == '@')
		{
			source.erase(source.begin());
		}
		if (source.empty())
		{
			KYTY_LOG_DEBUG( "KYTY_PAD_SCRIPT ignored: route source is empty\n");
			return;
		}
		std::string text;
		if (source.find('/') != std::string::npos || source.find('\\') != std::string::npos || source.front() == '.')
		{
			std::ifstream file(source, std::ios::binary | std::ios::ate);
			if (!file)
			{
				KYTY_LOG_DEBUG( "KYTY_PAD_SCRIPT cannot open route\n");
				return;
			}
			const auto size = file.tellg();
			if (size < 0 || static_cast<uint64_t>(size) > (1u << 20))
			{
				KYTY_LOG_DEBUG( "KYTY_PAD_SCRIPT route exceeds the size limit\n");
				return;
			}
			text.resize(static_cast<std::size_t>(size));
			file.seekg(0);
			file.read(text.data(), static_cast<std::streamsize>(text.size()));
			if (!file && !text.empty())
			{
				KYTY_LOG_DEBUG( "KYTY_PAD_SCRIPT route read failed\n");
				return;
			}
		} else
		{
			std::replace(source.begin(), source.end(), ';', '\n');
			text = source;
		}

		std::string error;
		if (!PadScriptParseText(text, &m_entries, &error) || m_entries.empty())
		{
			KYTY_LOG_DEBUG( "KYTY_PAD_SCRIPT ignored: %s\n", error.empty() ? "route is empty" : error.c_str());
			m_entries.clear();
			return;
		}
		m_loaded = true;
		KYTY_LOG_DEBUG( "KYTY_PAD_SCRIPT loaded entries=%zu\n", m_entries.size());
	}

	std::mutex                               m_mutex;
	std::vector<PadScriptEntry>              m_entries;
	std::chrono::steady_clock::time_point    m_origin{};
	PadScriptState                            m_last_state {std::numeric_limits<uint32_t>::max(), 0, 0, 0, 0, 0};
	bool                                     m_initialized = false;
	bool                                     m_loaded      = false;
	bool                                     m_started     = false;
};

static PadScriptRuntime g_pad_script;

static void AgentPadApplyToButtonsAndAxes(bool advance_tap, uint32_t* buttons, uint8_t* left_x, uint8_t* left_y, uint8_t* right_x,
                                          uint8_t* right_y, uint8_t* l2, uint8_t* r2)
{
	Core::LockGuard lock(g_agent_pad.mutex);
	uint32_t         tap_bits = 0;
	switch (g_agent_pad.tap_phase)
	{
		case AgentPadOverlay::TapPhase::ReleaseBeforePress:
			// First guest sample stays released so edge detectors see 0 → 1.
			tap_bits = 0;
			if (advance_tap)
			{
				g_agent_pad.tap_phase = AgentPadOverlay::TapPhase::Pressed;
			}
			break;
		case AgentPadOverlay::TapPhase::Pressed:
			tap_bits = g_agent_pad.tap_button;
			if (advance_tap)
			{
				g_agent_pad.tap_phase = AgentPadOverlay::TapPhase::ReleaseAfterPress;
				++g_agent_pad.delivered_taps;
			}
			break;
		case AgentPadOverlay::TapPhase::ReleaseAfterPress:
			tap_bits = 0;
			if (advance_tap)
			{
				g_agent_pad.tap_phase  = AgentPadOverlay::TapPhase::None;
				g_agent_pad.tap_button = 0;
			}
			break;
		case AgentPadOverlay::TapPhase::None: break;
	}

	const bool active = g_agent_pad.buttons != 0 || tap_bits != 0 || [&]() {
		for (int i = 0; i < static_cast<int>(Axis::AxisMax); ++i)
		{
			if (g_agent_pad.axis_set[i])
			{
				return true;
			}
		}
		return false;
	}();
	if (active && advance_tap)
	{
		++g_agent_pad.read_state_samples;
	} else if (active)
	{
		++g_agent_pad.read_samples;
	}
	if (buttons != nullptr)
	{
		*buttons |= g_agent_pad.buttons | tap_bits;
	}
	if (g_agent_pad.axis_set[static_cast<int>(Axis::LeftX)] && left_x != nullptr)
	{
		*left_x = g_agent_pad.axes[static_cast<int>(Axis::LeftX)];
	}
	if (g_agent_pad.axis_set[static_cast<int>(Axis::LeftY)] && left_y != nullptr)
	{
		*left_y = g_agent_pad.axes[static_cast<int>(Axis::LeftY)];
	}
	if (g_agent_pad.axis_set[static_cast<int>(Axis::RightX)] && right_x != nullptr)
	{
		*right_x = g_agent_pad.axes[static_cast<int>(Axis::RightX)];
	}
	if (g_agent_pad.axis_set[static_cast<int>(Axis::RightY)] && right_y != nullptr)
	{
		*right_y = g_agent_pad.axes[static_cast<int>(Axis::RightY)];
	}
	if (g_agent_pad.axis_set[static_cast<int>(Axis::TriggerLeft)] && l2 != nullptr)
	{
		*l2 = g_agent_pad.axes[static_cast<int>(Axis::TriggerLeft)];
		if (*l2 > 0 && buttons != nullptr)
		{
			*buttons |= PAD_BUTTON_L2;
		}
	}
	if (g_agent_pad.axis_set[static_cast<int>(Axis::TriggerRight)] && r2 != nullptr)
	{
		*r2 = g_agent_pad.axes[static_cast<int>(Axis::TriggerRight)];
		if (*r2 > 0 && buttons != nullptr)
		{
			*buttons |= PAD_BUTTON_R2;
		}
	}
}

bool AgentPadButtonFromName(const char* name, uint32_t* out_button)
{
	if (name == nullptr || out_button == nullptr)
	{
		return false;
	}
	if (std::strcmp(name, "cross") == 0)
	{
		*out_button = PAD_BUTTON_CROSS;
		return true;
	}
	if (std::strcmp(name, "circle") == 0)
	{
		*out_button = PAD_BUTTON_CIRCLE;
		return true;
	}
	if (std::strcmp(name, "square") == 0)
	{
		*out_button = PAD_BUTTON_SQUARE;
		return true;
	}
	if (std::strcmp(name, "triangle") == 0)
	{
		*out_button = PAD_BUTTON_TRIANGLE;
		return true;
	}
	if (std::strcmp(name, "up") == 0)
	{
		*out_button = PAD_BUTTON_UP;
		return true;
	}
	if (std::strcmp(name, "down") == 0)
	{
		*out_button = PAD_BUTTON_DOWN;
		return true;
	}
	if (std::strcmp(name, "left") == 0)
	{
		*out_button = PAD_BUTTON_LEFT;
		return true;
	}
	if (std::strcmp(name, "right") == 0)
	{
		*out_button = PAD_BUTTON_RIGHT;
		return true;
	}
	if (std::strcmp(name, "l1") == 0)
	{
		*out_button = PAD_BUTTON_L1;
		return true;
	}
	if (std::strcmp(name, "r1") == 0)
	{
		*out_button = PAD_BUTTON_R1;
		return true;
	}
	if (std::strcmp(name, "l2") == 0)
	{
		*out_button = PAD_BUTTON_L2;
		return true;
	}
	if (std::strcmp(name, "r2") == 0)
	{
		*out_button = PAD_BUTTON_R2;
		return true;
	}
	if (std::strcmp(name, "l3") == 0)
	{
		*out_button = PAD_BUTTON_L3;
		return true;
	}
	if (std::strcmp(name, "r3") == 0)
	{
		*out_button = PAD_BUTTON_R3;
		return true;
	}
	if (std::strcmp(name, "options") == 0)
	{
		*out_button = PAD_BUTTON_OPTIONS;
		return true;
	}
	if (std::strcmp(name, "touch_pad") == 0 || std::strcmp(name, "touchpad") == 0)
	{
		*out_button = PAD_BUTTON_TOUCH_PAD;
		return true;
	}
	return false;
}

bool AgentPadAxisFromName(const char* name, Axis* out_axis)
{
	if (name == nullptr || out_axis == nullptr)
	{
		return false;
	}
	if (std::strcmp(name, "left_x") == 0)
	{
		*out_axis = Axis::LeftX;
		return true;
	}
	if (std::strcmp(name, "left_y") == 0)
	{
		*out_axis = Axis::LeftY;
		return true;
	}
	if (std::strcmp(name, "right_x") == 0)
	{
		*out_axis = Axis::RightX;
		return true;
	}
	if (std::strcmp(name, "right_y") == 0)
	{
		*out_axis = Axis::RightY;
		return true;
	}
	if (std::strcmp(name, "l2") == 0 || std::strcmp(name, "trigger_left") == 0)
	{
		*out_axis = Axis::TriggerLeft;
		return true;
	}
	if (std::strcmp(name, "r2") == 0 || std::strcmp(name, "trigger_right") == 0)
	{
		*out_axis = Axis::TriggerRight;
		return true;
	}
	return false;
}

void AgentPadSetButton(uint32_t button, bool down)
{
	Core::LockGuard lock(g_agent_pad.mutex);
	if (down)
	{
		g_agent_pad.buttons |= button;
	} else
	{
		g_agent_pad.buttons &= ~button;
	}
}

void AgentPadSetAxis(Axis axis, uint8_t value)
{
	const int axis_id = static_cast<int>(axis);
	if (axis_id < 0 || axis_id >= static_cast<int>(Axis::AxisMax))
	{
		return;
	}
	Core::LockGuard lock(g_agent_pad.mutex);
	g_agent_pad.axes[axis_id]     = value;
	g_agent_pad.axis_set[axis_id] = true;
	if (axis == Axis::TriggerLeft)
	{
		if (value > 0)
		{
			g_agent_pad.buttons |= PAD_BUTTON_L2;
		} else
		{
			g_agent_pad.buttons &= ~PAD_BUTTON_L2;
		}
	}
	if (axis == Axis::TriggerRight)
	{
		if (value > 0)
		{
			g_agent_pad.buttons |= PAD_BUTTON_R2;
		} else
		{
			g_agent_pad.buttons &= ~PAD_BUTTON_R2;
		}
	}
}

bool AgentPadScheduleTap(uint32_t button)
{
	if (button == 0)
	{
		return false;
	}
	Core::LockGuard lock(g_agent_pad.mutex);
	if (g_agent_pad.tap_phase != AgentPadOverlay::TapPhase::None || (g_agent_pad.buttons & button) != 0)
	{
		return false;
	}
	// The pulse is release/press/release at guest sample boundaries. A host
	// wall-clock sleep cannot guarantee an edge reaches a title that polls more
	// than once per frame.
	g_agent_pad.tap_button = button;
	g_agent_pad.tap_phase  = AgentPadOverlay::TapPhase::ReleaseBeforePress;
	return true;
}

void AgentPadClear()
{
	Core::LockGuard lock(g_agent_pad.mutex);
	g_agent_pad.buttons = 0;
	g_agent_pad.tap_phase  = AgentPadOverlay::TapPhase::None;
	g_agent_pad.tap_button = 0;
	for (int i = 0; i < static_cast<int>(Axis::AxisMax); ++i)
	{
		g_agent_pad.axis_set[i] = false;
		g_agent_pad.axes[i]     = (i < 4) ? 128 : 0;
	}
}

void AgentPadGetState(uint32_t* buttons, uint8_t* axes)
{
	Core::LockGuard lock(g_agent_pad.mutex);
	if (buttons != nullptr)
	{
		*buttons = g_agent_pad.buttons;
	}
	if (axes != nullptr)
	{
		for (int i = 0; i < static_cast<int>(Axis::AxisMax); ++i)
		{
			axes[i] = g_agent_pad.axes[i];
		}
	}
}

void AgentPadGetReadStats(AgentPadReadStats* out)
{
	if (out == nullptr)
	{
		return;
	}
	Core::LockGuard lock(g_agent_pad.mutex);
	out->read_state_samples = g_agent_pad.read_state_samples;
	out->read_samples       = g_agent_pad.read_samples;
	out->delivered_taps     = g_agent_pad.delivered_taps;
	out->tap_pending        = g_agent_pad.tap_phase != AgentPadOverlay::TapPhase::None;
}

void AgentPadApplyReadStateSample(uint32_t* buttons)
{
	uint8_t unused = 0;
	AgentPadApplyToButtonsAndAxes(true, buttons, &unused, &unused, &unused, &unused, &unused, &unused);
}

void ControllerSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	EXIT_IF(g_controller != nullptr);

	g_controller = new GameController;
}

void ControllerSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void ControllerSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

void GameController::Connect(int id)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(m_connected_ids.Contains(id));

	m_connected_ids.Add(id);

	CheckActive();
}

void GameController::Disconnect(int id)
{
	Core::LockGuard lock(m_mutex);

	EXIT_IF(!m_connected_ids.Contains(id));

	m_connected_ids.Remove(id);

	CheckActive();
}

void GameController::CheckActive()
{
	bool reset          = false;
	bool next_connected = false;
	int  next_active    = CONTROLLER_KEYBOARD_ID;

	// A real SDL controller owns axes and its own buttons when present. The
	// keyboard remains an independent button source and is merged in Button().
	for (uint32_t i = 0; i < m_connected_ids.Size(); i++)
	{
		const int id = m_connected_ids.At(i);
		if (id != CONTROLLER_KEYBOARD_ID)
		{
			next_active    = id;
			next_connected = true;
			break;
		}
	}

	if (!next_connected && m_connected_ids.Contains(CONTROLLER_KEYBOARD_ID))
	{
		next_connected = true;
	}

	if (m_connected != next_connected || (next_connected && m_active_id != next_active))
	{
		m_active_id = next_active;
		m_connected = next_connected;
		if (next_connected)
		{
			m_connected_count++;
		}
		reset = true;
	}

	if (reset)
	{
		m_states_num = 0;
		m_last_state = ControllerState();
	}
}

ControllerState GameController::GetLastState() const
{
	if (m_states_num == 0)
	{
		return m_last_state;
	}

	auto last = (m_first_state + m_states_num - 1) % STATES_MAX;

	return m_states[last];
}

void GameController::AddState(const ControllerState& state)
{
	if (m_states_num >= STATES_MAX)
	{
		m_states_num  = STATES_MAX - 1;
		m_first_state = (m_first_state + 1) % STATES_MAX;
	}

	auto index = (m_first_state + m_states_num) % STATES_MAX;

	m_states[index] = state;
	m_last_state    = state;

	m_private[index].obtained = false;

	m_states_num++;
}

void GameController::Button(int id, uint32_t button, bool down)
{
	Core::LockGuard lock(m_mutex);

	if (m_active_id == id || id == CONTROLLER_KEYBOARD_ID)
	{
		auto state = GetLastState();

		state.time = Kernel::KernelGetProcessTime();

		if (down)
		{
			state.buttons |= button;
		} else
		{
			state.buttons &= ~button;
		}

		AddState(state);
	}
}

void GameController::Axis(int id, Controller::Axis axis, int value)
{
	Core::LockGuard lock(m_mutex);

	if (m_active_id == id)
	{
		auto state = GetLastState();

		state.time = Kernel::KernelGetProcessTime();

		int axis_id = static_cast<int>(axis);

		EXIT_IF(axis_id < 0 || axis_id >= static_cast<int>(Controller::Axis::AxisMax));

		state.axes[axis_id] = value;

		if (axis == Controller::Axis::TriggerLeft)
		{
			if (value > 0)
			{
				state.buttons |= PAD_BUTTON_L2;
			} else
			{
				state.buttons &= ~PAD_BUTTON_L2;
			}
		}

		if (axis == Controller::Axis::TriggerRight)
		{
			if (value > 0)
			{
				state.buttons |= PAD_BUTTON_R2;
			} else
			{
				state.buttons &= ~PAD_BUTTON_R2;
			}
		}

		AddState(state);
	}
}

void GameController::GetConnectionInfo(bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
}

void GameController::ReadState(ControllerState* state, bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(state == nullptr);

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;
	*state = GetLastState();
}

int GameController::ReadStates(ControllerState* states, int states_num, bool* flag, int* count)
{
	EXIT_IF(flag == nullptr);
	EXIT_IF(count == nullptr);
	EXIT_IF(states == nullptr);
	EXIT_IF(states_num < 1 || states_num > static_cast<int>(STATES_MAX));

	Core::LockGuard lock(m_mutex);

	*flag  = m_connected;
	*count = m_connected_count;

	int ret_num = 0;

	if (m_connected)
	{
		if (m_states_num == 0)
		{
			ret_num   = 1;
			states[0] = m_last_state;
		} else
		{
			for (uint32_t i = 0; i < m_states_num; i++)
			{
				if (ret_num >= states_num)
				{
					break;
				}
				auto index = (m_first_state + i) % STATES_MAX;
				if (!m_private[index].obtained)
				{
					m_private[index].obtained = true;

					states[ret_num++] = m_states[index];
				}
			}
		}
	}

	return ret_num;
}

void ControllerConnect(int id)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Connect(id);
}

void ControllerDisconnect(int id)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Disconnect(id);
}

void ControllerButton(int id, uint32_t button, bool down)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Button(id, button, down);
}

void ControllerAxis(int id, Axis axis, int value)
{
	EXIT_IF(g_controller == nullptr);

	g_controller->Axis(id, axis, value);
}

// SCE_PAD_ERROR_* (public Sony codes; used for ordinary guest failures, not EXIT).
constexpr int PAD_ERROR_INVALID_ARG        = static_cast<int>(0x80920001u);
constexpr int PAD_ERROR_INVALID_HANDLE     = static_cast<int>(0x80920003u);
constexpr int PAD_ERROR_NOT_INITIALIZED    = static_cast<int>(0x80920005u);
constexpr int PAD_ERROR_DEVICE_NO_HANDLE   = static_cast<int>(0x80920008u);

// Virtual keyboard/gamepad id: always connected after PadOpen so titles see a pad
// without requiring a physical SDL controller (playability on hosts with only a keyboard).
constexpr int kPrimaryHandle = 1;

static bool IsPrimaryPadHandle(int handle)
{
	return handle == 0 || handle == kPrimaryHandle;
}

static bool g_pad_initialized = false;
static bool g_pad_opened      = false;
// Counts PadReadState calls since last PadOpen (many titles Open then Read twice per tick).
static int  g_reads_since_open = 0;

static int PadOpenCore(int user_id, int type, int index, const void* param, bool extended)
{
	PRINT_NAME();

	if (!g_pad_initialized)
	{
		return PAD_ERROR_NOT_INITIALIZED;
	}

	// user_id -1: no logged-in user → no handle (same contract as SCE docs / other HLEs).
	if (user_id == -1)
	{
		return PAD_ERROR_DEVICE_NO_HANDLE;
	}

	const bool type_accepted = extended ? (type >= 0 && type <= 2) : (type == 0);
	if (user_id != 1 || !type_accepted || index != 0 || (!extended && param != nullptr))
	{
		return PAD_ERROR_DEVICE_NO_HANDLE;
	}

	// Ensure a virtual pad is present for keyboard input and for connected=true.
	EXIT_IF(g_controller == nullptr);
	bool connected       = false;
	int  connected_count = 0;
	g_controller->GetConnectionInfo(&connected, &connected_count);
	if (!connected)
	{
		g_controller->Connect(CONTROLLER_KEYBOARD_ID);
	}

	// Real OS returns ALREADY_OPENED on re-open; guest must use GetHandle for the prior handle.
	// Observed Gen5 titles poll Open each frame and then ReadState with that handle, so return
	// the live handle instead of ALREADY_OPENED for this Open → ReadState pattern.
	g_reads_since_open = 0;

	if (g_pad_opened)
	{
		return kPrimaryHandle;
	}

	g_pad_opened = true;

	return kPrimaryHandle;
}

int KYTY_SYSV_ABI PadInit()
{
	PRINT_NAME();

	g_pad_initialized = true;

	return OK;
}

int KYTY_SYSV_ABI PadOpen(int user_id, int type, int index, const void* param)
{
	return PadOpenCore(user_id, type, index, param, false);
}

int KYTY_SYSV_ABI PadOpenExt(int user_id, int type, int index, const void* param)
{
	return PadOpenCore(user_id, type, index, param, true);
}

int KYTY_SYSV_ABI PadClose(int handle)
{
	PRINT_NAME();

	if (handle != 0 && handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	return OK;
}

int KYTY_SYSV_ABI PadGetHandle(int user_id, int type, int index)
{
	PRINT_NAME();

	if (!g_pad_initialized)
	{
		return PAD_ERROR_NOT_INITIALIZED;
	}
	if (user_id == -1)
	{
		return PAD_ERROR_DEVICE_NO_HANDLE;
	}
	if (!g_pad_opened || user_id != 1 || type != 0 || index != 0)
	{
		return PAD_ERROR_DEVICE_NO_HANDLE;
	}

	return kPrimaryHandle;
}

int KYTY_SYSV_ABI PadSetMotionSensorState(int handle, bool enable)
{
	PRINT_NAME();

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int KYTY_SYSV_ABI PadSetTiltCorrectionState(int handle, bool enable)
{
	PRINT_NAME();

	if (handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	KYTY_LOG_DEBUG("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int KYTY_SYSV_ABI PadSetAngularVelocityDeadbandState(int handle, bool enable)
{
	PRINT_NAME();

	if (handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	KYTY_LOG_DEBUG("\t enable = %s\n", (enable ? "true" : "false"));

	return OK;
}

int KYTY_SYSV_ABI PadResetOrientation(int handle)
{
	PRINT_NAME();

	if (handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	// PadReadState and PadRead expose the neutral host orientation, so there is
	// no calibration state to retain after a reset.
	return OK;
}

static void FillPadControllerInformation(PadControllerInformation* info, int connected_count)
{
	info->touch_pixel_density   = 44.86f;
	info->touch_resolution_x    = 1920;
	info->touch_resolution_y    = 943;
	info->stick_dead_zone_left  = controller_get_axis(-32768, 32767, 8000) - 128;
	info->stick_dead_zone_right = controller_get_axis(-32768, 32767, 8000) - 128;
	info->connection_type       = 0;
	info->connected_count       = (connected_count > 0 ? (connected_count > 255 ? 255 : connected_count) : 1);
	info->connected             = true;
	info->device_class          = 0;
}

int KYTY_SYSV_ABI PadGetControllerInformation(int handle, PadControllerInformation* info)
{
	PRINT_NAME();

	EXIT_IF(g_controller == nullptr);

	int  connected_count = 0;
	bool connected       = false;

	g_controller->GetConnectionInfo(&connected, &connected_count);

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (info == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	FillPadControllerInformation(info, connected_count);

	return OK;
}

int KYTY_SYSV_ABI PadGetExtControllerInformation(int handle, void* info)
{
	PRINT_NAME();

	EXIT_IF(g_controller == nullptr);

	if (!IsPrimaryPadHandle(handle))
	{
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (info == nullptr)
	{
		return PAD_ERROR_INVALID_ARG;
	}

	int  connected_count = 0;
	bool connected       = false;
	g_controller->GetConnectionInfo(&connected, &connected_count);

	auto* out = static_cast<PadExtControllerInformation*>(info);
	std::memset(out, 0, sizeof(*out));
	FillPadControllerInformation(&out->base, connected_count);
	out->device_class_ext    = 0;
	out->connected_ext       = 1;
	out->connection_type_ext = 0;

	return OK;
}

int KYTY_SYSV_ABI PadDeviceClassGetExtendedInformation(int handle, void* info)
{
	PRINT_NAME();

	if (!IsPrimaryPadHandle(handle))
	{
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (info == nullptr)
	{
		return PAD_ERROR_INVALID_ARG;
	}

	auto* out = static_cast<PadDeviceClassExtendedInformation*>(info);
	std::memset(out, 0, sizeof(*out));
	out->device_class = 0;

	return OK;
}

int KYTY_SYSV_ABI PadDeviceClassParseData(int handle, const PadData* data, void* class_data)
{
	PRINT_NAME();

	if (!IsPrimaryPadHandle(handle))
	{
		return PAD_ERROR_INVALID_HANDLE;
	}
	if (data == nullptr || class_data == nullptr)
	{
		return PAD_ERROR_INVALID_ARG;
	}

	auto* out = static_cast<PadDeviceClassData*>(class_data);
	std::memset(out, 0, sizeof(*out));
	out->device_class = 0;
	out->data_valid   = data->connected;
	return OK;
}

int KYTY_SYSV_ABI PadReadState(int handle, PadData* data)
{
	PRINT_NAME();

	EXIT_IF(g_controller == nullptr);

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState state;

	g_controller->ReadState(&state, &connected, &connected_count);

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (data == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	// Optional diagnostic (KYTY_AUTO_CROSS=1): synthesize button edges so multi-
	// screen splash/title flows can advance without a physical pad. Continuous
	// hold only yields one rising edge — later logos (e.g. after Motion Twin)
	// never see a new press. Observed pattern is PadOpen + two PadReadState per
	// tick: release on the first read and press on the second so both
	// within-tick (curr & ~prev) and frame-to-frame edge detectors fire.
	uint32_t auto_buttons = 0;
	g_reads_since_open++;
	if (const char* auto_cross = std::getenv("KYTY_AUTO_CROSS"); auto_cross != nullptr && auto_cross[0] == '1')
	{
		static uint64_t first_read = 0;
		const uint64_t  now        = Kernel::KernelGetProcessTime();
		if (first_read == 0)
		{
			first_read = now;
		}
		const uint64_t elapsed = now - first_read;
		// Warm up 2s, then 1s period: 250ms press window, 750ms release.
		if (elapsed > 2'000'000ull)
		{
			const uint64_t period = 1'000'000ull;
			const uint64_t phase  = (elapsed - 2'000'000ull) % period;
			const bool     press  = phase < 250'000ull;
			// Second read of the open cycle carries the press; first stays released.
			if (press && g_reads_since_open >= 2)
			{
				auto_buttons = PAD_BUTTON_CROSS;
				// Every 4th period also pulse Options|TouchPad for UIs that
				// only listen on those (still only on the pressed sample).
				const uint64_t period_index = (elapsed - 2'000'000ull) / period;
				if ((period_index % 4ull) == 3ull)
				{
					auto_buttons |= PAD_BUTTON_OPTIONS | PAD_BUTTON_TOUCH_PAD;
				}
			}
		}
	}

	PadScriptState scripted {};
	(void)g_pad_script.Sample(&scripted);
	const auto script_axis = [](int8_t value) -> uint8_t { return value < 0 ? 0u : (value > 0 ? 255u : 128u); };

	const uint64_t now_ts = Kernel::KernelGetProcessTime();

	data->buttons                = state.buttons | auto_buttons | scripted.buttons;
	data->left_stick_x           = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::LeftX))) != 0
	                                    ? script_axis(scripted.left_x)
	                                    : state.axes[static_cast<int>(Axis::LeftX)];
	data->left_stick_y           = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::LeftY))) != 0
	                                    ? script_axis(scripted.left_y)
	                                    : state.axes[static_cast<int>(Axis::LeftY)];
	data->right_stick_x          = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::RightX))) != 0
	                                    ? script_axis(scripted.right_x)
	                                    : state.axes[static_cast<int>(Axis::RightX)];
	data->right_stick_y          = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::RightY))) != 0
	                                    ? script_axis(scripted.right_y)
	                                    : state.axes[static_cast<int>(Axis::RightY)];
	data->analog_buttons_l2      = state.axes[static_cast<int>(Axis::TriggerLeft)];
	data->analog_buttons_r2      = state.axes[static_cast<int>(Axis::TriggerRight)];
	AgentPadApplyToButtonsAndAxes(true, &data->buttons, &data->left_stick_x, &data->left_stick_y, &data->right_stick_x,
	                              &data->right_stick_y, &data->analog_buttons_l2, &data->analog_buttons_r2);
	data->orientation_x          = 0.0f;
	data->orientation_y          = 0.0f;
	data->orientation_z          = 0.0f;
	data->orientation_w          = 1.0f;
	// Resting DualSense gravity (m/s^2); zero accel can look like a disconnected IMU sample.
	data->acceleration_x         = 0.0f;
	data->acceleration_y         = 0.0f;
	data->acceleration_z         = 9.8f;
	data->angular_velocity_x     = 0.0f;
	data->angular_velocity_y     = 0.0f;
	data->angular_velocity_z     = 0.0f;
	data->touch_data_touch_num   = 0;
	data->touch_data_touch0_x    = 0;
	data->touch_data_touch0_y    = 0;
	data->touch_data_touch0_id   = 1;
	data->touch_data_touch1_x    = 0;
	data->touch_data_touch1_y    = 0;
	data->touch_data_touch1_id   = 2;
	data->connected              = true;
	// Always refresh timestamp so held samples still look live to edge/time logic.
	data->timestamp              = now_ts;
	data->connected_count        = (connected_count > 0 ? connected_count : 1);
	data->device_unique_data_len = 0;
	return OK;
}

int KYTY_SYSV_ABI PadRead(int handle, PadData* data, int num)
{
	PRINT_NAME();

	if (num < 1 || num > 64) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (data == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	EXIT_IF(g_controller == nullptr);

	int             connected_count = 0;
	bool            connected       = false;
	ControllerState states[64];

	int ret_num = g_controller->ReadStates(states, num, &connected, &connected_count);
	PadScriptState scripted {};
	const bool script_active = g_pad_script.Sample(&scripted);
	const auto script_axis = [](int8_t value) -> uint8_t { return value < 0 ? 0u : (value > 0 ? 255u : 128u); };

	if (!connected)
	{
		ret_num = 1;
	}

	for (int i = 0; i < ret_num; i++)
	{
		data[i].buttons                = states[i].buttons | scripted.buttons;
		data[i].left_stick_x           = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::LeftX))) != 0
		                                    ? script_axis(scripted.left_x)
		                                    : states[i].axes[static_cast<int>(Axis::LeftX)];
		data[i].left_stick_y           = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::LeftY))) != 0
		                                    ? script_axis(scripted.left_y)
		                                    : states[i].axes[static_cast<int>(Axis::LeftY)];
		data[i].right_stick_x          = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::RightX))) != 0
		                                    ? script_axis(scripted.right_x)
		                                    : states[i].axes[static_cast<int>(Axis::RightX)];
		data[i].right_stick_y          = (scripted.axis_mask & (1u << static_cast<uint8_t>(Axis::RightY))) != 0
		                                    ? script_axis(scripted.right_y)
		                                    : states[i].axes[static_cast<int>(Axis::RightY)];
		data[i].analog_buttons_l2      = states[i].axes[static_cast<int>(Axis::TriggerLeft)];
		data[i].analog_buttons_r2      = states[i].axes[static_cast<int>(Axis::TriggerRight)];
		// Advance the tap FSM at most once per PadRead call (first sample only) so a
		// large num cannot collapse release/press/release into one HLE invocation.
		AgentPadApplyToButtonsAndAxes(i == 0, &data[i].buttons, &data[i].left_stick_x, &data[i].left_stick_y, &data[i].right_stick_x,
		                              &data[i].right_stick_y, &data[i].analog_buttons_l2, &data[i].analog_buttons_r2);
		data[i].orientation_x          = 0.0f;
		data[i].orientation_y          = 0.0f;
		data[i].orientation_z          = 0.0f;
		data[i].orientation_w          = 1.0f;
		data[i].acceleration_x         = 0.0f;
		data[i].acceleration_y         = 0.0f;
		data[i].acceleration_z         = 0.0f;
		data[i].angular_velocity_x     = 0.0f;
		data[i].angular_velocity_y     = 0.0f;
		data[i].angular_velocity_z     = 0.0f;
		data[i].touch_data_touch_num   = 0;
		data[i].touch_data_touch0_x    = 0;
		data[i].touch_data_touch0_y    = 0;
		data[i].touch_data_touch0_id   = 1;
		data[i].touch_data_touch1_x    = 0;
		data[i].touch_data_touch1_y    = 0;
		data[i].touch_data_touch1_id   = 2;
		data[i].connected              = connected || script_active;
		data[i].timestamp              = states[i].time;
		data[i].connected_count        = (connected_count > 0 ? connected_count : (script_active ? 1 : 0));
		data[i].device_unique_data_len = 0;
	}

	return ret_num;
}

int KYTY_SYSV_ABI PadSetVibrationMode(int handle, int mode)
{
	PRINT_NAME();

	if (handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	KYTY_LOG_DEBUG("\t mode = %d\n", mode);

	return OK;
}

int KYTY_SYSV_ABI PadSetTriggerEffect(int handle, const void* param)
{
	PRINT_NAME();

	if (handle != kPrimaryHandle)
	{
		return PAD_ERROR_INVALID_HANDLE;
	}

	// DualSense adaptive-trigger program: accepted and ignored (no host hardware).
	(void)param;

	return OK;
}

int KYTY_SYSV_ABI PadGetTriggerEffectState(int handle, PadTriggerEffectStateInformation* info)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = %d\n", handle);
	if (info == nullptr)
	{
		return PAD_ERROR_INVALID_ARG;
	}
	// Idle state for both adaptive triggers (L2/R2).
	info->state[0] = 0;
	info->state[1] = 0;
	return OK;
}

int KYTY_SYSV_ABI PadSetVibration(int handle, const PadVibrationParam* param)
{
	PRINT_NAME();

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t large_motor = %d\n", static_cast<int>(param->large_motor));
	KYTY_LOG_DEBUG("\t small_motor = %d\n", static_cast<int>(param->small_motor));

	return OK;
}

int KYTY_SYSV_ABI PadResetLightBar(int handle)
{
	PRINT_NAME();

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return OK;
}

int KYTY_SYSV_ABI PadSetLightBar(int handle, const PadLightBarParam* param)
{
	PRINT_NAME();

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (param == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return OK;
}

} // namespace Kyty::Libs::Controller

#endif // KYTY_EMU_ENABLED
