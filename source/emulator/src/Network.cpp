#include "Emulator/Network.h"

#include "Kyty/Core/ByteBuffer.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"
#include "Kyty/Core/VirtualMemory.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX || defined(__APPLE__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#define KYTY_NET_HOST_POSIX 1
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX && !defined(__APPLE__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#define KYTY_NET_HOST_EPOLL 1
#else
#define KYTY_NET_HOST_EPOLL 0
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Network {

class Network
{
public:
	class Id
	{
	public:
		static constexpr int MAX_ID = 65536;

		enum class Type : uint32_t
		{
			Invalid    = 0,
			Http       = 1,
			Ssl        = 2,
			Template   = 3,
			Connection = 4,
			Request    = 5,
		};

		explicit Id(int id): m_id(static_cast<uint32_t>(id) & 0xffffu), m_type(static_cast<uint32_t>(id) >> 16u) {}
		[[nodiscard]] int  ToInt() const { return static_cast<int>(m_id + (static_cast<uint32_t>(m_type) << 16u)); }
		[[nodiscard]] bool IsValid() const { return GetType() != Type::Invalid; }
		[[nodiscard]] Type GetType() const
		{
			switch (m_type)
			{
				case static_cast<uint32_t>(Type::Http): return Type::Http; break;
				case static_cast<uint32_t>(Type::Ssl): return Type::Ssl; break;
				case static_cast<uint32_t>(Type::Template): return Type::Template; break;
				case static_cast<uint32_t>(Type::Connection): return Type::Connection; break;
				case static_cast<uint32_t>(Type::Request): return Type::Request; break;
				default: return Type::Invalid;
			}
		}

		friend class Network;

	private:
		Id() = default;
		static Id Invalid() { return {}; }
		static Id Create(int net_id, Type type)
		{
			Id r;
			r.m_id   = net_id;
			r.m_type = static_cast<uint32_t>(type);
			return r;
		}
		[[nodiscard]] int GetId() const { return static_cast<int>(m_id); }

		uint32_t m_id   = 0;
		uint32_t m_type = static_cast<uint32_t>(Type::Invalid);
	};

	using HttpsCallback = KYTY_SYSV_ABI int (*)(int, unsigned int, void* const*, int, void*);

	Network()          = default;
	virtual ~Network() = default;

	KYTY_CLASS_NO_COPY(Network);

	int  PoolCreate(const char* name, int size);
	bool PoolDestroy(int memid);

	Id   SslInit(uint64_t pool_size);
	bool SslTerm(Id ssl_ctx_id);

	Id   HttpInit(int memid, Id ssl_ctx_id, uint64_t pool_size);
	bool HttpTerm(Id http_ctx_id);
	Id   HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver, bool is_auto_proxy_conf);
	bool HttpDeleteTemplate(Id tmpl_id);
	bool HttpSetNonblock(Id id, bool enable);
	bool HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg);
	bool HttpsDisableOption(Id id, uint32_t ssl_flags);
	bool HttpAddRequestHeader(Id id, const char* name, const char* value, bool add);
	bool HttpValid(Id http_ctx_id);
	bool HttpValidTemplate(Id tmpl_id);
	bool HttpValidConnection(Id conn_id);
	bool HttpValidRequest(Id req_id);
	Id   HttpCreateConnectionWithURL(Id tmpl_id, const char* url, bool enable_keep_alive);
	bool HttpDeleteConnection(Id conn_id);
	Id   HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url, uint64_t content_length);
	bool HttpDeleteRequest(Id req_id);
	bool HttpSetResolveTimeOut(Id id, uint32_t usec);
	bool HttpSetResolveRetry(Id id, int32_t retry);
	bool HttpSetConnectTimeOut(Id id, uint32_t usec);
	bool HttpSetSendTimeOut(Id id, uint32_t usec);
	bool HttpSetRecvTimeOut(Id id, uint32_t usec);
	bool HttpSetAutoRedirect(Id id, int enable);
	bool HttpSetAuthEnabled(Id id, int enable);

private:
	struct Pool
	{
		bool   used = false;
		String name;
		int    size = 0;
	};

	struct Ssl
	{
		bool     used = false;
		uint64_t size = 0;
	};

	struct Http
	{
		bool     used       = false;
		uint64_t size       = 0;
		int      memid      = 0;
		int      ssl_ctx_id = 0;
	};

	struct HttpHeader
	{
		String name;
		String value;
	};

	struct HttpBase
	{
		Vector<HttpHeader> headers;
		bool               used            = false;
		bool               nonblock        = false;
		bool               auto_redirect   = true;
		bool               auth_enabled    = true;
		HttpsCallback      ssl_cbfunc      = nullptr;
		void*              ssl_user_arg    = nullptr;
		uint32_t           ssl_flags       = 0xA7;
		int                http_ctx_id     = 0;
		uint32_t           resolve_timeout = 1'000000;
		int32_t            resolve_retry   = 4;
		uint32_t           connect_timeout = 30'000000;
		uint32_t           send_timeout    = 120'000000;
		uint32_t           recv_timeout    = 120'000000;
	};

	struct HttpTemplate: public HttpBase
	{
		String user_agent;
		int    http_ver           = 0;
		bool   is_auto_proxy_conf = true;
	};

	struct HttpConnection: public HttpTemplate
	{
		explicit HttpConnection(const HttpTemplate& tmpl): HttpTemplate(tmpl) {}
		// int    tmpl_id = 0;
		String url;
		bool   enable_keep_alive = false;
	};

	struct HttpRequest: public HttpConnection
	{
		explicit HttpRequest(HttpConnection& conn): HttpConnection(conn) {}
		// int      conn_id = 0;
		String   method;
		String   url;
		uint64_t content_length = 0;
	};

	static constexpr int POOLS_MAX = 32;
	static constexpr int SSL_MAX   = 32;
	static constexpr int HTTP_MAX  = 32;

	Core::Mutex            m_mutex;
	Pool                   m_pools[POOLS_MAX];
	Ssl                    m_ssl[SSL_MAX];
	Http                   m_http[HTTP_MAX];
	Vector<HttpTemplate>   m_templates;
	Vector<HttpConnection> m_connections;
	Vector<HttpRequest>    m_requests;
};

static Network* g_net = nullptr;

void NetworkSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	EXIT_IF(g_net != nullptr);

	g_net = new Network;
}

void NetworkSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void NetworkSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

int Network::PoolCreate(const char* name, int size)
{
	Core::LockGuard lock(m_mutex);

	for (int id = 0; id < POOLS_MAX; id++)
	{
		if (!m_pools[id].used)
		{
			m_pools[id].used = true;
			m_pools[id].size = size;
			m_pools[id].name = String::FromUtf8(name);

			return id;
		}
	}

	return -1;
}

bool Network::PoolDestroy(int memid)
{
	Core::LockGuard lock(m_mutex);

	if (memid >= 0 && memid < POOLS_MAX && m_pools[memid].used)
	{
		m_pools[memid].used = false;

		return true;
	}

	return false;
}

Network::Id Network::SslInit(uint64_t pool_size)
{
	Core::LockGuard lock(m_mutex);

	for (int id = 0; id < SSL_MAX; id++)
	{
		if (!m_ssl[id].used)
		{
			m_ssl[id].used = true;
			m_ssl[id].size = pool_size;

			return Id::Create(id, Id::Type::Ssl);
		}
	}

	return Id::Invalid();
}

bool Network::SslTerm(Id ssl_ctx_id)
{
	Core::LockGuard lock(m_mutex);

	if (ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 && ssl_ctx_id.GetId() < SSL_MAX && m_ssl[ssl_ctx_id.GetId()].used)
	{
		m_ssl[ssl_ctx_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpInit(int memid, Id ssl_ctx_id, uint64_t pool_size)
{
	Core::LockGuard lock(m_mutex);

	if (ssl_ctx_id.GetType() == Id::Type::Ssl && ssl_ctx_id.GetId() >= 0 && ssl_ctx_id.GetId() < SSL_MAX &&
	    m_ssl[ssl_ctx_id.GetId()].used && memid >= 0 && memid < POOLS_MAX && m_pools[memid].used)
	{
		for (int id = 0; id < HTTP_MAX; id++)
		{
			if (!m_http[id].used)
			{
				m_http[id].used       = true;
				m_http[id].size       = pool_size;
				m_http[id].ssl_ctx_id = ssl_ctx_id.GetId();
				m_http[id].memid      = memid;

				return Id::Create(id, Id::Type::Http);
			}
		}
	}

	return Id::Invalid();
}

bool Network::HttpValid(Id http_ctx_id)
{
	Core::LockGuard lock(m_mutex);

	return (http_ctx_id.GetType() == Id::Type::Http && http_ctx_id.GetId() >= 0 && http_ctx_id.GetId() < HTTP_MAX &&
	        m_http[http_ctx_id.GetId()].used);
}

bool Network::HttpValidTemplate(Id tmpl_id)
{
	Core::LockGuard lock(m_mutex);

	return (tmpl_id.GetType() == Id::Type::Template && m_templates.IndexValid(tmpl_id.GetId()) && m_templates.At(tmpl_id.GetId()).used);
}

bool Network::HttpValidConnection(Id conn_id)
{
	Core::LockGuard lock(m_mutex);

	return (conn_id.GetType() == Id::Type::Connection && m_connections.IndexValid(conn_id.GetId()) &&
	        m_connections.At(conn_id.GetId()).used);
}

bool Network::HttpValidRequest(Id req_id)
{
	Core::LockGuard lock(m_mutex);

	return (req_id.GetType() == Id::Type::Request && m_requests.IndexValid(req_id.GetId()) && m_requests.At(req_id.GetId()).used);
}

bool Network::HttpTerm(Id http_ctx_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id))
	{
		m_http[http_ctx_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateTemplate(Id http_ctx_id, const char* user_agent, int http_ver, bool is_auto_proxy_conf)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValid(http_ctx_id))
	{
		HttpTemplate tn {};
		tn.used               = true;
		tn.http_ver           = http_ver;
		tn.user_agent         = String::FromUtf8(user_agent);
		tn.is_auto_proxy_conf = is_auto_proxy_conf;
		tn.http_ctx_id        = http_ctx_id.GetId();
		tn.nonblock           = false;

		int index = 0;
		for (auto& t: m_templates)
		{
			if (!t.used)
			{
				t = tn;
				return Id::Create(index, Id::Type::Template);
			}
			index++;
		}

		if (index < Id::MAX_ID)
		{
			m_templates.Add(tn);
			return Id::Create(index, Id::Type::Template);
		}
	}

	return Id::Invalid();
}

Network::Id Network::HttpCreateConnectionWithURL(Id tmpl_id, const char* url, bool enable_keep_alive)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidTemplate(tmpl_id))
	{
		HttpConnection cn(m_templates[tmpl_id.GetId()]);
		cn.used              = true;
		cn.enable_keep_alive = enable_keep_alive;
		cn.url               = String::FromUtf8(url);
		// cn.tmpl_id           = tmpl_id.ToInt();

		int index = 0;
		for (auto& t: m_connections)
		{
			if (!t.used)
			{
				t = cn;
				return Id::Create(index, Id::Type::Connection);
			}
			index++;
		}

		if (index < Id::MAX_ID)
		{
			m_connections.Add(cn);
			return Id::Create(index, Id::Type::Connection);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteConnection(Id conn_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidConnection(conn_id))
	{
		m_connections[conn_id.GetId()].used = false;

		return true;
	}

	return false;
}

Network::Id Network::HttpCreateRequestWithURL2(Id conn_id, const char* method, const char* url, uint64_t content_length)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidConnection(conn_id))
	{
		HttpRequest cn(m_connections[conn_id.GetId()]);
		cn.used   = true;
		cn.method = String::FromUtf8(method);
		cn.url    = String::FromUtf8(url);
		// cn.conn_id        = conn_id.ToInt();
		cn.content_length = content_length;

		int index = 0;
		for (auto& t: m_requests)
		{
			if (!t.used)
			{
				t = cn;
				return Id::Create(index, Id::Type::Request);
			}
			index++;
		}

		if (index < Id::MAX_ID)
		{
			m_requests.Add(cn);
			return Id::Create(index, Id::Type::Request);
		}
	}

	return Id::Invalid();
}

bool Network::HttpDeleteRequest(Id req_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidRequest(req_id))
	{
		m_requests[req_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::HttpDeleteTemplate(Id tmpl_id)
{
	Core::LockGuard lock(m_mutex);

	if (HttpValidTemplate(tmpl_id))
	{
		m_templates[tmpl_id.GetId()].used = false;

		return true;
	}

	return false;
}

bool Network::HttpSetNonblock(Id id, bool enable)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->nonblock = enable;
		return true;
	}

	return false;
}

bool Network::HttpsSetSslCallback(Id id, HttpsCallback cbfunc, void* user_arg)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->ssl_cbfunc   = cbfunc;
		base->ssl_user_arg = user_arg;
		return true;
	}

	return false;
}

bool Network::HttpsDisableOption(Id id, uint32_t ssl_flags)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->ssl_flags &= ~ssl_flags;
		return true;
	}

	return false;
}

bool Network::HttpAddRequestHeader(Id id, const char* name, const char* value, bool add)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		HttpHeader nh({String::FromUtf8(name), String::FromUtf8(value)});
		if (add)
		{
			base->headers.Add(nh);
		} else
		{
			for (auto& h: base->headers)
			{
				if (h.name == nh.name)
				{
					h.value = nh.value;
				}
			}
		}
		return true;
	}

	return false;
}

bool Network::HttpSetResolveTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	}

	if (base != nullptr)
	{
		base->resolve_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetResolveRetry(Id id, int32_t retry)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	}

	if (base != nullptr)
	{
		base->resolve_retry = retry;
		return true;
	}

	return false;
}

bool Network::HttpSetConnectTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->connect_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetSendTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->send_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetRecvTimeOut(Id id, uint32_t usec)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->recv_timeout = usec;
		return true;
	}

	return false;
}

bool Network::HttpSetAutoRedirect(Id id, int enable)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->auto_redirect = (enable != 0);
		return true;
	}

	return false;
}

bool Network::HttpSetAuthEnabled(Id id, int enable)
{
	Core::LockGuard lock(m_mutex);

	HttpBase* base = nullptr;

	if (HttpValidTemplate(id))
	{
		base = &m_templates[id.GetId()];
	} else if (HttpValidConnection(id))
	{
		base = &m_connections[id.GetId()];
	} else if (HttpValidRequest(id))
	{
		base = &m_requests[id.GetId()];
	}

	if (base != nullptr)
	{
		base->auth_enabled = (enable != 0);
		return true;
	}

	return false;
}

namespace Net {

LIB_NAME("Net", "Net");

struct NetEtherAddr
{
	uint8_t data[6] = {0};
};

int KYTY_SYSV_ABI NetInit()
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NetTerm()
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NetPoolCreate(const char* name, int size, int flags)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t name = %s\n", name);
	KYTY_LOG_DEBUG("\t size = %d\n", size);
	KYTY_LOG_DEBUG("\t flags = %d\n", flags);

	EXIT_IF(g_net == nullptr);

	if (flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (size == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	int id = g_net->PoolCreate(name, size);

	if (id < 0)
	{
		return NET_ERROR_ENFILE;
	}

	return id;
}

int KYTY_SYSV_ABI NetPoolDestroy(int memid)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->PoolDestroy(memid))
	{
		return NET_ERROR_EBADF;
	}

	return OK;
}

int KYTY_SYSV_ABI NetInetPton(int af, const char* src, void* dst)
{
	PRINT_NAME();

	if (af != 2) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (src == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (dst == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (strcmp(src, "127.0.0.1") != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t src = %.16s\n", src);

	*static_cast<uint32_t*>(dst) = 0x7f000001;

	return OK;
}

int KYTY_SYSV_ABI NetEtherNtostr(const NetEtherAddr* n, char* str, size_t len)
{
	PRINT_NAME();

	NetEtherAddr zero {};

	if (len != 18) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (n == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (str == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (memcmp(n->data, zero.data, sizeof(zero.data)) != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	strcpy(str, "00:00:00:00:00:00"); // NOLINT

	return OK;
}

int KYTY_SYSV_ABI NetGetMacAddress(NetEtherAddr* addr, int flags)
{
	PRINT_NAME();

	if (addr == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (flags != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	memset(addr->data, 0, sizeof(addr->data));

	return OK;
}

struct SocketState
{
	std::mutex              mutex;
	std::condition_variable operations_done;
	bool                    closing    = false;
	uint32_t                operations = 0;
	bool                    datagram   = false;
	bool                    bound      = false;
	uint16_t                port       = 0;
	uint8_t                 addr[4] {};
#if KYTY_NET_HOST_POSIX
	int host_fd = -1;
#endif
};

using SocketStatePtr = std::shared_ptr<SocketState>;

static std::atomic<int> g_next_socket_id {1};
static std::mutex g_sockets_mutex;
static std::unordered_map<int, SocketStatePtr> g_sockets;

constexpr uint64_t kMaxIpv4UdpPayload  = 65'507; // IPv4 payload: 65,535 - 20-byte IP - 8-byte UDP header.
constexpr size_t   kSocketIoChunkSize  = 64u * 1024u;
constexpr int      kMaxEpollEvents     = 1024;
constexpr int      kMaxSocketInfoSize  = 64 * 1024;

#if KYTY_NET_HOST_EPOLL
static void PurgeSocketFromEpolls(int socket_id, int host_socket_fd);
#endif

static SocketStatePtr FindSocketState(int id)
{
	std::lock_guard lock(g_sockets_mutex);
	const auto      it = g_sockets.find(id);
	return it == g_sockets.end() ? nullptr : it->second;
}

#if KYTY_NET_HOST_POSIX
static bool BeginSocketOperation(const SocketStatePtr& state, int* host_fd)
{
	if (state == nullptr || host_fd == nullptr)
	{
		return false;
	}

	std::lock_guard lock(state->mutex);
	if (state->closing || state->host_fd < 0)
	{
		return false;
	}
	state->operations++;
	*host_fd = state->host_fd;
	return true;
}

static void EndSocketOperation(const SocketStatePtr& state)
{
	std::lock_guard lock(state->mutex);
	EXIT_IF(state->operations == 0);
	state->operations--;
	if (state->operations == 0)
	{
		state->operations_done.notify_all();
	}
}
#endif

static bool SocketIsDatagram(const SocketStatePtr& state)
{
	if (state == nullptr)
	{
		return false;
	}
	std::lock_guard lock(state->mutex);
	return state->datagram;
}

static bool IsGuestReadableRange(const void* pointer, uint64_t size)
{
	return pointer != nullptr && size != 0 && Core::VirtualMemory::IsRangeReadable(reinterpret_cast<uint64_t>(pointer), size);
}

// The VM's full-range validator is the established guest-address boundary.
// Outputs are validated before host I/O and again immediately before copying.
static bool IsGuestOutputRange(void* pointer, uint64_t size)
{
	return pointer != nullptr && size != 0 && Core::VirtualMemory::IsRangeWritable(reinterpret_cast<uint64_t>(pointer), size);
}

static bool GuestLengthToHostSize(uint64_t length, size_t* host_size)
{
	if (host_size == nullptr || length > std::numeric_limits<size_t>::max())
	{
		return false;
	}
#if KYTY_NET_HOST_POSIX
	if (length > static_cast<uint64_t>(std::numeric_limits<ssize_t>::max()))
	{
		return false;
	}
#endif
	*host_size = static_cast<size_t>(length);
	return true;
}

template <typename T>
class HostArray
{
public:
	[[nodiscard]] bool Allocate(size_t size)
	{
		if (size == 0)
		{
			m_data.reset();
			m_size = 0;
			return true;
		}
		if (size > std::numeric_limits<size_t>::max() / sizeof(T))
		{
			return false;
		}

		auto data = std::unique_ptr<T[]>(new (std::nothrow) T[size]);
		if (data == nullptr)
		{
			return false;
		}
		m_data = std::move(data);
		m_size = size;
		return true;
	}

	[[nodiscard]] T*       Data() { return m_data.get(); }
	[[nodiscard]] const T* Data() const { return m_data.get(); }
	[[nodiscard]] size_t   Size() const { return m_size; }

private:
	std::unique_ptr<T[]> m_data;
	size_t               m_size = 0;
};

static int CopyGuestInput(const void* input, uint64_t length, HostArray<uint8_t>* copy)
{
	if (copy == nullptr)
	{
		return NET_ERROR_EINVAL;
	}
	(void)copy->Allocate(0);
	if (length == 0)
	{
		return OK;
	}

	size_t host_size = 0;
	if (!GuestLengthToHostSize(length, &host_size))
	{
		return NET_ERROR_EMSGSIZE;
	}
	if (!IsGuestReadableRange(input, length))
	{
		return NET_ERROR_EFAULT;
	}

	if (!copy->Allocate(host_size))
	{
		return NET_ERROR_ENOMEM;
	}
	return Core::VirtualMemory::CopyFromGuest(copy->Data(), reinterpret_cast<uint64_t>(input), host_size) ? OK : NET_ERROR_EFAULT;
}

static int PrepareGuestOutput(void* output, uint64_t length, HostArray<uint8_t>* copy)
{
	if (copy == nullptr)
	{
		return NET_ERROR_EINVAL;
	}
	(void)copy->Allocate(0);
	if (length == 0)
	{
		return OK;
	}

	size_t host_size = 0;
	if (!GuestLengthToHostSize(length, &host_size))
	{
		return NET_ERROR_EMSGSIZE;
	}
	if (!IsGuestOutputRange(output, length))
	{
		return NET_ERROR_EFAULT;
	}

	if (!copy->Allocate(host_size))
	{
		return NET_ERROR_ENOMEM;
	}
	return OK;
}

static int CopyGuestOutput(void* output, const uint8_t* source, size_t length)
{
	if (length == 0)
	{
		return OK;
	}
	if (source == nullptr || !IsGuestOutputRange(output, length))
	{
		return NET_ERROR_EFAULT;
	}
	return Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(output), source, length) ? OK : NET_ERROR_EFAULT;
}

struct GuestSockaddrIn
{
	uint16_t port = 0;
	uint8_t  addr[4] {};
};

static bool ReadGuestSockaddrIn(const void* addr, uint32_t len, GuestSockaddrIn* out)
{
	constexpr uint32_t min_sockaddr_size = 8;
	if (addr == nullptr || len < min_sockaddr_size || out == nullptr || !IsGuestReadableRange(addr, min_sockaddr_size))
	{
		return false;
	}

	uint8_t bytes[min_sockaddr_size] {};
	if (!Core::VirtualMemory::CopyFromGuest(bytes, reinterpret_cast<uint64_t>(addr), sizeof(bytes)))
	{
		return false;
	}
	if (bytes[1] != 2)
	{
		return false;
	}
	out->port = static_cast<uint16_t>((static_cast<uint16_t>(bytes[2]) << 8u) | bytes[3]);
	std::memcpy(out->addr, bytes + 4, sizeof(out->addr));
	return true;
}

static bool WriteGuestSockaddrIn(void* addr, int* len, int max_len, uint16_t port, const uint8_t ip[4])
{
	constexpr int min_sockaddr_size = 8;
	if (addr == nullptr || len == nullptr || max_len < min_sockaddr_size || ip == nullptr ||
	    !IsGuestOutputRange(addr, min_sockaddr_size) || !IsGuestOutputRange(len, sizeof(*len)))
	{
		return false;
	}
	uint8_t out[min_sockaddr_size] {};
	out[0] = 16;
	out[1] = 2;
	out[2] = static_cast<uint8_t>((port >> 8u) & 0xffu);
	out[3] = static_cast<uint8_t>(port & 0xffu);
	std::memcpy(out + 4, ip, 4);
	const int write_len = max_len < 16 ? max_len : 16;
	return Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(addr), out, sizeof(out)) &&
	       Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(len), &write_len, sizeof(write_len));
}

#if KYTY_NET_HOST_POSIX
static int HostErrnoToNet(int host_errno)
{
	switch (host_errno)
	{
		case EBADF: return NET_ERROR_EBADF;
		case EINTR: return NET_ERROR_EINTR;
		case EIO: return NET_ERROR_EIO;
		case EFAULT: return NET_ERROR_EFAULT;
		case EINVAL: return NET_ERROR_EINVAL;
		case EACCES: return NET_ERROR_EACCES;
		case EAGAIN: return NET_ERROR_EAGAIN;
		case EALREADY: return NET_ERROR_EALREADY;
		case EINPROGRESS: return NET_ERROR_EINPROGRESS;
		case ENOTSOCK: return NET_ERROR_ENOTSOCK;
		case EDESTADDRREQ: return NET_ERROR_EDESTADDRREQ;
		case EMSGSIZE: return NET_ERROR_EMSGSIZE;
		case EPROTOTYPE: return NET_ERROR_EPROTOTYPE;
		case ENOPROTOOPT: return NET_ERROR_ENOPROTOOPT;
		case EPROTONOSUPPORT: return NET_ERROR_EPROTONOSUPPORT;
		case EOPNOTSUPP: return NET_ERROR_EOPNOTSUPP;
		case EAFNOSUPPORT: return NET_ERROR_EAFNOSUPPORT;
		case EADDRINUSE: return NET_ERROR_EADDRINUSE;
		case EADDRNOTAVAIL: return NET_ERROR_EADDRNOTAVAIL;
		case ENETDOWN: return NET_ERROR_ENETDOWN;
		case ENETUNREACH: return NET_ERROR_ENETUNREACH;
		case ECONNABORTED: return NET_ERROR_ECONNABORTED;
		case ECONNRESET: return NET_ERROR_ECONNRESET;
		case ENOBUFS: return NET_ERROR_ENOBUFS;
		case EISCONN: return NET_ERROR_EISCONN;
		case ENOTCONN: return NET_ERROR_ENOTCONN;
		case ETIMEDOUT: return NET_ERROR_ETIMEDOUT;
		case ECONNREFUSED: return NET_ERROR_ECONNREFUSED;
		case EPIPE: return NET_ERROR_EPIPE;
		default: return NET_ERROR_EINVAL;
	}
}

static bool TranslateGuestSocketParams(int family, int type, int protocol, int* host_family, int* host_type,
                                       int* host_protocol)
{
	if (host_family == nullptr || host_type == nullptr || host_protocol == nullptr)
	{
		return false;
	}

	switch (family)
	{
		case 2: *host_family = AF_INET; break;
		case 28: *host_family = AF_INET6; break;
		default: return false;
	}

	switch (type & 0xf)
	{
		case 1: *host_type = SOCK_STREAM; break;
		case 2: *host_type = SOCK_DGRAM; break;
		default: return false;
	}

	if (protocol == 0)
	{
		*host_protocol = (*host_type == SOCK_STREAM ? IPPROTO_TCP : IPPROTO_UDP);
	}
	else if (protocol == 6)
	{
		*host_protocol = IPPROTO_TCP;
	}
	else if (protocol == 17)
	{
		*host_protocol = IPPROTO_UDP;
	}
	else
	{
		return false;
	}

	return true;
}

static bool GuestToHostSockaddrIn(const GuestSockaddrIn& guest, sockaddr_in* out)
{
	if (out == nullptr)
	{
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port   = htons(guest.port);
	std::memcpy(&out->sin_addr, guest.addr, sizeof(guest.addr));
	return true;
}
#endif

int KYTY_SYSV_ABI NetSocket(const char* name, int family, int type, int protocol)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t name = %s\n", name != nullptr ? name : "(null)");
	KYTY_LOG_DEBUG("\t family = %d type = %d protocol = %d\n", family, type, protocol);

	auto state = std::make_shared<SocketState>();
#if KYTY_NET_HOST_POSIX
	int host_family   = 0;
	int host_type     = 0;
	int host_protocol = 0;
	if (!TranslateGuestSocketParams(family, type, protocol, &host_family, &host_type, &host_protocol))
	{
		return NET_ERROR_EINVAL;
	}
	state->datagram = (host_type == SOCK_DGRAM);
	state->host_fd = ::socket(host_family, host_type, host_protocol);
	if (state->host_fd < 0)
	{
		return HostErrnoToNet(errno);
	}
#else
	(void)family;
	(void)type;
	(void)protocol;
#endif

	const int id = g_next_socket_id.fetch_add(1, std::memory_order_relaxed);
	{
		std::lock_guard lock(g_sockets_mutex);
		g_sockets[id] = std::move(state);
	}
	return id;
}

int KYTY_SYSV_ABI NetSocketClose(int id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	SocketStatePtr state;
	{
		std::lock_guard lock(g_sockets_mutex);
		const auto      it = g_sockets.find(id);
		if (it == g_sockets.end())
		{
			return NET_ERROR_EBADF;
		}
		state = it->second;
		g_sockets.erase(it);
	}

#if KYTY_NET_HOST_POSIX
	int host_fd = -1;
	{
		std::lock_guard lock(state->mutex);
		state->closing = true;
		host_fd        = state->host_fd;
	}

	if (host_fd >= 0)
	{
		// shutdown wakes blocking accept/recv while the operation lease keeps
		// the descriptor from being closed and reused underneath that syscall.
		(void)::shutdown(host_fd, SHUT_RDWR);
	}
	{
		std::unique_lock lock(state->mutex);
		state->operations_done.wait(lock, [&state]() { return state->operations == 0; });
	}
#if KYTY_NET_HOST_EPOLL
	if (host_fd >= 0)
	{
		PurgeSocketFromEpolls(id, host_fd);
	}
#endif
	{
		std::lock_guard lock(state->mutex);
		state->host_fd = -1;
	}
	if (host_fd >= 0)
	{
		(void)::close(host_fd);
	}
#else
	std::lock_guard lock(state->mutex);
	state->closing = true;
#endif

	return OK;
}

int KYTY_SYSV_ABI NetBind(int id, const void* addr, int len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	GuestSockaddrIn guest_addr {};
	if (len < 0 || !ReadGuestSockaddrIn(addr, static_cast<uint32_t>(len), &guest_addr))
	{
		return NET_ERROR_EINVAL;
	}

#if KYTY_NET_HOST_POSIX
	sockaddr_in host_addr {};
	if (!GuestToHostSockaddrIn(guest_addr, &host_addr))
	{
		return NET_ERROR_EINVAL;
	}
	int host_fd = -1;
	if (!BeginSocketOperation(state, &host_fd))
	{
		return NET_ERROR_EBADF;
	}
	const int bind_result = ::bind(host_fd, reinterpret_cast<sockaddr*>(&host_addr), sizeof(host_addr));
	const int bind_errno  = errno;
	EndSocketOperation(state);
	if (bind_result != 0)
	{
		return HostErrnoToNet(bind_errno);
	}
	#endif

	std::lock_guard lock(state->mutex);
	if (state->closing)
	{
		return NET_ERROR_EBADF;
	}
	state->bound = true;
	state->port  = guest_addr.port;
	std::memcpy(state->addr, guest_addr.addr, sizeof(guest_addr.addr));
	return OK;
}

int KYTY_SYSV_ABI NetConnect(int id, const void* addr, int len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	GuestSockaddrIn guest_addr {};
	if (len < 0 || !ReadGuestSockaddrIn(addr, static_cast<uint32_t>(len), &guest_addr))
	{
		return NET_ERROR_EINVAL;
	}
#if KYTY_NET_HOST_POSIX
	sockaddr_in host_addr {};
	if (!GuestToHostSockaddrIn(guest_addr, &host_addr))
	{
		return NET_ERROR_EINVAL;
	}
	int host_fd = -1;
	if (!BeginSocketOperation(state, &host_fd))
	{
		return NET_ERROR_EBADF;
	}
	const int connect_result = ::connect(host_fd, reinterpret_cast<sockaddr*>(&host_addr), sizeof(host_addr));
	const int connect_errno  = errno;
	EndSocketOperation(state);
	if (connect_result != 0)
	{
		return HostErrnoToNet(connect_errno);
	}
#endif

	std::lock_guard lock(state->mutex);
	if (state->closing)
	{
		return NET_ERROR_EBADF;
	}
	state->bound = true;
	state->port  = guest_addr.port;
	std::memcpy(state->addr, guest_addr.addr, sizeof(guest_addr.addr));
	return OK;
}

int KYTY_SYSV_ABI NetListen(int id, int backlog)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d backlog = %d\n", id, backlog);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
#if KYTY_NET_HOST_POSIX
	int host_fd = -1;
	if (!BeginSocketOperation(state, &host_fd))
	{
		return NET_ERROR_EBADF;
	}
	const int listen_result = ::listen(host_fd, backlog);
	const int listen_errno  = errno;
	EndSocketOperation(state);
	if (listen_result != 0)
	{
		return HostErrnoToNet(listen_errno);
	}
#else
	(void)backlog;
#endif
	return OK;
}

int KYTY_SYSV_ABI NetAccept(int id, void* addr, int* len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	bool write_peer = false;
	int  max_len    = 0;
	if (addr != nullptr || len != nullptr)
	{
		if (addr == nullptr || len == nullptr)
		{
			return NET_ERROR_EINVAL;
		}
		if (!IsGuestOutputRange(addr, 8) || !IsGuestOutputRange(len, sizeof(*len)))
		{
			return NET_ERROR_EFAULT;
		}
		if (!Core::VirtualMemory::CopyFromGuest(&max_len, reinterpret_cast<uint64_t>(len), sizeof(max_len)))
		{
			return NET_ERROR_EFAULT;
		}
		if (max_len < 8)
		{
			return NET_ERROR_EINVAL;
		}
		write_peer = true;
	}

	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}

	auto accepted_state = std::make_shared<SocketState>();
#if KYTY_NET_HOST_POSIX
	sockaddr_in peer {};
	socklen_t   peer_len = sizeof(peer);
	int host_fd = -1;
	if (!BeginSocketOperation(state, &host_fd))
	{
		return NET_ERROR_EBADF;
	}
	accepted_state->host_fd = ::accept(host_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
	const int accept_errno  = errno;
	EndSocketOperation(state);
	if (accepted_state->host_fd < 0)
	{
		return HostErrnoToNet(accept_errno);
	}
	accepted_state->bound = true;
	accepted_state->port  = ntohs(peer.sin_port);
	std::memcpy(accepted_state->addr, &peer.sin_addr, sizeof(accepted_state->addr));
	if (write_peer && !WriteGuestSockaddrIn(addr, len, max_len, accepted_state->port, accepted_state->addr))
	{
		(void)::close(accepted_state->host_fd);
		accepted_state->host_fd = -1;
		return NET_ERROR_EFAULT;
	}
#else
	(void)write_peer;
	(void)max_len;
#endif

	const int accepted = g_next_socket_id.fetch_add(1, std::memory_order_relaxed);
	{
		std::lock_guard lock(g_sockets_mutex);
		g_sockets[accepted] = std::move(accepted_state);
	}
	return accepted;
}

int64_t KYTY_SYSV_ABI NetSend(int id, const void* buf, uint64_t len, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d len = %" PRIu64 " flags = %d\n", id, len, flags);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	if (len == 0)
	{
		return 0;
	}
	if (len > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
	{
		return NET_ERROR_EMSGSIZE;
	}
	const bool datagram = SocketIsDatagram(state);
	if (datagram && len > kMaxIpv4UdpPayload)
	{
		return NET_ERROR_EMSGSIZE;
	}
	if (!IsGuestReadableRange(buf, len))
	{
		return NET_ERROR_EFAULT;
	}
#if KYTY_NET_HOST_POSIX
	const uint64_t max_chunk = datagram ? len : std::min<uint64_t>(len, kSocketIoChunkSize);
	uint64_t       total     = 0;
	while (total < len)
	{
		const uint64_t chunk_length = std::min<uint64_t>(len - total, max_chunk);
		const auto* chunk = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(buf) + total);
		HostArray<uint8_t> payload;
		const int copy_result = CopyGuestInput(chunk, chunk_length, &payload);
		if (copy_result != OK)
		{
			return total != 0 ? static_cast<int64_t>(total) : copy_result;
		}

		int host_fd = -1;
		if (!BeginSocketOperation(state, &host_fd))
		{
			return total != 0 ? static_cast<int64_t>(total) : NET_ERROR_EBADF;
		}
		const auto sent = ::send(host_fd, payload.Data(), payload.Size(), flags);
		const int send_errno = errno;
		EndSocketOperation(state);
		if (sent < 0)
		{
			return total != 0 ? static_cast<int64_t>(total) : HostErrnoToNet(send_errno);
		}
		total += static_cast<uint64_t>(sent);
		if (static_cast<size_t>(sent) != payload.Size())
		{
			return static_cast<int64_t>(total);
		}
	}
	return static_cast<int64_t>(total);
#else
	(void)datagram;
	(void)flags;
	return NET_ERROR_EOPNOTSUPP;
#endif
}

int64_t KYTY_SYSV_ABI NetSendto(int id, const void* buf, uint64_t len, int flags, const void* addr, uint32_t addr_len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d len = %" PRIu64 " flags = %d addr_len = %u\n", id, len, flags, addr_len);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	GuestSockaddrIn guest_addr {};
	if (!ReadGuestSockaddrIn(addr, addr_len, &guest_addr))
	{
		return NET_ERROR_EINVAL;
	}
	if (len > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
	{
		return NET_ERROR_EMSGSIZE;
	}
	const bool datagram = SocketIsDatagram(state);
	if (datagram && len > kMaxIpv4UdpPayload)
	{
		return NET_ERROR_EMSGSIZE;
	}
	if (len != 0 && !IsGuestReadableRange(buf, len))
	{
		return NET_ERROR_EFAULT;
	}
#if KYTY_NET_HOST_POSIX
	sockaddr_in host_addr {};
	if (!GuestToHostSockaddrIn(guest_addr, &host_addr))
	{
		return NET_ERROR_EINVAL;
	}
	if (len == 0)
	{
		int host_fd = -1;
		if (!BeginSocketOperation(state, &host_fd))
		{
			return NET_ERROR_EBADF;
		}
		const auto sent = ::sendto(host_fd, nullptr, 0, flags, reinterpret_cast<const sockaddr*>(&host_addr), sizeof(host_addr));
		const int send_errno = errno;
		EndSocketOperation(state);
		return sent < 0 ? HostErrnoToNet(send_errno) : sent;
	}
	const uint64_t max_chunk = datagram ? len : std::min<uint64_t>(len, kSocketIoChunkSize);
	uint64_t       total     = 0;
	while (total < len)
	{
		const uint64_t chunk_length = std::min<uint64_t>(len - total, max_chunk);
		const auto* chunk = reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(buf) + total);
		HostArray<uint8_t> payload;
		const int copy_result = CopyGuestInput(chunk, chunk_length, &payload);
		if (copy_result != OK)
		{
			return total != 0 ? static_cast<int64_t>(total) : copy_result;
		}

		int host_fd = -1;
		if (!BeginSocketOperation(state, &host_fd))
		{
			return total != 0 ? static_cast<int64_t>(total) : NET_ERROR_EBADF;
		}
		const auto sent = ::sendto(host_fd, payload.Data(), payload.Size(), flags,
		                           reinterpret_cast<const sockaddr*>(&host_addr), sizeof(host_addr));
		const int send_errno = errno;
		EndSocketOperation(state);
		if (sent < 0)
		{
			return total != 0 ? static_cast<int64_t>(total) : HostErrnoToNet(send_errno);
		}
		total += static_cast<uint64_t>(sent);
		if (static_cast<size_t>(sent) != payload.Size())
		{
			return static_cast<int64_t>(total);
		}
	}
	return static_cast<int64_t>(total);
#else
	(void)datagram;
	(void)guest_addr;
	return NET_ERROR_EOPNOTSUPP;
#endif
}

int64_t KYTY_SYSV_ABI NetRecv(int id, void* buf, uint64_t len, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d len = %" PRIu64 " flags = %d\n", id, len, flags);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	if (len == 0)
	{
		return 0;
	}
	if (len > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
	{
		return NET_ERROR_EMSGSIZE;
	}
	const bool datagram = SocketIsDatagram(state);
	if (datagram && len > kMaxIpv4UdpPayload)
	{
		return NET_ERROR_EMSGSIZE;
	}
	if (!IsGuestOutputRange(buf, len))
	{
		return NET_ERROR_EFAULT;
	}
	const uint64_t chunk_length = datagram ? len : std::min<uint64_t>(len, kSocketIoChunkSize);
	HostArray<uint8_t> payload;
	const int prepare_result = PrepareGuestOutput(buf, chunk_length, &payload);
	if (prepare_result != OK)
	{
		return prepare_result;
	}
#if KYTY_NET_HOST_POSIX
	int host_fd = -1;
	if (!BeginSocketOperation(state, &host_fd))
	{
		return NET_ERROR_EBADF;
	}
	const auto received = ::recv(host_fd, payload.Data(), payload.Size(), flags);
	const int recv_errno = errno;
	EndSocketOperation(state);
	if (received < 0)
	{
		return HostErrnoToNet(recv_errno);
	}
	const int output_result = CopyGuestOutput(buf, payload.Data(), static_cast<size_t>(received));
	if (output_result != OK)
	{
		return output_result;
	}
	return received;
#else
	(void)datagram;
	(void)flags;
	return NET_ERROR_EOPNOTSUPP;
#endif
}

int KYTY_SYSV_ABI NetGetsockname(int id, void* addr, int* len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	if (addr == nullptr || len == nullptr)
	{
		return NET_ERROR_EINVAL;
	}
	if (!IsGuestOutputRange(addr, 8) || !IsGuestOutputRange(len, sizeof(*len)))
	{
		return NET_ERROR_EFAULT;
	}
	int max_len = 0;
	if (!Core::VirtualMemory::CopyFromGuest(&max_len, reinterpret_cast<uint64_t>(len), sizeof(max_len)))
	{
		return NET_ERROR_EFAULT;
	}
	if (max_len < 8)
	{
		return NET_ERROR_EINVAL;
	}

	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	uint16_t stored_port = 0;
	uint8_t  stored_addr[4] {};
	{
		std::lock_guard lock(state->mutex);
		if (state->closing)
		{
			return NET_ERROR_EBADF;
		}
		if (!state->bound)
		{
			return NET_ERROR_EINVAL;
		}
		stored_port = state->port;
		std::memcpy(stored_addr, state->addr, sizeof(stored_addr));
	}
#if KYTY_NET_HOST_POSIX
	sockaddr_in host_addr {};
	socklen_t   host_len = sizeof(host_addr);
	int host_fd = -1;
	if (!BeginSocketOperation(state, &host_fd))
	{
		return NET_ERROR_EBADF;
	}
	const int getsockname_result = ::getsockname(host_fd, reinterpret_cast<sockaddr*>(&host_addr), &host_len);
	EndSocketOperation(state);
	if (getsockname_result == 0 &&
	    host_addr.sin_family == AF_INET)
	{
		uint8_t ip[4] {};
		std::memcpy(ip, &host_addr.sin_addr, 4);
		return WriteGuestSockaddrIn(addr, len, max_len, ntohs(host_addr.sin_port), ip) ? OK : NET_ERROR_EFAULT;
	}
#endif
	return WriteGuestSockaddrIn(addr, len, max_len, stored_port, stored_addr) ? OK : NET_ERROR_EFAULT;
}

int KYTY_SYSV_ABI NetGetsockopt(int id, int level, int option, void* value, int* value_len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d level = %d option = %d\n", id, level, option);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	if (value == nullptr || value_len == nullptr || level != 0xffff)
	{
		return NET_ERROR_EINVAL;
	}
	if (!IsGuestOutputRange(value, sizeof(int)) || !IsGuestOutputRange(value_len, sizeof(*value_len)))
	{
		return NET_ERROR_EFAULT;
	}
	int guest_value_len = 0;
	if (!Core::VirtualMemory::CopyFromGuest(&guest_value_len, reinterpret_cast<uint64_t>(value_len), sizeof(guest_value_len)))
	{
		return NET_ERROR_EFAULT;
	}
	if (guest_value_len < static_cast<int>(sizeof(int)))
	{
		return NET_ERROR_EINVAL;
	}
	{
		std::lock_guard lock(state->mutex);
		if (state->closing)
		{
			return NET_ERROR_EBADF;
		}
	}
	int stored = 0;
	switch (option)
	{
		case 0x1200: stored = 0; break; // ORBIS_NET_SO_NBIO: blocking
		case 0x0004: stored = 0; break; // SO_REUSEADDR
		case 0x1007: stored = 0; break; // SO_ERROR
		default: return NET_ERROR_EINVAL;
	}
	if (!IsGuestOutputRange(value, sizeof(stored)) || !IsGuestOutputRange(value_len, sizeof(*value_len)))
	{
		return NET_ERROR_EFAULT;
	}
	const int stored_len = static_cast<int>(sizeof(stored));
	return Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(value), &stored, sizeof(stored)) &&
	               Core::VirtualMemory::CopyToGuest(reinterpret_cast<uint64_t>(value_len), &stored_len, sizeof(stored_len))
	           ? OK
	           : NET_ERROR_EFAULT;
}

int KYTY_SYSV_ABI NetSelect(int nfds, void* readfds, void* writefds, void* exceptfds, void* timeout)
{
	// select(0, nullptr, nullptr, nullptr, &timeout) is the POSIX sleep form.
	// Returning immediately turns a timed wait into a hot loop in guest middleware.
	if (nfds < 0)
	{
		return NET_ERROR_EINVAL;
	}
	if (nfds != 0 || readfds != nullptr || writefds != nullptr || exceptfds != nullptr)
	{
		return NET_ERROR_EOPNOTSUPP;
	}

	if (timeout == nullptr)
	{
		// A null timeout blocks indefinitely. Use bounded sleeps so shutdown and signal
		// delivery remain responsive without pretending a descriptor query succeeded.
		for (;;)
		{
			Core::Thread::SleepMicro(1'000'000);
		}
	}

	// PS5 timeval uses 64-bit time_t and suseconds_t on the x86-64 ABI.
	const auto* tv             = static_cast<const int64_t*>(timeout);
	const int64_t seconds      = tv[0];
	const int64_t microseconds = tv[1];
	if (seconds < 0 || microseconds < 0 || microseconds >= 1'000'000)
	{
		return NET_ERROR_EINVAL;
	}

	const uint64_t seconds_u      = static_cast<uint64_t>(seconds);
	const uint64_t microseconds_u = static_cast<uint64_t>(microseconds);
	if (seconds_u > (UINT64_MAX - microseconds_u) / 1'000'000ull)
	{
		return NET_ERROR_EINVAL;
	}
	uint64_t remaining = seconds_u * 1'000'000ull + microseconds_u;
	while (remaining != 0)
	{
		const uint32_t slice = static_cast<uint32_t>(std::min<uint64_t>(remaining, 1'000'000ull));
		Core::Thread::SleepMicro(slice);
		remaining -= slice;
	}
	return 0;
}

const char* KYTY_SYSV_ABI NetInetNtop(int af, const void* src, char* dst, int size)
{
	PRINT_NAME();
	if (src == nullptr || dst == nullptr || size <= 0)
	{
		return nullptr;
	}
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return ::inet_ntop(af, src, dst, static_cast<socklen_t>(size));
#else
	(void)af;
	(void)size;
	return nullptr;
#endif
}

int KYTY_SYSV_ABI NetSetsockopt(int id, int level, int option, const void* /*value*/, int /*value_len*/)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d level = %d option = %d\n", id, level, option);
	const auto state = FindSocketState(id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	std::lock_guard lock(state->mutex);
	return state->closing ? NET_ERROR_EBADF : OK;
}

uint32_t KYTY_SYSV_ABI NetHtonl(uint32_t hostlong)
{
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return htonl(hostlong);
#else
	return hostlong;
#endif
}

uint16_t KYTY_SYSV_ABI NetHtons(uint16_t hostshort)
{
#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	return htons(hostshort);
#else
	return hostshort;
#endif
}

uint32_t KYTY_SYSV_ABI NetNtohl(uint32_t netlong)
{
	return NetHtonl(netlong);
}

uint16_t KYTY_SYSV_ABI NetNtohs(uint16_t netshort)
{
	return NetHtons(netshort);
}

int KYTY_SYSV_ABI NetResolverCreate(const char* name, int memid, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t name = %s memid = %d flags = %d\n", name != nullptr ? name : "(null)", memid, flags);
	static std::atomic<int> next_resolver {1};
	return next_resolver.fetch_add(1, std::memory_order_relaxed);
}

int KYTY_SYSV_ABI NetResolverDestroy(int rid)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t rid = %d\n", rid);
	return OK;
}

int KYTY_SYSV_ABI NetResolverGetError(int rid, int* result)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t rid = %d\n", rid);
	if (result == nullptr)
	{
		return NET_ERROR_EINVAL;
	}
	*result = 0;
	return OK;
}

// --- Net epoll / resolver / socket-info -------------------------------------

namespace {

// Guest-visible epoll event: events + reserved words followed by the identity
// and the caller-supplied data slot (24 bytes total on Gen5).
struct NetEpollEventGuest
{
	uint32_t events;
	uint32_t reserved;
	uint64_t ident;
	uint64_t data;
};

static_assert(sizeof(NetEpollEventGuest) == 24);

struct NetEpollRegistration
{
	uint64_t ident;
	uint64_t data;
};

// Each epoll instance is independently reference-counted through operation
// leases. Destruction removes it from the registry first, wakes waiters, then
// closes descriptors only after all outstanding leases finish.
struct NetEpollState
{
	std::mutex                                      mutex;
	std::condition_variable                         operations_done;
	bool                                            closing    = false;
	uint32_t                                        operations = 0;
	uint64_t                                        generation = 0;
	std::unordered_map<int, NetEpollRegistration>  registrations;
#if KYTY_NET_HOST_EPOLL
	int host_fd = -1;
	int wake_fd = -1;
#endif
};

using NetEpollStatePtr = std::shared_ptr<NetEpollState>;

constexpr int kEpollIdBase = 0x1000000;
constexpr int kEpollWakeMarker = -1;

std::atomic<int>      g_next_epoll_id {kEpollIdBase};
std::atomic<uint64_t> g_next_epoll_generation {1};
std::mutex            g_epolls_mutex;
std::unordered_map<int, NetEpollStatePtr> g_epolls;

static NetEpollStatePtr FindEpollState(int epoll_id)
{
	std::lock_guard lock(g_epolls_mutex);
	const auto      it = g_epolls.find(epoll_id);
	return it == g_epolls.end() ? nullptr : it->second;
}

#if KYTY_NET_HOST_EPOLL
static bool BeginEpollOperation(const NetEpollStatePtr& state, int* host_fd, int* wake_fd, uint64_t* generation)
{
	if (state == nullptr || host_fd == nullptr || wake_fd == nullptr || generation == nullptr)
	{
		return false;
	}

	std::lock_guard lock(state->mutex);
	if (state->closing || state->host_fd < 0 || state->wake_fd < 0)
	{
		return false;
	}
	state->operations++;
	*host_fd    = state->host_fd;
	*wake_fd    = state->wake_fd;
	*generation = state->generation;
	return true;
}

static void EndEpollOperation(const NetEpollStatePtr& state)
{
	std::lock_guard lock(state->mutex);
	EXIT_IF(state->operations == 0);
	state->operations--;
	if (state->operations == 0)
	{
		state->operations_done.notify_all();
	}
}

static bool IsEpollCurrent(const NetEpollStatePtr& state, uint64_t generation)
{
	std::lock_guard lock(state->mutex);
	return !state->closing && state->generation == generation;
}

static void WakeEpoll(int wake_fd)
{
	const uint64_t wake_value = 1;
	(void)::write(wake_fd, &wake_value, sizeof(wake_value));
}

static void DrainEpollWake(int wake_fd)
{
	uint64_t wake_value = 0;
	while (::read(wake_fd, &wake_value, sizeof(wake_value)) == static_cast<ssize_t>(sizeof(wake_value)))
	{
	}
}
#endif

} // namespace

#if KYTY_NET_HOST_EPOLL
static void PurgeSocketFromEpolls(int socket_id, int host_socket_fd)
{
	std::vector<NetEpollStatePtr> epolls;
	{
		std::lock_guard lock(g_epolls_mutex);
		epolls.reserve(g_epolls.size());
		for (const auto& [unused_id, state]: g_epolls)
		{
			(void)unused_id;
			epolls.push_back(state);
		}
	}

	for (const auto& state: epolls)
	{
		int host_epoll_fd = -1;
		int wake_fd       = -1;
		uint64_t generation = 0;
		if (!BeginEpollOperation(state, &host_epoll_fd, &wake_fd, &generation))
		{
			continue;
		}
		(void)wake_fd;
		(void)::epoll_ctl(host_epoll_fd, EPOLL_CTL_DEL, host_socket_fd, nullptr);
		{
			std::lock_guard lock(state->mutex);
			if (!state->closing && state->generation == generation)
			{
				state->registrations.erase(socket_id);
			}
		}
		EndEpollOperation(state);
	}
}
#endif

int KYTY_SYSV_ABI NetEpollCreate(const char* name, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t name  = %s\n", name != nullptr ? name : "(null)");
	KYTY_LOG_DEBUG("\t flags = %d\n", flags);

	if (name == nullptr || flags != 0)
	{
		return NET_ERROR_EINVAL;
	}

#if KYTY_NET_HOST_EPOLL
	const int host_fd = ::epoll_create1(EPOLL_CLOEXEC);
	if (host_fd < 0)
	{
		return HostErrnoToNet(errno);
	}
	const int wake_fd = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (wake_fd < 0)
	{
		const int eventfd_errno = errno;
		(void)::close(host_fd);
		return HostErrnoToNet(eventfd_errno);
	}
	epoll_event wake_event {};
	wake_event.events  = EPOLLIN;
	wake_event.data.fd = kEpollWakeMarker;
	if (::epoll_ctl(host_fd, EPOLL_CTL_ADD, wake_fd, &wake_event) != 0)
	{
		const int control_errno = errno;
		(void)::close(wake_fd);
		(void)::close(host_fd);
		return HostErrnoToNet(control_errno);
	}

	auto state        = std::make_shared<NetEpollState>();
	state->host_fd    = host_fd;
	state->wake_fd    = wake_fd;
	state->generation = g_next_epoll_generation.fetch_add(1, std::memory_order_relaxed);
	const int id = g_next_epoll_id.fetch_add(1, std::memory_order_relaxed);
	{
		std::lock_guard lock(g_epolls_mutex);
		g_epolls[id] = std::move(state);
	}
	return id;
#else
	return NET_ERROR_ENOTSUP;
#endif
}

int KYTY_SYSV_ABI NetEpollDestroy(int epoll_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t epoll_id = %d\n", epoll_id);

#if KYTY_NET_HOST_EPOLL
	NetEpollStatePtr state;
	{
		std::lock_guard lock(g_epolls_mutex);
		const auto      it = g_epolls.find(epoll_id);
		if (it == g_epolls.end())
		{
			return NET_ERROR_EBADF;
		}
		state = it->second;
		g_epolls.erase(it);
	}

	int host_fd = -1;
	int wake_fd = -1;
	{
		std::lock_guard lock(state->mutex);
		state->closing = true;
		state->generation++;
		host_fd = state->host_fd;
		wake_fd = state->wake_fd;
	}
	if (wake_fd >= 0)
	{
		WakeEpoll(wake_fd);
	}
	{
		std::unique_lock lock(state->mutex);
		state->operations_done.wait(lock, [&state]() { return state->operations == 0; });
		state->host_fd = -1;
		state->wake_fd = -1;
		state->registrations.clear();
	}
	if (wake_fd >= 0)
	{
		(void)::close(wake_fd);
	}
	if (host_fd >= 0)
	{
		(void)::close(host_fd);
	}
	return OK;
#else
	(void)epoll_id;
	return NET_ERROR_ENOTSUP;
#endif
}

int KYTY_SYSV_ABI NetEpollControl(int epoll_id, int operation, int socket_id, const void* event)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t epoll_id  = %d\n", epoll_id);
	KYTY_LOG_DEBUG("\t operation = %d\n", operation);
	KYTY_LOG_DEBUG("\t socket    = %d\n", socket_id);
	KYTY_LOG_DEBUG("\t event     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(event));

	constexpr int NET_EPOLL_CTL_ADD = 1;
	constexpr int NET_EPOLL_CTL_MOD = 2;
	constexpr int NET_EPOLL_CTL_DEL = 3;

	if (operation < NET_EPOLL_CTL_ADD || operation > NET_EPOLL_CTL_DEL)
	{
		return NET_ERROR_EINVAL;
	}
	if ((operation == NET_EPOLL_CTL_ADD || operation == NET_EPOLL_CTL_MOD) && event == nullptr)
	{
		return NET_ERROR_EINVAL;
	}
	if (operation == NET_EPOLL_CTL_DEL && event != nullptr)
	{
		return NET_ERROR_EINVAL;
	}

#if KYTY_NET_HOST_EPOLL
	epoll_event host_event {};
	NetEpollRegistration registration {};
	int host_operation = 0;

	if (operation == NET_EPOLL_CTL_ADD || operation == NET_EPOLL_CTL_MOD)
	{
		if (!IsGuestReadableRange(event, sizeof(NetEpollEventGuest)))
		{
			return NET_ERROR_EFAULT;
		}
		NetEpollEventGuest guest_event {};
		if (!Core::VirtualMemory::CopyFromGuest(&guest_event, reinterpret_cast<uint64_t>(event), sizeof(guest_event)))
		{
			return NET_ERROR_EFAULT;
		}
		host_event.events       = guest_event.events;
		host_event.data.fd      = socket_id;
		registration = NetEpollRegistration {guest_event.ident, guest_event.data};
	}
	switch (operation)
	{
		case NET_EPOLL_CTL_ADD: host_operation = EPOLL_CTL_ADD; break;
		case NET_EPOLL_CTL_MOD: host_operation = EPOLL_CTL_MOD; break;
		case NET_EPOLL_CTL_DEL: host_operation = EPOLL_CTL_DEL; break;
		default: return NET_ERROR_EINVAL;
	}

	const auto socket_state = FindSocketState(socket_id);
	if (socket_state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	int host_socket_fd = -1;
	if (!BeginSocketOperation(socket_state, &host_socket_fd))
	{
		return NET_ERROR_ENOTSOCK;
	}

	const auto epoll_state = FindEpollState(epoll_id);
	if (epoll_state == nullptr)
	{
		EndSocketOperation(socket_state);
		return NET_ERROR_EBADF;
	}
	int host_epoll_fd = -1;
	int wake_fd = -1;
	uint64_t generation = 0;
	if (!BeginEpollOperation(epoll_state, &host_epoll_fd, &wake_fd, &generation))
	{
		EndSocketOperation(socket_state);
		return NET_ERROR_EBADF;
	}
	(void)wake_fd;
	const int control_result =
	    ::epoll_ctl(host_epoll_fd, host_operation, host_socket_fd, operation == NET_EPOLL_CTL_DEL ? nullptr : &host_event);
	const int control_errno = errno;
	int result = OK;
	if (control_result != 0)
	{
		result = HostErrnoToNet(control_errno);
	} else
	{
		std::lock_guard lock(epoll_state->mutex);
		if (epoll_state->closing || epoll_state->generation != generation)
		{
			result = NET_ERROR_EBADF;
		} else if (operation == NET_EPOLL_CTL_DEL)
		{
			epoll_state->registrations.erase(socket_id);
		} else
		{
			epoll_state->registrations[socket_id] = registration;
		}
	}
	EndEpollOperation(epoll_state);
	EndSocketOperation(socket_state);
	return result;
#else
	return NET_ERROR_ENOTSUP;
#endif
}

int KYTY_SYSV_ABI NetEpollWait(int epoll_id, void* events, int max_events, int timeout_ms)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t epoll_id   = %d\n", epoll_id);
	KYTY_LOG_DEBUG("\t events     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(events));
	KYTY_LOG_DEBUG("\t max_events = %d\n", max_events);
	KYTY_LOG_DEBUG("\t timeout_ms = %d\n", timeout_ms);

	if (events == nullptr)
	{
		return NET_ERROR_EFAULT;
	}
	if (max_events <= 0)
	{
		return NET_ERROR_EINVAL;
	}
	if (max_events > kMaxEpollEvents)
	{
		return NET_ERROR_EMSGSIZE;
	}

#if KYTY_NET_HOST_EPOLL
	if (static_cast<uint64_t>(max_events) > std::numeric_limits<size_t>::max() / sizeof(NetEpollEventGuest) ||
	    static_cast<uint64_t>(max_events) > std::numeric_limits<size_t>::max() / sizeof(epoll_event))
	{
		return NET_ERROR_EMSGSIZE;
	}
	const uint64_t output_size = static_cast<uint64_t>(max_events) * sizeof(NetEpollEventGuest);
	if (!IsGuestOutputRange(events, output_size))
	{
		return NET_ERROR_EFAULT;
	}

	const auto epoll_state = FindEpollState(epoll_id);
	if (epoll_state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	int host_epoll_fd = -1;
	int wake_fd = -1;
	uint64_t generation = 0;
	if (!BeginEpollOperation(epoll_state, &host_epoll_fd, &wake_fd, &generation))
	{
		return NET_ERROR_EBADF;
	}

	HostArray<epoll_event>           host_events;
	HostArray<NetEpollEventGuest>    guest_events;
	if (!host_events.Allocate(static_cast<size_t>(max_events)) || !guest_events.Allocate(static_cast<size_t>(max_events)))
	{
		EndEpollOperation(epoll_state);
		return NET_ERROR_ENOMEM;
	}
	const int host_timeout = timeout_ms < 0 ? -1 : timeout_ms;
	const int result       = ::epoll_wait(host_epoll_fd, host_events.Data(), max_events, host_timeout);
	const int wait_errno    = errno;
	if (result < 0)
	{
		const bool current = IsEpollCurrent(epoll_state, generation);
		EndEpollOperation(epoll_state);
		return current ? HostErrnoToNet(wait_errno) : NET_ERROR_EBADF;
	}

	bool cancelled = false;
	size_t guest_event_count = 0;
	for (int i = 0; i < result; i++)
	{
		const int socket_id = host_events.Data()[static_cast<size_t>(i)].data.fd;
		if (socket_id == kEpollWakeMarker)
		{
			DrainEpollWake(wake_fd);
			cancelled = true;
			continue;
		}
		NetEpollRegistration registration {};
		{
			std::lock_guard lock(epoll_state->mutex);
			if (epoll_state->closing || epoll_state->generation != generation)
			{
				cancelled = true;
				break;
			}
			const auto it = epoll_state->registrations.find(socket_id);
			if (it == epoll_state->registrations.end())
			{
				continue;
			}
			registration = it->second;
		}
		guest_events.Data()[guest_event_count++] =
		    {host_events.Data()[static_cast<size_t>(i)].events, 0, registration.ident, registration.data};
	}
	if (!cancelled && !IsEpollCurrent(epoll_state, generation))
	{
		cancelled = true;
	}
	EndEpollOperation(epoll_state);
	if (cancelled)
	{
		return NET_ERROR_EBADF;
	}
	const size_t written_size = guest_event_count * sizeof(NetEpollEventGuest);
	const int output_result = CopyGuestOutput(events, reinterpret_cast<const uint8_t*>(guest_events.Data()), written_size);
	if (output_result != OK)
	{
		return output_result;
	}
	return static_cast<int>(guest_event_count);
#else
	return NET_ERROR_ENOTSUP;
#endif
}

int KYTY_SYSV_ABI NetResolverStartNtoa(int rid, const char* hostname, void* address, int timeout_ms, int retries, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t resolver_id = %d\n", rid);
	KYTY_LOG_DEBUG("\t hostname    = %s\n", hostname != nullptr ? hostname : "(null)");
	KYTY_LOG_DEBUG("\t address     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	KYTY_LOG_DEBUG("\t timeout_ms  = %d\n", timeout_ms);
	KYTY_LOG_DEBUG("\t retries     = %d\n", retries);
	KYTY_LOG_DEBUG("\t flags       = %d\n", flags);

	if (hostname == nullptr || address == nullptr)
	{
		return NET_ERROR_EINVAL;
	}

#if KYTY_NET_HOST_POSIX
	// Literal dotted-quad: no host resolution required.
	if (::inet_pton(AF_INET, hostname, address) == 1)
	{
		return OK;
	}

	addrinfo  hints {};
	hints.ai_family   = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo* result = nullptr;
	const int ret    = ::getaddrinfo(hostname, nullptr, &hints, &result);
	if (ret != 0 || result == nullptr)
	{
		if (result != nullptr)
		{
			::freeaddrinfo(result);
		}
		return ret == EAI_NONAME ? NET_ERROR_RESOLVER_ENOHOST : NET_ERROR_RESOLVER_EINTERNAL;
	}

	for (auto* ai = result; ai != nullptr; ai = ai->ai_next)
	{
		if (ai->ai_family == AF_INET && ai->ai_addr != nullptr && ai->ai_addrlen >= sizeof(sockaddr_in))
		{
			const auto* in = reinterpret_cast<const sockaddr_in*>(ai->ai_addr);
			std::memcpy(address, &in->sin_addr, sizeof(in->sin_addr));
			::freeaddrinfo(result);
			return OK;
		}
	}

	::freeaddrinfo(result);
	return NET_ERROR_RESOLVER_ENORECORD;
#else
	return NET_ERROR_RESOLVER_ENOTIMPLEMENTED;
#endif
}

int KYTY_SYSV_ABI NetResolverStartAton(int rid, const void* addr, char* hostname, int hostname_len, int timeout_ms, int retries,
                                       int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t resolver_id = %d\n", rid);
	KYTY_LOG_DEBUG("\t addr        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(addr));
	KYTY_LOG_DEBUG("\t hostname    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(hostname));
	KYTY_LOG_DEBUG("\t hostname_len = %d\n", hostname_len);
	KYTY_LOG_DEBUG("\t timeout_ms  = %d\n", timeout_ms);
	KYTY_LOG_DEBUG("\t retries     = %d\n", retries);
	KYTY_LOG_DEBUG("\t flags       = %d\n", flags);

	if (addr == nullptr || hostname == nullptr || hostname_len <= 0)
	{
		return NET_ERROR_EINVAL;
	}

#if KYTY_NET_HOST_POSIX
	sockaddr_in guest_addr {};
	std::memcpy(&guest_addr.sin_addr, addr, sizeof(guest_addr.sin_addr));
	guest_addr.sin_family = AF_INET;

	if (::getnameinfo(reinterpret_cast<sockaddr*>(&guest_addr), sizeof(guest_addr), hostname, static_cast<socklen_t>(hostname_len),
	                  nullptr, 0, NI_NAMEREQD) != 0)
	{
		return NET_ERROR_RESOLVER_ENOHOST;
	}
	return OK;
#else
	return NET_ERROR_RESOLVER_ENOTIMPLEMENTED;
#endif
}

int KYTY_SYSV_ABI NetGetSockInfo(int socket_id, void* info, int info_size, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t socket    = %d\n", socket_id);
	KYTY_LOG_DEBUG("\t info      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(info));
	KYTY_LOG_DEBUG("\t info_size = %d\n", info_size);
	KYTY_LOG_DEBUG("\t flags     = %d\n", flags);

	if (info == nullptr || info_size <= 0)
	{
		return NET_ERROR_EINVAL;
	}
	if (info_size > kMaxSocketInfoSize)
	{
		return NET_ERROR_EMSGSIZE;
	}

	const auto state = FindSocketState(socket_id);
	if (state == nullptr)
	{
		return NET_ERROR_EBADF;
	}
	HostArray<uint8_t> output;
	const int prepare_result = PrepareGuestOutput(info, static_cast<uint64_t>(info_size), &output);
	if (prepare_result != OK)
	{
		return prepare_result;
	}
	std::memset(output.Data(), 0, output.Size());

	(void)flags;

#if KYTY_NET_HOST_POSIX
	// Report the locally-bound address, mirroring getsockname semantics. The
	// caller decides which fields matter, so fill what the host exposes.
	sockaddr_storage host_addr {};
	socklen_t        host_len = sizeof(host_addr);
	int host_fd = -1;
	if (!BeginSocketOperation(state, &host_fd))
	{
		return NET_ERROR_EBADF;
	}
	const int getsockname_result = ::getsockname(host_fd, reinterpret_cast<sockaddr*>(&host_addr), &host_len);
	EndSocketOperation(state);
	if (getsockname_result == 0)
	{
		const auto* in = reinterpret_cast<const sockaddr_in*>(&host_addr);
		if (in->sin_family == AF_INET && info_size >= 8)
		{
			auto* bytes = output.Data();
			bytes[0]    = 16;
			bytes[1]    = 2;
			const uint16_t port = ntohs(in->sin_port);
			bytes[2] = static_cast<uint8_t>((port >> 8u) & 0xffu);
			bytes[3] = static_cast<uint8_t>(port & 0xffu);
			std::memcpy(bytes + 4, &in->sin_addr, 4);
		}
	}
#endif

	return CopyGuestOutput(info, output.Data(), output.Size());
}

} // namespace Net

namespace Ssl {

LIB_NAME("Ssl", "Ssl");

int KYTY_SYSV_ABI SslInit(uint64_t pool_size)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t size = %" PRIu64 "\n", pool_size);

	EXIT_IF(g_net == nullptr);

	if (pool_size == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	auto id = g_net->SslInit(pool_size);

	if (!id.IsValid())
	{
		return SSL_ERROR_OUT_OF_SIZE;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI SslTerm(int ssl_ctx_id)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->SslTerm(Network::Id(ssl_ctx_id)))
	{
		return SSL_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI SslClose(int ssl_id)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t ssl_id = %d\n", ssl_id);

	return OK;
}

} // namespace Ssl

namespace Http {

struct HttpEpoll
{
	Network::Id http_ctx_id = Network::Id(0);
	Network::Id request_id  = Network::Id(0);
	void*       user_arg    = nullptr;
};

LIB_NAME("Http", "Http");

int KYTY_SYSV_ABI HttpInit(int memid, int ssl_ctx_id, uint64_t pool_size)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t memid      = %d\n", memid);
	KYTY_LOG_DEBUG("\t ssl_ctx_id = %d\n", ssl_ctx_id);
	KYTY_LOG_DEBUG("\t size       = %" PRIu64 "\n", pool_size);

	EXIT_IF(g_net == nullptr);

	if (pool_size == 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	auto id = g_net->HttpInit(memid, Network::Id(ssl_ctx_id), pool_size);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpTerm(int http_ctx_id)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpTerm(Network::Id(http_ctx_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpCreateTemplate(int http_ctx_id, const char* user_agent, int http_ver, int is_auto_proxy_conf)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t http_ctx_id        = %d\n", http_ctx_id);
	KYTY_LOG_DEBUG("\t user_agent         = %s\n", user_agent);
	KYTY_LOG_DEBUG("\t http_ver           = %d\n", http_ver);
	KYTY_LOG_DEBUG("\t is_auto_proxy_conf = %d\n", is_auto_proxy_conf);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateTemplate(Network::Id(http_ctx_id), user_agent, http_ver, is_auto_proxy_conf != 0);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpDeleteTemplate(int tmpl_id)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteTemplate(Network::Id(tmpl_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetNonblock(int id, int enable)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id     = %d\n", id);
	KYTY_LOG_DEBUG("\t enable = %d\n", enable);

	if (!g_net->HttpSetNonblock(Network::Id(id), enable != 0))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpsSetSslCallback(int id, HttpsCallback cbfunc, void* user_arg)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id     = %d\n", id);

	if (!g_net->HttpsSetSslCallback(Network::Id(id), cbfunc, user_arg))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpsDisableOption(int id, uint32_t ssl_flags)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id        = %d\n", id);
	KYTY_LOG_DEBUG("\t ssl_flags = %u\n", ssl_flags);

	if (!g_net->HttpsDisableOption(Network::Id(id), ssl_flags))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetResolveTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id   = %d\n", id);
	KYTY_LOG_DEBUG("\t usec = %u\n", usec);

	if (!g_net->HttpSetResolveTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetResolveRetry(int id, int32_t retry)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id    = %d\n", id);
	KYTY_LOG_DEBUG("\t retry = %d\n", retry);

	if (!g_net->HttpSetResolveRetry(Network::Id(id), retry))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetConnectTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id   = %d\n", id);
	KYTY_LOG_DEBUG("\t usec = %u\n", usec);

	if (!g_net->HttpSetConnectTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetSendTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id   = %d\n", id);
	KYTY_LOG_DEBUG("\t usec = %u\n", usec);

	if (!g_net->HttpSetSendTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetRecvTimeOut(int id, uint32_t usec)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id   = %d\n", id);
	KYTY_LOG_DEBUG("\t usec = %u\n", usec);

	if (!g_net->HttpSetRecvTimeOut(Network::Id(id), usec))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetAutoRedirect(int id, int enable)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id     = %d\n", id);
	KYTY_LOG_DEBUG("\t enable = %d\n", enable);

	if (!g_net->HttpSetAutoRedirect(Network::Id(id), enable))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpSetAuthEnabled(int id, int enable)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id     = %d\n", id);
	KYTY_LOG_DEBUG("\t enable = %d\n", enable);

	if (!g_net->HttpSetAuthEnabled(Network::Id(id), enable))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpAddRequestHeader(int id, const char* name, const char* value, uint32_t mode)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id    = %d\n", id);
	KYTY_LOG_DEBUG("\t name  = %s\n", name);
	KYTY_LOG_DEBUG("\t value = %s\n", value);
	KYTY_LOG_DEBUG("\t mode  = %u\n", mode);

	if (mode != 0 && mode != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	if (!g_net->HttpAddRequestHeader(Network::Id(id), name, value, mode == 1))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpCreateEpoll(int http_ctx_id, HttpEpollHandle* eh)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	if (eh == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	if (!g_net->HttpValid(Network::Id(http_ctx_id))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	*eh = new HttpEpoll;

	(*eh)->http_ctx_id = Network::Id(http_ctx_id);

	return OK;
}

int KYTY_SYSV_ABI HttpDestroyEpoll(int http_ctx_id, HttpEpollHandle eh)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t http_ctx_id = %d\n", http_ctx_id);

	EXIT_IF(g_net == nullptr);

	if (eh == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	if (!g_net->HttpValid(Network::Id(http_ctx_id))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	delete eh;

	return OK;
}

int KYTY_SYSV_ABI HttpSetEpoll(int id, HttpEpollHandle eh, void* user_arg)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id = %d\n", id);

	if (eh == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	if (!g_net->HttpValidRequest(Network::Id(id))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	eh->request_id = Network::Id(id);
	eh->user_arg   = user_arg;

	return OK;
}

int KYTY_SYSV_ABI HttpUnsetEpoll(int id)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t id = %d\n", id);

	if (!g_net->HttpValidRequest(Network::Id(id))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return OK;
}

int KYTY_SYSV_ABI HttpSendRequest(int request_id, const void* /*post_data*/, size_t /*size*/)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t request_id = %d\n", request_id);

	return HTTP_ERROR_TIMEOUT;
}

int KYTY_SYSV_ABI HttpCreateConnectionWithURL(int tmpl_id, const char* url, int enable_keep_alive)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t tmpl_id           = %d\n", tmpl_id);
	KYTY_LOG_DEBUG("\t url               = %s\n", url);
	KYTY_LOG_DEBUG("\t enable_keep_alive = %d\n", enable_keep_alive);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateConnectionWithURL(Network::Id(tmpl_id), url, enable_keep_alive != 0);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpDeleteConnection(int conn_id)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t conn_id = %d\n", conn_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteConnection(Network::Id(conn_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

int KYTY_SYSV_ABI HttpCreateRequestWithURL2(int conn_id, const char* method, const char* url, uint64_t content_length)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t conn_id        = %d\n", conn_id);
	KYTY_LOG_DEBUG("\t url            = %s\n", url);
	KYTY_LOG_DEBUG("\t method         = %s\n", method);
	KYTY_LOG_DEBUG("\t content_length = %" PRIu64 "\n", content_length);

	EXIT_IF(g_net == nullptr);

	auto id = g_net->HttpCreateRequestWithURL2(Network::Id(conn_id), method, url, content_length);

	if (!id.IsValid())
	{
		return HTTP_ERROR_OUT_OF_MEMORY;
	}

	return id.ToInt();
}

int KYTY_SYSV_ABI HttpDeleteRequest(int req_id)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t req_id = %d\n", req_id);

	EXIT_IF(g_net == nullptr);

	if (!g_net->HttpDeleteRequest(Network::Id(req_id)))
	{
		return HTTP_ERROR_INVALID_ID;
	}

	return OK;
}

} // namespace Http

namespace NetCtl {

LIB_NAME("NetCtl", "NetCtl");

struct NetInAddr
{
	uint32_t s_addr = 0;
};

struct NetEtherAddr
{
	uint8_t data[6];
};

struct NetCtlNatInfo
{
	unsigned int size       = sizeof(NetCtlNatInfo);
	int          stunStatus = 0;
	int          natType    = 0;
	NetInAddr    mappedAddr;
};

union NetCtlInfo
{
	uint32_t     device;
	NetEtherAddr ether_addr;
	uint32_t     mtu;
	uint32_t     link;
	NetEtherAddr bssid;
	char         ssid[32 + 1];
	uint32_t     wifi_security;
	uint8_t      rssi_dbm;
	uint8_t      rssi_percentage;
	uint8_t      channel;
	uint32_t     ip_config;
	char         dhcp_hostname[255 + 1];
	char         pppoe_auth_name[127 + 1];
	char         ip_address[16];
	char         netmask[16];
	char         default_route[16];
	char         primary_dns[16];
	char         secondary_dns[16];
	uint32_t     http_proxy_config;
	char         http_proxy_server[255 + 1];
	uint16_t     http_proxy_port;
};

int KYTY_SYSV_ABI NetCtlInit()
{
	PRINT_NAME();

	return OK;
}

void KYTY_SYSV_ABI NetCtlTerm()
{
	PRINT_NAME();
}

int KYTY_SYSV_ABI NetCtlGetNatInfo(NetCtlNatInfo* nat_info)
{
	PRINT_NAME();

	NetCtlNatInfo staged {};
	if (nat_info == nullptr ||
	    !Core::VirtualMemory::CopyFromGuest(&staged, reinterpret_cast<uint64_t>(nat_info), sizeof(staged)))
	{
		return NET_ERROR_EFAULT;
	}
	if (staged.size != sizeof(NetCtlNatInfo))
	{
		return NET_ERROR_EINVAL;
	}

	staged.stunStatus        = 1;
	staged.natType           = 3;
	staged.mappedAddr.s_addr = 0x7f000001;
	return Net::CopyGuestOutput(nat_info, reinterpret_cast<const uint8_t*>(&staged), sizeof(staged));
}

int KYTY_SYSV_ABI NetCtlCheckCallback()
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NetCtlGetState(int* state)
{
	PRINT_NAME();

	const int staged = 0; // Disconnected
	return Net::CopyGuestOutput(state, reinterpret_cast<const uint8_t*>(&staged), sizeof(staged));
}

int KYTY_SYSV_ABI NetCtlRegisterCallback(NetCtlCallback func, void* /*arg*/, int* cid)
{
	PRINT_NAME();

	if (func == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	const int staged = 1;
	return Net::CopyGuestOutput(cid, reinterpret_cast<const uint8_t*>(&staged), sizeof(staged));
}

int KYTY_SYSV_ABI NetCtlGetInfo(int code, NetCtlInfo* info)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t code = %d\n", code);

	NetCtlInfo output {};
	memset(&output, 0, sizeof(output));
	uint64_t   output_size = 0;
	switch (code)
	{
		case 2:
			memset(output.ether_addr.data, 0, sizeof(output.ether_addr.data));
			output_size = sizeof(output.ether_addr);
			break;
		case 11: output.ip_config = 0; output_size = sizeof(output.ip_config); break;
		case 14:
			memcpy(output.ip_address, "127.0.0.1", sizeof("127.0.0.1"));
			output_size = sizeof(output.ip_address);
			break;
		case 15:
			memcpy(output.netmask, "255.0.0.0", sizeof("255.0.0.0"));
			output_size = sizeof(output.netmask);
			break;
		default: EXIT("unknown code: %d\n", code);
	}

	return Net::CopyGuestOutput(info, reinterpret_cast<const uint8_t*>(&output), output_size);
}

} // namespace NetCtl

namespace NpManager {

LIB_NAME("NpManager", "NpManager");

struct NpTitleId
{
	char    id[12 + 1];
	uint8_t padding[3];
};

struct NpTitleSecret
{
	uint8_t data[128];
};

struct NpCountryCode
{
	char data[2];
	char term;
	char padding[1];
};

struct NpAgeRestriction
{
	NpCountryCode country_code;
	int8_t        age;
	uint8_t       padding[3];
};

struct NpContentRestriction
{
	size_t                  size;
	int8_t                  default_age_restriction;
	char                    padding[3];
	int32_t                 age_restriction_count;
	const NpAgeRestriction* age_restriction;
};

struct NpOnlineId
{
	char data[16];
	char term;
	char dummy[3];
};

struct NpId
{
	NpOnlineId handle;
	uint8_t    opt[8];
	uint8_t    reserved[8];
};

struct NpCreateAsyncRequestParameter
{
	size_t                   size;
	Kernel::KernelCpumask cpu_affinity_mask;
	int                      thread_priority;
	uint8_t                  padding[4];
};

int KYTY_SYSV_ABI NpCheckCallback()
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpSetNpTitleId(const NpTitleId* title_id, const NpTitleSecret* title_secret)
{
	PRINT_NAME();

	if (title_id == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (title_secret == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t title_id = %.12s\n", title_id->id);
	KYTY_LOG_DEBUG("\t title_secret = %s\n", String::HexFromBin(Core::ByteBuffer(title_secret->data, 128)).C_Str());

	return OK;
}

int KYTY_SYSV_ABI NpSetContentRestriction(const NpContentRestriction* restriction)
{
	PRINT_NAME();

	if (restriction == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (restriction->size != sizeof(NpContentRestriction)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t default_age_restriction = %" PRIi8 "\n", restriction->default_age_restriction);
	KYTY_LOG_DEBUG("\t age_restriction_count   = %" PRIi32 "\n", restriction->age_restriction_count);

	for (int i = 0; i < restriction->age_restriction_count; i++)
	{
		KYTY_LOG_DEBUG("\t age_restriction[%d].age = %" PRIi8 "\n", i, restriction->age_restriction[i].age);
		KYTY_LOG_DEBUG("\t age_restriction[%d].country_code.data = %.2s\n", i, restriction->age_restriction[i].country_code.data);
	}

	return OK;
}

int KYTY_SYSV_ABI NpRegisterStateCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

void KYTY_SYSV_ABI NpRegisterGamePresenceCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();
}

int KYTY_SYSV_ABI NpRegisterPlusEventCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpRegisterNpReachabilityStateCallback(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpGetNpId(int user_id, NpId* np_id)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);

	if (np_id == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	int s = snprintf(np_id->handle.data, 16, "Kyty");

	if (s >= 16) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	np_id->handle.term = 0;

	return OK;
}

int KYTY_SYSV_ABI NpGetOnlineId(int user_id, NpOnlineId* online_id)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);

	if (online_id == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	int s = snprintf(online_id->data, 16, "Kyty");

	if (s >= 16) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	online_id->term = 0;

	return OK;
}

int KYTY_SYSV_ABI NpCreateAsyncRequest(const NpCreateAsyncRequestParameter* param)
{
	PRINT_NAME();

	if (param == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t size              = %" PRIu64 "\n", param->size);
	KYTY_LOG_DEBUG("\t cpu_affinity_mask = %" PRIu64 "\n", param->cpu_affinity_mask);
	KYTY_LOG_DEBUG("\t thread_priority   = %d\n", param->thread_priority);

	static std::atomic_int id = 0;

	if (id >= 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	return ++id;
}

int KYTY_SYSV_ABI NpDeleteRequest(int req_id)
{
	PRINT_NAME();

	if (req_id != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t req_id = %d\n", req_id);

	return OK;
}

int KYTY_SYSV_ABI NpCheckNpAvailability(int req_id, const char* user, void* result)
{
	PRINT_NAME();

	if (req_id != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (user == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (result != nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t req_id = %d\n", req_id);
	KYTY_LOG_DEBUG("\t user   = %s\n", user);

	return OK;
}

int KYTY_SYSV_ABI NpPollAsync(int req_id, int* result)
{
	PRINT_NAME();

	if (req_id != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (result == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t req_id = %d\n", req_id);

	*result = 0;

	return 0;
}

int KYTY_SYSV_ABI NpGetState(int user_id, uint32_t* state)
{
	PRINT_NAME();

	if (state == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t user_id = %d\n", user_id);

	*state = 1; // Signed out

	return OK;
}

int KYTY_SYSV_ABI NpHasSignedUp(int user_id, bool* has_signed_up)
{
	PRINT_NAME();

	constexpr int np_error_invalid_argument = static_cast<int>(0x80550003u);
	// The writable output contract is not established. Do not synthesize an
	// offline-account value through an unchecked guest pointer.
	(void)user_id;
	(void)has_signed_up;
	return np_error_invalid_argument;
}

} // namespace NpManager

namespace NpManagerForToolkit {

LIB_NAME("NpManagerForToolkit", "NpManager");

int KYTY_SYSV_ABI NpRegisterStateCallbackForToolkit(void* /*callback*/, void* /*userdata*/)
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NpCheckCallbackForLib()
{
	PRINT_NAME();

	return OK;
}

} // namespace NpManagerForToolkit

namespace NpTrophy {

LIB_NAME("NpTrophy", "NpTrophy");

struct NpTrophyFlagArray
{
	uint32_t flag_bits[4];
};

int KYTY_SYSV_ABI NpTrophyCreateHandle(int* handle)
{
	PRINT_NAME();

	if (handle == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	*handle = 1;

	return OK;
}

int KYTY_SYSV_ABI NpTrophyCreateContext(int* context, int user_id, uint32_t service_label, uint64_t options)
{
	PRINT_NAME();

	if (context == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (options != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	*context = 1;

	KYTY_LOG_DEBUG("\t user_id       = %d\n", user_id);
	KYTY_LOG_DEBUG("\t service_label = %u\n", service_label);

	return OK;
}

int KYTY_SYSV_ABI NpTrophyRegisterContext(int context, int handle, uint64_t options)
{
	PRINT_NAME();

	if (options != 0) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (context != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t context = %d\n", context);
	KYTY_LOG_DEBUG("\t handle  = %d\n", handle);

	return OK;
}

int KYTY_SYSV_ABI NpTrophyDestroyHandle(int handle)
{
	PRINT_NAME();

	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t handle  = %d\n", handle);

	return OK;
}

int KYTY_SYSV_ABI NpTrophyGetTrophyUnlockState(int context, int handle, NpTrophyFlagArray* flags, uint32_t* count)
{
	PRINT_NAME();

	if (flags == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (count == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (context != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (handle != 1) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t context = %d\n", context);
	KYTY_LOG_DEBUG("\t handle  = %d\n", handle);

	flags->flag_bits[0] = 0;
	flags->flag_bits[1] = 0;
	flags->flag_bits[2] = 0;
	flags->flag_bits[3] = 0;

	*count = 0;

	return OK;
}

} // namespace NpTrophy

namespace NpWebApi {

LIB_NAME("NpWebApi", "NpWebApi");

int KYTY_SYSV_ABI NpWebApiInitialize(int http_ctx_id, size_t pool_size)
{
	PRINT_NAME();

	EXIT_IF(g_net == nullptr);

	KYTY_LOG_DEBUG("\t http_ctx_id = %d\n", http_ctx_id);
	KYTY_LOG_DEBUG("\t pool_size   = %" PRIu64 "\n", pool_size);

	if (!g_net->HttpValid(Network::Id(http_ctx_id))) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	static int id = 0;

	return ++id;
}

int KYTY_SYSV_ABI NpWebApiTerminate(int lib_ctx_id)
{
	PRINT_NAME();

	KYTY_LOG_DEBUG("\t lib_ctx_id = %d\n", lib_ctx_id);

	return OK;
}

} // namespace NpWebApi

} // namespace Kyty::Libs::Network

#endif // KYTY_EMU_ENABLED
