#include "Kyty/Core/Common.h"
#include "Kyty/Core/String.h"

#include "Emulator/Common.h"
#include "Emulator/Log.h"
#include "Emulator/Libs/Errno.h"
#include "Emulator/Libs/Libs.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

// Public-domain outline rasterizer (TrueType and CFF). Static linkage keeps it
// private to this translation unit.
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs {

LIB_VERSION("Font", 1, "Font", 1, 1);

namespace Font {

static constexpr int FONT_ERROR_INVALID_PARAMETER   = static_cast<int>(0x80460002u);
static constexpr int FONT_ERROR_INVALID_FONT_HANDLE = static_cast<int>(0x80460005u);
static constexpr int FONT_ERROR_FONT_OPEN_FAILED    = static_cast<int>(0x80460036u);
static constexpr int FONT_ERROR_NO_SUPPORT_GLYPH    = static_cast<int>(0x80460042u);
static constexpr int FONT_ERROR_NO_SUPPORT_SURFACE  = static_cast<int>(0x80460050u);

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
	KYTY_LOG_DEBUG("\t font_memory      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_memory));
	KYTY_LOG_DEBUG("\t address          = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(address));
	KYTY_LOG_DEBUG("\t size_byte        = 0x%" PRIx32 "\n", size_byte);
	KYTY_LOG_DEBUG("\t mem_interface    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mem_interface));
	KYTY_LOG_DEBUG("\t mspace_object    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(mspace_object));
	KYTY_LOG_DEBUG("\t destroy_callback = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(destroy_callback));
	KYTY_LOG_DEBUG("\t destroy_object   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(destroy_object));

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
	KYTY_LOG_DEBUG("\t memory    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
	KYTY_LOG_DEBUG("\t selection = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(selection));
	KYTY_LOG_DEBUG("\t edition   = 0x%016" PRIx64 "\n", edition);
	KYTY_LOG_DEBUG("\t library   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
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
	KYTY_LOG_DEBUG("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	return OK;
}

static int KYTY_SYSV_ABI FontSupportExternalFonts(void* library)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	return OK;
}

// sceFontAttachDeviceCacheBuffer — NID CUKn5pX-NVY (library, buffer, size).
// Cache is optional for boot; accept guest buffer (null buffer observed with size).
static int KYTY_SYSV_ABI FontAttachDeviceCacheBuffer(void* library, void* buffer, uint32_t size)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t library = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	KYTY_LOG_DEBUG("\t buffer  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(buffer));
	KYTY_LOG_DEBUG("\t size    = 0x%" PRIx32 "\n", size);
	return OK;
}

struct RendererState
{
	const FontMemory* memory;
	void*             selection;
	uint64_t          edition;
};

// Font face decoded from guest-supplied font bytes. The bytes are copied so the
// face outlives the guest buffer; font sets without bytes have no face.
static constexpr uint32_t FONT_DATA_MAX_SIZE = 64u * 1024u * 1024u;

struct FontFace
{
	std::vector<uint8_t> bytes;
	stbtt_fontinfo       info {};
};

static std::shared_ptr<const FontFace> LoadFontFace(const void* data, uint32_t size)
{
	const auto* src  = static_cast<const uint8_t*>(data);
	auto        face = std::make_shared<FontFace>();
	face->bytes.assign(src, src + size);
	const int offset = stbtt_GetFontOffsetForIndex(face->bytes.data(), 0);
	if (offset < 0 || stbtt_InitFont(&face->info, face->bytes.data(), offset) == 0)
	{
		return nullptr;
	}
	return face;
}

// Opaque font handle returned by OpenFontSet / OpenFontMemory / OpenFontInstance.
// Layout is host-owned; guest only treats the pointer as an opaque FontHandle.
struct FontHandleState
{
	void*                           library;
	std::shared_ptr<const FontFace> face;
	uint32_t                        font_set_type;
	uint32_t                        open_mode;
	int                             attribute;
	void*                           renderer;
	float                           scale_w;
	float                           scale_h;
	float                           effect_slant;
	float                           effect_weight_delta_x;
	float                           effect_weight_delta_y;
	uint32_t                        effect_weight_mode;
};

static int OpenFontHandle(void* library, std::shared_ptr<const FontFace> face, uint32_t font_set_type,
                          uint32_t open_mode, void** font_out)
{
	if (font_out == nullptr)
	{
		return FONT_ERROR_INVALID_PARAMETER;
	}
	auto* handle               = new FontHandleState {};
	handle->library            = library;
	handle->face               = std::move(face);
	handle->font_set_type      = font_set_type;
	handle->open_mode          = open_mode;
	handle->attribute          = 0;
	handle->renderer           = nullptr;
	// Default pixel scale used by later SetScalePixel / layout paths.
	handle->scale_w            = 8.0f;
	handle->scale_h            = 16.0f;
	handle->effect_slant       = 0.0f;
	handle->effect_weight_delta_x = 0.0f;
	handle->effect_weight_delta_y = 0.0f;
	handle->effect_weight_mode = 0;
	*font_out                  = handle;
	return OK;
}

// sceFontOpenFontSet — NID cKYtVmeSTcw
// ABI: (library, font_set_type, open_mode, detail*, handle*). detail may be null.
// Observed on a Gen5 title: open_mode=2 with a null detail.
// System font sets carry no guest bytes, so the handle has no face and glyph
// queries report NO_SUPPORT_GLYPH.
static int KYTY_SYSV_ABI FontOpenFontSet(void* library, uint32_t font_set_type, uint32_t open_mode,
                                         const void* detail, void** font_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t library       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	KYTY_LOG_DEBUG("\t font_set_type = 0x%" PRIx32 "\n", font_set_type);
	KYTY_LOG_DEBUG("\t open_mode     = 0x%" PRIx32 "\n", open_mode);
	KYTY_LOG_DEBUG("\t detail        = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(detail));
	KYTY_LOG_DEBUG("\t font_out      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_out));
	return OpenFontHandle(library, nullptr, font_set_type, open_mode, font_out);
}

// sceFontOpenFontMemory — NID KXUpebrFk1U
// After package font APR resolve, a Gen5 title opens guest-mapped OTF bytes:
// (library, font_address, font_size, detail*, handle*). Observed with CFF outlines.
static int KYTY_SYSV_ABI FontOpenFontMemory(void* library, const void* font_address, uint32_t font_size,
                                            const void* detail, void** font_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t library      = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(library));
	KYTY_LOG_DEBUG("\t font_address = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_address));
	KYTY_LOG_DEBUG("\t font_size    = 0x%" PRIx32 "\n", font_size);
	KYTY_LOG_DEBUG("\t detail       = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(detail));
	KYTY_LOG_DEBUG("\t font_out     = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_out));
	(void)detail;
	if (font_address == nullptr || font_size == 0 || font_size > FONT_DATA_MAX_SIZE)
	{
		return FONT_ERROR_INVALID_PARAMETER;
	}
	auto face = LoadFontFace(font_address, font_size);
	if (face == nullptr)
	{
		return FONT_ERROR_FONT_OPEN_FAILED;
	}
	return OpenFontHandle(library, std::move(face), 0, 0, font_out);
}

// sceFontOpenFontInstance — NID JzCH3SCFnAU (source may be null; setup_font optional).
static int KYTY_SYSV_ABI FontOpenFontInstance(void* font_handle, void* setup_font, void** font_out)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t font_handle = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_handle));
	KYTY_LOG_DEBUG("\t setup_font  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(setup_font));
	KYTY_LOG_DEBUG("\t font_out    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_out));
	if (font_handle == nullptr)
	{
		return OpenFontHandle(nullptr, nullptr, 0, 0, font_out);
	}
	const auto* source = static_cast<const FontHandleState*>(font_handle);
	return OpenFontHandle(source->library, source->face, source->font_set_type, source->open_mode, font_out);
}

// sceFontCloseFont — NID vzHs3C8lWJk
static int KYTY_SYSV_ABI FontCloseFont(void* font_handle)
{
	PRINT_NAME();
	delete static_cast<FontHandleState*>(font_handle);
	return OK;
}

// sceFontCreateRendererWithEdition — NID WaSFJoRWXaI
// (memory, selection, edition, renderer_out). Observed after OpenFontMemory.
static int KYTY_SYSV_ABI FontCreateRendererWithEdition(const FontMemory* memory, void* selection,
                                                       uint64_t edition, void** renderer)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t memory    = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(memory));
	KYTY_LOG_DEBUG("\t selection = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(selection));
	KYTY_LOG_DEBUG("\t edition   = 0x%016" PRIx64 "\n", edition);
	KYTY_LOG_DEBUG("\t renderer  = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(renderer));
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
	KYTY_LOG_DEBUG("\t handle   = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(font_handle));
	KYTY_LOG_DEBUG("\t renderer = 0x%016" PRIx64 "\n", reinterpret_cast<uint64_t>(renderer));
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
	KYTY_LOG_DEBUG("\t handle = 0x%016" PRIx64 " w=%f h=%f\n", reinterpret_cast<uint64_t>(font_handle),
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
// Weights are scales around 1.0 (neutral); the font keeps the deviation from
// 1.0, limited to +/-0.04.
static int KYTY_SYSV_ABI FontSetEffectWeight(void* font_handle, float weight_x, float weight_y, uint32_t mode)
{
	PRINT_NAME();
	auto* font = static_cast<FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return -1;
	}
	font->effect_weight_delta_x = std::clamp(weight_x - 1.0f, -0.04f, 0.04f);
	font->effect_weight_delta_y = std::clamp(weight_y - 1.0f, -0.04f, 0.04f);
	font->effect_weight_mode    = mode;
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

// Guest layout descriptors (three floats each).
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

// Font units scaled to pixels and rounded to the 26.6 fixed-point grid used by
// the guest font driver.
static float ScaleFontUnits(int units, float scale)
{
	return std::round(static_cast<float>(units) * scale * 64.0f) / 64.0f;
}

// sceFontGetHorizontalLayout — NID imxVx8lm+KM
// Baseline and line height come from the face bounding box at the current
// pixel scale; a vertical weight effect widens both edges by half its size.
static int KYTY_SYSV_ABI FontGetHorizontalLayout(void* font_handle, FontHorizontalLayout* layout)
{
	PRINT_NAME();
	if (layout == nullptr)
	{
		return FONT_ERROR_INVALID_PARAMETER;
	}
	*layout          = {};
	const auto* font = static_cast<const FontHandleState*>(font_handle);
	if (font == nullptr)
	{
		return FONT_ERROR_INVALID_FONT_HANDLE;
	}
	if (font->face == nullptr)
	{
		// Known gap: system font sets have no face here, yet the guest requires
		// their layout. These placeholder values keep its text layout running;
		// real values need the system font set's face.
		layout->base_line_y   = font->scale_h * 0.75f;
		layout->line_height   = font->scale_h;
		layout->effect_height = font->scale_h;
		return OK;
	}
	const stbtt_fontinfo& info  = font->face->info;
	const float           scale = stbtt_ScaleForMappingEmToPixels(&info, font->scale_h);
	int                   x0    = 0;
	int                   y0    = 0;
	int                   x1    = 0;
	int                   y1    = 0;
	stbtt_GetFontBoundingBox(&info, &x0, &y0, &x1, &y1);
	const float half_effect = static_cast<float>(static_cast<int>(font->effect_weight_delta_y * font->scale_h * 64.0f) / 2) /
	                          64.0f;
	const float top         = ScaleFontUnits(y1, scale) + half_effect;
	const float bottom      = ScaleFontUnits(y0, scale) - half_effect;
	layout->base_line_y     = top;
	layout->line_height     = top - bottom;
	layout->effect_height   = half_effect;
	KYTY_LOG_DEBUG("\t handle = 0x%016" PRIx64 " base_line=%f line_height=%f\n", reinterpret_cast<uint64_t>(font_handle),
	               static_cast<double>(layout->base_line_y), static_cast<double>(layout->line_height));
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

// Guest glyph metrics (width/height + horizontal/vertical bearings), in pixels.
// Horizontal bearing_y is the distance from the baseline up to the glyph top.
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

struct FontGlyphRef
{
	const stbtt_fontinfo* info;
	int                   index;
	float                 scale_x;
	float                 scale_y;
};

// Resolves a character code to a glyph of the handle's face at its pixel scale.
static int FindGlyph(const FontHandleState* font, uint32_t code, FontGlyphRef* glyph)
{
	if (font == nullptr)
	{
		return FONT_ERROR_INVALID_FONT_HANDLE;
	}
	if (font->face == nullptr)
	{
		return FONT_ERROR_NO_SUPPORT_GLYPH;
	}
	const stbtt_fontinfo& info  = font->face->info;
	const int             index = stbtt_FindGlyphIndex(&info, static_cast<int>(code));
	if (index == 0)
	{
		return FONT_ERROR_NO_SUPPORT_GLYPH;
	}
	glyph->info    = &info;
	glyph->index   = index;
	glyph->scale_x = stbtt_ScaleForMappingEmToPixels(&info, font->scale_w);
	glyph->scale_y = stbtt_ScaleForMappingEmToPixels(&info, font->scale_h);
	return OK;
}

// Vertical metrics stay zero: only horizontal layout is implemented.
static void FillGlyphMetrics(const FontGlyphRef& glyph, FontGlyphMetrics* metrics)
{
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	stbtt_GetGlyphBox(glyph.info, glyph.index, &x0, &y0, &x1, &y1);
	int advance = 0;
	int lsb     = 0;
	stbtt_GetGlyphHMetrics(glyph.info, glyph.index, &advance, &lsb);
	*metrics                      = {};
	metrics->width                = static_cast<float>(x1 - x0) * glyph.scale_x;
	metrics->height               = static_cast<float>(y1 - y0) * glyph.scale_y;
	metrics->horizontal.bearing_x = static_cast<float>(x0) * glyph.scale_x;
	metrics->horizontal.bearing_y = static_cast<float>(y1) * glyph.scale_y;
	metrics->horizontal.advance   = static_cast<float>(advance) * glyph.scale_x;
}

// sceFontGetRenderCharGlyphMetrics — NID IQtleGLL5pQ (handle, code, metrics*)
static int KYTY_SYSV_ABI FontGetRenderCharGlyphMetrics(void* font_handle, uint32_t code,
                                                       FontGlyphMetrics* metrics)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle = 0x%016" PRIx64 " code=0x%" PRIx32 " metrics=0x%016" PRIx64 "\n",
	       reinterpret_cast<uint64_t>(font_handle), code, reinterpret_cast<uint64_t>(metrics));
	if (metrics == nullptr)
	{
		return FONT_ERROR_INVALID_PARAMETER;
	}
	*metrics = {};
	FontGlyphRef glyph {};
	const int    result = FindGlyph(static_cast<const FontHandleState*>(font_handle), code, &glyph);
	if (result != OK)
	{
		return result;
	}
	FillGlyphMetrics(glyph, metrics);
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
	KYTY_LOG_DEBUG("\t surf=%p buf=%p pitch=%d px=%d %dx%d\n", static_cast<void*>(surf), buffer,
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
		return FONT_ERROR_INVALID_PARAMETER;
	}
	auto*        font = static_cast<FontHandleState*>(font_handle);
	FontGlyphRef ref {};
	const int    result = FindGlyph(font, code, &ref);
	if (result != OK)
	{
		return result;
	}
	auto* glyph      = new GlyphState {};
	glyph->font      = font;
	glyph->code      = code;
	glyph->attribute = attribute;
	FillGlyphMetrics(ref, &glyph->metrics);
	*glyph_out = glyph;
	KYTY_LOG_DEBUG("\t handle=0x%016" PRIx64 " code=0x%" PRIx32 " glyph=%p\n",
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

// trans_image is a staging image pointer; the glyph is written straight into
// the guest surface, so it is always null.
struct FontRenderResult
{
	const void*      trans_image;
	FontSurfaceImage surface_image;
	struct
	{
		uint32_t x;
		uint32_t y;
		uint32_t w;
		uint32_t h;
	} update_rect;
	FontGlyphImageMetrics image_metrics;
};

static constexpr int FONT_GLYPH_BITMAP_MAX_DIM = 2048;

struct FontPixelRect
{
	int x0;
	int y0;
	int x1;
	int y1;
};

static bool IsRenderableSurface(const FontRenderSurface* surf)
{
	if (surf == nullptr || surf->buffer == nullptr || surf->width <= 0 || surf->height <= 0)
	{
		return false;
	}
	if (surf->pixel_size_byte != 1 && surf->pixel_size_byte != 4)
	{
		return false;
	}
	return surf->width_byte >= surf->width * surf->pixel_size_byte;
}

static FontPixelRect SurfaceClip(const FontRenderSurface& surf)
{
	const auto clamp_x = [&](uint32_t v) { return static_cast<int>(std::min(v, static_cast<uint32_t>(surf.width))); };
	const auto clamp_y = [&](uint32_t v) { return static_cast<int>(std::min(v, static_cast<uint32_t>(surf.height))); };
	return {clamp_x(surf.scissor.x0), clamp_y(surf.scissor.y0), clamp_x(surf.scissor.x1), clamp_y(surf.scissor.y1)};
}

// Rasterizes the glyph with the pen's sub-pixel phase and writes its coverage
// into every channel of each pixel inside the clipped glyph box. Returns the
// written rectangle, which is empty when the glyph is clipped away.
static int DrawGlyphToSurface(const FontGlyphRef& glyph, const FontRenderSurface& surf, float x, float y,
                              FontPixelRect* written)
{
	const float pen_x   = std::floor(x);
	const float pen_y   = std::floor(y);
	const float shift_x = x - pen_x;
	const float shift_y = y - pen_y;
	int         bx0     = 0;
	int         by0     = 0;
	int         bx1     = 0;
	int         by1     = 0;
	stbtt_GetGlyphBitmapBoxSubpixel(glyph.info, glyph.index, glyph.scale_x, glyph.scale_y, shift_x, shift_y, &bx0, &by0,
	                                &bx1, &by1);
	const int glyph_w = bx1 - bx0;
	const int glyph_h = by1 - by0;
	if (glyph_w > FONT_GLYPH_BITMAP_MAX_DIM || glyph_h > FONT_GLYPH_BITMAP_MAX_DIM)
	{
		return FONT_ERROR_NO_SUPPORT_GLYPH;
	}
	const int dst_x = static_cast<int>(pen_x) + bx0;
	const int dst_y = static_cast<int>(pen_y) + by0;
	const auto clip = SurfaceClip(surf);
	*written        = {std::max(dst_x, clip.x0), std::max(dst_y, clip.y0), std::min(dst_x + glyph_w, clip.x1),
	                   std::min(dst_y + glyph_h, clip.y1)};
	if (written->x0 >= written->x1 || written->y0 >= written->y1)
	{
		*written = {written->x0, written->y0, written->x0, written->y0};
		return OK;
	}
	std::vector<uint8_t> coverage(static_cast<size_t>(glyph_w) * static_cast<size_t>(glyph_h));
	stbtt_MakeGlyphBitmapSubpixel(glyph.info, coverage.data(), glyph_w, glyph_h, glyph_w, glyph.scale_x, glyph.scale_y,
	                              shift_x, shift_y, glyph.index);
	auto*      base       = static_cast<uint8_t*>(surf.buffer);
	const auto pixel_size = static_cast<size_t>(surf.pixel_size_byte);
	for (int row = written->y0; row < written->y1; ++row)
	{
		const uint8_t* src = coverage.data() + static_cast<size_t>(row - dst_y) * static_cast<size_t>(glyph_w);
		uint8_t*       dst = base + static_cast<size_t>(row) * static_cast<size_t>(surf.width_byte);
		for (int column = written->x0; column < written->x1; ++column)
		{
			std::memset(dst + static_cast<size_t>(column) * pixel_size, src[column - dst_x], pixel_size);
		}
	}
	return OK;
}

// Image metrics snap the glyph box and advance outward to whole pixels relative
// to the pen position. stride is the snapped distance to the next pen position
// that still clears the glyph's right edge.
static FontGlyphImageMetrics GlyphImageMetrics(const FontGlyphMetrics& metrics, float x, float y)
{
	const float left_f    = x + metrics.horizontal.bearing_x;
	const float top_f     = y + metrics.horizontal.bearing_y;
	const float right_f   = left_f + metrics.width;
	const float bottom_f  = top_f - metrics.height;
	const float advance_f = x + metrics.horizontal.advance;
	const int   left      = static_cast<int>(std::floor(left_f));
	const int   top       = static_cast<int>(std::ceil(top_f));
	const int   right     = static_cast<int>(std::ceil(right_f));
	const int   bottom    = static_cast<int>(std::floor(bottom_f));
	const int   advance   = static_cast<int>(std::ceil(advance_f));
	int         stride    = advance;
	if (advance_f != static_cast<float>(advance))
	{
		const int fitted = static_cast<int>((static_cast<float>(right) - right_f) + advance_f);
		stride           = std::max(advance_f <= right_f ? fitted : right + 1, fitted);
	}
	FontGlyphImageMetrics image {};
	image.bearing_x = static_cast<float>(left) - x;
	image.bearing_y = static_cast<float>(top) - y;
	image.advance   = static_cast<float>(advance) - x;
	image.stride    = static_cast<float>(stride) - x;
	image.width     = static_cast<uint32_t>(std::max(0, right - left));
	image.height    = static_cast<uint32_t>(std::max(0, top - bottom));
	return image;
}

static void FillRenderResult(const FontRenderSurface& surf, const FontPixelRect& written,
                             const FontGlyphMetrics& metrics, float x, float y, FontRenderResult* result)
{
	const auto pixel_size                 = static_cast<size_t>(surf.pixel_size_byte);
	*result                               = {};
	result->update_rect.x                 = static_cast<uint32_t>(written.x0);
	result->update_rect.y                 = static_cast<uint32_t>(written.y0);
	result->update_rect.w                 = static_cast<uint32_t>(written.x1 - written.x0);
	result->update_rect.h                 = static_cast<uint32_t>(written.y1 - written.y0);
	result->surface_image.address         = static_cast<uint8_t*>(surf.buffer) +
	                                        static_cast<size_t>(written.y0) * static_cast<size_t>(surf.width_byte) +
	                                        static_cast<size_t>(written.x0) * pixel_size;
	result->surface_image.width_byte      = static_cast<uint32_t>(surf.width_byte);
	result->surface_image.pixel_size_byte = static_cast<uint8_t>(surf.pixel_size_byte);
	result->image_metrics                 = GlyphImageMetrics(metrics, x, y);
}

// sceFontRenderCharGlyphImageHorizontal — NID kAenWy1Zw5o
// (handle, code, surface, x, y, metrics*, result*). (x, y) is the pen position
// on the baseline in surface pixels.
static int KYTY_SYSV_ABI FontRenderCharGlyphImageHorizontal(void* font_handle, uint32_t code,
                                                            FontRenderSurface* surf, float x, float y,
                                                            FontGlyphMetrics* metrics,
                                                            FontRenderResult* result)
{
	PRINT_NAME();
	KYTY_LOG_DEBUG("\t handle=0x%016" PRIx64 " code=0x%" PRIx32 " surf=%p x=%f y=%f\n",
	       reinterpret_cast<uint64_t>(font_handle), code, static_cast<void*>(surf), static_cast<double>(x),
	       static_cast<double>(y));
	if (metrics == nullptr || result == nullptr)
	{
		return FONT_ERROR_INVALID_PARAMETER;
	}
	*metrics = {};
	*result  = {};
	if (!IsRenderableSurface(surf))
	{
		return FONT_ERROR_NO_SUPPORT_SURFACE;
	}
	FontGlyphRef glyph {};
	int          rc = FindGlyph(static_cast<const FontHandleState*>(font_handle), code, &glyph);
	if (rc != OK)
	{
		return rc;
	}
	FillGlyphMetrics(glyph, metrics);
	FontPixelRect written {};
	rc = DrawGlyphToSurface(glyph, *surf, x, y, &written);
	if (rc != OK)
	{
		return rc;
	}
	FillRenderResult(*surf, written, *metrics, x, y, result);
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
	// Gen5 Font_v1 open + renderer + layout + glyph metrics.
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
	LIB_FUNC("kAenWy1Zw5o", Font::FontRenderCharGlyphImageHorizontal);
}

} // namespace Kyty::Libs

#endif // KYTY_EMU_ENABLED
