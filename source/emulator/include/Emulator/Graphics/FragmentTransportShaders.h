#pragma once

#include <cstddef>
#include <cstdint>

namespace Kyty::Libs::Graphics::FragmentTransport {

enum class Kernel
{
	Scan,
	Pack
};

struct Binary
{
	const uint32_t* words;
	size_t          word_count;
};

[[nodiscard]] Binary GetKernelBinary(Kernel kernel) noexcept;

} // namespace Kyty::Libs::Graphics::FragmentTransport
