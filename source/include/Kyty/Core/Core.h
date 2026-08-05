#ifndef INCLUDE_KYTY_CORE_CORE_H_
#define INCLUDE_KYTY_CORE_CORE_H_

#include "Kyty/Core/Subsystems.h"

namespace Kyty::Core {

class CoreSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<CoreSubsystem>::Instance(); }
	const char*       Id() override { return "Core"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_CORE_CORE_H_ */
