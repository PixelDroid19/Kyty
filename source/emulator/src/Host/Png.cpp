#include "Emulator/Host/Png.h"

#include "miniz.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

namespace Kyty::Emulator::Host {

namespace {

void* PngAlloc(void* /*opaque*/, size_t items, size_t size)
{
	if (items != 0 && size > SIZE_MAX / items)
	{
		return nullptr;
	}
	return std::malloc(items * size);
}

void PngFree(void* /*opaque*/, void* address)
{
	std::free(address);
}

void PngAppendU32(std::vector<uint8_t>* out, uint32_t value)
{
	out->push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
	out->push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
	out->push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
	out->push_back(static_cast<uint8_t>(value & 0xffu));
}

void PngAppendChunk(std::vector<uint8_t>* out, const char type[4], const uint8_t* data, uint32_t size)
{
	PngAppendU32(out, size);
	const size_t type_offset = out->size();
	out->insert(out->end(), type, type + 4);
	if (data != nullptr && size != 0)
	{
		out->insert(out->end(), data, data + size);
	}
	mz_ulong crc = mz_crc32(0, nullptr, 0);
	crc          = mz_crc32(crc, out->data() + type_offset, 4u + size);
	PngAppendU32(out, static_cast<uint32_t>(crc));
}

bool PngDeflate(const std::vector<uint8_t>& input, std::vector<uint8_t>* output)
{
	if (output == nullptr)
	{
		return false;
	}
	output->clear();

	mz_stream stream {};
	stream.zalloc = PngAlloc;
	stream.zfree  = PngFree;
	if (mz_deflateInit2(&stream, MZ_DEFAULT_COMPRESSION, MZ_DEFLATED, MZ_DEFAULT_WINDOW_BITS, 8, MZ_DEFAULT_STRATEGY) != MZ_OK)
	{
		return false;
	}

	constexpr size_t ChunkSize = 64 * 1024;
	uint8_t          chunk[ChunkSize];
	stream.next_in  = input.empty() ? nullptr : input.data();
	stream.avail_in = static_cast<unsigned int>(input.size());
	int rc          = MZ_OK;
	while (rc != MZ_STREAM_END)
	{
		stream.next_out  = chunk;
		stream.avail_out = static_cast<unsigned int>(sizeof(chunk));
		rc               = mz_deflate(&stream, MZ_FINISH);
		if (rc != MZ_OK && rc != MZ_STREAM_END)
		{
			mz_deflateEnd(&stream);
			return false;
		}
		const size_t produced = sizeof(chunk) - stream.avail_out;
		output->insert(output->end(), chunk, chunk + produced);
	}
	mz_deflateEnd(&stream);
	return true;
}

} // namespace

bool WriteRgba8Png(const char* path, const uint8_t* pixels, uint32_t width, uint32_t height, uint32_t row_pitch_pixels)
{
	if (path == nullptr || path[0] == '\0' || pixels == nullptr || width == 0 || height == 0 || row_pitch_pixels < width || width > 8192 ||
	    height > 8192)
	{
		return false;
	}
	const uint64_t filtered_row_size = static_cast<uint64_t>(width) * 4u + 1u;
	const uint64_t filtered_size     = filtered_row_size * height;
	if (filtered_row_size > UINT32_MAX || filtered_size > UINT32_MAX || filtered_size > std::numeric_limits<size_t>::max())
	{
		return false;
	}

	std::vector<uint8_t> filtered(static_cast<size_t>(filtered_size));
	for (uint32_t y = 0; y < height; y++)
	{
		uint8_t*       dst = filtered.data() + static_cast<uint64_t>(y) * filtered_row_size;
		const uint8_t* src = pixels + static_cast<uint64_t>(y) * row_pitch_pixels * 4u;
		dst[0]            = 0; // PNG filter type None keeps agent scoring deterministic.
		std::memcpy(dst + 1, src, static_cast<uint64_t>(width) * 4u);
	}

	std::vector<uint8_t> compressed;
	if (!PngDeflate(filtered, &compressed) || compressed.size() > UINT32_MAX)
	{
		return false;
	}

	std::vector<uint8_t> png;
	png.reserve(8u + 25u + compressed.size() + 12u);
	const uint8_t signature[8] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1au, '\n'};
	png.insert(png.end(), signature, signature + sizeof(signature));

	uint8_t ihdr[13] {};
	ihdr[0]  = static_cast<uint8_t>((width >> 24u) & 0xffu);
	ihdr[1]  = static_cast<uint8_t>((width >> 16u) & 0xffu);
	ihdr[2]  = static_cast<uint8_t>((width >> 8u) & 0xffu);
	ihdr[3]  = static_cast<uint8_t>(width & 0xffu);
	ihdr[4]  = static_cast<uint8_t>((height >> 24u) & 0xffu);
	ihdr[5]  = static_cast<uint8_t>((height >> 16u) & 0xffu);
	ihdr[6]  = static_cast<uint8_t>((height >> 8u) & 0xffu);
	ihdr[7]  = static_cast<uint8_t>(height & 0xffu);
	ihdr[8]  = 8; // bit depth
	ihdr[9]  = 6; // RGBA
	ihdr[10] = 0; // deflate
	ihdr[11] = 0; // adaptive filtering
	ihdr[12] = 0; // no interlace
	PngAppendChunk(&png, "IHDR", ihdr, sizeof(ihdr));
	PngAppendChunk(&png, "IDAT", compressed.data(), static_cast<uint32_t>(compressed.size()));
	PngAppendChunk(&png, "IEND", nullptr, 0);

	FILE* file = std::fopen(path, "wb");
	if (file == nullptr)
	{
		return false;
	}
	const bool written = std::fwrite(png.data(), 1, png.size(), file) == png.size();
	return std::fclose(file) == 0 && written;
}

} // namespace Kyty::Emulator::Host
