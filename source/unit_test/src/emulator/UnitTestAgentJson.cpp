#include "Kyty/Agent/Json.h"
#include "Emulator/Agent/Protocol.h"
#include "Kyty/UnitTest.h"

UT_BEGIN(AgentJson);

TEST(AgentJson, EscapesControlCharactersInOneCanonicalEncoder)
{
	using Kyty::Agent::JsonEscape;
	using Kyty::Agent::JsonString;

	EXPECT_EQ(JsonEscape(nullptr), "");
	EXPECT_EQ(JsonEscape("quote=\" slash=\\ line=\n tab=\t"), "quote=\\\" slash=\\\\ line=\\n tab=\\t");
	EXPECT_EQ(JsonEscape("\x01"), "\\u0001");
	EXPECT_EQ(JsonString("value"), "\"value\"");
}

TEST(AgentJson, FindsOnlyTopLevelSemanticArgumentKeys)
{
	using Kyty::Emulator::Agent::ArgsGetString;
	using Kyty::Emulator::Agent::ArgsHasKey;

	std::string code;
	EXPECT_TRUE(ArgsHasKey(R"({"co\u0064e":"device_lost"})", "code"));
	EXPECT_TRUE(ArgsGetString(R"({"co\u0064e":"device_lost"})", "code", &code));
	EXPECT_EQ(code, "device_lost");
	EXPECT_FALSE(ArgsHasKey(R"({"meta":{"code":1}})", "code"));
	EXPECT_FALSE(ArgsHasKey(R"({"message":"code"})", "code"));
}

UT_END();
