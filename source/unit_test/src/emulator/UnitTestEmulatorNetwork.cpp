#include "Kyty/Core/VirtualMemory.h"
#include "Kyty/UnitTest.h"

#include "Emulator/Config.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Network.h"
#include "Emulator/Network/HttpUri.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#define KYTY_NETWORK_TEST_HOST_EPOLL 1
#else
#define KYTY_NETWORK_TEST_HOST_EPOLL 0
#endif

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

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

uint64_t AllocateGuestPage()
{
	const uint64_t page_size = Core::VirtualMemory::GetPageSize();
	return page_size == 0 ? 0 : Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
}

void StoreGuestSockaddr(uint8_t* address, uint16_t port)
{
	ASSERT_NE(address, nullptr);
	std::memset(address, 0, 8);
	address[0] = 16;
	address[1] = 2;
	address[2] = static_cast<uint8_t>(port >> 8u);
	address[3] = static_cast<uint8_t>(port & 0xffu);
	address[4] = 127;
	address[7] = 1;
}

uint16_t LoadGuestSockaddrPort(const uint8_t* address)
{
	return static_cast<uint16_t>((static_cast<uint16_t>(address[2]) << 8u) | address[3]);
}

bool WaitForFlag(const std::atomic<bool>& flag, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (!flag.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
	{
		std::this_thread::yield();
	}
	return flag.load(std::memory_order_acquire);
}

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
int CreateLoopbackSocket(int type, uint16_t* port)
{
	if (port == nullptr)
	{
		return -1;
	}

	const int fd = ::socket(AF_INET, type, 0);
	if (fd < 0)
	{
		return -1;
	}

	sockaddr_in address {};
	address.sin_family      = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port        = 0;
	if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		::close(fd);
		return -1;
	}

	socklen_t address_len = sizeof(address);
	if (::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &address_len) != 0)
	{
		::close(fd);
		return -1;
	}
	*port = ntohs(address.sin_port);
	return fd;
}

int ConnectLoopback(uint16_t port)
{
	const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
	{
		return -1;
	}

	sockaddr_in address {};
	address.sin_family      = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port        = htons(port);
	if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
	{
		::close(fd);
		return -1;
	}
	return fd;
}
#endif

Loader::SymbolResolve NetCtlFunction(const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"NetCtl";
	query.library_version      = 1;
	query.module               = U"NetCtl";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

Loader::SymbolResolve NpManagerFunction(const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"NpManager";
	query.library_version      = 1;
	query.module               = U"NpManager";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

Loader::SymbolResolve NpFunction(const char16_t* library, const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = library;
	query.library_version      = 1;
	query.module               = library;
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

} // namespace

TEST(EmulatorNetwork, NetCtlGetInfoReturnsLoopbackNetmask)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));
	const auto* record = symbols.Find(NetCtlFunction(u"obuxdTiwkF8"));
	ASSERT_NE(record, nullptr);

	using GetInfo = int(KYTY_SYSV_ABI*)(int, void*);
	const uint64_t guest_output = AllocateGuestPage();
	ASSERT_NE(guest_output, 0u);
	auto get_info = reinterpret_cast<GetInfo>(record->vaddr);
	auto* const output = reinterpret_cast<uint8_t*>(guest_output);

	std::memset(output, 0x5a, 16);
	ASSERT_EQ(get_info(2, output), 0);
	EXPECT_TRUE(std::all_of(output, output + 6, [](uint8_t value) { return value == 0; }));

	*reinterpret_cast<uint32_t*>(output) = 0xffffffffu;
	ASSERT_EQ(get_info(11, output), 0);
	EXPECT_EQ(*reinterpret_cast<uint32_t*>(output), 0u);

	std::memset(output, 0x5a, 16);
	ASSERT_EQ(get_info(14, output), 0);
	EXPECT_STREQ(reinterpret_cast<char*>(output), "127.0.0.1");

	auto* const netmask = reinterpret_cast<char*>(output);
	ASSERT_EQ(get_info(15, netmask), 0);
	EXPECT_STREQ(netmask, "255.0.0.0");
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_output));
}

TEST(EmulatorNetwork, NetCtlGetInfoRejectsNonGuestFreedAndReadOnlyOutput)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));
	const auto* record = symbols.Find(NetCtlFunction(u"obuxdTiwkF8"));
	ASSERT_NE(record, nullptr);

	using GetInfo = int(KYTY_SYSV_ABI*)(int, void*);
	auto get_info = reinterpret_cast<GetInfo>(record->vaddr);
	std::array<char, 16> host_output {};
	host_output.fill(static_cast<char>(0x5a));
	EXPECT_EQ(get_info(15, host_output.data()), Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_TRUE(std::all_of(host_output.begin(), host_output.end(), [](char value) { return value == static_cast<char>(0x5a); }));

	const uint64_t stale_output = AllocateGuestPage();
	ASSERT_NE(stale_output, 0u);
	ASSERT_TRUE(Core::VirtualMemory::Free(stale_output));
	EXPECT_EQ(get_info(15, reinterpret_cast<void*>(stale_output)), Kyty::Libs::Network::NET_ERROR_EFAULT);

	const uint64_t read_only_output = AllocateGuestPage();
	ASSERT_NE(read_only_output, 0u);
	auto* const read_only_bytes = reinterpret_cast<uint8_t*>(read_only_output);
	std::memset(read_only_bytes, 0x5a, 16);
	ASSERT_TRUE(Core::VirtualMemory::Protect(read_only_output, Core::VirtualMemory::GetPageSize(),
	                                         Core::VirtualMemory::Mode::Read));
	EXPECT_EQ(get_info(15, read_only_bytes), Kyty::Libs::Network::NET_ERROR_EFAULT);
	ASSERT_TRUE(Core::VirtualMemory::Protect(read_only_output, Core::VirtualMemory::GetPageSize(),
	                                         Core::VirtualMemory::Mode::ReadWrite));
	EXPECT_TRUE(std::all_of(read_only_bytes, read_only_bytes + 16,
	                        [](uint8_t value) { return value == static_cast<uint8_t>(0x5a); }));
	EXPECT_TRUE(Core::VirtualMemory::Free(read_only_output));
}

TEST(EmulatorNetwork, NetCtlNatInfoAndScalarOutputsUseGuestMemory)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));
	const auto* nat_record = symbols.Find(NetCtlFunction(u"JO4yuTuMoKI"));
	const auto* state_record = symbols.Find(NetCtlFunction(u"uBPlr0lbuiI"));
	const auto* callback_record = symbols.Find(NetCtlFunction(u"UJ+Z7Q+4ck0"));
	ASSERT_NE(nat_record, nullptr);
	ASSERT_NE(state_record, nullptr);
	ASSERT_NE(callback_record, nullptr);

	using GetNatInfo = int(KYTY_SYSV_ABI*)(Libs::Network::NetCtl::NetCtlNatInfo*);
	using GetState = int(KYTY_SYSV_ABI*)(int*);
	using RegisterCallback = int(KYTY_SYSV_ABI*)(Libs::Network::NetCtl::NetCtlCallback, void*, int*);
	struct NatInfo
	{
		uint32_t size;
		int32_t  stun_status;
		int32_t  nat_type;
		uint32_t mapped_addr;
	};
	static_assert(sizeof(NatInfo) == 16);

	auto get_nat = reinterpret_cast<GetNatInfo>(nat_record->vaddr);
	auto get_state = reinterpret_cast<GetState>(state_record->vaddr);
	auto register_callback = reinterpret_cast<RegisterCallback>(callback_record->vaddr);
	const uint64_t guest_output = AllocateGuestPage();
	ASSERT_NE(guest_output, 0u);
	auto* const nat_info = reinterpret_cast<NatInfo*>(guest_output);
	auto* const state = reinterpret_cast<int*>(guest_output + 64u);
	auto* const cid = reinterpret_cast<int*>(guest_output + 68u);

	*nat_info = {static_cast<uint32_t>(sizeof(NatInfo)), -1, -1, 0};
	ASSERT_EQ(get_nat(reinterpret_cast<Libs::Network::NetCtl::NetCtlNatInfo*>(nat_info)), 0);
	EXPECT_EQ(nat_info->size, static_cast<uint32_t>(sizeof(NatInfo)));
	EXPECT_EQ(nat_info->stun_status, 1);
	EXPECT_EQ(nat_info->nat_type, 3);
	EXPECT_EQ(nat_info->mapped_addr, 0x7f000001u);
	ASSERT_EQ(get_state(state), 0);
	EXPECT_EQ(*state, 0);
	ASSERT_EQ(register_callback(nullptr, nullptr, cid), 0);
	EXPECT_EQ(*cid, 1);

	*nat_info = {0, -1, -1, 0};
	EXPECT_EQ(get_nat(reinterpret_cast<Libs::Network::NetCtl::NetCtlNatInfo*>(nat_info)),
	          Kyty::Libs::Network::NET_ERROR_EINVAL);
	EXPECT_EQ(nat_info->stun_status, -1);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_output));
}

TEST(EmulatorNetwork, NetCtlSiblingOutputsRejectNonGuestFreedAndReadOnlyMemory)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));
	const auto* nat_record = symbols.Find(NetCtlFunction(u"JO4yuTuMoKI"));
	const auto* state_record = symbols.Find(NetCtlFunction(u"uBPlr0lbuiI"));
	const auto* callback_record = symbols.Find(NetCtlFunction(u"UJ+Z7Q+4ck0"));
	ASSERT_NE(nat_record, nullptr);
	ASSERT_NE(state_record, nullptr);
	ASSERT_NE(callback_record, nullptr);

	using GetNatInfo = int(KYTY_SYSV_ABI*)(Libs::Network::NetCtl::NetCtlNatInfo*);
	using GetState = int(KYTY_SYSV_ABI*)(int*);
	using RegisterCallback = int(KYTY_SYSV_ABI*)(Libs::Network::NetCtl::NetCtlCallback, void*, int*);
	struct NatInfo
	{
		uint32_t size;
		int32_t  stun_status;
		int32_t  nat_type;
		uint32_t mapped_addr;
	};
	static_assert(sizeof(NatInfo) == 16);

	auto get_nat = reinterpret_cast<GetNatInfo>(nat_record->vaddr);
	auto get_state = reinterpret_cast<GetState>(state_record->vaddr);
	auto register_callback = reinterpret_cast<RegisterCallback>(callback_record->vaddr);
	NatInfo host_nat {static_cast<uint32_t>(sizeof(NatInfo)), -1, -1, 0};
	int     host_state = 0x5a5a5a5a;
	int     host_cid   = 0x5a5a5a5a;
	EXPECT_EQ(get_nat(reinterpret_cast<Libs::Network::NetCtl::NetCtlNatInfo*>(&host_nat)),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(get_state(&host_state), Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(register_callback(nullptr, nullptr, &host_cid), Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(host_nat.stun_status, -1);
	EXPECT_EQ(host_state, 0x5a5a5a5a);
	EXPECT_EQ(host_cid, 0x5a5a5a5a);

	const uint64_t stale_output = AllocateGuestPage();
	ASSERT_NE(stale_output, 0u);
	ASSERT_TRUE(Core::VirtualMemory::Free(stale_output));
	EXPECT_EQ(get_nat(reinterpret_cast<Libs::Network::NetCtl::NetCtlNatInfo*>(stale_output)),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(get_state(reinterpret_cast<int*>(stale_output)), Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(register_callback(nullptr, nullptr, reinterpret_cast<int*>(stale_output)),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);

	const uint64_t read_only_output = AllocateGuestPage();
	ASSERT_NE(read_only_output, 0u);
	auto* const nat_info = reinterpret_cast<NatInfo*>(read_only_output);
	auto* const state = reinterpret_cast<int*>(read_only_output + 64u);
	auto* const cid = reinterpret_cast<int*>(read_only_output + 68u);
	*nat_info = {static_cast<uint32_t>(sizeof(NatInfo)), -1, -1, 0};
	*state = 0x5a5a5a5a;
	*cid = 0x5a5a5a5a;
	ASSERT_TRUE(Core::VirtualMemory::Protect(read_only_output, Core::VirtualMemory::GetPageSize(),
	                                         Core::VirtualMemory::Mode::Read));
	EXPECT_EQ(get_nat(reinterpret_cast<Libs::Network::NetCtl::NetCtlNatInfo*>(nat_info)),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(get_state(state), Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(register_callback(nullptr, nullptr, cid), Kyty::Libs::Network::NET_ERROR_EFAULT);
	ASSERT_TRUE(Core::VirtualMemory::Protect(read_only_output, Core::VirtualMemory::GetPageSize(),
	                                         Core::VirtualMemory::Mode::ReadWrite));
	EXPECT_EQ(nat_info->stun_status, -1);
	EXPECT_EQ(*state, 0x5a5a5a5a);
	EXPECT_EQ(*cid, 0x5a5a5a5a);
	EXPECT_TRUE(Core::VirtualMemory::Free(read_only_output));
}

TEST(EmulatorNetwork, NpManagerHasSignedUpRejectsUnprovenOutputWithoutWriting)
{
	if (!Config::IsInitialized())
	{
		Config::ConfigSubsystem::Instance()->Init(Core::SubsystemsList::Instance());
	}
	Log::LogSubsystem::Instance()->Init(Core::SubsystemsList::Instance());

	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNet_1", &symbols));
	const auto* record = symbols.Find(NpManagerFunction(u"Oad3rvY-NJQ"));
	ASSERT_NE(record, nullptr);

	using HasSignedUp = int(KYTY_SYSV_ABI*)(int, bool*);
	auto has_signed_up = reinterpret_cast<HasSignedUp>(record->vaddr);
	const uint64_t guest_output = AllocateGuestPage();
	ASSERT_NE(guest_output, 0u);
	auto* signed_up = reinterpret_cast<bool*>(guest_output);
	*signed_up = true;
	EXPECT_EQ(has_signed_up(1, signed_up), static_cast<int>(0x80550003u));
	EXPECT_TRUE(*signed_up);
	EXPECT_EQ(has_signed_up(1, nullptr), static_cast<int>(0x80550003u));
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_output));

#if !defined(_WIN32)
	const uint64_t stale_output = AllocateGuestPage();
	ASSERT_NE(stale_output, 0u);
	ASSERT_TRUE(Core::VirtualMemory::Free(stale_output));

	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		const int result = has_signed_up(1, reinterpret_cast<bool*>(stale_output));
		::_Exit(result == static_cast<int>(0x80550003u) ? 0 : 1);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	EXPECT_TRUE(WIFEXITED(status));
	if (WIFEXITED(status))
	{
		EXPECT_EQ(WEXITSTATUS(status), 0);
	}
#endif
}

TEST(EmulatorNetwork, NpGameIntentRejectsUnprovenOutputLayoutWithoutWriting)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpGameIntent_1", &symbols));
	const auto* record = symbols.Find(NpFunction(u"NpGameIntent", u"jEIXUAr9XE8"));
	ASSERT_NE(record, nullptr);

	const uint64_t page_size = Core::VirtualMemory::GetPageSize();
	ASSERT_GT(page_size, 0u);
	const uint64_t storage = Core::VirtualMemory::Alloc(0, page_size * 5u, Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(storage, 0u);

	auto* output = reinterpret_cast<uint8_t*>(storage);
	std::memset(output, 0xa5, static_cast<size_t>(page_size * 5u));
	using ReceiveIntent = int(KYTY_SYSV_ABI*)(void*);
	auto receive_intent = reinterpret_cast<ReceiveIntent>(record->vaddr);
	EXPECT_EQ(receive_intent(output), static_cast<int>(0x80553804u));
	EXPECT_TRUE(std::all_of(output, output + page_size * 5u, [](uint8_t value) { return value == 0xa5; }));

	EXPECT_TRUE(Core::VirtualMemory::Free(storage));
}

TEST(EmulatorNetwork, NpUniversalDataSystemRejectsUnprovenMemoryStatLayoutWithoutWriting)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libNpUniversalDataSystem_1", &symbols));
	const auto* record = symbols.Find(NpFunction(u"NpUniversalDataSystem", u"su7jW3VDDb4"));
	ASSERT_NE(record, nullptr);

	std::array<uint8_t, 32> output {};
	output.fill(0xa5);
	using GetMemoryStat = int(KYTY_SYSV_ABI*)(void*);
	auto get_memory_stat = reinterpret_cast<GetMemoryStat>(record->vaddr);
	EXPECT_EQ(get_memory_stat(output.data()), static_cast<int>(0x80553102u));
	EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](uint8_t value) { return value == 0xa5; }));
}

TEST(EmulatorNetwork, NetSendtoRejectsUnreadableGuestSockaddr)
{
#if defined(_WIN32)
	GTEST_SKIP() << "unmapped-address isolation uses fork on POSIX hosts";
#else
	using namespace Libs::Network::Net;

	const int socket_id = NetSocket("network-unreadable-sockaddr", 2, 2, 17);
	ASSERT_GT(socket_id, 0);

	const uint64_t page_size = Core::VirtualMemory::GetPageSize();
	ASSERT_GT(page_size, 0u);
	const uint64_t guest_address = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(guest_address, 0u);
	ASSERT_TRUE(Core::VirtualMemory::Free(guest_address));
	const uint64_t guest_payload = Core::VirtualMemory::Alloc(0, page_size, Core::VirtualMemory::Mode::ReadWrite);
	ASSERT_NE(guest_payload, 0u);
	*reinterpret_cast<char*>(guest_payload) = 'x';

	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		const auto result = NetSendto(socket_id, reinterpret_cast<const void*>(guest_payload), 1, 0,
		                              reinterpret_cast<const void*>(guest_address), 8);
		::_Exit(result == Kyty::Libs::Network::NET_ERROR_EINVAL ? 0 : 1);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	EXPECT_TRUE(WIFEXITED(status));
	if (WIFEXITED(status))
	{
		EXPECT_EQ(WEXITSTATUS(status), 0);
	}
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_payload));
	EXPECT_EQ(NetSocketClose(socket_id), 0);
#endif
}

TEST(EmulatorNetwork, NetSendCopiesGuestPayloadAndRejectsNonGuestRanges)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
	GTEST_SKIP() << "host UDP regression is Linux-specific";
#else
	using namespace Libs::Network::Net;

	uint16_t receiver_port = 0;
	const int receiver = CreateLoopbackSocket(SOCK_DGRAM, &receiver_port);
	ASSERT_GE(receiver, 0);
	const timeval receive_timeout {0, 500'000};
	ASSERT_EQ(::setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout)), 0);

	const int socket_id = NetSocket("network-payload-copy", 2, 2, 17);
	ASSERT_GT(socket_id, 0);
	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	StoreGuestSockaddr(guest_bytes, receiver_port);

	constexpr char non_guest_payload[] = "host";
	EXPECT_EQ(NetSend(socket_id, non_guest_payload, sizeof(non_guest_payload), 0), Kyty::Libs::Network::NET_ERROR_EFAULT);
	EXPECT_EQ(NetSendto(socket_id, non_guest_payload, sizeof(non_guest_payload), 0, guest_bytes, 8),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);

	const uint64_t stale_payload = AllocateGuestPage();
	ASSERT_NE(stale_payload, 0u);
	ASSERT_TRUE(Core::VirtualMemory::Free(stale_payload));
	EXPECT_EQ(NetSendto(socket_id, reinterpret_cast<const void*>(stale_payload), 1, 0, guest_bytes, 8),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);

	constexpr char guest_payload[] = "guest";
	std::memcpy(guest_bytes + 64, guest_payload, sizeof(guest_payload));
	EXPECT_EQ(NetSendto(socket_id, guest_bytes + 64, sizeof(guest_payload), 0, guest_bytes, 8),
	          static_cast<int64_t>(sizeof(guest_payload)));

	std::array<char, 16> received {};
	const auto received_size = ::recv(receiver, received.data(), received.size(), 0);
	ASSERT_EQ(received_size, static_cast<ssize_t>(sizeof(guest_payload)));
	EXPECT_EQ(std::memcmp(received.data(), guest_payload, sizeof(guest_payload)), 0);

	EXPECT_EQ(NetSocketClose(socket_id), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
	EXPECT_EQ(::close(receiver), 0);
#endif
}

TEST(EmulatorNetwork, NetRejectsOversizedStagingBeforeGuestAccess)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
	GTEST_SKIP() << "host socket staging limits are Linux-specific";
#else
	using namespace Libs::Network::Net;

	const int socket_id = NetSocket("network-staging-bound", 2, 2, 17);
	ASSERT_GT(socket_id, 0);
	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	StoreGuestSockaddr(guest_bytes, 9);
	const void* const invalid_guest  = reinterpret_cast<const void*>(static_cast<uintptr_t>(1));
	void* const       invalid_output = reinterpret_cast<void*>(static_cast<uintptr_t>(1));

	EXPECT_EQ(NetSend(socket_id, invalid_guest, 65'508, 0), Kyty::Libs::Network::NET_ERROR_EMSGSIZE);
	EXPECT_EQ(NetSendto(socket_id, invalid_guest, 65'508, 0, guest_bytes, 8), Kyty::Libs::Network::NET_ERROR_EMSGSIZE);
	EXPECT_EQ(NetRecv(socket_id, invalid_output, 65'508, 0), Kyty::Libs::Network::NET_ERROR_EMSGSIZE);
	EXPECT_EQ(NetGetSockInfo(socket_id, invalid_output, 65'537, 0), Kyty::Libs::Network::NET_ERROR_EMSGSIZE);
	EXPECT_EQ(NetEpollWait(1, invalid_output, 1025, 0), Kyty::Libs::Network::NET_ERROR_EMSGSIZE);

	EXPECT_EQ(NetSocketClose(socket_id), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

TEST(EmulatorNetwork, NetOutputBuffersRejectNonGuestRanges)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
	GTEST_SKIP() << "host UDP regression is Linux-specific";
#else
	using namespace Libs::Network::Net;

	const int socket_id = NetSocket("network-output-range", 2, 2, 17);
	ASSERT_GT(socket_id, 0);
	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	StoreGuestSockaddr(guest_bytes, 0);
	ASSERT_EQ(NetBind(socket_id, guest_bytes, 8), 0);

	auto* const guest_name = guest_bytes + 16;
	auto* const guest_name_len = reinterpret_cast<int*>(guest_bytes + 32);
	*guest_name_len = 8;
	ASSERT_EQ(NetGetsockname(socket_id, guest_name, guest_name_len), 0);
	const uint16_t guest_port = LoadGuestSockaddrPort(guest_name);
	ASSERT_NE(guest_port, 0u);

	std::array<uint8_t, 8> non_guest_name {};
	int non_guest_name_len = static_cast<int>(non_guest_name.size());
	EXPECT_EQ(NetGetsockname(socket_id, non_guest_name.data(), &non_guest_name_len), Kyty::Libs::Network::NET_ERROR_EFAULT);

	int non_guest_value = 0;
	int non_guest_value_len = sizeof(non_guest_value);
	EXPECT_EQ(NetGetsockopt(socket_id, 0xffff, 0x0004, &non_guest_value, &non_guest_value_len),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);

	std::array<uint8_t, 16> non_guest_info {};
	EXPECT_EQ(NetGetSockInfo(socket_id, non_guest_info.data(), static_cast<int>(non_guest_info.size()), 0),
	          Kyty::Libs::Network::NET_ERROR_EFAULT);
	ASSERT_TRUE(Core::VirtualMemory::Protect(guest_storage, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::Read));
	EXPECT_EQ(NetGetsockname(socket_id, guest_name, guest_name_len), Kyty::Libs::Network::NET_ERROR_EFAULT);
	ASSERT_TRUE(
	    Core::VirtualMemory::Protect(guest_storage, Core::VirtualMemory::GetPageSize(), Core::VirtualMemory::Mode::ReadWrite));

	const int sender = ::socket(AF_INET, SOCK_DGRAM, 0);
	ASSERT_GE(sender, 0);
	sockaddr_in destination {};
	destination.sin_family      = AF_INET;
	destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	destination.sin_port        = htons(guest_port);
	constexpr char packet[] = "r";
	ASSERT_EQ(::sendto(sender, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)),
	          static_cast<ssize_t>(sizeof(packet)));
	char non_guest_receive = 0;
	EXPECT_EQ(NetRecv(socket_id, &non_guest_receive, 1, 0), Kyty::Libs::Network::NET_ERROR_EFAULT);

	EXPECT_EQ(::close(sender), 0);
	EXPECT_EQ(NetSocketClose(socket_id), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

TEST(EmulatorNetwork, NetLoopbackDatagramAndPeerLifecycle)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
	GTEST_SKIP() << "host socket integration is Linux-specific";
#else
	using namespace Libs::Network::Net;

	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);

	StoreGuestSockaddr(guest_bytes, 0);
	const int receiver = NetSocket("network-loopback-receiver", 2, 2, 17);
	ASSERT_GT(receiver, 0);
	ASSERT_EQ(NetBind(receiver, guest_bytes, 8), 0);
	auto* const receiver_name = guest_bytes + 16;
	auto* const receiver_name_len = reinterpret_cast<int*>(guest_bytes + 32);
	*receiver_name_len = 8;
	ASSERT_EQ(NetGetsockname(receiver, receiver_name, receiver_name_len), 0);
	ASSERT_EQ(NetFcntl(receiver, 4, 0x0004), 0);
	EXPECT_EQ(NetFcntl(receiver, 3, 0), 0x0006);
	EXPECT_EQ(NetRecvfrom(receiver, guest_bytes + 64, 1, 0, nullptr, nullptr), Kyty::Libs::Network::NET_ERROR_EAGAIN);

	uint16_t sender_port = 0;
	const int sender = CreateLoopbackSocket(SOCK_DGRAM, &sender_port);
	ASSERT_GE(sender, 0);
	sockaddr_in destination {};
	destination.sin_family      = AF_INET;
	destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	destination.sin_port        = htons(LoadGuestSockaddrPort(receiver_name));
	constexpr char packet[] = "menu";
	ASSERT_EQ(::sendto(sender, packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)),
	          static_cast<ssize_t>(sizeof(packet)));
	auto* const peer = guest_bytes + 96;
	auto* const peer_len = reinterpret_cast<uint32_t*>(guest_bytes + 112);
	*peer_len = 8;
	ASSERT_EQ(NetRecvfrom(receiver, guest_bytes + 64, sizeof(packet), 0, peer, peer_len), static_cast<int64_t>(sizeof(packet)));
	EXPECT_EQ(std::memcmp(guest_bytes + 64, packet, sizeof(packet)), 0);
	EXPECT_EQ(LoadGuestSockaddrPort(peer), sender_port);
	EXPECT_EQ(NetSocketClose(receiver), 0);
	EXPECT_EQ(::close(sender), 0);

	uint16_t listener_port = 0;
	const int listener = CreateLoopbackSocket(SOCK_STREAM, &listener_port);
	ASSERT_GE(listener, 0);
	ASSERT_EQ(::listen(listener, 1), 0);
	StoreGuestSockaddr(guest_bytes, listener_port);
	const int client = NetSocket("network-loopback-client", 2, 1, 6);
	ASSERT_GT(client, 0);
	ASSERT_EQ(NetConnect(client, guest_bytes, 8), 0);
	const int accepted = ::accept(listener, nullptr, nullptr);
	ASSERT_GE(accepted, 0);
	*peer_len = 8;
	ASSERT_EQ(NetGetpeername(client, peer, peer_len), 0);
	EXPECT_EQ(LoadGuestSockaddrPort(peer), listener_port);
	EXPECT_EQ(NetShutdown(client, 2), 0);
	EXPECT_EQ(NetSocketClose(client), 0);
	EXPECT_EQ(::close(accepted), 0);
	EXPECT_EQ(::close(listener), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

TEST(EmulatorNetwork, NetEpollDestroyCancelsWaiterBeforeRecreate)
{
#if !KYTY_NETWORK_TEST_HOST_EPOLL
	GTEST_SKIP() << "epoll cancellation is Linux-specific";
#else
	const pid_t child = ::fork();
	ASSERT_GE(child, 0);
	if (child == 0)
	{
		::alarm(2);
		const int epoll_id = Libs::Network::Net::NetEpollCreate("network-epoll-wait", 0);
		if (epoll_id <= 0)
		{
			::_Exit(1);
		}
		const uint64_t guest_events = AllocateGuestPage();
		if (guest_events == 0)
		{
			::_Exit(2);
		}

		std::atomic<bool> waiter_started {false};
		std::promise<int> waiter_promise;
		auto waiter_result = waiter_promise.get_future();
		std::thread waiter([&]() {
			waiter_started.store(true, std::memory_order_release);
			waiter_promise.set_value(Libs::Network::Net::NetEpollWait(epoll_id, reinterpret_cast<void*>(guest_events), 1, -1));
		});

		if (!WaitForFlag(waiter_started, std::chrono::milliseconds(250)))
		{
			::_Exit(3);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(20));
		if (Libs::Network::Net::NetEpollDestroy(epoll_id) != 0)
		{
			::_Exit(4);
		}
		const int recreated = Libs::Network::Net::NetEpollCreate("network-epoll-recreated", 0);
		if (recreated <= 0 || recreated == epoll_id)
		{
			::_Exit(5);
		}
		if (waiter_result.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready)
		{
			::_Exit(6);
		}
		const int wait_result = waiter_result.get();
		waiter.join();
		if (Libs::Network::Net::NetEpollDestroy(recreated) != 0 || !Core::VirtualMemory::Free(guest_events) ||
		    wait_result != Kyty::Libs::Network::NET_ERROR_EBADF)
		{
			::_Exit(7);
		}
		::alarm(0);
		::_Exit(0);
	}

	int status = 0;
	ASSERT_EQ(::waitpid(child, &status, 0), child);
	EXPECT_TRUE(WIFEXITED(status));
	if (WIFEXITED(status))
	{
		EXPECT_EQ(WEXITSTATUS(status), 0);
	}
#endif
}

TEST(EmulatorNetwork, NetEpollCloseChurnDropsClosedSocketRegistration)
{
#if !KYTY_NETWORK_TEST_HOST_EPOLL
	GTEST_SKIP() << "epoll registration churn is Linux-specific";
#else
	using namespace Libs::Network::Net;

	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	struct Event
	{
		uint32_t events;
		uint32_t reserved;
		uint64_t ident;
		uint64_t data;
	};
	auto* const event = reinterpret_cast<Event*>(guest_bytes + 128);
	*event = {1u, 0u, 0x1234u, 0x5678u};

	const int epoll_id = NetEpollCreate("network-epoll-close-churn", 0);
	ASSERT_GT(epoll_id, 0);
	const int closed_socket = NetSocket("network-epoll-closed", 2, 2, 17);
	ASSERT_GT(closed_socket, 0);
	ASSERT_EQ(NetEpollControl(epoll_id, 1, closed_socket, event), 0);
	ASSERT_EQ(NetSocketClose(closed_socket), 0);

	StoreGuestSockaddr(guest_bytes, 0);
	const int churn_socket = NetSocket("network-epoll-churn", 2, 2, 17);
	ASSERT_GT(churn_socket, 0);
	ASSERT_EQ(NetBind(churn_socket, guest_bytes, 8), 0);
	auto* const guest_name = guest_bytes + 16;
	auto* const guest_name_len = reinterpret_cast<int*>(guest_bytes + 32);
	*guest_name_len = 8;
	ASSERT_EQ(NetGetsockname(churn_socket, guest_name, guest_name_len), 0);

	uint16_t sender_port = 0;
	const int sender = CreateLoopbackSocket(SOCK_DGRAM, &sender_port);
	ASSERT_GE(sender, 0);
	sockaddr_in destination {};
	destination.sin_family      = AF_INET;
	destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	destination.sin_port        = htons(LoadGuestSockaddrPort(guest_name));
	const char packet = 'x';
	ASSERT_EQ(::sendto(sender, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)),
	          static_cast<ssize_t>(sizeof(packet)));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	EXPECT_EQ(NetEpollWait(epoll_id, guest_bytes + 256, 1, 0), 0);

	EXPECT_EQ(::close(sender), 0);
	EXPECT_EQ(NetSocketClose(churn_socket), 0);
	EXPECT_EQ(NetEpollDestroy(epoll_id), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

TEST(EmulatorNetwork, NetEpollDelDropsReadyRegistration)
{
#if !KYTY_NETWORK_TEST_HOST_EPOLL
	GTEST_SKIP() << "epoll registration race is Linux-specific";
#else
	using namespace Libs::Network::Net;

	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	StoreGuestSockaddr(guest_bytes, 0);
	const int socket_id = NetSocket("network-epoll-del-ready", 2, 2, 17);
	ASSERT_GT(socket_id, 0);
	ASSERT_EQ(NetBind(socket_id, guest_bytes, 8), 0);
	auto* const guest_name = guest_bytes + 16;
	auto* const guest_name_len = reinterpret_cast<int*>(guest_bytes + 32);
	*guest_name_len = 8;
	ASSERT_EQ(NetGetsockname(socket_id, guest_name, guest_name_len), 0);

	struct Event
	{
		uint32_t events;
		uint32_t reserved;
		uint64_t ident;
		uint64_t data;
	};
	auto* const event = reinterpret_cast<Event*>(guest_bytes + 128);
	*event = {1u, 0u, 0x1122u, 0x3344u};
	const int epoll_id = NetEpollCreate("network-epoll-del-ready", 0);
	ASSERT_GT(epoll_id, 0);
	ASSERT_EQ(NetEpollControl(epoll_id, 1, socket_id, event), 0);

	uint16_t sender_port = 0;
	const int sender = CreateLoopbackSocket(SOCK_DGRAM, &sender_port);
	ASSERT_GE(sender, 0);
	sockaddr_in destination {};
	destination.sin_family      = AF_INET;
	destination.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	destination.sin_port        = htons(LoadGuestSockaddrPort(guest_name));
	const char packet = 'r';
	ASSERT_EQ(::sendto(sender, &packet, sizeof(packet), 0, reinterpret_cast<sockaddr*>(&destination), sizeof(destination)),
	          static_cast<ssize_t>(sizeof(packet)));
	std::this_thread::sleep_for(std::chrono::milliseconds(10));
	ASSERT_EQ(NetEpollControl(epoll_id, 3, socket_id, nullptr), 0);
	EXPECT_EQ(NetEpollWait(epoll_id, guest_bytes + 256, 1, 0), 0);

	EXPECT_EQ(::close(sender), 0);
	EXPECT_EQ(NetEpollDestroy(epoll_id), 0);
	EXPECT_EQ(NetSocketClose(socket_id), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

TEST(EmulatorNetwork, NetAcceptRejectsNonGuestOutputRange)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
	GTEST_SKIP() << "host accept regression is Linux-specific";
#else
	using namespace Libs::Network::Net;

	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	StoreGuestSockaddr(guest_bytes, 0);
	const int socket_id = NetSocket("network-accept-output-range", 2, 1, 6);
	ASSERT_GT(socket_id, 0);
	ASSERT_EQ(NetBind(socket_id, guest_bytes, 8), 0);
	ASSERT_EQ(NetListen(socket_id, 1), 0);
	auto* const guest_name = guest_bytes + 16;
	auto* const guest_name_len = reinterpret_cast<int*>(guest_bytes + 32);
	*guest_name_len = 8;
	ASSERT_EQ(NetGetsockname(socket_id, guest_name, guest_name_len), 0);
	const int client = ConnectLoopback(LoadGuestSockaddrPort(guest_name));
	ASSERT_GE(client, 0);

	std::array<uint8_t, 8> non_guest_peer {};
	int non_guest_peer_len = static_cast<int>(non_guest_peer.size());
	const int accepted = NetAccept(socket_id, non_guest_peer.data(), &non_guest_peer_len);
	EXPECT_EQ(accepted, Kyty::Libs::Network::NET_ERROR_EFAULT);
	if (accepted > 0)
	{
		EXPECT_EQ(NetSocketClose(accepted), 0);
	}

	EXPECT_EQ(::close(client), 0);
	EXPECT_EQ(NetSocketClose(socket_id), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

TEST(EmulatorNetwork, NetSocketCloseCancelsPendingRecv)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
	GTEST_SKIP() << "socket cancellation is Linux-specific";
#else
	using namespace Libs::Network::Net;

	uint16_t listener_port = 0;
	const int listener = CreateLoopbackSocket(SOCK_STREAM, &listener_port);
	ASSERT_GE(listener, 0);
	ASSERT_EQ(::listen(listener, 1), 0);
	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	StoreGuestSockaddr(guest_bytes, listener_port);
	const int socket_id = NetSocket("network-close-recv", 2, 1, 6);
	ASSERT_GT(socket_id, 0);
	ASSERT_EQ(NetConnect(socket_id, guest_bytes, 8), 0);
	const int peer = ::accept(listener, nullptr, nullptr);
	ASSERT_GE(peer, 0);

	std::atomic<bool> recv_started {false};
	std::promise<int64_t> recv_promise;
	auto recv_result = recv_promise.get_future();
	std::thread receiver([&]() {
		recv_started.store(true, std::memory_order_release);
		recv_promise.set_value(NetRecv(socket_id, guest_bytes + 64, 1, 0));
	});
	ASSERT_TRUE(WaitForFlag(recv_started, std::chrono::milliseconds(250)));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	auto close_result = std::async(std::launch::async, [socket_id]() { return NetSocketClose(socket_id); });
	const auto close_status = close_result.wait_for(std::chrono::milliseconds(250));
	if (close_status != std::future_status::ready)
	{
		::shutdown(peer, SHUT_RDWR);
	}
	EXPECT_EQ(close_status, std::future_status::ready);
	ASSERT_EQ(close_result.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
	EXPECT_EQ(close_result.get(), 0);
	ASSERT_EQ(recv_result.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
	(void)recv_result.get();
	receiver.join();

	EXPECT_EQ(::close(peer), 0);
	EXPECT_EQ(::close(listener), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

TEST(EmulatorNetwork, NetSocketCloseCancelsPendingAccept)
{
#if KYTY_PLATFORM != KYTY_PLATFORM_LINUX
	GTEST_SKIP() << "socket cancellation is Linux-specific";
#else
	using namespace Libs::Network::Net;

	const uint64_t guest_storage = AllocateGuestPage();
	ASSERT_NE(guest_storage, 0u);
	auto* const guest_bytes = reinterpret_cast<uint8_t*>(guest_storage);
	StoreGuestSockaddr(guest_bytes, 0);
	const int socket_id = NetSocket("network-close-accept", 2, 1, 6);
	ASSERT_GT(socket_id, 0);
	ASSERT_EQ(NetBind(socket_id, guest_bytes, 8), 0);
	ASSERT_EQ(NetListen(socket_id, 1), 0);
	auto* const guest_name = guest_bytes + 16;
	auto* const guest_name_len = reinterpret_cast<int*>(guest_bytes + 32);
	*guest_name_len = 8;
	ASSERT_EQ(NetGetsockname(socket_id, guest_name, guest_name_len), 0);
	const uint16_t listener_port = LoadGuestSockaddrPort(guest_name);
	ASSERT_NE(listener_port, 0u);

	std::atomic<bool> accept_started {false};
	std::promise<int> accept_promise;
	auto accept_result = accept_promise.get_future();
	std::thread accepter([&]() {
		accept_started.store(true, std::memory_order_release);
		accept_promise.set_value(NetAccept(socket_id, nullptr, nullptr));
	});
	ASSERT_TRUE(WaitForFlag(accept_started, std::chrono::milliseconds(250)));
	std::this_thread::sleep_for(std::chrono::milliseconds(20));

	auto close_result = std::async(std::launch::async, [socket_id]() { return NetSocketClose(socket_id); });
	const auto close_status = close_result.wait_for(std::chrono::milliseconds(250));
	int fallback_client = -1;
	if (close_status != std::future_status::ready)
	{
		fallback_client = ConnectLoopback(listener_port);
	}
	EXPECT_EQ(close_status, std::future_status::ready);
	ASSERT_EQ(close_result.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
	EXPECT_EQ(close_result.get(), 0);
	ASSERT_EQ(accept_result.wait_for(std::chrono::milliseconds(500)), std::future_status::ready);
	const int accepted_socket = accept_result.get();
	accepter.join();
	if (accepted_socket > 0)
	{
		EXPECT_EQ(NetSocketClose(accepted_socket), 0);
	}
	if (fallback_client >= 0)
	{
		EXPECT_EQ(::close(fallback_client), 0);
	}
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_storage));
#endif
}

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

#if KYTY_NETWORK_TEST_HOST_EPOLL
	const int epoll_id = NetEpollCreate("test-epoll", 0);
	EXPECT_GT(epoll_id, 0);
	EXPECT_EQ(NetEpollDestroy(epoll_id), 0);
	EXPECT_LT(NetEpollDestroy(epoll_id), 0);
#else
	EXPECT_EQ(NetEpollCreate("test-epoll", 0), Kyty::Libs::Network::NET_ERROR_ENOTSUP);
#endif
}

TEST(EmulatorNetwork, NetEpollControlRoundTripKeepsRegistrationState)
{
#if !KYTY_NETWORK_TEST_HOST_EPOLL
	GTEST_SKIP() << "epoll control is Linux-specific";
#else
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

	const uint64_t guest_event_address = AllocateGuestPage();
	ASSERT_NE(guest_event_address, 0u);
	auto* event = reinterpret_cast<Event*>(guest_event_address);
	*event = {1u, 0u, 0x1234u, 0x5678u};
	EXPECT_EQ(NetEpollControl(epoll_id, 1, socket_id, event), 0);
	event->events = 5u;
	EXPECT_EQ(NetEpollControl(epoll_id, 2, socket_id, event), 0);
	EXPECT_EQ(NetEpollControl(epoll_id, 3, socket_id, nullptr), 0);
	EXPECT_TRUE(Core::VirtualMemory::Free(guest_event_address));
	EXPECT_EQ(NetEpollDestroy(epoll_id), 0);
	EXPECT_EQ(NetSocketClose(socket_id), 0);
#endif
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
