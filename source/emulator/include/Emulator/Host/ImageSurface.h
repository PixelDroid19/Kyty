#ifndef EMULATOR_INCLUDE_EMULATOR_HOST_IMAGESURFACE_H_
#define EMULATOR_INCLUDE_EMULATOR_HOST_IMAGESURFACE_H_

#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include <cstdint>

namespace Kyty::Emulator::Host {

enum class HostImageSurfaceOrder: uint8_t
{
	Unknown,
	Rgb,
	Rgba,
	Argb,
	Bgr,
	Bgra,
	Abgr,
	Alpha,
};

struct HostImageSurfaceMetadata
{
	uint32_t              width           = 0;
	uint32_t              height          = 0;
	uint32_t              pitch           = 0;
	int                   bits_per_pixel  = 0;
	int                   bytes_per_pixel = 0;
	HostImageSurfaceOrder order           = HostImageSurfaceOrder::Unknown;
};

struct HostImageSurfaceRect
{
	uint32_t x      = 0;
	uint32_t y      = 0;
	uint32_t width  = 0;
	uint32_t height = 0;
};

class HostImageSurfacePrivate;

// Owns a host image surface without exposing its SDL implementation to
// emulator-facing callers. Native access is deliberately untyped and kept for
// compatibility with the existing window icon path only.
class HostImageSurface
{
public:
	static HostImageSurface* LoadPng(const Core::String& file_name);
	static HostImageSurface* Create(uint32_t width, uint32_t height, int bits_per_pixel);
	// Adopts exclusive ownership of native_surface. The caller must relinquish
	// it and must not free it after this call.
	static HostImageSurface* FromNative(void* native_surface);

	virtual ~HostImageSurface();

	[[nodiscard]] HostImageSurface* Clone() const;
	void                            SaveBmp(const Core::String& file_name) const;
	[[nodiscard]] bool              BlitTo(HostImageSurface* destination, const HostImageSurfaceRect& source,
	                                       const HostImageSurfaceRect& target) const;

	[[nodiscard]] const HostImageSurfaceMetadata& GetMetadata() const;
	[[nodiscard]] const void*                     GetPixels() const;
	[[nodiscard]] void*                           GetPixels();
	[[nodiscard]] void*                           GetNativeHandle() const;

	void DbgPrint(const Core::String& name) const;

	KYTY_CLASS_NO_COPY(HostImageSurface);

private:
	explicit HostImageSurface(HostImageSurfacePrivate* surface);
	static HostImageSurface* CreateFromNative(void* native_surface, void* decoded_pixels);

	HostImageSurfacePrivate* m_surface = nullptr;
};

} // namespace Kyty::Emulator::Host

#endif /* EMULATOR_INCLUDE_EMULATOR_HOST_IMAGESURFACE_H_ */
