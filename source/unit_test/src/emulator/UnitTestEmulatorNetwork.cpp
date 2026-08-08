#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Network.h"
#include "Emulator/Network/HttpUri.h"

#include <array>
#include <cstring>

UT_BEGIN(EmulatorNetwork);

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

TEST(EmulatorNetwork, NetEpollRejectsInvalidArgumentsBeforeHostAccess)
{
	using namespace Libs::Network::Net;

	void* events_slot = nullptr;

	// Null/zero argument validation must not touch the host epoll instance.
	EXPECT_LT(NetEpollControl(1, 0, 1, nullptr), 0);
	EXPECT_LT(NetEpollControl(1, 4, 1, nullptr), 0);
	EXPECT_LT(NetEpollWait(1, nullptr, 1, 0), 0);
	EXPECT_LT(NetEpollWait(1, &events_slot, 0, 0), 0);
}

TEST(EmulatorNetwork, NetEpollCreateDestroyRoundTrip)
{
	using namespace Libs::Network::Net;

	const int epoll_id = NetEpollCreate("test-epoll", 0);
	EXPECT_GT(epoll_id, 0);
	EXPECT_EQ(NetEpollDestroy(epoll_id), 0);
	EXPECT_LT(NetEpollDestroy(epoll_id), 0);
}

TEST(EmulatorNetwork, NetEpollControlRoundTripKeepsRegistrationState)
{
	using namespace Libs::Network::Net;

	struct Event
	{
		uint32_t events;
		uint32_t reserved;
		uint64_t ident;
		uint64_t data;
	};

	const int socket_id = NetSocket("test-epoll-socket", 2, 2, 17);
	ASSERT_GT(socket_id, 0);
	const int epoll_id = NetEpollCreate("test-epoll-control", 0);
	ASSERT_GT(epoll_id, 0);

	Event event {1u, 0u, 0x1234u, 0x5678u};
	EXPECT_EQ(NetEpollControl(epoll_id, 1, socket_id, &event), 0);
	event.events = 5u;
	EXPECT_EQ(NetEpollControl(epoll_id, 2, socket_id, &event), 0);
	EXPECT_EQ(NetEpollControl(epoll_id, 3, socket_id, nullptr), 0);
	EXPECT_EQ(NetEpollDestroy(epoll_id), 0);
	EXPECT_EQ(NetSocketClose(socket_id), 0);
}

TEST(EmulatorNetwork, NetResolverRejectsNullInputs)
{
	using namespace Libs::Network::Net;

	void* address_slot = nullptr;
	char* host_slot    = nullptr;

	EXPECT_LT(NetResolverStartNtoa(1, nullptr, &address_slot, 0, 0, 0), 0);
	EXPECT_LT(NetResolverStartNtoa(1, "host", nullptr, 0, 0, 0), 0);
	EXPECT_LT(NetResolverStartAton(1, nullptr, host_slot, 64, 0, 0, 0), 0);
	EXPECT_LT(NetResolverStartAton(1, &address_slot, nullptr, 0, 0, 0, 0), 0);
}

TEST(EmulatorNetwork, NetResolverResolvesLiteralAddressWithoutHostLookup)
{
	using namespace Libs::Network::Net;

	// A dotted-quad literal must be answered from the address itself, so the
	// test is deterministic and does not require a network stack.
	uint8_t address[4] {};
	EXPECT_EQ(NetResolverStartNtoa(1, "127.0.0.1", address, 0, 0, 0), 0);
	EXPECT_EQ(address[0], 127u);
	EXPECT_EQ(address[3], 1u);
}

TEST(EmulatorNetwork, NetGetSockInfoValidatesArguments)
{
	using namespace Libs::Network::Net;

	void* info_slot = nullptr;

	EXPECT_LT(NetGetSockInfo(1, nullptr, 0, 0), 0);
	EXPECT_LT(NetGetSockInfo(0, &info_slot, 16, 0), 0);
}

UT_END();
