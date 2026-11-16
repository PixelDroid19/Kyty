#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Kyty/UnitTest.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#ifdef KYTY_EMU_ENABLED

UT_BEGIN(EmulatorJson2);

namespace {

using Kyty::Loader::SymbolDatabase;
using Kyty::Loader::SymbolResolve;
using Kyty::Loader::SymbolRecord;
using Kyty::Loader::SymbolType;

// Host mirrors of the guest-facing Json2 wrappers built by LibJson2. The HLE
// entry points dereference these directly, so the same layout works in tests.
enum : uint32_t
{
	JsonNull     = 0,
	JsonBoolean  = 1,
	JsonInteger  = 2,
	JsonUInteger = 3,
	JsonReal     = 4,
	JsonString   = 5,
	JsonArray    = 6,
	JsonObject   = 7,
};

struct TestJsonValue
{
	void*    parent    = nullptr;
	void*    rootparam = nullptr;
	union
	{
		bool     boolean;
		int64_t  integer;
		uint64_t uinteger;
		double   real;
		void*    pointer;
	};
	char     padding[4] {};
	uint32_t type = JsonNull;
};

static_assert(sizeof(TestJsonValue) == 32);
static_assert(offsetof(TestJsonValue, type) == 28);

struct TestJsonString
{
	char* data = nullptr;
};

struct TestJsonArray
{
	std::vector<TestJsonValue*>* items = nullptr;
};

struct TestJsonObject
{
	std::map<std::string, TestJsonValue*>* members = nullptr;
};

// std::pair<const std::string, JsonValue> as seen by the guest: the key wrapper
// at +0 and the value at +8.
struct TestJsonObjectPair
{
	TestJsonString key;
	TestJsonValue  value;
};

static_assert(sizeof(TestJsonObjectPair) == 40);
static_assert(offsetof(TestJsonObjectPair, value) == 8);

using ArrayBeginFn       = void* (*)(void*, const void*);
using ArrayEndFn         = void* (*)(void*, const void*);
using IteratorDerefFn    = const void* (*)(const void*);
using IteratorIncrementFn = void* (*)(void*);
using IteratorNotEqualFn = bool (*)(const void*, const void*);
using IteratorDestroyFn  = void (*)(void*);
using GetStringFn        = const void* (*)(const void*);
using GetArrayFn         = const void* (*)(const void*);
using GetObjectFn        = const void* (*)(const void*);
using GetUIntegerFn      = const void* (*)(const void*);

SymbolResolve JsonFunc(const char16_t* nid)
{
	SymbolResolve sr {};
	sr.name                 = nid;
	sr.library              = U"Json2";
	sr.library_version      = 1;
	sr.module               = U"Json";
	sr.module_version_major = 1;
	sr.module_version_minor = 1;
	sr.type                 = SymbolType::Func;
	return sr;
}

struct Json2Fns
{
	ArrayBeginFn        begin          = nullptr;
	ArrayEndFn          end            = nullptr;
	IteratorDerefFn     deref          = nullptr;
	IteratorIncrementFn increment      = nullptr;
	IteratorNotEqualFn  not_equal      = nullptr;
	IteratorDestroyFn   destroy        = nullptr;
	ArrayBeginFn        object_begin   = nullptr;
	ArrayEndFn          object_end     = nullptr;
	IteratorDerefFn     object_deref   = nullptr;
	IteratorIncrementFn object_inc     = nullptr;
	IteratorNotEqualFn  object_ne      = nullptr;
	IteratorDestroyFn   object_destroy = nullptr;
	GetStringFn         get_string     = nullptr;
	GetArrayFn          get_array      = nullptr;
	GetObjectFn         get_object     = nullptr;
	GetUIntegerFn       get_uinteger   = nullptr;
};

template <typename T>
T Lookup(SymbolDatabase* symbols, const char16_t* nid)
{
	const auto* record = symbols->Find(JsonFunc(nid));
	return record != nullptr ? reinterpret_cast<T>(record->vaddr) : nullptr;
}

bool LoadJson2Fns(Json2Fns* fns)
{
	SymbolDatabase symbols;
	if (!Kyty::Libs::Init(U"libJson2_1", &symbols))
	{
		return false;
	}
	fns->begin        = Lookup<ArrayBeginFn>(&symbols, u"bcH5EnFE2xY");
	fns->end          = Lookup<ArrayEndFn>(&symbols, u"WXF2ihRF+B8");
	fns->deref        = Lookup<IteratorDerefFn>(&symbols, u"wcgr5mte7T8");
	fns->increment    = Lookup<IteratorIncrementFn>(&symbols, u"w5+VCznos5E");
	fns->not_equal    = Lookup<IteratorNotEqualFn>(&symbols, u"5AZPp99ogrc");
	fns->destroy      = Lookup<IteratorDestroyFn>(&symbols, u"9yLjn46Ypfs");
	fns->get_string   = Lookup<GetStringFn>(&symbols, u"epJ6x2LV0kU");
	fns->get_array    = Lookup<GetArrayFn>(&symbols, u"ONT8As5R1ug");
	fns->get_object   = Lookup<GetObjectFn>(&symbols, u"IlsmvBtMkak");
	fns->get_uinteger = Lookup<GetUIntegerFn>(&symbols, u"sn4HNCtNRzY");
	fns->object_begin   = Lookup<ArrayBeginFn>(&symbols, u"xhAcaIwnrgk");
	fns->object_end     = Lookup<ArrayEndFn>(&symbols, u"ivMCitpSQNk");
	fns->object_deref   = Lookup<IteratorDerefFn>(&symbols, u"ZCd6IYoD3Bc");
	fns->object_inc     = Lookup<IteratorIncrementFn>(&symbols, u"DlWmn2ZQuWY");
	fns->object_ne      = Lookup<IteratorNotEqualFn>(&symbols, u"+isUKw4zud4");
	fns->object_destroy = Lookup<IteratorDestroyFn>(&symbols, u"hoINmSMlYjI");
	return fns->begin != nullptr && fns->end != nullptr && fns->deref != nullptr && fns->increment != nullptr &&
	       fns->not_equal != nullptr && fns->destroy != nullptr && fns->get_string != nullptr &&
	       fns->get_array != nullptr && fns->get_object != nullptr && fns->get_uinteger != nullptr &&
	       fns->object_begin != nullptr && fns->object_end != nullptr && fns->object_deref != nullptr &&
	       fns->object_inc != nullptr && fns->object_ne != nullptr && fns->object_destroy != nullptr;
}

} // namespace

TEST(EmulatorJson2, RegistersArrayIteratorAndValueGetterNids)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libJson2_1", &symbols));

	const char16_t* nids[] = {u"bcH5EnFE2xY", u"WXF2ihRF+B8", u"wcgr5mte7T8", u"w5+VCznos5E", u"5AZPp99ogrc",
	                          u"9yLjn46Ypfs", u"epJ6x2LV0kU", u"ONT8As5R1ug", u"IlsmvBtMkak", u"sn4HNCtNRzY"};
	for (const char16_t* nid: nids)
	{
		EXPECT_NE(symbols.Find(JsonFunc(nid)), nullptr) << nid;
	}

	Json2Fns fns;
	ASSERT_TRUE(LoadJson2Fns(&fns));
}

TEST(EmulatorJson2, IteratesArrayElementsInOrder)
{
	Json2Fns fns;
	ASSERT_TRUE(LoadJson2Fns(&fns));

	TestJsonValue first {};
	first.type          = JsonInteger;
	first.integer       = 10;
	TestJsonValue second {};
	second.type         = JsonUInteger;
	second.uinteger     = 20;
	TestJsonValue third {};
	third.type    = JsonInteger;
	third.integer = 30;
	std::vector<TestJsonValue*> items {&first, &second, &third};
	TestJsonArray               array {&items};

	void* begin_iter = nullptr;
	void* end_iter   = nullptr;
	EXPECT_EQ(fns.begin(&begin_iter, &array), &begin_iter);
	EXPECT_EQ(fns.end(&end_iter, &array), &end_iter);
	EXPECT_NE(begin_iter, end_iter);

	std::vector<uint64_t> visited;
	while (fns.not_equal(&begin_iter, &end_iter))
	{
		const auto* value = static_cast<const TestJsonValue*>(fns.deref(&begin_iter));
		ASSERT_NE(value, nullptr);
		visited.push_back(value->uinteger);
		EXPECT_EQ(fns.increment(&begin_iter), &begin_iter);
	}
	EXPECT_EQ(visited.size(), 3u);
	EXPECT_EQ(visited[0], 10u);
	EXPECT_EQ(visited[1], 20u);
	EXPECT_EQ(visited[2], 30u);
	EXPECT_FALSE(fns.not_equal(&begin_iter, &end_iter));
	fns.destroy(&begin_iter);
	fns.destroy(&end_iter);
}

TEST(EmulatorJson2, ReportsEmptyArrayIteratorsEqual)
{
	Json2Fns fns;
	ASSERT_TRUE(LoadJson2Fns(&fns));

	std::vector<TestJsonValue*> items;
	TestJsonArray               array {&items};

	void* begin_iter = reinterpret_cast<void*>(0x1);
	void* end_iter   = reinterpret_cast<void*>(0x1);
	fns.begin(&begin_iter, &array);
	fns.end(&end_iter, &array);
	EXPECT_EQ(begin_iter, nullptr);
	EXPECT_FALSE(fns.not_equal(&begin_iter, &end_iter));
	EXPECT_FALSE(fns.not_equal(&begin_iter, &begin_iter));

	const auto* null_value = static_cast<const TestJsonValue*>(fns.deref(&begin_iter));
	ASSERT_NE(null_value, nullptr);
	EXPECT_EQ(null_value->type, JsonNull);
	EXPECT_EQ(fns.increment(&begin_iter), &begin_iter);
	EXPECT_EQ(begin_iter, nullptr);

	TestJsonArray null_array {};
	void*         null_iter = nullptr;
	fns.begin(&null_iter, &null_array);
	EXPECT_EQ(null_iter, nullptr);
	fns.end(&null_iter, &null_array);
	EXPECT_EQ(null_iter, nullptr);
	fns.begin(&null_iter, nullptr);
	EXPECT_EQ(null_iter, nullptr);
}

TEST(EmulatorJson2, ReturnsTypedValuePayloads)
{
	Json2Fns fns;
	ASSERT_TRUE(LoadJson2Fns(&fns));

	TestJsonString string {};
	string.data = const_cast<char*>("rocket");
	TestJsonValue string_value {};
	string_value.type    = JsonString;
	string_value.pointer = &string;
	EXPECT_EQ(fns.get_string(&string_value), &string);

	std::vector<TestJsonValue*> items;
	TestJsonArray               array {&items};
	TestJsonValue               array_value {};
	array_value.type    = JsonArray;
	array_value.pointer = &array;
	EXPECT_EQ(fns.get_array(&array_value), &array);

	std::map<std::string, TestJsonValue*> members;
	TestJsonObject                        object {&members};
	TestJsonValue                         object_value {};
	object_value.type    = JsonObject;
	object_value.pointer = &object;
	EXPECT_EQ(fns.get_object(&object_value), &object);

	TestJsonValue signed_value {};
	signed_value.type    = JsonInteger;
	signed_value.integer = -7;
	const auto* signed_out = static_cast<const int64_t*>(fns.get_uinteger(&signed_value));
	ASSERT_NE(signed_out, nullptr);
	EXPECT_EQ(*signed_out, -7);

	TestJsonValue unsigned_value {};
	unsigned_value.type     = JsonUInteger;
	unsigned_value.uinteger = 0xfedcba9876543210ull;
	const auto* unsigned_out = static_cast<const uint64_t*>(fns.get_uinteger(&unsigned_value));
	ASSERT_NE(unsigned_out, nullptr);
	EXPECT_EQ(*unsigned_out, 0xfedcba9876543210ull);
}

TEST(EmulatorJson2, FallsBackForMismatchedValueTypes)
{
	Json2Fns fns;
	ASSERT_TRUE(LoadJson2Fns(&fns));

	TestJsonValue boolean_value {};
	boolean_value.type    = JsonBoolean;
	boolean_value.boolean = true;
	TestJsonValue real_value {};
	real_value.type = JsonReal;
	real_value.real = 1.5;
	TestJsonValue null_value {};

	const void* string_fallback = fns.get_string(&boolean_value);
	ASSERT_NE(string_fallback, nullptr);
	const auto* empty_string = static_cast<const TestJsonString*>(string_fallback);
	ASSERT_NE(empty_string->data, nullptr);
	EXPECT_STREQ(empty_string->data, "");
	EXPECT_EQ(fns.get_string(nullptr), string_fallback);

	const auto* empty_array = static_cast<const TestJsonArray*>(fns.get_array(&real_value));
	ASSERT_NE(empty_array, nullptr);
	ASSERT_NE(empty_array->items, nullptr);
	EXPECT_TRUE(empty_array->items->empty());
	EXPECT_EQ(fns.get_array(nullptr), empty_array);

	const auto* empty_object = static_cast<const TestJsonObject*>(fns.get_object(&null_value));
	ASSERT_NE(empty_object, nullptr);
	ASSERT_NE(empty_object->members, nullptr);
	EXPECT_TRUE(empty_object->members->empty());
	EXPECT_EQ(fns.get_object(nullptr), empty_object);

	const auto* zero = static_cast<const uint64_t*>(fns.get_uinteger(&real_value));
	ASSERT_NE(zero, nullptr);
	EXPECT_EQ(*zero, 0u);
	const auto* null_zero = static_cast<const uint64_t*>(fns.get_uinteger(nullptr));
	ASSERT_NE(null_zero, nullptr);
	EXPECT_EQ(*null_zero, 0u);
}

TEST(EmulatorJson2, RegistersObjectIteratorNids)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libJson2_1", &symbols));

	const char16_t* nids[] = {u"xhAcaIwnrgk", u"ivMCitpSQNk", u"ZCd6IYoD3Bc",
	                          u"DlWmn2ZQuWY", u"+isUKw4zud4", u"hoINmSMlYjI"};
	for (const char16_t* nid: nids)
	{
		EXPECT_NE(symbols.Find(JsonFunc(nid)), nullptr) << nid;
	}
}

TEST(EmulatorJson2, IteratesObjectMembersInOrder)
{
	Json2Fns fns;
	ASSERT_TRUE(LoadJson2Fns(&fns));

	TestJsonValue first {};
	first.type    = JsonInteger;
	first.integer = 1;
	TestJsonValue second {};
	second.type    = JsonInteger;
	second.integer = 2;
	std::map<std::string, TestJsonValue*> members;
	members.emplace("a", &first);
	members.emplace("b", &second);
	TestJsonObject object {&members};

	void* begin_iter = nullptr;
	void* end_iter   = nullptr;
	EXPECT_EQ(fns.object_begin(&begin_iter, &object), &begin_iter);
	EXPECT_EQ(fns.object_end(&end_iter, &object), &end_iter);
	ASSERT_NE(begin_iter, nullptr);
	ASSERT_NE(end_iter, nullptr);
	EXPECT_TRUE(fns.object_ne(&begin_iter, &end_iter));

	std::vector<std::string> keys;
	std::vector<int64_t>     values;
	while (fns.object_ne(&begin_iter, &end_iter))
	{
		const auto* pair = static_cast<const TestJsonObjectPair*>(fns.object_deref(&begin_iter));
		ASSERT_NE(pair, nullptr);
		ASSERT_NE(pair->key.data, nullptr);
		keys.emplace_back(pair->key.data);
		values.push_back(pair->value.integer);
		EXPECT_EQ(fns.object_inc(&begin_iter), &begin_iter);
	}
	EXPECT_EQ(keys.size(), 2u);
	EXPECT_EQ(keys[0], "a");
	EXPECT_EQ(keys[1], "b");
	EXPECT_EQ(values[0], 1);
	EXPECT_EQ(values[1], 2);
	EXPECT_FALSE(fns.object_ne(&begin_iter, &end_iter));
	fns.object_destroy(&begin_iter);
	fns.object_destroy(&end_iter);
	EXPECT_EQ(begin_iter, nullptr);
	EXPECT_EQ(end_iter, nullptr);
}

TEST(EmulatorJson2, ReportsEmptyAndNullObjectIteratorsEqual)
{
	Json2Fns fns;
	ASSERT_TRUE(LoadJson2Fns(&fns));

	std::map<std::string, TestJsonValue*> members;
	TestJsonObject                        object {&members};

	void* begin_iter = reinterpret_cast<void*>(0x1);
	void* end_iter   = reinterpret_cast<void*>(0x1);
	fns.object_begin(&begin_iter, &object);
	fns.object_end(&end_iter, &object);
	EXPECT_EQ(begin_iter, nullptr);
	EXPECT_EQ(end_iter, nullptr);
	EXPECT_FALSE(fns.object_ne(&begin_iter, &end_iter));

	const auto* pair = static_cast<const TestJsonObjectPair*>(fns.object_deref(&begin_iter));
	ASSERT_NE(pair, nullptr);
	EXPECT_EQ(pair->value.type, JsonNull);
	EXPECT_EQ(fns.object_inc(&begin_iter), &begin_iter);
	EXPECT_EQ(begin_iter, nullptr);

	TestJsonObject null_object {};
	void*          null_iter = reinterpret_cast<void*>(0x1);
	fns.object_begin(&null_iter, &null_object);
	EXPECT_EQ(null_iter, nullptr);
	fns.object_begin(&null_iter, nullptr);
	EXPECT_EQ(null_iter, nullptr);
	fns.object_destroy(&null_iter);
	EXPECT_EQ(null_iter, nullptr);

	void* one_null = nullptr;
	fns.object_begin(&begin_iter, &object);
	EXPECT_FALSE(fns.object_ne(&begin_iter, &one_null));
	EXPECT_FALSE(fns.object_ne(&one_null, &begin_iter));
	fns.object_destroy(&begin_iter);
}

namespace {

// Guest-constructed wrappers placed in guest memory, each at its 8-byte size and followed by a canary, so a
// constructor writing past the wrapper is caught.
struct GuestJsonPage
{
	static constexpr uint64_t CANARY = 0xC0DEC0DEC0DEC0DEull;

	GuestJsonPage()
	{
		address = Kyty::Core::VirtualMemory::Alloc(0, Kyty::Core::VirtualMemory::GetPageSize(),
		                                           Kyty::Core::VirtualMemory::Mode::ReadWrite);
		if (address != 0)
		{
			std::memset(reinterpret_cast<void*>(address), 0xA5, Kyty::Core::VirtualMemory::GetPageSize());
		}
	}
	~GuestJsonPage()
	{
		if (address != 0) { (void)Kyty::Core::VirtualMemory::Free(address); }
	}
	// Slot n is 48 bytes: a 32-byte value or an 8-byte wrapper, then the canary at +40.
	template <typename T>
	T* Slot(int n) const
	{
		auto* base = reinterpret_cast<uint8_t*>(address + static_cast<uint64_t>(n) * 48u);
		std::memcpy(base + 40, &CANARY, sizeof(CANARY));
		return reinterpret_cast<T*>(base);
	}
	bool CanaryIntact(int n) const
	{
		uint64_t value = 0;
		std::memcpy(&value, reinterpret_cast<const uint8_t*>(address + static_cast<uint64_t>(n) * 48u) + 40, sizeof(value));
		return value == CANARY;
	}
	// A wrapper only owns its first 8 bytes: bytes 8..39 must keep the fill.
	bool WrapperTailIntact(int n) const
	{
		const auto* base = reinterpret_cast<const uint8_t*>(address + static_cast<uint64_t>(n) * 48u);
		for (int i = 8; i < 40; i++)
		{
			if (base[i] != 0xA5) { return false; }
		}
		return CanaryIntact(n);
	}

	uint64_t address = 0;
};

} // namespace

TEST(EmulatorJson2, BuildsArraysObjectsAndValuesTheGuestConstructs)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libJson2_1", &symbols));
	auto* array_ctor    = Lookup<void* (*)(void*)>(&symbols, u"JP-PtKMiI1E");
	auto* array_dtor    = Lookup<void (*)(void*)>(&symbols, u"HJ8GpRT1aiw");
	auto* push_back     = Lookup<void (*)(void*, const void*)>(&symbols, u"zQtLRTqceMY");
	auto* array_empty   = Lookup<bool (*)(const void*)>(&symbols, u"9uP25i6ipno");
	auto* array_size    = Lookup<size_t (*)(const void*)>(&symbols, u"rQGJeNjOuUk");
	auto* array_back    = Lookup<const TestJsonValue* (*)(const void*)>(&symbols, u"bAM9Qwofus0");
	auto* object_ctor   = Lookup<void* (*)(void*)>(&symbols, u"OJPTonqdg0I");
	auto* object_copy   = Lookup<void* (*)(void*, const void*)>(&symbols, u"a+W7HHlwpBs");
	auto* object_dtor   = Lookup<void (*)(void*)>(&symbols, u"5JmzZt8twAo");
	auto* object_index  = Lookup<TestJsonValue* (*)(void*, const void*)>(&symbols, u"ERuf9y0DY84");
	auto* object_clear  = Lookup<void (*)(void*)>(&symbols, u"oH8aBmLU+fc");
	auto* object_assign = Lookup<void* (*)(void*, const void*)>(&symbols, u"urOpESTBZmo");
	auto* string_cctor  = Lookup<void* (*)(void*, const char*)>(&symbols, u"9KUZFjI1IxA");
	auto* string_copy   = Lookup<void* (*)(void*, const void*)>(&symbols, u"0CAesfH963Q");
	auto* string_dtor   = Lookup<void (*)(void*)>(&symbols, u"cG1VE2HMl6c");
	auto* value_bool    = Lookup<void* (*)(void*, bool)>(&symbols, u"UeuWT+yNdCQ");
	auto* value_long    = Lookup<void* (*)(void*, int64_t)>(&symbols, u"0lLK8+kDqmE");
	auto* value_ulong   = Lookup<void* (*)(void*, uint64_t)>(&symbols, u"x4AUdbhpRB0");
	auto* value_double  = Lookup<void* (*)(void*, double)>(&symbols, u"sOmU4vnx3s0");
	auto* value_cstr    = Lookup<void* (*)(void*, const char*)>(&symbols, u"b9V6fmppLXY");
	auto* value_string  = Lookup<void* (*)(void*, const void*)>(&symbols, u"sZIoMRGO+jk");
	auto* value_array   = Lookup<void* (*)(void*, const void*)>(&symbols, u"iZeYfOxtMRg");
	auto* value_object  = Lookup<void* (*)(void*, const void*)>(&symbols, u"3xUXnmUkXfo");
	auto* value_copy    = Lookup<void* (*)(void*, const void*)>(&symbols, u"fSb2oQTNrgA");
	auto* value_dtor    = Lookup<void (*)(void*)>(&symbols, u"WTtYf+cNnXI");
	auto* set_cstr      = Lookup<void (*)(void*, const char*)>(&symbols, u"n6FC+l9DU70");
	auto* set_object    = Lookup<void (*)(void*, const void*)>(&symbols, u"dFCphqnd+a4");
	auto* refer_array   = Lookup<void* (*)(void*)>(&symbols, u"nM5XqdeXFPw");
	auto* refer_object  = Lookup<void* (*)(void*)>(&symbols, u"-NxEk7XLkDY");
	auto* count         = Lookup<size_t (*)(const void*)>(&symbols, u"RBw+4NukeGQ");
	auto* index_uint    = Lookup<const TestJsonValue* (*)(const void*, uint64_t)>(&symbols, u"XlWbvieLj2M");
	auto* refer_key     = Lookup<TestJsonValue* (*)(void*, const void*)>(&symbols, u"wLsJlmgEIaI");
	auto* set_type      = Lookup<void (*)(void*, uint32_t)>(&symbols, u"IKQimvG9Wqs");
	for (const char16_t* nid: {u"JP-PtKMiI1E", u"HJ8GpRT1aiw", u"zQtLRTqceMY", u"9uP25i6ipno", u"rQGJeNjOuUk", u"bAM9Qwofus0", u"OJPTonqdg0I", u"a+W7HHlwpBs", u"5JmzZt8twAo", u"ERuf9y0DY84", u"oH8aBmLU+fc", u"urOpESTBZmo", u"9KUZFjI1IxA", u"0CAesfH963Q", u"cG1VE2HMl6c", u"UeuWT+yNdCQ", u"0lLK8+kDqmE", u"x4AUdbhpRB0", u"sOmU4vnx3s0", u"b9V6fmppLXY", u"sZIoMRGO+jk", u"iZeYfOxtMRg", u"3xUXnmUkXfo", u"fSb2oQTNrgA", u"WTtYf+cNnXI", u"n6FC+l9DU70", u"dFCphqnd+a4", u"nM5XqdeXFPw", u"-NxEk7XLkDY", u"RBw+4NukeGQ", u"XlWbvieLj2M", u"wLsJlmgEIaI"})
	{
		ASSERT_NE(symbols.Find(JsonFunc(nid)), nullptr) << nid;
	}

	GuestJsonPage page;
	ASSERT_NE(page.address, 0u);
	// Guest strings are read through guest memory, so the literals live in the page too.
	auto* hi = page.Slot<char>(10);
	std::strcpy(hi, "hi");
	auto* kyty = page.Slot<char>(11);
	std::strcpy(kyty, "kyty");
	auto* array = page.Slot<void>(0);
	ASSERT_EQ(array_ctor(array), array);
	EXPECT_TRUE(page.WrapperTailIntact(0));
	EXPECT_TRUE(array_empty(array));
	EXPECT_EQ(array_size(array), 0u);
	EXPECT_EQ(array_back(array)->type, JsonNull);

	auto* number = page.Slot<TestJsonValue>(1);
	value_long(number, -5);
	push_back(array, number);
	value_dtor(number);
	value_ulong(number, 7);
	push_back(array, number);
	value_dtor(number);
	value_bool(number, true);
	push_back(array, number);
	value_dtor(number);
	value_double(number, 1.5);
	push_back(array, number);
	value_dtor(number);
	value_cstr(number, hi);
	push_back(array, number);
	EXPECT_TRUE(page.CanaryIntact(1));
	EXPECT_FALSE(array_empty(array));
	ASSERT_EQ(array_size(array), 5u);
	EXPECT_EQ(array_back(array)->type, JsonString);

	// A value built from the array owns a copy: growing the array afterwards leaves it alone.
	auto* listed = page.Slot<TestJsonValue>(2);
	value_array(listed, array);
	push_back(array, number);
	ASSERT_EQ(listed->type, JsonArray);
	ASSERT_EQ(count(listed), 5u);
	EXPECT_EQ(index_uint(listed, 0)->integer, -5);
	EXPECT_EQ(index_uint(listed, 1)->type, JsonUInteger);
	EXPECT_EQ(index_uint(listed, 1)->uinteger, 7u);
	EXPECT_EQ(index_uint(listed, 3)->real, 1.5);
	EXPECT_EQ(refer_array(listed), listed->pointer);

	auto* key = page.Slot<TestJsonString>(3);
	string_cctor(key, "name");
	auto* key_copy = page.Slot<TestJsonString>(4);
	ASSERT_EQ(string_copy(key_copy, key), key_copy);
	EXPECT_TRUE(page.WrapperTailIntact(4));
	EXPECT_NE(key_copy->data, key->data);
	EXPECT_STREQ(key_copy->data, "name");

	auto* object = page.Slot<void>(5);
	ASSERT_EQ(object_ctor(object), object);
	EXPECT_TRUE(page.WrapperTailIntact(5));
	auto* member = object_index(object, key);
	ASSERT_NE(member, nullptr);
	EXPECT_EQ(member->type, JsonNull);
	set_cstr(member, kyty);
	EXPECT_EQ(object_index(object, key_copy), member);

	auto* object_twin = page.Slot<void>(6);
	object_copy(object_twin, object);
	EXPECT_TRUE(page.WrapperTailIntact(6));
	object_clear(object);
	auto* described = page.Slot<TestJsonValue>(7);
	value_object(described, object);
	EXPECT_EQ(count(described), 0u);
	value_dtor(described);
	value_object(described, object_twin);
	ASSERT_EQ(count(described), 1u);
	const auto* copied = refer_key(described, key);
	ASSERT_NE(copied, nullptr);
	EXPECT_NE(copied, object_index(object_twin, key));
	EXPECT_STREQ(static_cast<const TestJsonString*>(copied->pointer)->data, "kyty");
	EXPECT_EQ(refer_object(described), described->pointer);

	EXPECT_EQ(object_assign(object, object_twin), object);
	value_dtor(described);
	value_object(described, object);
	EXPECT_EQ(count(described), 1u);

	auto* duplicate = page.Slot<TestJsonValue>(8);
	value_copy(duplicate, described);
	EXPECT_TRUE(page.CanaryIntact(8));
	ASSERT_EQ(duplicate->type, JsonObject);
	EXPECT_NE(duplicate->pointer, described->pointer);
	EXPECT_EQ(count(duplicate), 1u);

	auto* text = page.Slot<TestJsonValue>(9);
	value_string(text, key);
	ASSERT_EQ(text->type, JsonString);
	EXPECT_STREQ(static_cast<const TestJsonString*>(text->pointer)->data, "name");
	set_object(text, object_twin);
	EXPECT_EQ(text->type, JsonObject);
	EXPECT_EQ(count(text), 1u);

	// A value given a container type can be referred to and filled at once.
	auto* body = page.Slot<TestJsonValue>(12);
	value_long(body, 1);
	set_type(body, JsonArray);
	push_back(refer_array(body), number);
	EXPECT_EQ(count(body), 1u);
	set_type(body, JsonObject);
	object_index(refer_object(body), key)->type = JsonBoolean;
	EXPECT_EQ(count(body), 1u);
	EXPECT_EQ(refer_key(body, key)->type, JsonBoolean);
	set_type(body, JsonString);
	EXPECT_STREQ(static_cast<const TestJsonString*>(body->pointer)->data, "");

	for (auto* value: {number, listed, described, duplicate, text, body})
	{
		value_dtor(value);
	}
	string_dtor(key);
	string_dtor(key_copy);
	object_dtor(object);
	object_dtor(object_twin);
	array_dtor(array);
	array_dtor(array); // a second destruction finds no container and does nothing
	for (int n = 0; n < 12; n++)
	{
		EXPECT_TRUE(page.CanaryIntact(n)) << n;
	}
}

TEST(EmulatorJson2, SerializesObjectsAsCompactKeyOrderedText)
{
	SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libJson2_1", &symbols));
	auto* object_ctor  = Lookup<void* (*)(void*)>(&symbols, u"OJPTonqdg0I");
	auto* object_dtor  = Lookup<void (*)(void*)>(&symbols, u"5JmzZt8twAo");
	auto* object_index = Lookup<TestJsonValue* (*)(void*, const void*)>(&symbols, u"ERuf9y0DY84");
	auto* object_size  = Lookup<size_t (*)(const void*)>(&symbols, u"fSGHm9RjN5U");
	auto* string_cctor = Lookup<void* (*)(void*, const char*)>(&symbols, u"9KUZFjI1IxA");
	auto* string_dtor  = Lookup<void (*)(void*)>(&symbols, u"cG1VE2HMl6c");
	auto* value_object = Lookup<void* (*)(void*, const void*)>(&symbols, u"3xUXnmUkXfo");
	auto* value_dtor   = Lookup<void (*)(void*)>(&symbols, u"WTtYf+cNnXI");
	auto* set_cstr     = Lookup<void (*)(void*, const char*)>(&symbols, u"n6FC+l9DU70");
	auto* serialize    = Lookup<int32_t (*)(const void*, void*)>(&symbols, u"R7FDWtcN6f8");
	ASSERT_NE(serialize, nullptr);
	ASSERT_NE(object_size, nullptr);

	GuestJsonPage page;
	ASSERT_NE(page.address, 0u);
	auto* quoted = page.Slot<char>(10);
	std::strcpy(quoted, "q\"\n");
	auto* key_b = page.Slot<TestJsonString>(0);
	string_cctor(key_b, "b");
	auto* key_a = page.Slot<TestJsonString>(1);
	string_cctor(key_a, "a");
	auto* object = page.Slot<void>(2);
	object_ctor(object);
	set_cstr(object_index(object, key_b), quoted);
	auto* number = object_index(object, key_a);
	number->type    = JsonInteger;
	number->integer = -5;
	EXPECT_EQ(object_size(object), 2u);

	auto* value = page.Slot<TestJsonValue>(3);
	value_object(value, object);
	auto* text = page.Slot<TestJsonString>(4);
	string_cctor(text, "");
	EXPECT_EQ(serialize(value, text), 0);
	EXPECT_STREQ(text->data, R"({"a":-5,"b":"q\"\n"})");

	value_dtor(value);
	object_dtor(object);
	for (auto* str: {key_a, key_b, text})
	{
		string_dtor(str);
	}
	for (int n = 0; n < 5; n++)
	{
		EXPECT_TRUE(page.CanaryIntact(n)) << n;
	}
}

UT_END();

#endif // KYTY_EMU_ENABLED
