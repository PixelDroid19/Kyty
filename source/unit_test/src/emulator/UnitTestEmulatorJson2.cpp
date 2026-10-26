#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include "Kyty/UnitTest.h"

#include <cstddef>
#include <cstdint>
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

UT_END();

#endif // KYTY_EMU_ENABLED
