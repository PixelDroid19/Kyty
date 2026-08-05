#ifndef INCLUDE_KYTY_SCRIPTS_BUILDTOOLS_H_
#define INCLUDE_KYTY_SCRIPTS_BUILDTOOLS_H_

#include "Kyty/Core/Subsystems.h"

namespace Kyty::BuildTools {

class BuildToolsSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<BuildToolsSubsystem>::Instance(); }
	const char*       Id() override { return "BuildTools"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


} // namespace Kyty::BuildTools

#endif /* INCLUDE_KYTY_SCRIPTS_BUILDTOOLS_H_ */
