#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cinttypes>
#include <cstdint>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Json2", 1, "Json", 1, 1);

namespace Json2 {

// Guest-facing value layout is shared by the constructor and parser surface.

constexpr int32_t  JSON_PARSE_ERROR     = static_cast<int32_t>(0x80848101u);
constexpr int32_t  JSON_ARGUMENT_ERROR  = static_cast<int32_t>(0x80848120u);
constexpr size_t   JsonMaxDocumentBytes = 4u * 1024u * 1024u;
constexpr uint32_t JsonMaxDepth         = 64;
constexpr uint32_t JsonMaxNodes         = 100000;

enum JsonValueType : uint32_t
{
	JsonValueTypeNull = 0,
	JsonValueTypeBoolean,
	JsonValueTypeInteger,
	JsonValueTypeUInteger,
	JsonValueTypeReal,
	JsonValueTypeString,
	JsonValueTypeArray,
	JsonValueTypeObject,
};

struct JsonValue
{
	void*    parent    = nullptr;
	void*    rootparam = nullptr;
	union
	{
		bool     boolean;
		int64_t  integer;
		uint64_t uinteger;
		double   real;
		void*    ptr;
	};
	char     padding[4] {};
	uint32_t type = JsonValueTypeNull;
};

static_assert(sizeof(JsonValue) == 32);
static_assert(offsetof(JsonValue, type) == 28);

struct JsonInitParameter2
{
	void*    allocator                 = nullptr;
	void*    user_data                 = nullptr;
	size_t   file_buffer_size          = 0;
	uint32_t special_float_format_type = 0;
	uint32_t reserved[3]               = {};
};

static void JsonValueInit(JsonValue* self)
{
	if (self != nullptr)
	{
		*self = JsonValue {};
	}
}

static void JsonValueClear(JsonValue* self);
static const JsonValue* JsonNullValue();

static void* KYTY_SYSV_ABI JsonMemAllocatorCtor(void* self)
{
	PRINT_NAME();
	return self;
}

static void KYTY_SYSV_ABI JsonMemAllocatorDtor(void* self)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static JsonInitParameter2* KYTY_SYSV_ABI JsonInitParameter2Ctor(JsonInitParameter2* self)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		*self = JsonInitParameter2 {};
	}
	return self;
}

static void KYTY_SYSV_ABI JsonInitParameter2SetAllocator(JsonInitParameter2* self, void* allocator, void* user_data)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		self->allocator = allocator;
		self->user_data = user_data;
	}
}

static void KYTY_SYSV_ABI JsonInitParameter2SetFileBufferSize(JsonInitParameter2* self, size_t size)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		self->file_buffer_size = size;
	}
}

static void KYTY_SYSV_ABI JsonInitParameter2SetSpecialFloatFormatType(JsonInitParameter2* self, uint32_t type)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		self->special_float_format_type = type;
	}
}

static void* KYTY_SYSV_ABI JsonInitializerCtor(void* self)
{
	PRINT_NAME();
	return self;
}

static int KYTY_SYSV_ABI JsonInitializerInitialize(void* self, const JsonInitParameter2* init_param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t self       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	KYTY_LOG_DEBUG("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));
	return OK;
}

static int KYTY_SYSV_ABI JsonInitializerInitializeV1(void* self, const void* init_param)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t self       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	KYTY_LOG_DEBUG("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));
	return OK;
}

static void KYTY_SYSV_ABI JsonInitializerTerminate(void* self)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void KYTY_SYSV_ABI JsonInitializerDtor(void* self)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void* KYTY_SYSV_ABI JsonValueCtor(void* self)
{
	PRINT_NAME();
	JsonValueInit(static_cast<JsonValue*>(self));
	return self;
}

static void KYTY_SYSV_ABI JsonValueDtor(void* self)
{
	PRINT_NAME();
	JsonValueClear(static_cast<JsonValue*>(self));
}

static void* KYTY_SYSV_ABI JsonObjectCtor(void* self)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		std::memset(self, 0, 16);
	}
	return self;
}

static void KYTY_SYSV_ABI JsonObjectDtor(void* self)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

struct JsonString
{
	// Host heap string; lifetime owned by this wrapper until Dtor.
	char* data = nullptr;
};

struct JsonArray
{
	std::vector<JsonValue*>* items = nullptr;
};

struct JsonObject
{
	std::map<std::string, JsonValue*>* members = nullptr;
};

static std::mutex                             g_owned_mutex;
static std::unordered_map<void*, uint32_t>     g_owned_values;
static std::unordered_set<const JsonValue*>    g_owned_nodes;
static std::unordered_map<const char*, size_t> g_owned_strings;

static void JsonTrackOwned(void* pointer, uint32_t type)
{
	std::lock_guard lock(g_owned_mutex);
	g_owned_values.emplace(pointer, type);
}

static JsonValue* JsonAllocateNode()
{
	auto* value = new JsonValue {};
	std::lock_guard lock(g_owned_mutex);
	g_owned_nodes.insert(value);
	return value;
}

static void JsonDeleteNode(JsonValue* value)
{
	JsonValueClear(value);
	std::lock_guard lock(g_owned_mutex);
	g_owned_nodes.erase(value);
	delete value;
}

static bool JsonOwnsNode(const JsonValue* value)
{
	std::lock_guard lock(g_owned_mutex);
	return g_owned_nodes.find(value) != g_owned_nodes.end();
}

static bool JsonReadValue(const JsonValue* source, JsonValue* value)
{
	// Indexed children live on the host heap; the virtual-memory registry only
	// describes guest mappings. Accept host nodes only while owned by this HLE.
	if (source == JsonNullValue() || JsonOwnsNode(source)) { *value = *source; return true; }
	return Core::VirtualMemory::CopyFromGuest(value, reinterpret_cast<uint64_t>(source), sizeof(*value));
}

static char* JsonAllocateString(const char* source, size_t bytes)
{
	if (bytes == SIZE_MAX) { return nullptr; }
	auto* data = static_cast<char*>(std::malloc(bytes + 1u));
	if (data == nullptr) { return nullptr; }
	if (bytes != 0) { std::memcpy(data, source, bytes); }
	data[bytes] = '\0';
	std::lock_guard lock(g_owned_mutex);
	g_owned_strings.emplace(data, bytes);
	return data;
}

static void JsonReleaseString(char* data)
{
	std::lock_guard lock(g_owned_mutex);
	g_owned_strings.erase(data);
	std::free(data);
}

static uint32_t JsonTakeOwnedType(void* pointer)
{
	std::lock_guard lock(g_owned_mutex);
	const auto it = g_owned_values.find(pointer);
	if (it == g_owned_values.end())
	{
		return UINT32_MAX;
	}
	const uint32_t type = it->second;
	g_owned_values.erase(it);
	return type;
}

static void JsonValueClear(JsonValue* self)
{
	if (self == nullptr)
	{
		return;
	}
	if ((self->type == JsonValueTypeString || self->type == JsonValueTypeArray || self->type == JsonValueTypeObject) &&
	    self->ptr != nullptr)
	{
		switch (JsonTakeOwnedType(self->ptr))
		{
			case JsonValueTypeString:
			{
				auto* str = static_cast<JsonString*>(self->ptr);
				JsonReleaseString(str->data);
				delete str;
				break;
			}
			case JsonValueTypeArray:
			{
				auto* array = static_cast<JsonArray*>(self->ptr);
				if (array->items != nullptr)
				{
					for (auto* child: *array->items)
					{
						JsonDeleteNode(child);
					}
				}
				delete array->items;
				delete array;
				break;
			}
			case JsonValueTypeObject:
			{
				auto* object = static_cast<JsonObject*>(self->ptr);
				if (object->members != nullptr)
				{
					for (auto& [key, child]: *object->members)
					{
						(void)key;
						JsonDeleteNode(child);
					}
				}
				delete object->members;
				delete object;
				break;
			}
			default: break;
		}
	}
	JsonValueInit(self);
}

static void* KYTY_SYSV_ABI JsonStringCtor(void* self)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		std::memset(self, 0, sizeof(JsonString));
	}
	return self;
}

static void* KYTY_SYSV_ABI JsonStringCStringCtor(JsonString* self, const char* str)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		const char* src = (str != nullptr ? str : "");
		const size_t n  = std::strlen(src);
		self->data      = JsonAllocateString(src, n);
	}
	return self;
}

static void KYTY_SYSV_ABI JsonStringDtor(JsonString* self)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		JsonReleaseString(self->data);
		self->data = nullptr;
	}
}

static const char* KYTY_SYSV_ABI JsonStringCStr(const JsonString* self)
{
	PRINT_NAME();
	if (self != nullptr && self->data != nullptr)
	{
		return self->data;
	}
	return "";
}

static size_t KYTY_SYSV_ABI JsonStringLength(const JsonString* self)
{
	PRINT_NAME();
	if (self != nullptr && self->data != nullptr)
	{
		return std::strlen(self->data);
	}
	return 0;
}

static void KYTY_SYSV_ABI JsonStringAssign(JsonString* self, const char* str)
{
	PRINT_NAME();
	if (self == nullptr)
	{
		return;
	}
	JsonReleaseString(self->data);
	self->data = nullptr;
	const char* src = (str != nullptr ? str : "");
	const size_t n  = std::strlen(src);
	self->data      = JsonAllocateString(src, n);
}

static void KYTY_SYSV_ABI JsonValueSetBool(JsonValue* self, bool value)
{
	PRINT_NAME();
	JsonValueClear(self);
	if (self != nullptr)
	{
		self->type    = JsonValueTypeBoolean;
		self->boolean = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetInt(JsonValue* self, int64_t value)
{
	PRINT_NAME();
	JsonValueClear(self);
	if (self != nullptr)
	{
		self->type    = JsonValueTypeInteger;
		self->integer = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetUInt(JsonValue* self, uint64_t value)
{
	PRINT_NAME();
	JsonValueClear(self);
	if (self != nullptr)
	{
		self->type     = JsonValueTypeUInteger;
		self->uinteger = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetDouble(JsonValue* self, double value)
{
	PRINT_NAME();
	JsonValueClear(self);
	if (self != nullptr)
	{
		self->type = JsonValueTypeReal;
		self->real = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetString(JsonValue* self, const JsonString* value)
{
	PRINT_NAME();
	JsonValueClear(self);
	if (self != nullptr)
	{
		self->type = JsonValueTypeString;
		// Borrow the guest JsonString wrapper; lifetime owned by the parent
		// context that constructed both objects.
		self->ptr = const_cast<JsonString*>(value);
	}
}

static void KYTY_SYSV_ABI JsonValueSetType(JsonValue* self, uint32_t type)
{
	PRINT_NAME();
	JsonValueClear(self);
	if (self != nullptr)
	{
		self->type = type;
	}
}

static void KYTY_SYSV_ABI JsonInitializerSetGlobalNullAccessCallback(void* cb, void* user)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t cb   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cb));
	KYTY_LOG_DEBUG("\t user = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user));
}

class JsonDocumentParser
{
public:
	JsonDocumentParser(const char* data, size_t size): m_cursor(data), m_end(data + size) {}

	bool Parse(JsonValue* output)
	{
		SkipSpace();
		if (!ParseValue(output, nullptr, 0))
		{
			return false;
		}
		SkipSpace();
		return m_cursor == m_end;
	}

private:
	static constexpr uint32_t MaxDepth = JsonMaxDepth;
	static constexpr uint32_t MaxNodes = JsonMaxNodes;

	void SkipSpace()
	{
		while (m_cursor < m_end && (*m_cursor == ' ' || *m_cursor == '\t' || *m_cursor == '\r' || *m_cursor == '\n'))
		{
			m_cursor++;
		}
	}

	bool Consume(const char* literal, size_t size)
	{
		if (static_cast<size_t>(m_end - m_cursor) < size || std::memcmp(m_cursor, literal, size) != 0)
		{
			return false;
		}
		m_cursor += size;
		return true;
	}

	static void AppendCodepoint(std::string* output, uint32_t codepoint)
	{
		if (codepoint < 0x80u)
		{
			output->push_back(static_cast<char>(codepoint));
		} else if (codepoint < 0x800u)
		{
			output->push_back(static_cast<char>(0xc0u | (codepoint >> 6u)));
			output->push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
		} else if (codepoint < 0x10000u)
		{
			output->push_back(static_cast<char>(0xe0u | (codepoint >> 12u)));
			output->push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
			output->push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
		} else
		{
			output->push_back(static_cast<char>(0xf0u | (codepoint >> 18u)));
			output->push_back(static_cast<char>(0x80u | ((codepoint >> 12u) & 0x3fu)));
			output->push_back(static_cast<char>(0x80u | ((codepoint >> 6u) & 0x3fu)));
			output->push_back(static_cast<char>(0x80u | (codepoint & 0x3fu)));
		}
	}

	bool ParseHex4(uint32_t* codepoint)
	{
		if (m_end - m_cursor < 4)
		{
			return false;
		}
		uint32_t value = 0;
		for (int i = 0; i < 4; i++)
		{
			const char ch = *m_cursor++;
			value <<= 4u;
			if (ch >= '0' && ch <= '9')
			{
				value |= static_cast<uint32_t>(ch - '0');
			} else if (ch >= 'a' && ch <= 'f')
			{
				value |= static_cast<uint32_t>(ch - 'a' + 10);
			} else if (ch >= 'A' && ch <= 'F')
			{
				value |= static_cast<uint32_t>(ch - 'A' + 10);
			} else
			{
				return false;
			}
		}
		*codepoint = value;
		return true;
	}

	bool ParseString(std::string* output)
	{
		if (m_cursor == m_end || *m_cursor++ != '"')
		{
			return false;
		}
		while (m_cursor < m_end)
		{
			const auto ch = static_cast<unsigned char>(*m_cursor++);
			if (ch == '"')
			{
				return true;
			}
			if (ch < 0x20u)
			{
				return false;
			}
			if (ch == '\\')
			{
				if (m_cursor == m_end)
				{
					return false;
				}
				const char escape = *m_cursor++;
				switch (escape)
				{
					case '"': output->push_back('"'); break;
					case '\\': output->push_back('\\'); break;
					case '/': output->push_back('/'); break;
					case 'b': output->push_back('\b'); break;
					case 'f': output->push_back('\f'); break;
					case 'n': output->push_back('\n'); break;
					case 'r': output->push_back('\r'); break;
					case 't': output->push_back('\t'); break;
					case 'u':
					{
						uint32_t codepoint = 0;
						if (!ParseHex4(&codepoint))
						{
							return false;
						}
						if (codepoint >= 0xd800u && codepoint <= 0xdbffu)
						{
							if (!Consume("\\u", 2))
							{
								return false;
							}
							uint32_t low = 0;
							if (!ParseHex4(&low) || low < 0xdc00u || low > 0xdfffu)
							{
								return false;
							}
							codepoint = 0x10000u + ((codepoint - 0xd800u) << 10u) + low - 0xdc00u;
						} else if (codepoint >= 0xdc00u && codepoint <= 0xdfffu)
						{
							return false;
						}
						AppendCodepoint(output, codepoint);
						break;
					}
					default: return false;
				}
			} else if (ch < 0x80u)
			{
				output->push_back(static_cast<char>(ch));
			} else
			{
				uint32_t codepoint = 0;
				uint32_t min_value = 0;
				int      following = 0;
				if ((ch & 0xe0u) == 0xc0u)
				{
					codepoint = ch & 0x1fu;
					min_value = 0x80u;
					following = 1;
				} else if ((ch & 0xf0u) == 0xe0u)
				{
					codepoint = ch & 0x0fu;
					min_value = 0x800u;
					following = 2;
				} else if ((ch & 0xf8u) == 0xf0u)
				{
					codepoint = ch & 0x07u;
					min_value = 0x10000u;
					following = 3;
				} else
				{
					return false;
				}
				const char* const start = m_cursor - 1;
				if (m_end - m_cursor < following)
				{
					return false;
				}
				for (int i = 0; i < following; i++)
				{
					const auto next = static_cast<unsigned char>(*m_cursor++);
					if ((next & 0xc0u) != 0x80u)
					{
						return false;
					}
					codepoint = (codepoint << 6u) | (next & 0x3fu);
				}
				if (codepoint < min_value || codepoint > 0x10ffffu || (codepoint >= 0xd800u && codepoint <= 0xdfffu))
				{
					return false;
				}
				output->append(start, static_cast<size_t>(following + 1));
			}
		}
		return false;
	}

	bool ParseNumber(JsonValue* output)
	{
		const char* const begin = m_cursor;
		const bool        negative = *m_cursor == '-';
		if (negative && ++m_cursor == m_end)
		{
			return false;
		}
		if (*m_cursor == '0')
		{
			m_cursor++;
		} else if (*m_cursor >= '1' && *m_cursor <= '9')
		{
			do { m_cursor++; } while (m_cursor < m_end && *m_cursor >= '0' && *m_cursor <= '9');
		} else
		{
			return false;
		}
		bool fractional = false;
		if (m_cursor < m_end && *m_cursor == '.')
		{
			fractional = true;
			m_cursor++;
			if (m_cursor == m_end || *m_cursor < '0' || *m_cursor > '9')
			{
				return false;
			}
			do { m_cursor++; } while (m_cursor < m_end && *m_cursor >= '0' && *m_cursor <= '9');
		}
		if (m_cursor < m_end && (*m_cursor == 'e' || *m_cursor == 'E'))
		{
			fractional = true;
			m_cursor++;
			if (m_cursor < m_end && (*m_cursor == '+' || *m_cursor == '-'))
			{
				m_cursor++;
			}
			if (m_cursor == m_end || *m_cursor < '0' || *m_cursor > '9')
			{
				return false;
			}
			do { m_cursor++; } while (m_cursor < m_end && *m_cursor >= '0' && *m_cursor <= '9');
		}
		if (!fractional)
		{
			if (negative)
			{
				int64_t value = 0;
				const auto [end, error] = std::from_chars(begin, m_cursor, value);
				if (error == std::errc {} && end == m_cursor)
				{
					output->type = JsonValueTypeInteger;
					output->integer = value;
					return true;
				}
			} else
			{
				uint64_t value = 0;
				const auto [end, error] = std::from_chars(begin, m_cursor, value);
				if (error == std::errc {} && end == m_cursor)
				{
					output->type = JsonValueTypeUInteger;
					output->uinteger = value;
					return true;
				}
			}
		}
		double value = 0.0;
		const auto [end, error] = std::from_chars(begin, m_cursor, value, std::chars_format::general);
		if (error != std::errc {} || end != m_cursor || !std::isfinite(value))
		{
			return false;
		}
		output->type = JsonValueTypeReal;
		output->real = value;
		return true;
	}

	bool ParseArray(JsonValue* output, uint32_t depth)
	{
		m_cursor++;
		output->type = JsonValueTypeArray;
		auto* array = new JsonArray {};
		array->items = new std::vector<JsonValue*>;
		output->ptr = array;
		JsonTrackOwned(array, JsonValueTypeArray);
		SkipSpace();
		if (m_cursor < m_end && *m_cursor == ']')
		{
			m_cursor++;
			return true;
		}
		for (;;)
		{
			auto* child = JsonAllocateNode();
			if (!ParseValue(child, output, depth + 1u))
			{
				JsonDeleteNode(child);
				return false;
			}
			array->items->push_back(child);
			SkipSpace();
			if (m_cursor == m_end)
			{
				return false;
			}
			if (*m_cursor == ']')
			{
				m_cursor++;
				return true;
			}
			if (*m_cursor++ != ',')
			{
				return false;
			}
			SkipSpace();
		}
	}

	bool ParseObject(JsonValue* output, uint32_t depth)
	{
		m_cursor++;
		output->type = JsonValueTypeObject;
		auto* object = new JsonObject {};
		object->members = new std::map<std::string, JsonValue*>;
		output->ptr = object;
		JsonTrackOwned(object, JsonValueTypeObject);
		SkipSpace();
		if (m_cursor < m_end && *m_cursor == '}')
		{
			m_cursor++;
			return true;
		}
		for (;;)
		{
			std::string key;
			if (!ParseString(&key))
			{
				return false;
			}
			SkipSpace();
			if (m_cursor == m_end || *m_cursor++ != ':')
			{
				return false;
			}
			SkipSpace();
			auto* child = JsonAllocateNode();
			if (!ParseValue(child, output, depth + 1u))
			{
				JsonDeleteNode(child);
				return false;
			}
			auto [it, inserted] = object->members->emplace(std::move(key), child);
			if (!inserted)
			{
				JsonDeleteNode(it->second);
				it->second = child;
			}
			SkipSpace();
			if (m_cursor == m_end)
			{
				return false;
			}
			if (*m_cursor == '}')
			{
				m_cursor++;
				return true;
			}
			if (*m_cursor++ != ',')
			{
				return false;
			}
			SkipSpace();
		}
	}

	bool ParseValue(JsonValue* output, JsonValue* parent, uint32_t depth)
	{
		if (depth > MaxDepth || ++m_nodes > MaxNodes || m_cursor == m_end)
		{
			return false;
		}
		JsonValueInit(output);
		output->parent = parent;
		switch (*m_cursor)
		{
			case 'n': return Consume("null", 4);
			case 't': output->type = JsonValueTypeBoolean; output->boolean = true; return Consume("true", 4);
			case 'f': output->type = JsonValueTypeBoolean; output->boolean = false; return Consume("false", 5);
			case '[': return ParseArray(output, depth);
			case '{': return ParseObject(output, depth);
			case '"':
			{
				std::string value;
				if (!ParseString(&value))
				{
					return false;
				}
				auto* string = new JsonString {};
				string->data = JsonAllocateString(value.data(), value.size());
				if (string->data == nullptr)
				{
					delete string;
					return false;
				}
				output->type = JsonValueTypeString;
				output->ptr = string;
				JsonTrackOwned(string, JsonValueTypeString);
				return true;
			}
			default:
				if (*m_cursor == '-' || (*m_cursor >= '0' && *m_cursor <= '9'))
				{
					return ParseNumber(output);
				}
				return false;
		}
	}

	const char* m_cursor = nullptr;
	const char* m_end = nullptr;
	uint32_t    m_nodes = 0;
};

static int32_t KYTY_SYSV_ABI JsonParserParse(JsonValue* dst, const char* src, size_t size)
{
	if (dst == nullptr || src == nullptr || size == 0 || size > JsonMaxDocumentBytes ||
	    !Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(dst), sizeof(JsonValue)) ||
	    !Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(src), size))
	{
		return JSON_ARGUMENT_ERROR;
	}
	JsonValueClear(dst);
	JsonDocumentParser parser(src, size);
	if (!parser.Parse(dst))
	{
		JsonValueClear(dst);
		return JSON_PARSE_ERROR;
	}
	return OK;
}

static const JsonValue* JsonNullValue()
{
	static const JsonValue null_value {};
	return &null_value;
}

static const JsonValue* KYTY_SYSV_ABI JsonValueIndexString(const JsonValue* self, const char* key)
{
	if (self == nullptr || self->type != JsonValueTypeObject || self->ptr == nullptr || key == nullptr)
	{
		return JsonNullValue();
	}
	std::string text;
	const auto key_address = reinterpret_cast<uint64_t>(key);
	for (size_t i = 0; i < 4096; i++)
	{
		if (key_address > UINT64_MAX - i || !Core::VirtualMemory::IsRangeReadable(key_address + i, 1))
		{
			EXIT("JsonValueIndexString: unreadable key at byte %zu\n", i);
			return JsonNullValue();
		}
		const char ch = *reinterpret_cast<const char*>(key_address + i);
		if (ch == '\0')
		{
			const auto* object = static_cast<const JsonObject*>(self->ptr);
			if (object->members == nullptr)
			{
				return JsonNullValue();
			}
			const auto found = object->members->find(text);
			const auto* result = (found == object->members->end() ? JsonNullValue() : found->second);
			return result;
		}
		text.push_back(ch);
	}
	EXIT("JsonValueIndexString: key exceeds 4096 bytes\n");
	return JsonNullValue();
}

static const JsonValue* KYTY_SYSV_ABI JsonValueIndexUInt(const JsonValue* self, uint64_t index)
{
	if (self == nullptr) { return JsonNullValue(); }
	JsonValue value {};
	EXIT_IF(!JsonReadValue(self, &value));
	if (value.type != JsonValueTypeArray || value.ptr == nullptr) { return JsonNullValue(); }

	std::lock_guard lock(g_owned_mutex);
	const auto owned = g_owned_values.find(value.ptr);
	EXIT_IF(owned == g_owned_values.end() || owned->second != JsonValueTypeArray);
	const auto* array = static_cast<const JsonArray*>(value.ptr);
	if (array->items == nullptr || index >= array->items->size()) { return JsonNullValue(); }
	// The parent owns the element. Returning it must not clone or extend the array.
	return (*array->items)[static_cast<size_t>(index)];
}

static uint32_t KYTY_SYSV_ABI JsonValueGetType(const JsonValue* self)
{
	return (self != nullptr ? self->type : JsonValueTypeNull);
}

static const double* KYTY_SYSV_ABI JsonValueGetReal(const JsonValue* self)
{
	JsonValue value {};
	EXIT_IF(self == nullptr || !JsonReadValue(self, &value));
	if (value.type != JsonValueTypeReal) { EXIT("JsonValueGetReal: unsupported value type %u\n", value.type); }
	// The ABI returns a reference with the value's lifetime, not a numeric copy.
	return &self->real;
}

static const bool* KYTY_SYSV_ABI JsonValueGetBoolean(const JsonValue* self)
{
	JsonValue value {};
	EXIT_IF(self == nullptr || !JsonReadValue(self, &value));
	if (value.type != JsonValueTypeBoolean) { EXIT("JsonValueGetBoolean: unsupported value type %u\n", value.type); }
	return &self->boolean;
}

static size_t KYTY_SYSV_ABI JsonValueCount(const JsonValue* self)
{
	if (self == nullptr || self->ptr == nullptr)
	{
		return 0;
	}
	if (self->type == JsonValueTypeArray)
	{
		const auto* array = static_cast<const JsonArray*>(self->ptr);
		return (array->items != nullptr ? array->items->size() : 0u);
	}
	if (self->type == JsonValueTypeObject)
	{
		const auto* object = static_cast<const JsonObject*>(self->ptr);
		return (object->members != nullptr ? object->members->size() : 0u);
	}
	return 0;
}

static bool JsonOwnsStorage(const void* pointer, uint32_t type)
{
	std::lock_guard lock(g_owned_mutex);
	const auto entry = g_owned_values.find(const_cast<void*>(pointer));
	return entry != g_owned_values.end() && entry->second == type;
}

static bool JsonOwnedStringSize(const char* pointer, size_t* bytes)
{
	std::lock_guard lock(g_owned_mutex);
	const auto entry = g_owned_strings.find(pointer);
	if (entry == g_owned_strings.end()) { return false; }
	*bytes = entry->second;
	return true;
}

static bool JsonReadStringWrapper(const void* source, JsonString* value)
{
	if (JsonOwnsStorage(source, JsonValueTypeString))
	{
		*value = *static_cast<const JsonString*>(source);
		return true;
	}
	return Core::VirtualMemory::CopyFromGuest(value, reinterpret_cast<uint64_t>(source), sizeof(*value));
}

static bool JsonReadCString(const char* source, size_t limit, std::string* text)
{
	if (source == nullptr) { return true; }
	uint64_t address = reinterpret_cast<uint64_t>(source);
	const auto page_size = Core::VirtualMemory::GetPageSize();
	while (text->size() <= limit)
	{
		char buffer[4096];
		const auto bytes = std::min<size_t>({sizeof(buffer), page_size - address % page_size, limit - text->size() + 1u});
		if (!Core::VirtualMemory::CopyFromGuest(buffer, address, bytes)) { return false; }
		const auto* end = static_cast<const char*>(std::memchr(buffer, '\0', bytes));
		text->append(buffer, end != nullptr ? static_cast<size_t>(end - buffer) : bytes);
		if (end != nullptr) { return true; }
		if (address > UINT64_MAX - bytes) { return false; }
		address += bytes;
	}
	return false;
}

static bool JsonReadString(const void* source, size_t limit, std::string* text)
{
	JsonString value {};
	if (!JsonReadStringWrapper(source, &value)) { return false; }
	size_t owned_bytes = 0;
	if (!JsonOwnedStringSize(value.data, &owned_bytes)) { return JsonReadCString(value.data, limit, text); }
	// Parsed strings can contain NUL bytes. Their host ownership metadata
	// retains the byte length without extending the guest wrapper layout.
	if (owned_bytes > limit) { return false; }
	text->assign(value.data, owned_bytes);
	return true;
}

static void KYTY_SYSV_ABI JsonValueToString(const JsonValue* self, JsonString* destination)
{
	JsonValue value {};
	JsonString old {};
	EXIT_IF(self == nullptr || destination == nullptr || !JsonReadValue(self, &value) ||
	        !JsonReadStringWrapper(destination, &old) ||
	        (!JsonOwnsStorage(destination, JsonValueTypeString) &&
	         !Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(destination), sizeof(*destination))));
	if (value.type != JsonValueTypeString)
	{
		EXIT("JsonValueToString: unsupported value type %u\n", value.type);
	}
	size_t old_bytes = 0;
	EXIT_IF(old.data != nullptr && !JsonOwnedStringSize(old.data, &old_bytes));
	std::string text;
	EXIT_IF(value.ptr != nullptr && !JsonReadString(value.ptr, JsonMaxDocumentBytes, &text));
	// Allocate before release because the output may also be the source wrapper.
	char* replacement = JsonAllocateString(text.data(), text.size());
	EXIT_IF(replacement == nullptr);
	JsonReleaseString(old.data);
	destination->data = replacement;
}

class JsonValueCopier
{
public:
	explicit JsonValueCopier(void* rootparam): m_rootparam(rootparam) {}

	bool Copy(JsonValue* output, const JsonValue* source, JsonValue* position, void* parent, uint32_t depth = 0)
	{
		JsonValue value {};
		if (depth > JsonMaxDepth || m_nodes == 0 || !JsonReadValue(source, &value)) { return false; }
		--m_nodes;
		output->type = value.type;
		output->parent = parent;
		output->rootparam = m_rootparam;
		if (value.type <= JsonValueTypeReal)
		{
			std::memcpy(&output->uinteger, &value.uinteger, sizeof(value.uinteger));
			return true;
		}
		if (value.type > JsonValueTypeObject) { return false; }
		if (value.ptr == nullptr) { return true; }
		if (value.type == JsonValueTypeString) { return CopyString(output, value.ptr); }
		if (!JsonOwnsStorage(value.ptr, value.type)) { return false; }
		return value.type == JsonValueTypeArray ? CopyArray(output, value, position, depth) :
		                                        CopyObject(output, value, position, depth);
	}

private:
	bool ReserveBytes(size_t bytes)
	{
		if (bytes > m_bytes) { return false; }
		m_bytes -= bytes;
		return true;
	}

	bool CopyString(JsonValue* output, const void* source)
	{
		std::string text;
		if (!JsonReadString(source, m_bytes, &text) || !ReserveBytes(text.size())) { return false; }
		auto* string = new JsonString {};
		string->data = JsonAllocateString(text.data(), text.size());
		if (string->data == nullptr) { delete string; return false; }
		output->ptr = string;
		JsonTrackOwned(string, JsonValueTypeString);
		return true;
	}

	JsonValue* CopyChild(const JsonValue* source, JsonValue* parent, uint32_t depth)
	{
		auto* child = JsonAllocateNode();
		if (Copy(child, source, child, parent, depth + 1u)) { return child; }
		JsonDeleteNode(child);
		return nullptr;
	}

	bool CopyArray(JsonValue* output, const JsonValue& value, JsonValue* position, uint32_t depth)
	{
		const auto* source = static_cast<const JsonArray*>(value.ptr);
		if (source->items == nullptr) { return true; }
		if (source->items->size() > m_nodes) { return false; }
		auto* array = new JsonArray {};
		array->items = new std::vector<JsonValue*>;
		output->ptr = array;
		JsonTrackOwned(array, JsonValueTypeArray);
		for (const auto* item: *source->items)
		{
			auto* child = CopyChild(item, position, depth);
			if (child == nullptr) { return false; }
			array->items->push_back(child);
		}
		return true;
	}

	bool CopyObject(JsonValue* output, const JsonValue& value, JsonValue* position, uint32_t depth)
	{
		const auto* source = static_cast<const JsonObject*>(value.ptr);
		if (source->members == nullptr) { return true; }
		if (source->members->size() > m_nodes) { return false; }
		auto* object = new JsonObject {};
		object->members = new std::map<std::string, JsonValue*>;
		output->ptr = object;
		JsonTrackOwned(object, JsonValueTypeObject);
		for (const auto& [key, item]: *source->members)
		{
			if (!ReserveBytes(key.size())) { return false; }
			auto* child = CopyChild(item, position, depth);
			if (child == nullptr) { return false; }
			object->members->emplace(key, child);
		}
		return true;
	}

	void* m_rootparam = nullptr;
	uint32_t m_nodes = JsonMaxNodes;
	size_t m_bytes = JsonMaxDocumentBytes;
};

static JsonValue* KYTY_SYSV_ABI JsonValueAssign(JsonValue* self, const JsonValue* source)
{
	JsonValue old {};
	EXIT_IF(self == nullptr || source == nullptr ||
	        !JsonReadValue(self, &old) ||
	        (!JsonOwnsNode(self) && !Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(self), sizeof(*self))));
	if (self == source) { return self; }
	JsonValue replacement {};
	JsonValueCopier copier(old.rootparam);
	// Clone before releasing the destination: source may be one of its children.
	if (!copier.Copy(&replacement, source, self, old.parent))
	{
		JsonValueClear(&replacement);
		EXIT("JsonValueAssign: invalid value or copy exceeds resource limits\n");
	}
	JsonValueClear(self);
	*self = replacement;
	return self;
}

} // namespace Json2

LIB_DEFINE(InitJson2_1)
{
	LIB_FUNC("-hJRce8wn1U", Json2::JsonMemAllocatorCtor);
	LIB_FUNC("WSOuge5IsCg", Json2::JsonInitParameter2Ctor);
	LIB_FUNC("GvGvswb0v34", Json2::JsonInitParameter2Ctor);
	LIB_FUNC("I2QC8PYhJWY", Json2::JsonInitParameter2SetAllocator);
	LIB_FUNC("W72B9ylU2JA", Json2::JsonInitParameter2SetAllocator);
	LIB_FUNC("Eu95jmqn5Rw", Json2::JsonInitParameter2SetFileBufferSize);
	LIB_FUNC("WVZBP4IyM+E", Json2::JsonInitParameter2SetSpecialFloatFormatType);
	LIB_FUNC("cK6bYHf-Q5E", Json2::JsonInitializerCtor);
	LIB_FUNC("IXW-z8pggfg", Json2::JsonInitializerInitialize);
	LIB_FUNC("Cxwy7wHq4J0", Json2::JsonInitializerInitializeV1);
	LIB_FUNC("PR5k1penBLM", Json2::JsonInitializerTerminate);
	LIB_FUNC("RujUxbr3haM", Json2::JsonInitializerDtor);
	LIB_FUNC("OcAgPxcq5Vk", Json2::JsonMemAllocatorDtor);
	LIB_FUNC("qBMjqyBn3OM", Json2::JsonValueCtor);
	LIB_FUNC("-wa17B7TGnw", Json2::JsonValueCtor);
	LIB_FUNC("WTtYf+cNnXI", Json2::JsonValueDtor);
	LIB_FUNC("0eUrW9JAxM0", Json2::JsonValueDtor);
	LIB_FUNC("OJPTonqdg0I", Json2::JsonObjectCtor);
	LIB_FUNC("5JmzZt8twAo", Json2::JsonObjectDtor);
	LIB_FUNC("qSmqLXXCPas", Json2::JsonStringCtor);
	LIB_FUNC("9KUZFjI1IxA", Json2::JsonStringCStringCtor);
	LIB_FUNC("cG1VE2HMl6c", Json2::JsonStringDtor);
	LIB_FUNC("L1KAkYWml-M", Json2::JsonStringCStr);
	LIB_FUNC("EUH+EmT-v9E", Json2::JsonStringLength);
	LIB_FUNC("cn9svYGWKDQ", Json2::JsonStringAssign);
	LIB_FUNC("5yHuiWXo2gg", Json2::JsonValueSetBool);
	LIB_FUNC("QxVVYhP-mvg", Json2::JsonValueSetInt);
	LIB_FUNC("SIe1ZmW7e7s", Json2::JsonValueSetUInt);
	LIB_FUNC("BSmWDIkV4w4", Json2::JsonValueSetDouble);
	LIB_FUNC("6l3Bv2gysNc", Json2::JsonValueSetString);
	LIB_FUNC("IKQimvG9Wqs", Json2::JsonValueSetType);
	LIB_FUNC("S5JxQnoGF3E", Json2::JsonParserParse);
	LIB_FUNC("HwDt5lD9Bfo", Json2::JsonValueIndexString);
	LIB_FUNC("XlWbvieLj2M", Json2::JsonValueIndexUInt);
	LIB_FUNC("SHtAad20YYM", Json2::JsonValueGetType);
	LIB_FUNC("3qrge7L-AU4", Json2::JsonValueGetReal);
	LIB_FUNC("zTwZdI8AZ5Y", Json2::JsonValueGetBoolean);
	LIB_FUNC("RBw+4NukeGQ", Json2::JsonValueCount);
	LIB_FUNC("4zrm6VrgIAw", Json2::JsonValueAssign);
	LIB_FUNC("Ncel8t2Rrpc", Json2::JsonValueToString);
	LIB_FUNC("+drDFyAS6u4", Json2::JsonInitializerSetGlobalNullAccessCallback);
	LIB_FUNC("00oCq0RwSAY", Json2::JsonInitializerSetGlobalNullAccessCallback);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
