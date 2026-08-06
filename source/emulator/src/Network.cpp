#include "Emulator/Network.h"

#include "Kyty/Core/ByteBuffer.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/String.h"
#include "Kyty/Core/Threads.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Kernel/Pthread.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Log.h"

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#if KYTY_PLATFORM == KYTY_PLATFORM_LINUX || defined(__APPLE__)
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#define KYTY_NET_HOST_POSIX 1
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
	bool     active  = true;
	bool     bound   = false;
	uint16_t port    = 0;
	uint8_t  addr[4] {};
#if KYTY_NET_HOST_POSIX
	int host_fd = -1;
#endif
};

static std::atomic<int> g_next_socket_id {1};
static std::unordered_map<int, SocketState> g_sockets;

static bool ParseGuestSockaddrIn(const void* addr, int len, uint16_t* port_out, uint8_t ip_out[4])
{
	if (addr == nullptr || len < 8 || port_out == nullptr || ip_out == nullptr)
	{
		return false;
	}
	const auto* bytes = static_cast<const uint8_t*>(addr);
	if (bytes[1] != 2)
	{
		return false;
	}
	*port_out = static_cast<uint16_t>((static_cast<uint16_t>(bytes[2]) << 8u) | bytes[3]);
	std::memcpy(ip_out, bytes + 4, 4);
	return true;
}

static void WriteGuestSockaddrIn(void* addr, int* len, int max_len, uint16_t port, const uint8_t ip[4])
{
	if (addr == nullptr || len == nullptr || max_len < 8 || ip == nullptr)
	{
		return;
	}
	auto* out = static_cast<uint8_t*>(addr);
	out[0]    = 16;
	out[1]    = 2;
	out[2]    = static_cast<uint8_t>((port >> 8u) & 0xffu);
	out[3]    = static_cast<uint8_t>(port & 0xffu);
	std::memcpy(out + 4, ip, 4);
	const int write_len = max_len < 16 ? max_len : 16;
	*len                = write_len;
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

static bool GuestToHostSockaddrIn(const void* addr, int len, sockaddr_in* out)
{
	uint16_t port = 0;
	uint8_t  ip[4] {};
	if (out == nullptr || !ParseGuestSockaddrIn(addr, len, &port, ip))
	{
		return false;
	}
	std::memset(out, 0, sizeof(*out));
	out->sin_family = AF_INET;
	out->sin_port   = htons(port);
	std::memcpy(&out->sin_addr, ip, 4);
	return true;
}
#endif

int KYTY_SYSV_ABI NetSocket(const char* name, int family, int type, int protocol)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t name = %s\n", name != nullptr ? name : "(null)");
	KYTY_LOG_DEBUG("\t family = %d type = %d protocol = %d\n", family, type, protocol);

	SocketState state {};
#if KYTY_NET_HOST_POSIX
	int host_family   = 0;
	int host_type     = 0;
	int host_protocol = 0;
	if (!TranslateGuestSocketParams(family, type, protocol, &host_family, &host_type, &host_protocol))
	{
		return NET_ERROR_EINVAL;
	}
	state.host_fd = ::socket(host_family, host_type, host_protocol);
	if (state.host_fd < 0)
	{
		return HostErrnoToNet(errno);
	}
#else
	(void)family;
	(void)type;
	(void)protocol;
#endif

	const int id = g_next_socket_id.fetch_add(1, std::memory_order_relaxed);
	g_sockets[id] = state;
	return id;
}

int KYTY_SYSV_ABI NetSocketClose(int id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}
#if KYTY_NET_HOST_POSIX
	if (it->second.host_fd >= 0)
	{
		::close(it->second.host_fd);
	}
#endif
	g_sockets.erase(it);
	return OK;
}

int KYTY_SYSV_ABI NetBind(int id, const void* addr, int len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}
	uint16_t port = 0;
	uint8_t  ip[4] {};
	if (!ParseGuestSockaddrIn(addr, len, &port, ip))
	{
		return NET_ERROR_EINVAL;
	}
#if KYTY_NET_HOST_POSIX
	sockaddr_in host_addr {};
	if (!GuestToHostSockaddrIn(addr, len, &host_addr))
	{
		return NET_ERROR_EINVAL;
	}
	if (::bind(it->second.host_fd, reinterpret_cast<sockaddr*>(&host_addr), sizeof(host_addr)) != 0)
	{
		return HostErrnoToNet(errno);
	}
#endif
	it->second.bound = true;
	it->second.port  = port;
	std::memcpy(it->second.addr, ip, 4);
	return OK;
}

int KYTY_SYSV_ABI NetConnect(int id, const void* addr, int len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}
	uint16_t port = 0;
	uint8_t  ip[4] {};
	if (!ParseGuestSockaddrIn(addr, len, &port, ip))
	{
		return NET_ERROR_EINVAL;
	}
#if KYTY_NET_HOST_POSIX
	sockaddr_in host_addr {};
	if (!GuestToHostSockaddrIn(addr, len, &host_addr))
	{
		return NET_ERROR_EINVAL;
	}
	if (::connect(it->second.host_fd, reinterpret_cast<sockaddr*>(&host_addr), sizeof(host_addr)) != 0)
	{
		return HostErrnoToNet(errno);
	}
#endif
	it->second.bound = true;
	it->second.port  = port;
	std::memcpy(it->second.addr, ip, 4);
	return OK;
}

int KYTY_SYSV_ABI NetListen(int id, int backlog)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d backlog = %d\n", id, backlog);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}
#if KYTY_NET_HOST_POSIX
	if (::listen(it->second.host_fd, backlog) != 0)
	{
		return HostErrnoToNet(errno);
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
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}

	SocketState accepted_state {};
#if KYTY_NET_HOST_POSIX
	sockaddr_in peer {};
	socklen_t   peer_len = sizeof(peer);
	accepted_state.host_fd =
	    ::accept(it->second.host_fd, reinterpret_cast<sockaddr*>(&peer), &peer_len);
	if (accepted_state.host_fd < 0)
	{
		return HostErrnoToNet(errno);
	}
	accepted_state.bound = true;
	accepted_state.port  = ntohs(peer.sin_port);
	std::memcpy(accepted_state.addr, &peer.sin_addr, 4);
	if (addr != nullptr && len != nullptr)
	{
		WriteGuestSockaddrIn(addr, len, *len, accepted_state.port, accepted_state.addr);
	}
#else
	(void)addr;
	(void)len;
#endif

	const int accepted = g_next_socket_id.fetch_add(1, std::memory_order_relaxed);
	g_sockets[accepted] = accepted_state;
	return accepted;
}

int64_t KYTY_SYSV_ABI NetSend(int id, const void* buf, uint64_t len, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d len = %" PRIu64 " flags = %d\n", id, len, flags);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}
	if (len != 0 && buf == nullptr)
	{
		return NET_ERROR_EFAULT;
	}
	if (len == 0)
	{
		return 0;
	}
#if KYTY_NET_HOST_POSIX
	const auto sent = ::send(it->second.host_fd, buf, static_cast<size_t>(len), flags);
	if (sent < 0)
	{
		return HostErrnoToNet(errno);
	}
	return sent;
#else
	(void)flags;
	return NET_ERROR_EOPNOTSUPP;
#endif
}

int64_t KYTY_SYSV_ABI NetRecv(int id, void* buf, uint64_t len, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d len = %" PRIu64 " flags = %d\n", id, len, flags);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}
	if (len != 0 && buf == nullptr)
	{
		return NET_ERROR_EFAULT;
	}
	if (len == 0)
	{
		return 0;
	}
#if KYTY_NET_HOST_POSIX
	const auto received = ::recv(it->second.host_fd, buf, static_cast<size_t>(len), flags);
	if (received < 0)
	{
		return HostErrnoToNet(errno);
	}
	return received;
#else
	(void)flags;
	return NET_ERROR_EOPNOTSUPP;
#endif
}

int KYTY_SYSV_ABI NetGetsockname(int id, void* addr, int* len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d\n", id);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end() || !it->second.bound)
	{
		return NET_ERROR_EINVAL;
	}
	if (addr == nullptr || len == nullptr || *len < 8)
	{
		return NET_ERROR_EINVAL;
	}
#if KYTY_NET_HOST_POSIX
	sockaddr_in host_addr {};
	socklen_t   host_len = sizeof(host_addr);
	if (::getsockname(it->second.host_fd, reinterpret_cast<sockaddr*>(&host_addr), &host_len) == 0 &&
	    host_addr.sin_family == AF_INET)
	{
		uint8_t ip[4] {};
		std::memcpy(ip, &host_addr.sin_addr, 4);
		WriteGuestSockaddrIn(addr, len, *len, ntohs(host_addr.sin_port), ip);
		return OK;
	}
#endif
	WriteGuestSockaddrIn(addr, len, *len, it->second.port, it->second.addr);
	return OK;
}

int KYTY_SYSV_ABI NetGetsockopt(int id, int level, int option, void* value, int* value_len)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t id = %d level = %d option = %d\n", id, level, option);
	const auto it = g_sockets.find(id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}
	if (value == nullptr || value_len == nullptr || level != 0xffff)
	{
		return NET_ERROR_EINVAL;
	}
	if (*value_len < static_cast<int>(sizeof(int)))
	{
		return NET_ERROR_EINVAL;
	}
	int stored = 0;
	switch (option)
	{
		case 0x1200: stored = 0; break; // ORBIS_NET_SO_NBIO: blocking
		case 0x0004: stored = 0; break; // SO_REUSEADDR
		case 0x1007: stored = 0; break; // SO_ERROR
		default: return NET_ERROR_EINVAL;
	}
	std::memcpy(value, &stored, sizeof(stored));
	*value_len = static_cast<int>(sizeof(stored));
	return OK;
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
	return (g_sockets.find(id) != g_sockets.end() ? OK : NET_ERROR_EBADF);
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

// The host reports readiness through its own epoll instance. The guest id maps
// 1:1 to the host descriptor; the caller data/identity are retained so the
// result array can be re-assembled with the original guest layout.
struct NetEpollState
{
	int  host_fd = -1;
	bool used    = false;
};

struct NetEpollRegistration
{
	uint64_t ident;
	uint64_t data;
};

constexpr int kEpollIdBase = 0x1000000;

std::mutex                             g_epoll_mutex;
std::vector<NetEpollState>             g_epoll_states;
std::unordered_map<int, int>           g_epoll_id_to_slot;
std::unordered_map<int, int>           g_epoll_slot_to_id;
std::unordered_map<int, NetEpollRegistration> g_epoll_registrations;

int AllocateEpollId()
{
	std::lock_guard lock(g_epoll_mutex);
	for (size_t slot = 0; slot < g_epoll_states.size(); slot++)
	{
		if (!g_epoll_states[slot].used)
		{
			g_epoll_states[slot].used = true;
			const int id               = kEpollIdBase + static_cast<int>(slot);
			g_epoll_id_to_slot[id]     = static_cast<int>(slot);
			g_epoll_slot_to_id[static_cast<int>(slot)] = id;
			return id;
		}
	}
	const int id = kEpollIdBase + static_cast<int>(g_epoll_states.size());
	g_epoll_states.push_back({});
	g_epoll_states.back().used = true;
	g_epoll_id_to_slot[id]     = static_cast<int>(g_epoll_states.size()) - 1;
	g_epoll_slot_to_id[static_cast<int>(g_epoll_states.size()) - 1] = id;
	return id;
}

void ReleaseEpollId(int id)
{
	std::lock_guard lock(g_epoll_mutex);
	const auto      it = g_epoll_id_to_slot.find(id);
	if (it == g_epoll_id_to_slot.end())
	{
		return;
	}
	const int slot = it->second;
	if (slot >= 0 && slot < static_cast<int>(g_epoll_states.size()))
	{
		g_epoll_states[static_cast<size_t>(slot)].used = false;
		if (g_epoll_states[static_cast<size_t>(slot)].host_fd >= 0)
		{
			::close(g_epoll_states[static_cast<size_t>(slot)].host_fd);
			g_epoll_states[static_cast<size_t>(slot)].host_fd = -1;
		}
	}
	g_epoll_slot_to_id.erase(slot);
	g_epoll_id_to_slot.erase(id);
}

} // namespace

int KYTY_SYSV_ABI NetEpollCreate(const char* name, int flags)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t name  = %s\n", name != nullptr ? name : "(null)");
	KYTY_LOG_DEBUG("\t flags = %d\n", flags);

	if (name == nullptr || flags != 0)
	{
		return NET_ERROR_EINVAL;
	}

#if KYTY_NET_HOST_POSIX
	const int host_fd = ::epoll_create1(0);
	if (host_fd < 0)
	{
		return HostErrnoToNet(errno);
	}
	{
		std::lock_guard lock(g_epoll_mutex);
		const int       id   = kEpollIdBase + static_cast<int>(g_epoll_states.size());
		g_epoll_states.push_back({host_fd, true});
		g_epoll_id_to_slot[id]     = static_cast<int>(g_epoll_states.size()) - 1;
		g_epoll_slot_to_id[static_cast<int>(g_epoll_states.size()) - 1] = id;
		return id;
	}
#else
	return NET_ERROR_ENOTSUP;
#endif
}

int KYTY_SYSV_ABI NetEpollDestroy(int epoll_id)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t epoll_id = %d\n", epoll_id);

	{
		std::lock_guard lock(g_epoll_mutex);
		const auto      it = g_epoll_id_to_slot.find(epoll_id);
		if (it == g_epoll_id_to_slot.end())
		{
			return NET_ERROR_EBADF;
		}
		const int slot = it->second;
		if (slot >= 0 && slot < static_cast<int>(g_epoll_states.size()))
		{
			g_epoll_states[static_cast<size_t>(slot)].used = false;
			if (g_epoll_states[static_cast<size_t>(slot)].host_fd >= 0)
			{
				::close(g_epoll_states[static_cast<size_t>(slot)].host_fd);
				g_epoll_states[static_cast<size_t>(slot)].host_fd = -1;
			}
		}
		g_epoll_slot_to_id.erase(slot);
		g_epoll_id_to_slot.erase(epoll_id);
	}

	std::lock_guard lock(g_epoll_mutex);
	for (auto it = g_epoll_registrations.begin(); it != g_epoll_registrations.end();)
	{
		if ((it->first >> 16) == epoll_id)
		{
			it = g_epoll_registrations.erase(it);
		} else
		{
			++it;
		}
	}
	return OK;
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

	int host_fd = -1;
	{
		const auto socket_it = g_sockets.find(socket_id);
		if (socket_it == g_sockets.end())
		{
			return NET_ERROR_EBADF;
		}
#if KYTY_NET_HOST_POSIX
		host_fd = socket_it->second.host_fd;
		if (host_fd < 0)
		{
			return NET_ERROR_ENOTSOCK;
		}
#else
		(void)host_fd;
		return NET_ERROR_ENOTSUP;
#endif
	}

	int slot = -1;
	{
		std::lock_guard lock(g_epoll_mutex);
		const auto      it = g_epoll_id_to_slot.find(epoll_id);
		if (it == g_epoll_id_to_slot.end())
		{
			return NET_ERROR_EBADF;
		}
		slot = it->second;
	}
	if (slot < 0 || slot >= static_cast<int>(g_epoll_states.size()))
	{
		return NET_ERROR_EBADF;
	}

	const int host_epoll_fd = g_epoll_states[static_cast<size_t>(slot)].host_fd;
	if (host_epoll_fd < 0)
	{
		return NET_ERROR_EBADF;
	}

#if KYTY_NET_HOST_POSIX
	epoll_event host_event {};
	const int   key = (epoll_id << 16) | (socket_id & 0xffff);

	if (operation == NET_EPOLL_CTL_ADD || operation == NET_EPOLL_CTL_MOD)
	{
		const auto* guest_event = static_cast<const NetEpollEventGuest*>(event);
		host_event.events       = guest_event->events;
		host_event.data.fd      = socket_id;

		std::lock_guard lock(g_epoll_mutex);
		g_epoll_registrations[key] = NetEpollRegistration {guest_event->ident, guest_event->data};
	} else
	{
		std::lock_guard lock(g_epoll_mutex);
		g_epoll_registrations.erase(key);
	}

	if (::epoll_ctl(host_epoll_fd, operation, host_fd, &host_event) != 0)
	{
		return HostErrnoToNet(errno);
	}
	return OK;
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

	int slot = -1;
	{
		std::lock_guard lock(g_epoll_mutex);
		const auto      it = g_epoll_id_to_slot.find(epoll_id);
		if (it == g_epoll_id_to_slot.end())
		{
			return NET_ERROR_EBADF;
		}
		slot = it->second;
	}
	if (slot < 0 || slot >= static_cast<int>(g_epoll_states.size()))
	{
		return NET_ERROR_EBADF;
	}

	const int host_epoll_fd = g_epoll_states[static_cast<size_t>(slot)].host_fd;
	if (host_epoll_fd < 0)
	{
		return NET_ERROR_EBADF;
	}

#if KYTY_NET_HOST_POSIX
	std::vector<epoll_event> host_events(static_cast<size_t>(max_events));
	const int host_timeout = timeout_ms < 0 ? -1 : timeout_ms;
	const int result       = ::epoll_wait(host_epoll_fd, host_events.data(), max_events, host_timeout);
	if (result < 0)
	{
		return HostErrnoToNet(errno);
	}

	auto* guest_out = static_cast<NetEpollEventGuest*>(events);
	for (int i = 0; i < result; i++)
	{
		const int socket_id = host_events[static_cast<size_t>(i)].data.fd;
		const int key       = (epoll_id << 16) | (socket_id & 0xffff);
		NetEpollRegistration registration {};
		{
			std::lock_guard lock(g_epoll_mutex);
			const auto      it = g_epoll_registrations.find(key);
			if (it != g_epoll_registrations.end())
			{
				registration = it->second;
			}
		}
		guest_out[i].events   = host_events[static_cast<size_t>(i)].events;
		guest_out[i].reserved = 0;
		guest_out[i].ident    = registration.ident;
		guest_out[i].data     = registration.data;
	}
	return result;
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

	const auto it = g_sockets.find(socket_id);
	if (it == g_sockets.end())
	{
		return NET_ERROR_EBADF;
	}

	std::memset(info, 0, static_cast<size_t>(info_size));

#if KYTY_NET_HOST_POSIX
	// Report the locally-bound address, mirroring getsockname semantics. The
	// caller decides which fields matter, so fill what the host exposes.
	sockaddr_storage host_addr {};
	socklen_t        host_len = sizeof(host_addr);
	if (it->second.host_fd >= 0 && ::getsockname(it->second.host_fd, reinterpret_cast<sockaddr*>(&host_addr), &host_len) == 0)
	{
		const auto* in = reinterpret_cast<const sockaddr_in*>(&host_addr);
		if (in->sin_family == AF_INET && info_size >= 8)
		{
			auto* bytes = static_cast<uint8_t*>(info);
			bytes[0]    = 16;
			bytes[1]    = 2;
			const uint16_t port = ntohs(in->sin_port);
			bytes[2] = static_cast<uint8_t>((port >> 8u) & 0xffu);
			bytes[3] = static_cast<uint8_t>(port & 0xffu);
			std::memcpy(bytes + 4, &in->sin_addr, 4);
		}
	}
#else
	(void)flags;
#endif

	return OK;
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

	if (nat_info == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (nat_info->size != sizeof(NetCtlNatInfo)) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	nat_info->stunStatus        = 1;
	nat_info->natType           = 3;
	nat_info->mappedAddr.s_addr = 0x7f000001;

	return OK;
}

int KYTY_SYSV_ABI NetCtlCheckCallback()
{
	PRINT_NAME();

	return OK;
}

int KYTY_SYSV_ABI NetCtlGetState(int* state)
{
	PRINT_NAME();

	if (state == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	*state = 0; // Disconnected

	return OK;
}

int KYTY_SYSV_ABI NetCtlRegisterCallback(NetCtlCallback func, void* /*arg*/, int* cid)
{
	PRINT_NAME();

	if (func == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }
	if (cid == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	*cid = 1;

	return OK;
}

int KYTY_SYSV_ABI NetCtlGetInfo(int code, NetCtlInfo* info)
{
	PRINT_NAME();

	if (info == nullptr) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

	KYTY_LOG_DEBUG("\t code = %d\n", code);

	switch (code)
	{
		case 2: memset(info->ether_addr.data, 0, sizeof(info->ether_addr.data)); break;
		case 11: info->ip_config = 0; break;
		case 14: strcpy(info->ip_address, "127.0.0.1"); break;
		default: EXIT("unknown code: %d\n", code);
	}

	return OK;
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
