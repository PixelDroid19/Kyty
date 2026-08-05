#ifndef INCLUDE_KYTY_GAME_SDLSUBSYSTEM_H_
#define INCLUDE_KYTY_GAME_SDLSUBSYSTEM_H_

#include "Kyty/Core/Subsystems.h"

namespace Kyty::Core {

class SDLSubsystem: public Core::Subsystem
{
public:
	static Subsystem* Instance() { return Core::Singleton<SDLSubsystem>::Instance(); }
	const char*       Id() override { return "SDL"; }
	void              Init(Core::SubsystemsList* parent) override;
	void              Destroy(Core::SubsystemsList* parent) override;
	void              UnexpectedShutdown(Core::SubsystemsList* parent) override;
};


} // namespace Kyty::Core

#endif /* INCLUDE_KYTY_GAME_SDLSUBSYSTEM_H_ */
