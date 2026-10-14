#include "Kyty/Core/JsonReader.h"
#include "Kyty/Core/SafeDelete.h"
#include "Kyty/UnitTest.h"

#include "../../../lib/Core/src/FileInternal.h"

#include <cstdint>
#include <limits>
#include <string>

UT_BEGIN(CoreInputBounds);

TEST(CoreInputBounds, WholeStringBufferLengthLeavesTerminatorRoom)
{
	constexpr uint64_t max_size = std::numeric_limits<uint32_t>::max();

	EXPECT_TRUE(Core::FileInternal::CanReadWholeStringBuffer(max_size - 1u, 1u));
	EXPECT_FALSE(Core::FileInternal::CanReadWholeStringBuffer(max_size, 1u));
	EXPECT_FALSE(Core::FileInternal::CanReadWholeStringBuffer(max_size + 1u, 2u));
	EXPECT_FALSE(Core::FileInternal::CanReadWholeStringBuffer(max_size, 0u));
}

TEST(CoreInputBounds, JsonRequiresOneCompleteValue)
{
	const Core::String valid = Core::String::FromUtf8("{\"key\":[1,true,null]} \r\n");
	const Core::Json*  json  = Core::Json::Create(valid);

	ASSERT_NE(json, nullptr);
	EXPECT_TRUE(json->IsObject());
	auto* mutable_json = const_cast<Core::Json*>(json);
	Delete(mutable_json);

	const Core::String trailing = Core::String::FromUtf8("{\"key\":true} trailing");
	const Core::Json*  trailing_json = Core::Json::Create(trailing);
	EXPECT_EQ(trailing_json, nullptr);
	if (trailing_json != nullptr)
	{
		auto* unexpected_json = const_cast<Core::Json*>(trailing_json);
		Delete(unexpected_json);
	}

	const Core::String empty = Core::String::FromUtf8(" \t\r\n");
	const Core::Json*  empty_json = Core::Json::Create(empty);
	EXPECT_EQ(empty_json, nullptr);
	if (empty_json != nullptr)
	{
		auto* unexpected_json = const_cast<Core::Json*>(empty_json);
		Delete(unexpected_json);
	}
}

TEST(CoreInputBounds, JsonRejectsMissingValuesAndClosingQuotes)
{
	const char* malformed[] = {"{\"x\":}", "[1,]", "\"unterminated"};
	for (const char* input: malformed)
	{
		const Core::String source = Core::String::FromUtf8(input);
		const Core::Json*  json   = Core::Json::Create(source);
		EXPECT_EQ(json, nullptr);
		if (json != nullptr)
		{
			auto* unexpected_json = const_cast<Core::Json*>(json);
			Delete(unexpected_json);
		}
	}
}

TEST(CoreInputBounds, JsonRejectsNonStringObjectKeys)
{
	const char* malformed[] = {"{1:0}", "{true:0}", "{[]:0}", "{{}:0}"};
	for (const char* input: malformed)
	{
		const Core::String source = Core::String::FromUtf8(input);
		const Core::Json*  json   = Core::Json::Create(source);
		EXPECT_EQ(json, nullptr);
		if (json != nullptr)
		{
			auto* unexpected_json = const_cast<Core::Json*>(json);
			Delete(unexpected_json);
		}
	}
}

TEST(CoreInputBounds, JsonRejectsMalformedAndOutOfRangeNumbers)
{
	const char* malformed[] = {"-", "01", "1.", "1e", "1e+", "1+2", "--1", "1e400"};
	for (const char* input: malformed)
	{
		const Core::String source = Core::String::FromUtf8(input);
		const Core::Json*  json   = Core::Json::Create(source);
		EXPECT_EQ(json, nullptr);
		if (json != nullptr)
		{
			auto* unexpected_json = const_cast<Core::Json*>(json);
			Delete(unexpected_json);
		}
	}
}

TEST(CoreInputBounds, JsonPreservesFiniteDoublesAndChecksInt64Conversions)
{
	const Core::String source = Core::String::FromUtf8(
	    "{\"max\":9223372036854775807,\"min\":-9223372036854775808,\"fraction\":12.75,"
	    "\"negative_fraction\":-12.75,\"exponent\":1e2,\"beyond_int64\":9223372036854775808,"
	    "\"big\":1e100}");
	const Core::Json* json = Core::Json::Create(source);

	ASSERT_NE(json, nullptr);

	int64_t converted = 0;
	EXPECT_TRUE(json->GetItem("max")->TryToInt(&converted));
	EXPECT_EQ(converted, std::numeric_limits<int64_t>::max());
	EXPECT_EQ(json->GetItem("max")->ToInt(), std::numeric_limits<int64_t>::max());

	EXPECT_TRUE(json->GetItem("min")->TryToInt(&converted));
	EXPECT_EQ(converted, std::numeric_limits<int64_t>::min());
	EXPECT_EQ(json->GetItem("min")->ToInt(), std::numeric_limits<int64_t>::min());

	EXPECT_EQ(json->GetItem("fraction")->ToInt(), 12);
	EXPECT_EQ(json->GetItem("negative_fraction")->ToInt(), -12);
	EXPECT_EQ(json->GetItem("exponent")->ToInt(), 100);

	const Core::Json* beyond_int64 = json->GetItem("beyond_int64");
	converted                     = 77;
	EXPECT_FALSE(beyond_int64->TryToInt(&converted));
	EXPECT_EQ(converted, 77);
	EXPECT_EQ(beyond_int64->ToFloat(), 9223372036854775808.0);
	EXPECT_EXIT(beyond_int64->ToInt(), ::testing::ExitedWithCode(65), "");
	EXPECT_EXIT(json->GetInt("beyond_int64", 0), ::testing::ExitedWithCode(65), "");

	const Core::Json* big = json->GetItem("big");
	EXPECT_EQ(big->ToFloat(), 1e100);
	converted = 77;
	EXPECT_FALSE(big->TryToInt(&converted));
	EXPECT_EQ(converted, 77);
	EXPECT_FALSE(big->TryToInt(nullptr));
	EXPECT_EXIT(big->ToInt(), ::testing::ExitedWithCode(65), "");

	auto* mutable_json = const_cast<Core::Json*>(json);
	Delete(mutable_json);
}

TEST(CoreInputBounds, JsonRejectsExcessiveNesting)
{
	constexpr size_t depth = 512;
	std::string     array_input(depth, '[');
	array_input += '0';
	array_input.append(depth, ']');

	const Core::String array_source = Core::String::FromUtf8(array_input.c_str());
	const Core::Json*  json         = Core::Json::Create(array_source);
	EXPECT_EQ(json, nullptr);
	if (json != nullptr)
	{
		auto* unexpected_json = const_cast<Core::Json*>(json);
		Delete(unexpected_json);
	}

	std::string object_input;
	for (size_t i = 0; i < depth; i++)
	{
		object_input += "{\"k\":";
	}
	object_input += '0';
	object_input.append(depth, '}');

	const Core::String object_source = Core::String::FromUtf8(object_input.c_str());
	const Core::Json*  object_json  = Core::Json::Create(object_source);
	EXPECT_EQ(object_json, nullptr);
	if (object_json != nullptr)
	{
		auto* unexpected_json = const_cast<Core::Json*>(object_json);
		Delete(unexpected_json);
	}
}

UT_END();
