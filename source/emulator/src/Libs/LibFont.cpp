#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <cinttypes>
#include <cstdint>
#include <cstring>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Font", 1, "Font", 1, 1);

namespace Font {

// Guest SceFontMemory descriptor filled by sceFontMemoryInit. Layout matches
// the public Prospero font memory handle used by CreateLibrary / renderer paths.
struct FontMemory
{
	uint16_t type;
	uint16_t attr;
	uint32_t size;
	void*    address;
	void*    mspace_object;
	const void* mem_interface;
	void*    destroy_callback;
	void*    destroy_object;
	void*    user_object;
	void*    parent_object;
};

// sceFontMemoryInit — NID whrS4oksXc4
// (font_memory, address, size_byte, mem_interface, mspace_object,
//  destroy_callback, destroy_object). Observed size 0x800000 with mspace from
// sceLibcMspaceCreate. Fills the descriptor only; allocation is guest-owned.
static int KYTY_SYSV_ABI FontMemoryInit(FontMemory* font_memory, void* address, uint32_t size_byte,
                                        const void* mem_interface, void* mspace_object,
                                        void* destroy_callback, void* destroy_object)
{
	PRINT_NAME();
	printf("\t font_memory      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_memory));
	printf("\t address          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	printf("\t size_byte        = 0x%" PRIx32 "\n", size_byte);
	printf("\t mem_interface    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mem_interface));
	printf("\t mspace_object    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mspace_object));
	printf("\t destroy_callback = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(destroy_callback));
	printf("\t destroy_object   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(destroy_object));

	if (font_memory == nullptr)
	{
		return -1;
	}

	std::memset(font_memory, 0, sizeof(FontMemory));
	font_memory->type             = 1;
	font_memory->attr             = 0;
	font_memory->size             = size_byte;
	font_memory->address          = address;
	font_memory->mspace_object    = mspace_object;
	font_memory->mem_interface    = mem_interface;
	font_memory->destroy_callback = destroy_callback;
	font_memory->destroy_object   = destroy_object;
	return OK;
}

// sceFontMemoryTerm — NID h6hIgxXEiEc
static int KYTY_SYSV_ABI FontMemoryTerm(FontMemory* font_memory)
{
	PRINT_NAME();
	if (font_memory != nullptr)
	{
		font_memory->type = 0;
	}
	return OK;
}

struct LibraryState
{
	const FontMemory* memory;
	void*             selection;
	uint64_t          edition;
};

// sceFontCreateLibraryWithEdition — NID n590hj5Oe-k
// (memory, selection, edition, library_out)
static int KYTY_SYSV_ABI FontCreateLibraryWithEdition(const FontMemory* memory, void* selection,
                                                      uint64_t edition, void** library)
{
	PRINT_NAME();
	printf("\t memory    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
	printf("\t selection = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(selection));
	printf("\t edition   = 0x%016" PRIx64 "\n", edition);
	printf("\t library   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	if (library == nullptr)
	{
		return -1;
	}
	auto* state     = new LibraryState {};
	state->memory   = memory;
	state->selection = selection;
	state->edition  = edition;
	*library        = state;
	return OK;
}

// sceFontCreateLibrary — NID nWrfPI4Okmg
static int KYTY_SYSV_ABI FontCreateLibrary(const FontMemory* memory, void* selection, void** library)
{
	return FontCreateLibraryWithEdition(memory, selection, 0, library);
}

// sceFontDestroyLibrary — NID FXP359ygujs
static int KYTY_SYSV_ABI FontDestroyLibrary(void** library)
{
	PRINT_NAME();
	if (library != nullptr && *library != nullptr)
	{
		delete static_cast<LibraryState*>(*library);
		*library = nullptr;
	}
	return OK;
}

// sceFontSupportSystemFonts / ExternalFonts — return OK so library setup continues.
static int KYTY_SYSV_ABI FontSupportSystemFonts(void* library)
{
	PRINT_NAME();
	printf("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	return OK;
}

static int KYTY_SYSV_ABI FontSupportExternalFonts(void* library)
{
	PRINT_NAME();
	printf("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	return OK;
}

// sceFontAttachDeviceCacheBuffer — NID CUKn5pX-NVY (library, buffer, size).
// Cache is optional for boot; accept guest buffer (null buffer observed with size).
static int KYTY_SYSV_ABI FontAttachDeviceCacheBuffer(void* library, void* buffer, uint32_t size)
{
	PRINT_NAME();
	printf("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	printf("\t buffer  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer));
	printf("\t size    = 0x%" PRIx32 "\n", size);
	return OK;
}

struct RendererState
{
	const FontMemory* memory;
	void*             selection;
	uint64_t          edition;
};

// Opaque font handle returned by OpenFontSet / OpenFontMemory / OpenFontInstance.
// Layout is host-owned; guest only treats the pointer as an opaque FontHandle.
// Glyph rasterization is synthetic (filled rectangle) until a real face decoder
// is wired; that is enough for boot layout and non-null render results.
static constexpr uint32_t kFontGlyphMaxDim = 64;

struct FontTransImage
{
	uint8_t* address;
	uint32_t width_byte;
	uint32_t image_width;
	uint32_t image_height;
};

struct FontHandleState
{
	void*       library;
	const void* data;
	uint32_t    size;
	uint32_t    font_set_type;
	uint32_t    open_mode;
	int         attribute;
	void*       renderer;
	float       scale_w;
	float       scale_h;
	float       effect_slant;
	float       effect_weight_x;
	float       effect_weight_y;
	uint32_t    effect_weight_mode;
	uint8_t     glyph_bits[kFontGlyphMaxDim * kFontGlyphMaxDim];
	FontTransImage trans_image;
};

static int OpenFontHandle(void* library, const void* data, uint32_t size, uint32_t font_set_type,
                          uint32_t open_mode, void** font_out)
{
	if (font_out == nullptr)
	{
		return -1;
	}
	auto* handle               = new FontHandleState {};
	handle->library            = library;
	handle->data               = data;
	handle->size               = size;
	handle->font_set_type      = font_set_type;
	handle->open_mode          = open_mode;
	handle->attribute          = 0;
	handle->renderer           = nullptr;
	// Default pixel scale used by later SetScalePixel / layout paths.
	handle->scale_w            = 8.0f;
	handle->scale_h            = 16.0f;
	handle->effect_slant       = 0.0f;
	handle->effect_weight_x    = 0.0f;
	handle->effect_weight_y    = 0.0f;
	handle->effect_weight_mode = 0;
	*font_out                  = handle;
	return OK;
}

// sceFontOpenFontSet — NID cKYtVmeSTcw
// ABI: (library, font_set_type, open_mode, detail*, handle*). detail may be null.
// Observed Astro: font_set_type=0x180700c7, open_mode=2, detail=null.
static int KYTY_SYSV_ABI FontOpenFontSet(void* library, uint32_t font_set_type, uint32_t open_mode,
                                         const void* detail, void** font_out)
{
	PRINT_NAME();
	printf("\t library       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	printf("\t font_set_type = 0x%" PRIx32 "\n", font_set_type);
	printf("\t open_mode     = 0x%" PRIx32 "\n", open_mode);
	printf("\t detail        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(detail));
	printf("\t font_out      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_out));
	return OpenFontHandle(library, detail, 0, font_set_type, open_mode, font_out);
}

// sceFontOpenFontMemory — NID KXUpebrFk1U
// After package font APR resolve, Astro opens guest-mapped OTF bytes:
// (library, font_address, font_size, detail*, handle*). Observed size 0x13534 (Cobe-Heavy.otf).
static int KYTY_SYSV_ABI FontOpenFontMemory(void* library, const void* font_address, uint32_t font_size,
                                            const void* detail, void** font_out)
{
	PRINT_NAME();
	printf("\t library      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	printf("\t font_address = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_address));
	printf("\t font_size    = 0x%" PRIx32 "\n", font_size);
	printf("\t detail       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(detail));
	printf("\t font_out     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_out));
	(void)detail;
	return OpenFontHandle(library, font_address, font_size, 0, 0, font_out);
}

// sceFontOpenFontInstance — NID JzCH3SCFnAU (source may be null; setup_font optional).
static int KYTY_SYSV_ABI FontOpenFontInstance(void* font_handle, void* setup_font, void** font_out)
{
	PRINT_NAME();
	printf("\t font_handle = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_handle));
	printf("\t setup_font  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(setup_font));
	printf("\t font_out    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_out));
	if (font_handle == nullptr)
	{
		return OpenFontHandle(nullptr, setup_font, 0, 0, 0, font_out);
	}
	const auto* source = static_cast<const FontHandleState*>(font_handle);
	return OpenFontHandle(source->library, source->data, source->size, source->font_set_type,
	                      source->open_mode, font_out);
}

// sceFontCloseFont — NID vzHs3C8lWJk
static int KYTY_SYSV_ABI FontCloseFont(void* font_handle)
{
	PRINT_NAME();
	delete static_cast<FontHandleState*>(font_handle);
	return OK;
}

// sceFontCreateRendererWithEdition — NID WaSFJoRWXaI
// (memory, selection, edition, renderer_out). Observed Astro after OpenFontMemory.
static int KYTY_SYSV_ABI FontCreateRendererWithEdition(const FontMemory* memory, void* selection,
                                                       uint64_t edition, void** renderer)
{
	PRINT_NAME();
	printf("\t memory    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
	printf("\t selection = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(selection));
	printf("\t edition   = 0x%016" PRIx64 "\n", edition);
	printf("\t renderer  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(renderer));
	if (renderer == nullptr)
	{
		return -1;
	}
	auto* state      = new RendererState {};
	state->memory    = memory;
	state->selection = selection;
	state->edition   = edition;
	*renderer        = state;
	return OK;
}

// sceFontDestroyRenderer — NID exAxkyVLt0s
static int KYTY_SYSV_ABI FontDestroyRenderer(void** renderer)
{
	PRINT_NAME();
	if (renderer != nullptr && *renderer != nullptr)
	{
		delete static_cast<RendererState*>(*renderer);
		*renderer = nullptr;
	}
	return OK;
}

// sceFontBindRenderer — NID 3OdRkSjOcog
static int KYTY_SYSV_ABI FontBindRenderer(void* font_handle, void* renderer)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return -1;
	}
	font->renderer = renderer;
	printf("\t handle   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_handle));
	printf("\t renderer = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(renderer));
	return OK;
}

// sceFontUnbindRenderer — NID 1QjhKxrsOB8
static int KYTY_SYSV_ABI FontUnbindRenderer(void* font_handle)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return -1;
	}
	font->renderer = nullptr;
	return OK;
}

// sceFontRebindRenderer — NID Z2cdsqJH+5k
static int KYTY_SYSV_ABI FontRebindRenderer(void* font_handle)
{
	PRINT_NAME();
	if (font_handle == nullptr)
	{
		return -1;
	}
	return OK;
}

// sceFontSetScalePixel — NID N1EBMeGhf7E
static int KYTY_SYSV_ABI FontSetScalePixel(void* font_handle, float w, float h)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return -1;
	}
	font->scale_w = w;
	font->scale_h = h;
	printf("\t handle = 0x%016" PRIx64 " w=%f h=%f\n", reinterpret_cast<uint64_t>(font_handle),
	       static_cast<double>(w), static_cast<double>(h));
	return OK;
}

// sceFontSetEffectSlant — NID TMtqoFQjjbA
static int KYTY_SYSV_ABI FontSetEffectSlant(void* font_handle, float slant_ratio)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return -1;
	}
	font->effect_slant = slant_ratio;
	return OK;
}

// sceFontSetEffectWeight — NID v0phZwa4R5o
static int KYTY_SYSV_ABI FontSetEffectWeight(void* font_handle, float weight_x, float weight_y, uint32_t mode)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return -1;
	}
	font->effect_weight_x    = weight_x;
	font->effect_weight_y    = weight_y;
	font->effect_weight_mode = mode;
	return OK;
}

// sceFontDefineAttribute — NID 8h-SOB-asgk
static int KYTY_SYSV_ABI FontDefineAttribute(void* font_handle, int attribute, int* old_attribute)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (old_attribute != nullptr)
	{
		*old_attribute = (font != nullptr ? font->attribute : 0);
	}
	if (font != nullptr)
	{
		font->attribute = attribute;
	}
	return OK;
}

// Guest layout descriptors (three floats each). Values are synthetic until a
// real face rasterizer is wired; must be non-zero so text layout continues.
struct FontHorizontalLayout
{
	float base_line_y;
	float line_height;
	float effect_height;
};

struct FontVerticalLayout
{
	float base_line_x;
	float line_width;
	float effect_width;
};

// sceFontGetHorizontalLayout — NID imxVx8lm+KM
static int KYTY_SYSV_ABI FontGetHorizontalLayout(void* font_handle, FontHorizontalLayout* layout)
{
	PRINT_NAME();
	if (font_handle == nullptr || layout == nullptr)
	{
		return -1;
	}
	const auto* font  = static_cast<const FontHandleState*>(font_handle);
	const float height = (font->scale_h > 0.0f ? font->scale_h : 16.0f);
	layout->base_line_y   = height * 0.75f;
	layout->line_height   = height;
	layout->effect_height = height;
	printf("\t handle = 0x%016" PRIx64 " layout=%p h=%f\n", reinterpret_cast<uint64_t>(font_handle),
	       static_cast<void*>(layout), static_cast<double>(height));
	return OK;
}

// sceFontGetVerticalLayout — NID 3BrWWFU+4ts
static int KYTY_SYSV_ABI FontGetVerticalLayout(void* font_handle, FontVerticalLayout* layout)
{
	PRINT_NAME();
	if (font_handle == nullptr || layout == nullptr)
	{
		return -1;
	}
	const auto* font = static_cast<const FontHandleState*>(font_handle);
	const float width = (font->scale_w > 0.0f ? font->scale_w : 16.0f);
	layout->base_line_x  = 0.0f;
	layout->line_width   = width;
	layout->effect_width = width;
	return OK;
}

// sceFontSetupRenderScalePixel — NID 6vGCkkQJOcI (stores render-time scale; same fields as scale for now).
static int KYTY_SYSV_ABI FontSetupRenderScalePixel(void* font_handle, float w, float h)
{
	return FontSetScalePixel(font_handle, w, h);
}

// sceFontSetupRenderEffectSlant — NID lz9y9UFO2UU
static int KYTY_SYSV_ABI FontSetupRenderEffectSlant(void* font_handle, float slant_ratio)
{
	return FontSetEffectSlant(font_handle, slant_ratio);
}

// sceFontSetupRenderEffectWeight — NID XIGorvLusDQ
static int KYTY_SYSV_ABI FontSetupRenderEffectWeight(void* font_handle, float weight_x, float weight_y,
                                                     uint32_t mode)
{
	return FontSetEffectWeight(font_handle, weight_x, weight_y, mode);
}

// Guest glyph metrics (width/height + horizontal/vertical bearings). Synthetic
// until a face rasterizer is bound; non-zero advances keep text layout moving.
struct FontGlyphMetrics
{
	float width;
	float height;
	struct
	{
		float bearing_x;
		float bearing_y;
		float advance;
	} horizontal;
	struct
	{
		float bearing_x;
		float bearing_y;
		float advance;
	} vertical;
};

static void FillSyntheticGlyphMetrics(const FontHandleState* font, FontGlyphMetrics* metrics)
{
	if (metrics == nullptr)
	{
		return;
	}
	const float width  = (font != nullptr && font->scale_w > 0.0f) ? font->scale_w : 8.0f;
	const float height = (font != nullptr && font->scale_h > 0.0f) ? font->scale_h : 16.0f;
	metrics->width                = width;
	metrics->height               = height;
	metrics->horizontal.bearing_x = 0.0f;
	metrics->horizontal.bearing_y = height * 0.75f;
	metrics->horizontal.advance   = width;
	metrics->vertical.bearing_x   = 0.0f;
	metrics->vertical.bearing_y   = 0.0f;
	metrics->vertical.advance     = height;
}

// sceFontGetRenderCharGlyphMetrics — NID IQtleGLL5pQ (handle, code, metrics*)
static int KYTY_SYSV_ABI FontGetRenderCharGlyphMetrics(void* font_handle, uint32_t code,
                                                       FontGlyphMetrics* metrics)
{
	PRINT_NAME();
	printf("\t handle = 0x%016" PRIx64 " code=0x%" PRIx32 " metrics=0x%016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(font_handle), code, reinterpret_cast<uint64_t>(metrics));
	FillSyntheticGlyphMetrics(static_cast<const FontHandleState*>(font_handle), metrics);
	return OK;
}

// sceFontGetCharGlyphMetrics — NID L97d+3OgMlE
static int KYTY_SYSV_ABI FontGetCharGlyphMetrics(void* font_handle, uint32_t code, FontGlyphMetrics* metrics)
{
	return FontGetRenderCharGlyphMetrics(font_handle, code, metrics);
}

// sceFontRenderSurfaceInit — NID gdUCnU0gHdI
// Fills guest surface descriptor (buffer, pitch, pixel size, width, height).
struct FontRenderSurface
{
	void*    buffer;
	int32_t  width_byte;
	int8_t   pixel_size_byte;
	uint8_t  system_ext0;
	uint8_t  system_ext1;
	uint8_t  system_ext2;
	int32_t  width;
	int32_t  height;
	struct
	{
		uint32_t x0;
		uint32_t y0;
		uint32_t x1;
		uint32_t y1;
	} scissor;
	uint32_t system_use[22];
};

static void KYTY_SYSV_ABI FontRenderSurfaceInit(FontRenderSurface* surf, void* buffer, int buf_width_byte,
                                                int pixel_size_byte, int width, int height)
{
	PRINT_NAME();
	if (surf == nullptr)
	{
		return;
	}
	std::memset(surf, 0, sizeof(FontRenderSurface));
	surf->buffer          = buffer;
	surf->width_byte      = buf_width_byte;
	surf->pixel_size_byte = static_cast<int8_t>(pixel_size_byte);
	surf->width           = width > 0 ? width : 0;
	surf->height          = height > 0 ? height : 0;
	surf->scissor.x0      = 0;
	surf->scissor.y0      = 0;
	surf->scissor.x1      = static_cast<uint32_t>(surf->width);
	surf->scissor.y1      = static_cast<uint32_t>(surf->height);
	printf("\t surf=%p buf=%p pitch=%d px=%d %dx%d\n", static_cast<void*>(surf), buffer,
	       buf_width_byte, pixel_size_byte, width, height);
}

// sceFontRenderSurfaceSetScissor — NID vRxf4d0ulPs
static int KYTY_SYSV_ABI FontRenderSurfaceSetScissor(FontRenderSurface* surf, uint32_t x0, uint32_t y0,
                                                     uint32_t x1, uint32_t y1)
{
	PRINT_NAME();
	if (surf == nullptr)
	{
		return -1;
	}
	surf->scissor.x0 = x0;
	surf->scissor.y0 = y0;
	surf->scissor.x1 = x1;
	surf->scissor.y1 = y1;
	return OK;
}

// sceFontGenerateCharGlyph — NID C-4Qw5Srlyw: return a host-owned glyph token.
struct GlyphState
{
	FontHandleState* font;
	uint32_t         code;
	int              attribute;
	FontGlyphMetrics metrics;
};

static int KYTY_SYSV_ABI FontGenerateCharGlyph(void* font_handle, uint32_t code, int attribute,
                                               void** glyph_out)
{
	PRINT_NAME();
	if (glyph_out == nullptr)
	{
		return -1;
	}
	auto* glyph       = new GlyphState {};
	glyph->font       = static_cast<FontHandleState*>(font_handle);
	glyph->code       = code;
	glyph->attribute  = attribute;
	FillSyntheticGlyphMetrics(glyph->font, &glyph->metrics);
	*glyph_out = glyph;
	printf("\t handle=0x%016" PRIx64 " code=0x%" PRIx32 " glyph=%p\n",
	       reinterpret_cast<uint64_t>(font_handle), code, static_cast<void*>(glyph));
	return OK;
}

// sceFontDeleteGlyph — NID LHDoRWVFGqk
static int KYTY_SYSV_ABI FontDeleteGlyph(void* glyph)
{
	PRINT_NAME();
	delete static_cast<GlyphState*>(glyph);
	return OK;
}

// sceFontGlyphDefineAttribute — NID 8-zmgsxkBek
static int KYTY_SYSV_ABI FontGlyphDefineAttribute(void* glyph, int attribute, int* old_attribute)
{
	PRINT_NAME();
	auto* g = static_cast<GlyphState*>(glyph);
	if (old_attribute != nullptr)
	{
		*old_attribute = (g != nullptr ? g->attribute : 0);
	}
	if (g != nullptr)
	{
		g->attribute = attribute;
	}
	return OK;
}

struct FontSurfaceImage
{
	uint8_t* address;
	uint32_t width_byte;
	uint8_t  pixel_size_byte;
	uint8_t  pixel_format;
};

struct FontGlyphImageMetrics
{
	float    bearing_x;
	float    bearing_y;
	float    advance;
	float    stride;
	uint32_t width;
	uint32_t height;
};

struct FontRenderResult
{
	const FontTransImage* trans_image;
	FontSurfaceImage      surface_image;
	struct
	{
		uint32_t x;
		uint32_t y;
		uint32_t w;
		uint32_t h;
	} update_rect;
	FontGlyphImageMetrics image_metrics;
};

static void PrepareSyntheticGlyphImage(FontHandleState* font, uint32_t /*code*/)
{
	if (font == nullptr)
	{
		return;
	}
	uint32_t width  = static_cast<uint32_t>(font->scale_w > 0.0f ? font->scale_w : 8.0f);
	uint32_t height = static_cast<uint32_t>(font->scale_h > 0.0f ? font->scale_h : 16.0f);
	if (width > kFontGlyphMaxDim)
	{
		width = kFontGlyphMaxDim;
	}
	if (height > kFontGlyphMaxDim)
	{
		height = kFontGlyphMaxDim;
	}
	if (width == 0)
	{
		width = 1;
	}
	if (height == 0)
	{
		height = 1;
	}
	// Solid filled glyph box (visible block). Real outlines can replace this later.
	std::memset(font->glyph_bits, 0, sizeof(font->glyph_bits));
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			font->glyph_bits[y * kFontGlyphMaxDim + x] = 0xffu;
		}
	}
	font->trans_image.address      = font->glyph_bits;
	font->trans_image.width_byte   = kFontGlyphMaxDim;
	font->trans_image.image_width  = width;
	font->trans_image.image_height = height;
}

static void DrawGlyphToSurface(const FontTransImage& image, FontRenderSurface* surf, float x, float y)
{
	if (surf == nullptr || surf->buffer == nullptr || surf->width <= 0 || surf->height <= 0 ||
	    surf->width_byte <= 0 || surf->pixel_size_byte <= 0 || image.address == nullptr)
	{
		return;
	}
	auto*     dst        = static_cast<uint8_t*>(surf->buffer);
	const int pixel_size = surf->pixel_size_byte;
	const int start_x    = static_cast<int>(x) > 0 ? static_cast<int>(x) : 0;
	const int start_y    = static_cast<int>(y) > 0 ? static_cast<int>(y) : 0;
	const int end_x      = start_x + static_cast<int>(image.image_width) < surf->width
	                           ? start_x + static_cast<int>(image.image_width)
	                           : surf->width;
	const int end_y      = start_y + static_cast<int>(image.image_height) < surf->height
	                           ? start_y + static_cast<int>(image.image_height)
	                           : surf->height;
	for (int yy = start_y; yy < end_y; ++yy)
	{
		for (int xx = start_x; xx < end_x; ++xx)
		{
			const uint8_t src =
			    image.address[static_cast<uint32_t>(yy - start_y) * image.width_byte +
			                  static_cast<uint32_t>(xx - start_x)];
			if (src == 0)
			{
				continue;
			}
			uint8_t* pixel = dst + yy * surf->width_byte + xx * pixel_size;
			for (int i = 0; i < pixel_size; ++i)
			{
				pixel[i] = src;
			}
		}
	}
}

// sceFontRenderCharGlyphImageHorizontal — NIDs kAenWy1Zw5o / 3G4zhgKuxE8 / i6UNdSig1uE
// (handle, code, surface, x, y, metrics*, result*).
static int KYTY_SYSV_ABI FontRenderCharGlyphImageHorizontal(void* font_handle, uint32_t code,
                                                            FontRenderSurface* surf, float x, float y,
                                                            FontGlyphMetrics* metrics,
                                                            FontRenderResult* result)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return -1;
	}
	PrepareSyntheticGlyphImage(font, code);
	FontGlyphMetrics local_metrics {};
	auto*            draw_metrics = metrics != nullptr ? metrics : &local_metrics;
	FillSyntheticGlyphMetrics(font, draw_metrics);

	if (result != nullptr)
	{
		const int top_x = static_cast<int>(x + draw_metrics->horizontal.bearing_x);
		const int top_y = static_cast<int>(y - draw_metrics->horizontal.bearing_y);
		result->trans_image                   = &font->trans_image;
		result->surface_image.address         = surf != nullptr ? static_cast<uint8_t*>(surf->buffer) : nullptr;
		result->surface_image.width_byte      = surf != nullptr && surf->width_byte > 0
		                                           ? static_cast<uint32_t>(surf->width_byte)
		                                           : 0;
		result->surface_image.pixel_size_byte =
		    surf != nullptr && surf->pixel_size_byte > 0 ? static_cast<uint8_t>(surf->pixel_size_byte) : 0;
		result->surface_image.pixel_format = 0;
		result->update_rect.x              = top_x > 0 ? static_cast<uint32_t>(top_x) : 0;
		result->update_rect.y              = top_y > 0 ? static_cast<uint32_t>(top_y) : 0;
		result->update_rect.w              = font->trans_image.image_width;
		result->update_rect.h              = font->trans_image.image_height;
		result->image_metrics.bearing_x    = 0.0f;
		result->image_metrics.bearing_y    = draw_metrics->horizontal.bearing_y;
		result->image_metrics.advance      = draw_metrics->horizontal.advance;
		result->image_metrics.stride       = draw_metrics->horizontal.advance;
		result->image_metrics.width        = font->trans_image.image_width;
		result->image_metrics.height       = font->trans_image.image_height;
	}

	const float top_x = x + draw_metrics->horizontal.bearing_x;
	const float top_y = y - draw_metrics->horizontal.bearing_y;
	DrawGlyphToSurface(font->trans_image, surf, top_x, top_y);

	printf("\t handle=0x%016" PRIx64 " code=0x%" PRIx32 " surf=%p x=%f y=%f\n",
	       reinterpret_cast<uint64_t>(font_handle), code, static_cast<void*>(surf),
	       static_cast<double>(x), static_cast<double>(y));
	return OK;
}

} // namespace Font

LIB_DEFINE(InitFont_1)
{
	LIB_FUNC("whrS4oksXc4", Font::FontMemoryInit);
	LIB_FUNC("h6hIgxXEiEc", Font::FontMemoryTerm);
	LIB_FUNC("nWrfPI4Okmg", Font::FontCreateLibrary);
	LIB_FUNC("n590hj5Oe-k", Font::FontCreateLibraryWithEdition);
	LIB_FUNC("FXP359ygujs", Font::FontDestroyLibrary);
	LIB_FUNC("SsRbbCiWoGw", Font::FontSupportSystemFonts);
	LIB_FUNC("mz2iTY0MK4A", Font::FontSupportExternalFonts);
	LIB_FUNC("CUKn5pX-NVY", Font::FontAttachDeviceCacheBuffer);
	// Gen5 Font_v1 open + renderer + layout + glyph metrics for Astro font bring-up.
	LIB_FUNC("cKYtVmeSTcw", Font::FontOpenFontSet);
	LIB_FUNC("KXUpebrFk1U", Font::FontOpenFontMemory);
	LIB_FUNC("JzCH3SCFnAU", Font::FontOpenFontInstance);
	LIB_FUNC("vzHs3C8lWJk", Font::FontCloseFont);
	LIB_FUNC("WaSFJoRWXaI", Font::FontCreateRendererWithEdition);
	LIB_FUNC("exAxkyVLt0s", Font::FontDestroyRenderer);
	LIB_FUNC("3OdRkSjOcog", Font::FontBindRenderer);
	LIB_FUNC("1QjhKxrsOB8", Font::FontUnbindRenderer);
	LIB_FUNC("Z2cdsqJH+5k", Font::FontRebindRenderer);
	LIB_FUNC("N1EBMeGhf7E", Font::FontSetScalePixel);
	LIB_FUNC("TMtqoFQjjbA", Font::FontSetEffectSlant);
	LIB_FUNC("v0phZwa4R5o", Font::FontSetEffectWeight);
	LIB_FUNC("8h-SOB-asgk", Font::FontDefineAttribute);
	LIB_FUNC("imxVx8lm+KM", Font::FontGetHorizontalLayout);
	LIB_FUNC("3BrWWFU+4ts", Font::FontGetVerticalLayout);
	LIB_FUNC("6vGCkkQJOcI", Font::FontSetupRenderScalePixel);
	LIB_FUNC("lz9y9UFO2UU", Font::FontSetupRenderEffectSlant);
	LIB_FUNC("XIGorvLusDQ", Font::FontSetupRenderEffectWeight);
	LIB_FUNC("IQtleGLL5pQ", Font::FontGetRenderCharGlyphMetrics);
	LIB_FUNC("L97d+3OgMlE", Font::FontGetCharGlyphMetrics);
	LIB_FUNC("gdUCnU0gHdI", Font::FontRenderSurfaceInit);
	LIB_FUNC("vRxf4d0ulPs", Font::FontRenderSurfaceSetScissor);
	LIB_FUNC("C-4Qw5Srlyw", Font::FontGenerateCharGlyph);
	LIB_FUNC("LHDoRWVFGqk", Font::FontDeleteGlyph);
	LIB_FUNC("8-zmgsxkBek", Font::FontGlyphDefineAttribute);
	// Three NIDs alias the same horizontal glyph blit entry on Gen5 Font_v1.
	LIB_FUNC("kAenWy1Zw5o", Font::FontRenderCharGlyphImageHorizontal);
	LIB_FUNC("3G4zhgKuxE8", Font::FontRenderCharGlyphImageHorizontal);
	LIB_FUNC("i6UNdSig1uE", Font::FontRenderCharGlyphImageHorizontal);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
