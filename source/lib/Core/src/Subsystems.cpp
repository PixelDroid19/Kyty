#include "Kyty/Core/Subsystems.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/SafeDelete.h"
#include "Kyty/Sys/SysStdio.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

namespace Kyty::Core {

namespace {

std::atomic<SubsystemsList*> g_active_subsystems {nullptr};

} // namespace

class SubsystemPrivate
{
public:
	SubsystemPrivate() = default;

	virtual ~SubsystemPrivate()
	{
		if (fail_msg != nullptr)
		{
			DeleteArray(fail_msg);
		}
	}

	KYTY_CLASS_NO_COPY(SubsystemPrivate);

	bool  failed {false};
	char* fail_msg {nullptr};
};

class SubsystemsListPrivate
{
public:
	explicit SubsystemsListPrivate(SubsystemsList* p): parent(p) {}

	virtual ~SubsystemsListPrivate()
	{
		SubsListStruct* n = list;

		for (;;)
		{
			if (n == nullptr)
			{
				break;
			}

			SubsListStruct* nn = n;

			DepsListStruct* dl = n->deps;

			for (;;)
			{
				if (dl == nullptr)
				{
					break;
				}

				DepsListStruct* ddl = dl;

				dl = dl->next;

				Delete(ddl);
			}

			n = n->next;

			Delete(nn);
		}
	}

	void SetArgs(int argc, char** argv)
	{
		this->m_argc = argc;
		this->m_argv = argv;
	}

	void Add(Subsystem* s, std::initializer_list<Subsystem*> deps)
	{
		EXIT_IF(!s);

		const char* name = s->Id();

		EXIT_IF(FindByName(name) != nullptr);

		auto* nl = new SubsListStruct;

		nl->s         = s;
		nl->name      = name;
		nl->deps      = nullptr;
		nl->next      = list;
		nl->prev_init = nullptr;

		list = nl;

		for (auto* dep: deps)
		{
			EXIT_IF(!dep);

			const char* str = dep->Id();

			auto* l     = new DepsListStruct;
			l->dep_name = str;
			l->next     = nl->deps;

			nl->deps = l;
		}

		nl->initialized = false;
	}

	bool InitAll(bool print_msg)
	{
		fail_msg = nullptr;
		fail_name = nullptr;
		dependency_error.clear();

		// Resolve the complete pending graph before calling guest-facing Init
		// hooks. A malformed graph must have no partial lifecycle side effects.
		if (!ValidateDependencyGraph())
		{
			return false;
		}

		for (;;)
		{
			SubsListStruct* n = FindNextToInitialize();

			if (n == nullptr)
			{
				// A non-empty remainder means that the dependency graph is not
				// satisfiable. Historically this path returned success, leaving a
				// partially initialized process with no actionable diagnostic.
				SubsListStruct* blocked = FindFirstUninitialized();
				if (blocked != nullptr)
				{
					SetDependencyFailure(blocked);
					return false;
				}
				break;
			}

			n->s->Init(parent);

			if (n->s->m_p->failed)
			{
				fail_msg  = n->s->m_p->fail_msg;
				fail_name = n->name;
				return false;
			}

			if (print_msg)
			{
				printf("Initialized: %s\n", n->name);
			}

			n->initialized = true;

			SubsListStruct* last = last_init;
			last_init            = n;
			n->prev_init         = last;
		}

		return true;
	}

	void DestroyAll(bool print_msg)
	{
		SubsListStruct* n = last_init;

		for (;;)
		{
			if (n == nullptr)
			{
				break;
			}

			n->s->Destroy(parent);
			n->initialized = false;

			if (print_msg)
			{
				printf("Destroyed: %s\n", n->name);
			}

			n = n->prev_init;
		}

		last_init = nullptr;
	}

	void ShutdownAll()
	{
		SubsListStruct* n = last_init;

		for (;;)
		{
			if (n == nullptr)
			{
				break;
			}

			n->s->UnexpectedShutdown(parent);
			n->initialized = false;

			n = n->prev_init;
		}

		last_init = nullptr;
	}

	struct DepsListStruct
	{
		const char*     dep_name;
		DepsListStruct* next;
	};

	struct SubsListStruct
	{
		Subsystem*      s;
		const char*     name;
		DepsListStruct* deps;
		SubsListStruct* next;
		SubsListStruct* prev_init;
		bool            initialized;
	};

	[[nodiscard]] SubsListStruct* FindByName(const char* name) const
	{
		SubsListStruct* n = list;

		for (;;)
		{
			if ((n == nullptr) || std::strcmp(n->name, name) == 0)
			{
				break;
			}

			n = n->next;
		}

		return n;
	}

	[[nodiscard]] SubsListStruct* FindNextToInitialize() const
	{
		SubsListStruct* n = list;

		for (;;)
		{
			if (n == nullptr)
			{
				break;
			}

			if (!n->initialized)
			{
				DepsListStruct* d = n->deps;

				for (;;)
				{
					if (d == nullptr)
					{
						break;
					}

					SubsListStruct* s = FindByName(d->dep_name);

					if ((s == nullptr) || !s->initialized)
					{
						break;
					}

					d = d->next;
				}

				if (d == nullptr)
				{
					return n;
				}
			}

			n = n->next;
		}

		return nullptr;
	}

	[[nodiscard]] SubsListStruct* FindFirstUninitialized() const
	{
		SubsListStruct* n = list;

		while (n != nullptr)
		{
			if (!n->initialized)
			{
				return n;
			}

			n = n->next;
		}

		return nullptr;
	}

	void SetDependencyFailure(SubsListStruct* blocked)
	{
		EXIT_IF(!blocked);

		// Prefer the concrete missing edge even when the first blocked node is
		// only transitively dependent on it. This keeps diagnostics deterministic
		// for graphs such as consumer -> producer -> missing.
		for (SubsListStruct* node = list; node != nullptr; node = node->next)
		{
			if (node->initialized)
			{
				continue;
			}

			for (const DepsListStruct* dep = node->deps; dep != nullptr; dep = dep->next)
			{
				if (FindByName(dep->dep_name) == nullptr)
				{
					dependency_error = "missing dependency '";
					dependency_error += dep->dep_name;
					dependency_error += "' required by '";
					dependency_error += node->name;
					dependency_error += "'";
					fail_name = node->name;
					fail_msg  = dependency_error.c_str();
					return;
				}
			}
		}

		dependency_error = "dependency cycle or blocked dependency at '";
		dependency_error += blocked->name;
		dependency_error += "'";
		fail_name = blocked->name;
		fail_msg  = dependency_error.c_str();
	}

	[[nodiscard]] bool ValidateDependencyGraph()
	{
		SubsListStruct* first_uninitialized = FindFirstUninitialized();
		if (first_uninitialized == nullptr)
		{
			return true;
		}

		// Report a missing edge before looking for cycles so transitive failures
		// are not misclassified as a cycle.
		for (SubsListStruct* node = list; node != nullptr; node = node->next)
		{
			if (node->initialized)
			{
				continue;
			}

			for (const DepsListStruct* dep = node->deps; dep != nullptr; dep = dep->next)
			{
				if (FindByName(dep->dep_name) == nullptr)
				{
					SetDependencyFailure(node);
					return false;
				}
			}
		}

		size_t pending_count = 0;
		for (SubsListStruct* node = list; node != nullptr; node = node->next)
		{
			if (!node->initialized)
			{
				pending_count++;
			}
		}

		std::vector<SubsListStruct*> planned;
		for (;;)
		{
			bool progress = false;
			for (SubsListStruct* node = list; node != nullptr; node = node->next)
			{
				if (node->initialized || std::find(planned.begin(), planned.end(), node) != planned.end())
				{
					continue;
				}

				bool dependencies_ready = true;
				for (const DepsListStruct* dep = node->deps; dep != nullptr; dep = dep->next)
				{
					const SubsListStruct* dependency = FindByName(dep->dep_name);
					if (dependency == nullptr ||
					    (!dependency->initialized && std::find(planned.begin(), planned.end(), dependency) == planned.end()))
					{
						dependencies_ready = false;
						break;
					}
				}

				if (dependencies_ready)
				{
					planned.push_back(node);
					progress = true;
				}
			}

			if (!progress)
			{
				break;
			}
		}

		if (planned.size() != pending_count)
		{
			SetDependencyFailure(first_uninitialized);
			return false;
		}

		return true;
	}

	KYTY_CLASS_NO_COPY(SubsystemsListPrivate);

	SubsListStruct* list      = nullptr;
	SubsListStruct* last_init = nullptr;
	int             m_argc    = 0;
	char**          m_argv    = nullptr;
	const char*     fail_msg  = nullptr;
	const char*     fail_name = nullptr;
	std::string     dependency_error;
	SubsystemsList* parent;
};

// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
SubsystemsList::SubsystemsList(): m_p(static_cast<SubsystemsListPrivate*>(std::malloc(sizeof(SubsystemsListPrivate))))
{
	new (m_p) SubsystemsListPrivate(this);
}

SubsystemsList::~SubsystemsList()
{
	m_p->~SubsystemsListPrivate();
	// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
	std::free(m_p);
	// delete p;
}

void SubsystemsList::Add(Subsystem* s, std::initializer_list<Subsystem*> deps)
{
	m_p->Add(s, deps);
}

bool SubsystemsList::InitAll(bool print_msg)
{
	return m_p->InitAll(print_msg);
}

void SubsystemsList::DestroyAll(bool print_msg)
{
	m_p->DestroyAll(print_msg);
}

int* SubsystemsList::GetArgc()
{
	return &m_p->m_argc;
}

char** SubsystemsList::GetArgv()
{
	return m_p->m_argv;
}

// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
Subsystem::Subsystem(): m_p(static_cast<SubsystemPrivate*>(std::malloc(sizeof(SubsystemPrivate))))
{
	new (m_p) SubsystemPrivate;
}

Subsystem::~Subsystem()
{
	m_p->~SubsystemPrivate();
	// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
	std::free(m_p);
	// delete p;
}

void Subsystem::Fail(const char* format, ...)
{
	va_list args {};
	va_start(args, format);

	uint32_t len = sys_vscprintf(format, args);

	if (len != 0)
	{
		// NOLINTNEXTLINE(cppcoreguidelines-no-malloc,hicpp-no-malloc)
		char* d = static_cast<char*>(std::malloc(len + 1));
		std::memset(d, 0, len + 1);
		/*len = */ sys_vsnprintf(d, len, format, args);
		m_p->fail_msg = d;
		m_p->failed   = true;
	}

	va_end(args);
}

const char* SubsystemsList::GetFailName() const
{
	return m_p->fail_name;
}

const char* SubsystemsList::GetFailMsg() const
{
	return m_p->fail_msg;
}

void SubsystemsList::SetArgs(int argc, char* argv[])
{
	m_p->SetArgs(argc, argv);
}

void SubsystemsList::ShutdownAll()
{
	m_p->ShutdownAll();
}

SubsystemsList* SubsystemsList::Active() noexcept
{
	return g_active_subsystems.load(std::memory_order_acquire);
}

void SubsystemsList::SetActive(SubsystemsList* list) noexcept
{
	g_active_subsystems.store(list, std::memory_order_release);
}

ScopedSubsystemsList::ScopedSubsystemsList(SubsystemsList& list) noexcept: m_previous(SubsystemsList::Active())
{
	SubsystemsList::SetActive(&list);
}

ScopedSubsystemsList::~ScopedSubsystemsList()
{
	SubsystemsList::SetActive(m_previous);
}

} // namespace Kyty::Core
