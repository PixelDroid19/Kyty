#include "Kyty/UnitTest.h"

namespace Kyty::UnitTest {

KYTY_SUBSYSTEM_INIT(UnitTest) {}
KYTY_SUBSYSTEM_UNEXPECTED_SHUTDOWN(UnitTest) {}
KYTY_SUBSYSTEM_DESTROY(UnitTest) {}

bool unit_test_all()
{
	return true;
}

} // namespace Kyty::UnitTest
