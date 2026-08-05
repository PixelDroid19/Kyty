#include "Kyty/Math/MathAll.h" // IWYU pragma: associated
#include "Kyty/Math/Rand.h"

namespace Kyty::Math {

void MathSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent)
{
	Rand::Init();
}

void MathSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}

void MathSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

} // namespace Kyty::Math
