#ifndef INCLUDE_KYTY_CORE_SUBSYSTEMS_H_
#define INCLUDE_KYTY_CORE_SUBSYSTEMS_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/Singleton.h"

#include <initializer_list>

namespace Kyty::Core {

class Subsystem;
class SubsystemsListPrivate;
class SubsystemPrivate;

class SubsystemsList
{
public:
	SubsystemsList();
	virtual ~SubsystemsList();
	void SetArgs(int argc, char* argv[]);

	// void Add(Subsystem* s, const char* name, ...);
	void Add(Subsystem* s, std::initializer_list<Subsystem*> deps);

	bool InitAll(bool print_msg = false);
	void DestroyAll(bool print_msg = false);

	int*   GetArgc();
	char** GetArgv();

	[[nodiscard]] const char* GetFailName() const;
	[[nodiscard]] const char* GetFailMsg() const;

	void ShutdownAll();

	// The runtime owns its lifecycle list explicitly.  Instance() remains as a
	// source-compatible fallback for legacy callers and tests, but fatal paths
	// and host integrations should use the active list installed by the owner.
	[[nodiscard]] static SubsystemsList* Active() noexcept;
	static void                       SetActive(SubsystemsList* list) noexcept;

	static SubsystemsList* Instance() { return Core::Singleton<SubsystemsList>::Instance(); }

	KYTY_CLASS_NO_COPY(SubsystemsList);

private:
	SubsystemsListPrivate* m_p;
};

using SubsystemsListSingleton = Kyty::Core::Singleton<SubsystemsList>;

class ScopedSubsystemsList
{
public:
	explicit ScopedSubsystemsList(SubsystemsList& list) noexcept;
	~ScopedSubsystemsList();

	KYTY_CLASS_NO_COPY(ScopedSubsystemsList);

private:
	SubsystemsList* m_previous {nullptr};
};

class Subsystem
{
public:
	Subsystem();
	virtual ~Subsystem();

	virtual const char* Id()                                       = 0;
	virtual void        Init(SubsystemsList* parent)               = 0;
	virtual void        Destroy(SubsystemsList* parent)            = 0;
	virtual void        UnexpectedShutdown(SubsystemsList* parent) = 0;

	friend class SubsystemsListPrivate;

	KYTY_CLASS_NO_COPY(Subsystem);

protected:
	void Fail(const char* format, ...) KYTY_FORMAT_PRINTF(2, 3);

private:
	SubsystemPrivate* m_p;
};

} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_CORE_SUBSYSTEMS_H_ */
