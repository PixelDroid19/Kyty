#include "Kyty/Agent/Json.h"
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

UT_END();
