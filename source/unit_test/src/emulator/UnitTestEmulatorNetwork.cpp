#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Network/HttpUri.h"

#include <array>
#include <cstring>

namespace Kyty::UnitTest {

namespace {

Loader::SymbolResolve HttpFunction(const char16_t* library, const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = library;
	query.library_version      = 1;
	query.module               = U"Http";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

bool PointsIntoPool(const char* value, const char* pool, size_t pool_size)
{
	return value >= pool && value < pool + pool_size;
}

} // namespace

TEST(EmulatorNetwork, HttpUriParseUsesCallerOwnedPool)
{
	using namespace Libs::Network::Http;

	constexpr char uri[] = "https://user:pass@example.com/path?q=1#fragment";
	uint64_t       required_size = 0;
	ASSERT_EQ(HttpUriParse(nullptr, uri, nullptr, &required_size, 0), OK);
	ASSERT_GT(required_size, 0u);

	std::array<char, 256> pool {};
	ASSERT_LE(required_size, pool.size());
	HttpUriElement parsed {};
	ASSERT_EQ(HttpUriParse(&parsed, uri, pool.data(), nullptr, pool.size()), OK);
	EXPECT_FALSE(parsed.opaque);
	EXPECT_STREQ(parsed.scheme, "https");
	EXPECT_STREQ(parsed.username, "user");
	EXPECT_STREQ(parsed.password, "pass");
	EXPECT_STREQ(parsed.hostname, "example.com");
	EXPECT_STREQ(parsed.path, "/path");
	EXPECT_STREQ(parsed.query, "?q=1");
	EXPECT_STREQ(parsed.fragment, "#fragment");
	EXPECT_EQ(parsed.port, 443u);
	EXPECT_TRUE(PointsIntoPool(parsed.scheme, pool.data(), pool.size()));
	EXPECT_TRUE(PointsIntoPool(parsed.fragment, pool.data(), pool.size()));
}

TEST(EmulatorNetwork, HttpUriParsePreservesExplicitZeroPort)
{
	using namespace Libs::Network::Http;

	constexpr char uri[] = "http://example.com:0/";
	uint64_t       required_size = 0;
	ASSERT_EQ(HttpUriParse(nullptr, uri, nullptr, &required_size, 0), OK);

	std::array<char, 128> pool {};
	HttpUriElement parsed {};
	ASSERT_EQ(HttpUriParse(&parsed, uri, pool.data(), nullptr, pool.size()), OK);
	EXPECT_EQ(parsed.port, 0u);
}

TEST(EmulatorNetwork, HttpUriParseRejectsIncompleteOrInvalidOutput)
{
	using namespace Libs::Network::Http;

	constexpr char uri[] = "https://host";
	uint64_t       required_size = 0;
	HttpUriElement parsed {};
	std::array<char, 4> pool {};

	EXPECT_EQ(HttpUriParse(nullptr, nullptr, nullptr, &required_size, 0), Libs::Network::HTTP_ERROR_INVALID_URL);
	EXPECT_EQ(HttpUriParse(nullptr, "1bad://host", nullptr, &required_size, 0), Libs::Network::HTTP_ERROR_INVALID_URL);
	EXPECT_EQ(HttpUriParse(&parsed, uri, nullptr, &required_size, 0), Libs::Network::HTTP_ERROR_INVALID_VALUE);
	EXPECT_EQ(HttpUriParse(nullptr, uri, pool.data(), &required_size, pool.size()), Libs::Network::HTTP_ERROR_INVALID_VALUE);
	EXPECT_EQ(HttpUriParse(&parsed, uri, pool.data(), nullptr, pool.size()), Libs::Network::HTTP_ERROR_OUT_OF_MEMORY);
}

TEST(EmulatorNetwork, HttpUriParseRegistersExactHttpIdentity)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));

	const auto* record = symbols.Find(HttpFunction(u"Http", u"IWalAn-guFs"));
	ASSERT_NE(record, nullptr);
	EXPECT_EQ(symbols.Find(HttpFunction(u"libNet", u"IWalAn-guFs")), nullptr);
}

} // namespace Kyty::UnitTest
