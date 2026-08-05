#include "Kyty/UnitTest.h"

namespace Kyty::UnitTest {

void UnitTestSubsystem::Init([[maybe_unused]] Core::SubsystemsList* parent) {}
void UnitTestSubsystem::UnexpectedShutdown([[maybe_unused]] Core::SubsystemsList* parent) {}
void UnitTestSubsystem::Destroy([[maybe_unused]] Core::SubsystemsList* parent) {}

bool unit_test_all()
{
	return true;
}

} // namespace Kyty::UnitTest
