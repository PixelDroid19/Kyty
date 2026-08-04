#include "Kyty/UnitTest.h"

#include "Emulator/SystemContentPort.h"

#include <cstring>

UT_BEGIN(EmulatorSystemContentPort);

namespace {

bool TestMetadata(Kyty::Core::String* title_id, Kyty::Core::String* app_version)
{
	if (title_id == nullptr || app_version == nullptr)
	{
		return false;
	}
	*title_id    = U"TEST-TITLE";
	*app_version = U"1.0";
	return true;
}

bool TestIconPath(Kyty::Core::String* path)
{
	if (path == nullptr)
	{
		return false;
	}
	*path = U"/tmp/test-icon.png";
	return true;
}

bool TestParamString(const char* name, char* value, size_t value_size)
{
	if (name == nullptr || value == nullptr || value_size == 0 || std::strcmp(name, "TITLE") != 0)
	{
		return false;
	}
	std::strncpy(value, "Test title", value_size - 1);
	value[value_size - 1] = '\0';
	return true;
}

} // namespace

TEST(EmulatorSystemContentPort, ProviderIsNeutralAndResettable)
{
	Kyty::Emulator::SystemContentPort::Install({});

	Kyty::Core::String title_id;
	Kyty::Core::String app_version;
	EXPECT_FALSE(Kyty::Emulator::SystemContentPort::GetMetadata(&title_id, &app_version));

	Kyty::Emulator::SystemContentPort::Install({TestMetadata, TestIconPath, TestParamString});
	ASSERT_TRUE(Kyty::Emulator::SystemContentPort::GetMetadata(&title_id, &app_version));
	EXPECT_EQ(title_id, U"TEST-TITLE");
	EXPECT_EQ(app_version, U"1.0");

	Kyty::Core::String icon_path;
	ASSERT_TRUE(Kyty::Emulator::SystemContentPort::GetIconPath(&icon_path));
	EXPECT_EQ(icon_path, U"/tmp/test-icon.png");

	char title[16] = {};
	ASSERT_TRUE(Kyty::Emulator::SystemContentPort::GetParamString("TITLE", title, sizeof(title)));
	EXPECT_STREQ(title, "Test title");

	Kyty::Emulator::SystemContentPort::Install({});
	EXPECT_FALSE(Kyty::Emulator::SystemContentPort::GetIconPath(&icon_path));
}

UT_END();
