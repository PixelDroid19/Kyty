#include "Kyty/Core/Common.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <array>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("SystemGesture", 1, "SystemGesture", 1, 1);

namespace SystemGesture {

constexpr int SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT      = static_cast<int>(0x80890001u);
constexpr int SYSTEM_GESTURE_ERROR_NOT_SUPPORTED_GESTURE = static_cast<int>(0x80890002u);
constexpr int SYSTEM_GESTURE_ERROR_INDEX_OUT_OF_ARRAY    = static_cast<int>(0x80890004u);
constexpr int SYSTEM_GESTURE_ERROR_INVALID_HANDLE        = static_cast<int>(0x80890006u);
constexpr int SYSTEM_GESTURE_ERROR_INVALID_TOUCH_AREA    = static_cast<int>(0x80890007u);
constexpr int SYSTEM_GESTURE_ERROR_ALREADY_OPENED        = static_cast<int>(0x8089000Au);

constexpr int32_t SYSTEM_GESTURE_INPUT_TOUCH_PAD = 0;
constexpr int32_t kGestureHandle                 = 1;

enum TouchState : uint32_t
{
	TOUCH_STATE_INACTIVE = 0,
	TOUCH_STATE_BEGIN    = 1,
	TOUCH_STATE_ACTIVE   = 2,
	TOUCH_STATE_END      = 3,
};

constexpr uint32_t GESTURE_TYPE_FLICK = 0x20;

struct Vector2
{
	float x;
	float y;
};

struct Rectangle
{
	float   x;
	float   y;
	float   width;
	float   height;
	uint8_t reserve[8];
};

// ScePadTouchData inside the guest's ScePadData report.
struct PadTouch
{
	uint16_t x;
	uint16_t y;
	uint8_t  id;
	uint8_t  reserve[3];
};

struct PadTouchData
{
	uint8_t                 touch_num;
	uint8_t                 reserve[3];
	uint32_t                reserve1;
	std::array<PadTouch, 2> touch;
};

// ScePadData places touchData at 0x34 and the microsecond timestamp at 0x50.
constexpr size_t kPadDataTouchOffset     = 0x34;
constexpr size_t kPadDataTimestampOffset = 0x50;

struct TouchPadData
{
	int32_t        pad_handle;
	int32_t        report_number;
	const uint8_t* pad_data_buffer;
	uint8_t        reserve[8];
};

struct FlickEventProperty
{
	uint16_t primitive_id;
	uint16_t padding;
	Vector2  delta_vector;
	Vector2  released_position;
	Vector2  pressed_position;
	uint8_t  reserve[60];
};

struct TouchEvent
{
	uint32_t event_id;
	uint32_t event_state;
	uint32_t gesture_type;
	uint8_t  is_updated;
	uint8_t  padding[3];
	uint64_t updated_time;
	union
	{
		uint8_t            property_buf[88];
		FlickEventProperty flick;
	} property;
	uint8_t reserve[56];
};

static_assert(sizeof(PadTouchData) == 24);
static_assert(sizeof(TouchPadData) == 24);
static_assert(sizeof(FlickEventProperty) == 88);
static_assert(sizeof(TouchEvent) == 168);
static_assert(offsetof(TouchEvent, property) == 0x18);

// One finger on the touch pad, followed from press to release.
struct Primitive
{
	uint32_t state;
	uint16_t primitive_id;
	uint8_t  pad_touch_id;
	Vector2  pressed_position;
	Vector2  current_position;
	Vector2  delta_vector;
	uint64_t updated_time;
};

constexpr uint32_t kRecognizerMagic     = 0x47455352u; // "RSEG"
constexpr uint32_t kMaxRecognizerEvents = 2;

// Kept inside the guest's opaque SceSystemGestureTouchRecognizer (361 qwords).
struct RecognizerState
{
	uint32_t   magic;
	uint32_t   gesture_type;
	bool       has_rectangle;
	Rectangle  rectangle;
	uint32_t   next_event_id;
	uint32_t   event_count;
	TouchEvent events[kMaxRecognizerEvents];
};

static_assert(sizeof(RecognizerState) <= sizeof(uint64_t) * 361);

struct Context
{
	std::mutex               mutex;
	bool                     opened            = false;
	uint16_t                 next_primitive_id = 0;
	std::array<Primitive, 2> primitives {};
};

static Context g_context;

static bool IsValidHandle(int32_t handle)
{
	return handle == kGestureHandle && g_context.opened;
}

static const PadTouch* FindPadTouch(const PadTouchData& touch_data, uint8_t id)
{
	for (uint32_t i = 0; i < touch_data.touch_num && i < touch_data.touch.size(); i++)
	{
		if (touch_data.touch[i].id == id)
		{
			return &touch_data.touch[i];
		}
	}
	return nullptr;
}

static Primitive* FindPrimitive(uint8_t pad_touch_id)
{
	for (auto& p: g_context.primitives)
	{
		if ((p.state == TOUCH_STATE_BEGIN || p.state == TOUCH_STATE_ACTIVE) && p.pad_touch_id == pad_touch_id)
		{
			return &p;
		}
	}
	return nullptr;
}

static Primitive* FreePrimitive()
{
	for (auto& p: g_context.primitives)
	{
		if (p.state == TOUCH_STATE_INACTIVE)
		{
			return &p;
		}
	}
	return nullptr;
}

static void UpdatePrimitives(const PadTouchData& touch_data, uint64_t timestamp)
{
	for (auto& p: g_context.primitives)
	{
		if (p.state == TOUCH_STATE_END)
		{
			p.state = TOUCH_STATE_INACTIVE;
			continue;
		}
		if (p.state == TOUCH_STATE_INACTIVE)
		{
			continue;
		}
		const PadTouch* touch = FindPadTouch(touch_data, p.pad_touch_id);
		if (touch == nullptr)
		{
			// Released: the last movement stays as the flick's delta.
			p.state = TOUCH_STATE_END;
			continue;
		}
		Vector2 position {static_cast<float>(touch->x), static_cast<float>(touch->y)};
		p.delta_vector     = {position.x - p.current_position.x, position.y - p.current_position.y};
		p.current_position = position;
		p.state            = TOUCH_STATE_ACTIVE;
		p.updated_time     = timestamp;
	}

	for (uint32_t i = 0; i < touch_data.touch_num && i < touch_data.touch.size(); i++)
	{
		const auto& touch = touch_data.touch[i];
		if (FindPrimitive(touch.id) != nullptr)
		{
			continue;
		}
		Primitive* p = FreePrimitive();
		if (p == nullptr)
		{
			break;
		}
		Vector2 position {static_cast<float>(touch.x), static_cast<float>(touch.y)};
		p->state            = TOUCH_STATE_BEGIN;
		p->primitive_id     = g_context.next_primitive_id++;
		p->pad_touch_id     = touch.id;
		p->pressed_position = position;
		p->current_position = position;
		p->delta_vector     = {0.0f, 0.0f};
		p->updated_time     = timestamp;
	}
}

static bool Contains(const RecognizerState& state, const Vector2& position)
{
	if (!state.has_rectangle)
	{
		return true;
	}
	const auto& r = state.rectangle;
	return position.x >= r.x && position.x < r.x + r.width && position.y >= r.y && position.y < r.y + r.height;
}

static RecognizerState* GetState(void* touch_recognizer)
{
	auto* state = static_cast<RecognizerState*>(touch_recognizer);
	return (state != nullptr && state->magic == kRecognizerMagic) ? state : nullptr;
}

// A flick is a finger released while it was still moving; it is reported once,
// in the update that sees the release.
static void RecognizeFlicks(RecognizerState* state)
{
	state->event_count = 0;
	for (const auto& p: g_context.primitives)
	{
		bool moving = p.delta_vector.x != 0.0f || p.delta_vector.y != 0.0f;
		if (p.state != TOUCH_STATE_END || !moving || !Contains(*state, p.pressed_position) || state->event_count >= kMaxRecognizerEvents)
		{
			continue;
		}
		TouchEvent& e = state->events[state->event_count++];
		std::memset(&e, 0, sizeof(e));
		e.event_id                         = state->next_event_id++;
		e.event_state                      = TOUCH_STATE_END;
		e.gesture_type                     = GESTURE_TYPE_FLICK;
		e.is_updated                       = 1;
		e.updated_time                     = p.updated_time;
		e.property.flick.primitive_id      = p.primitive_id;
		e.property.flick.delta_vector      = p.delta_vector;
		e.property.flick.released_position = p.current_position;
		e.property.flick.pressed_position  = p.pressed_position;
	}
}

// sceSystemGestureInitializePrimitiveTouchRecognizer — NID 3pcAvmwKCvM
static int KYTY_SYSV_ABI SystemGestureInitializePrimitiveTouchRecognizer(const void* parameter)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t parameter = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(parameter));
	return OK;
}

// sceSystemGestureFinalizePrimitiveTouchRecognizer — NID 3QYCmMlOlCY
static int KYTY_SYSV_ABI SystemGestureFinalizePrimitiveTouchRecognizer()
{
	PRINT_NAME();
	return OK;
}

// sceSystemGestureOpen — NID qpo-mEOwje0. Returns the gesture handle.
static int32_t KYTY_SYSV_ABI SystemGestureOpen(int32_t input_type, const void* parameter)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t input_type = %" PRId32 "\n", input_type);
	KYTY_LOG_DEBUG("\t parameter  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(parameter));
	if (input_type != SYSTEM_GESTURE_INPUT_TOUCH_PAD)
	{
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	std::lock_guard lock(g_context.mutex);
	if (g_context.opened)
	{
		return SYSTEM_GESTURE_ERROR_ALREADY_OPENED;
	}
	g_context.opened     = true;
	g_context.primitives = {};
	return kGestureHandle;
}

// sceSystemGestureClose — NID j4yXIA2jJ68
static int KYTY_SYSV_ABI SystemGestureClose(int32_t gesture_handle)
{
	PRINT_NAME();
	std::lock_guard lock(g_context.mutex);
	if (!IsValidHandle(gesture_handle))
	{
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	g_context.opened = false;
	return OK;
}

// sceSystemGestureUpdatePrimitiveTouchRecognizer — NID GgFMb22sbbI
static int KYTY_SYSV_ABI SystemGestureUpdatePrimitiveTouchRecognizer(int32_t gesture_handle, const TouchPadData* input_data)
{
	PRINT_NAME();
	std::lock_guard lock(g_context.mutex);
	if (!IsValidHandle(gesture_handle))
	{
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	// The contract takes exactly one pad report per update.
	if (input_data == nullptr || input_data->report_number != 1 || input_data->pad_data_buffer == nullptr)
	{
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	PadTouchData touch_data {};
	uint64_t     timestamp = 0;
	std::memcpy(&touch_data, input_data->pad_data_buffer + kPadDataTouchOffset, sizeof(touch_data));
	std::memcpy(&timestamp, input_data->pad_data_buffer + kPadDataTimestampOffset, sizeof(timestamp));
	UpdatePrimitives(touch_data, timestamp);
	return OK;
}

// sceSystemGestureCreateTouchRecognizer — NID FWF8zkhr854
static int KYTY_SYSV_ABI SystemGestureCreateTouchRecognizer(int32_t gesture_handle, void* touch_recognizer, uint32_t gesture_type,
                                                            const Rectangle* rectangle, const void* parameter)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t gesture_type = 0x%" PRIx32 "\n", gesture_type);
	KYTY_LOG_DEBUG("\t rectangle    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(rectangle));
	KYTY_LOG_DEBUG("\t parameter    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(parameter));
	std::lock_guard lock(g_context.mutex);
	if (!IsValidHandle(gesture_handle))
	{
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	if (touch_recognizer == nullptr)
	{
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	if (gesture_type != GESTURE_TYPE_FLICK)
	{
		KYTY_LOG_WARN("sceSystemGestureCreateTouchRecognizer: gesture type 0x%" PRIx32 " is not implemented\n", gesture_type);
		return SYSTEM_GESTURE_ERROR_NOT_SUPPORTED_GESTURE;
	}
	if (rectangle != nullptr && (rectangle->width <= 0.0f || rectangle->height <= 0.0f))
	{
		return SYSTEM_GESTURE_ERROR_INVALID_TOUCH_AREA;
	}
	auto* state = static_cast<RecognizerState*>(touch_recognizer);
	std::memset(state, 0, sizeof(RecognizerState));
	state->magic         = kRecognizerMagic;
	state->gesture_type  = gesture_type;
	state->has_rectangle = (rectangle != nullptr);
	if (rectangle != nullptr)
	{
		state->rectangle = *rectangle;
	}
	return OK;
}

// sceSystemGestureUpdateTouchRecognizer — NID j4h82CQWENo
static int KYTY_SYSV_ABI SystemGestureUpdateTouchRecognizer(int32_t gesture_handle, void* touch_recognizer)
{
	PRINT_NAME();
	std::lock_guard lock(g_context.mutex);
	if (!IsValidHandle(gesture_handle))
	{
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	RecognizerState* state = GetState(touch_recognizer);
	if (state == nullptr)
	{
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	RecognizeFlicks(state);
	return OK;
}

// sceSystemGestureGetTouchEventsCount — NID h8uongcBNVs. Returns the event count.
static int KYTY_SYSV_ABI SystemGestureGetTouchEventsCount(int32_t gesture_handle, void* touch_recognizer)
{
	PRINT_NAME();
	std::lock_guard lock(g_context.mutex);
	if (!IsValidHandle(gesture_handle))
	{
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	RecognizerState* state = GetState(touch_recognizer);
	if (state == nullptr)
	{
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	return static_cast<int>(state->event_count);
}

// sceSystemGestureGetTouchEventByIndex — NID TSKvgSz5ChU
static int KYTY_SYSV_ABI SystemGestureGetTouchEventByIndex(int32_t gesture_handle, void* touch_recognizer, uint32_t index,
                                                           TouchEvent* touch_event)
{
	PRINT_NAME();
	std::lock_guard lock(g_context.mutex);
	if (!IsValidHandle(gesture_handle))
	{
		return SYSTEM_GESTURE_ERROR_INVALID_HANDLE;
	}
	RecognizerState* state = GetState(touch_recognizer);
	if (state == nullptr || touch_event == nullptr)
	{
		return SYSTEM_GESTURE_ERROR_INVALID_ARGUMENT;
	}
	if (index >= state->event_count)
	{
		return SYSTEM_GESTURE_ERROR_INDEX_OUT_OF_ARRAY;
	}
	*touch_event = state->events[index];
	return OK;
}

} // namespace SystemGesture

LIB_DEFINE(InitSystemGesture_1)
{
	LIB_FUNC("3pcAvmwKCvM", SystemGesture::SystemGestureInitializePrimitiveTouchRecognizer);
	LIB_FUNC("3QYCmMlOlCY", SystemGesture::SystemGestureFinalizePrimitiveTouchRecognizer);
	LIB_FUNC("qpo-mEOwje0", SystemGesture::SystemGestureOpen);
	LIB_FUNC("j4yXIA2jJ68", SystemGesture::SystemGestureClose);
	LIB_FUNC("GgFMb22sbbI", SystemGesture::SystemGestureUpdatePrimitiveTouchRecognizer);
	LIB_FUNC("FWF8zkhr854", SystemGesture::SystemGestureCreateTouchRecognizer);
	LIB_FUNC("j4h82CQWENo", SystemGesture::SystemGestureUpdateTouchRecognizer);
	LIB_FUNC("h8uongcBNVs", SystemGesture::SystemGestureGetTouchEventsCount);
	LIB_FUNC("TSKvgSz5ChU", SystemGesture::SystemGestureGetTouchEventByIndex);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
