#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Json2", 1, "Json2", 1, 1);

namespace Json2 {

// Gen5 libSceJson2 constructor surface used while Astro builds its parser
// context. Zero objects and return this; expand parse/serialize when hit.

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
		std::memset(self, 0, sizeof(JsonValue));
		self->type = JsonValueTypeNull;
	}
}

static void* KYTY_SYSV_ABI JsonMemAllocatorCtor(void* self)
{
	PRINT_NAME();
	return self;
}

static void KYTY_SYSV_ABI JsonMemAllocatorDtor(void* self)
{
	PRINT_NAME();
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
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
	printf("\t self       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	printf("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));
	return OK;
}

static int KYTY_SYSV_ABI JsonInitializerInitializeV1(void* self, const void* init_param)
{
	PRINT_NAME();
	printf("\t self       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
	printf("\t init_param = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(init_param));
	return OK;
}

static void KYTY_SYSV_ABI JsonInitializerTerminate(void* self)
{
	PRINT_NAME();
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

static void KYTY_SYSV_ABI JsonInitializerDtor(void* self)
{
	PRINT_NAME();
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
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
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
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
	printf("\t self = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(self));
}

struct JsonString
{
	// Host heap string; lifetime owned by this wrapper until Dtor.
	char* data = nullptr;
};

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
		self->data      = static_cast<char*>(std::malloc(n + 1));
		if (self->data != nullptr)
		{
			std::memcpy(self->data, src, n + 1);
		}
	}
	return self;
}

static void KYTY_SYSV_ABI JsonStringDtor(JsonString* self)
{
	PRINT_NAME();
	if (self != nullptr)
	{
		std::free(self->data);
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
	std::free(self->data);
	self->data = nullptr;
	const char* src = (str != nullptr ? str : "");
	const size_t n  = std::strlen(src);
	self->data      = static_cast<char*>(std::malloc(n + 1));
	if (self->data != nullptr)
	{
		std::memcpy(self->data, src, n + 1);
	}
}

static void KYTY_SYSV_ABI JsonValueSetBool(JsonValue* self, bool value)
{
	PRINT_NAME();
	JsonValueInit(self);
	if (self != nullptr)
	{
		self->type    = JsonValueTypeBoolean;
		self->boolean = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetInt(JsonValue* self, int64_t value)
{
	PRINT_NAME();
	JsonValueInit(self);
	if (self != nullptr)
	{
		self->type    = JsonValueTypeInteger;
		self->integer = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetUInt(JsonValue* self, uint64_t value)
{
	PRINT_NAME();
	JsonValueInit(self);
	if (self != nullptr)
	{
		self->type     = JsonValueTypeUInteger;
		self->uinteger = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetDouble(JsonValue* self, double value)
{
	PRINT_NAME();
	JsonValueInit(self);
	if (self != nullptr)
	{
		self->type = JsonValueTypeReal;
		self->real = value;
	}
}

static void KYTY_SYSV_ABI JsonValueSetString(JsonValue* self, const JsonString* value)
{
	PRINT_NAME();
	JsonValueInit(self);
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
	JsonValueInit(self);
	if (self != nullptr)
	{
		self->type = type;
	}
}

static void KYTY_SYSV_ABI JsonInitializerSetGlobalNullAccessCallback(void* cb, void* user)
{
	PRINT_NAME();
	printf("\t cb   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(cb));
	printf("\t user = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(user));
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
	LIB_FUNC("+drDFyAS6u4", Json2::JsonInitializerSetGlobalNullAccessCallback);
	LIB_FUNC("00oCq0RwSAY", Json2::JsonInitializerSetGlobalNullAccessCallback);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
