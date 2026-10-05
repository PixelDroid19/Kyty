#pragma once

#include <vector>

namespace Kyty::Libs::LibC {

// A C++ runtime object the guest imports by its mangled name.
struct CxxRttiObject
{
	const char* symbol;
	const void* object;
};

// The type_info objects the C++ runtime defines for every fundamental type T,
// T* and const T* (_ZTI<T>, _ZTIP<T>, _ZTIPK<T>), and the vtables of its
// fundamental and enum type_info classes and of std::bad_weak_ptr.
[[nodiscard]] const std::vector<CxxRttiObject>& CxxRttiObjects();

} // namespace Kyty::Libs::LibC
