#include "Emulator/Loader/Elf.h"

#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/File.h"

#include "Emulator/Log.h"

#include <algorithm>
#include <limits>
#include <new>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader {

constexpr Elf64_Word kSectionTypeNoBits = 8;
// Host-side policy for simultaneously retained eager ELF metadata, not a guest ABI limit.
constexpr uint64_t kMaxEagerElfMetadataBytes = 64ull * 1024ull * 1024ull;

static bool IsRangeWithin(uint64_t offset, uint64_t size, uint64_t limit)
{
	return offset <= limit && size <= limit - offset;
}

static bool IsEagerElfMetadataWithinBudget(uint64_t retained_bytes, uint64_t replaced_bytes, uint64_t added_bytes)
{
	if (retained_bytes > kMaxEagerElfMetadataBytes || replaced_bytes > retained_bytes) { return false; }
	const uint64_t retained_without_replaced = retained_bytes - replaced_bytes;
	return added_bytes <= kMaxEagerElfMetadataBytes - retained_without_replaced;
}

static bool ReadExact(Core::File& file, uint64_t offset, void* destination, uint64_t size)
{
	if (destination == nullptr && size != 0) { return false; }
	if (size > std::numeric_limits<size_t>::max()) { return false; }
	if (!IsRangeWithin(offset, size, file.Size()) || !file.Seek(offset)) { return false; }

	auto* bytes = static_cast<uint8_t*>(destination);
	uint64_t done = 0;
	while (done < size)
	{
		const auto chunk = static_cast<uint32_t>(std::min<uint64_t>(size - done, std::numeric_limits<uint32_t>::max()));
		uint32_t bytes_read = 0;
		if (file.Read(bytes + static_cast<size_t>(done), chunk, &bytes_read) != 0 || bytes_read != chunk) { return false; }
		done += chunk;
	}
	return true;
}

static bool CanExportElf(const Elf64& elf, uint64_t source_size, bool require_section_headers)
{
	const auto* ehdr = elf.GetEhdr();
	if (ehdr == nullptr) { return false; }

	const auto* phdr = elf.GetPhdr();
	if (ehdr->e_phnum != 0 && phdr == nullptr) { return false; }
	const uint64_t phdr_size = static_cast<uint64_t>(ehdr->e_phnum) * sizeof(Elf64_Phdr);
	if (phdr_size != 0 && !IsRangeWithin(ehdr->e_phoff, phdr_size, std::numeric_limits<uint64_t>::max())) { return false; }

	for (Elf64_Half i = 0; i < ehdr->e_phnum; i++)
	{
		const auto& segment = phdr[i];
		if (segment.p_filesz > std::numeric_limits<uint32_t>::max() || segment.p_filesz > std::numeric_limits<size_t>::max() ||
		    (segment.p_filesz != 0 && !IsRangeWithin(segment.p_offset, segment.p_filesz, std::numeric_limits<uint64_t>::max())))
		{
			return false;
		}
	}

	const auto* shdr = elf.GetShdr();
	if (ehdr->e_shnum != 0 && shdr == nullptr) { return !require_section_headers; }
	const uint64_t shdr_size = static_cast<uint64_t>(ehdr->e_shnum) * sizeof(Elf64_Shdr);
	if (shdr_size != 0 && !IsRangeWithin(ehdr->e_shoff, shdr_size, std::numeric_limits<uint64_t>::max())) { return false; }

	for (Elf64_Half i = 0; shdr != nullptr && i < ehdr->e_shnum; i++)
	{
		const auto& section = shdr[i];
		if (section.sh_size == 0 || section.sh_type == kSectionTypeNoBits) { continue; }
		if (section.sh_size > std::numeric_limits<uint32_t>::max() || section.sh_size > std::numeric_limits<size_t>::max())
		{
			return false;
		}
		if (!IsRangeWithin(section.sh_offset, section.sh_size, source_size) ||
		    !IsRangeWithin(section.sh_offset, section.sh_size, std::numeric_limits<uint64_t>::max()))
		{
			return false;
		}
	}

	return true;
}

static bool IsSelfSegmentTableValid(const SelfHeader& self, const SelfSegment* segments, uint64_t file_size)
{
	if (self.segments_num == 0) { return segments == nullptr; }
	if (segments == nullptr) { return false; }

	for (uint16_t i = 0; i < self.segments_num; i++)
	{
		const auto& segment = segments[i];
		if (!IsRangeWithin(segment.offset, segment.compressed_size, file_size) || (segment.type & 0x2u) != 0 ||
		    (segment.type & 0x8u) != 0 || segment.compressed_size != segment.decompressed_size)
		{
			return false;
		}
	}

	return true;
}

static SelfHeader* load_self(Core::File& f)
{
	if (!IsRangeWithin(f.Tell(), sizeof(SelfHeader), f.Size()))
	{
		return nullptr;
	}

	auto* self = new (std::nothrow) SelfHeader {};
	if (self == nullptr || !ReadExact(f, f.Tell(), self, sizeof(SelfHeader)))
	{
		delete self;
		return nullptr;
	}

	return self;
}

static SelfSegment* load_self_segments(Core::File& f, uint16_t num)
{
	const uint64_t table_size = static_cast<uint64_t>(sizeof(SelfSegment)) * num;
	if (num == 0 || !IsRangeWithin(f.Tell(), table_size, f.Size()))
	{
		return nullptr;
	}

	auto* segs = new (std::nothrow) SelfSegment[num] {};
	if (segs == nullptr || !ReadExact(f, f.Tell(), segs, table_size))
	{
		delete[] segs;
		return nullptr;
	}

	return segs;
}

static Elf64_Ehdr* load_ehdr_64(Core::File& f)
{
	if (!IsRangeWithin(f.Tell(), sizeof(Elf64_Ehdr), f.Size()))
	{
		return nullptr;
	}

	auto* ehdr = new (std::nothrow) Elf64_Ehdr {};
	if (ehdr == nullptr || !ReadExact(f, f.Tell(), ehdr, sizeof(Elf64_Ehdr)))
	{
		delete ehdr;
		return nullptr;
	}

	return ehdr;
}

static void save_ehdr_64(Core::File& f, const Elf64_Ehdr* ehdr)
{
	EXIT_IF(ehdr == nullptr);

	uint32_t bytes_written = 0;

	f.Write(ehdr, sizeof(Elf64_Ehdr), &bytes_written);

	EXIT_IF(bytes_written == 0);
}

static Elf64_Phdr* load_phdr_64(Core::File& f, uint64_t offset, Elf64_Half num)
{
	if (num == 0) { return nullptr; }
	const uint64_t table_size = static_cast<uint64_t>(sizeof(Elf64_Phdr)) * num;
	auto*          phdr      = new (std::nothrow) Elf64_Phdr[num] {};
	if (phdr == nullptr || !ReadExact(f, offset, phdr, table_size))
	{
		delete[] phdr;
		return nullptr;
	}

	return phdr;
}

static void save_phdr_64(Core::File& f, uint64_t offset, Elf64_Half num, const Elf64_Phdr* phdr)
{
	if (num == 0) { return; }
	EXIT_IF(phdr == nullptr);

	uint32_t bytes_written = 0;

	f.Seek(offset);
	f.Write(phdr, sizeof(Elf64_Phdr) * num, &bytes_written);

	EXIT_IF(bytes_written == 0);
}

static Elf64_Shdr* load_shdr_64(Core::File& f, uint64_t offset, Elf64_Half num)
{
	if (num == 0)
	{
		return nullptr;
	}

	const uint64_t table_size = static_cast<uint64_t>(sizeof(Elf64_Shdr)) * num;
	auto*          shdr      = new (std::nothrow) Elf64_Shdr[num] {};
	if (shdr == nullptr || !ReadExact(f, offset, shdr, table_size))
	{
		delete[] shdr;
		return nullptr;
	}

	return shdr;
}

static void save_shdr_64(Core::File& f, uint64_t offset, Elf64_Half num, const Elf64_Shdr* shdr)
{
	if (num == 0)
	{
		return;
	}

	EXIT_IF(shdr == nullptr);

	uint32_t bytes_written = 0;

	f.Seek(offset);
	f.Write(shdr, sizeof(Elf64_Shdr) * num, &bytes_written);

	EXIT_IF(bytes_written == 0);
}

static bool load_dynamic_64(Elf64* f, uint64_t offset, uint64_t size, void** out)
{
	if (f == nullptr || out == nullptr || size > std::numeric_limits<size_t>::max()) { return false; }
	*out = nullptr;
	if (size == 0) { return true; }

	auto* dynamic_data = new (std::nothrow) uint8_t[static_cast<size_t>(size)];
	if (dynamic_data == nullptr) { return false; }
	if (!f->LoadSegment(reinterpret_cast<uint64_t>(dynamic_data), offset, size))
	{
		delete[] dynamic_data;
		return false;
	}

	*out = dynamic_data;
	return true;
}

static char* load_str_table(Core::File& f, uint64_t offset, uint32_t size)
{
	auto* str_table = new (std::nothrow) char[size == 0 ? 1 : size] {};
	if (str_table == nullptr || !ReadExact(f, offset, str_table, size))
	{
		delete[] str_table;
		return nullptr;
	}
	return str_table;
}

static void dbg_print_ehdr_64(Elf64_Ehdr* ehdr, Core::File& f)
{
	f.Printf("ehdr->e_ident = ");
	for (auto i: ehdr->e_ident)
	{
		f.Printf("%02x", i);
	}
	f.Printf("\n");

	f.Printf("ehdr->e_type = 0x%04" PRIx16 "\n", ehdr->e_type);
	f.Printf("ehdr->e_machine = 0x%04" PRIx16 "\n", ehdr->e_machine);
	f.Printf("ehdr->e_version = 0x%08" PRIx32 "\n", ehdr->e_version);

	f.Printf("ehdr->e_entry = 0x%016" PRIx64 "\n", ehdr->e_entry);
	f.Printf("ehdr->e_phoff = 0x%016" PRIx64 "\n", ehdr->e_phoff);
	f.Printf("ehdr->e_shoff = 0x%016" PRIx64 "\n", ehdr->e_shoff);
	f.Printf("ehdr->e_flags = 0x%08" PRIx32 "\n", ehdr->e_flags);
	f.Printf("ehdr->e_ehsize = 0x%04" PRIx16 "\n", ehdr->e_ehsize);
	f.Printf("ehdr->e_phentsize = 0x%04" PRIx16 "\n", ehdr->e_phentsize);
	f.Printf("ehdr->e_phnum = %" PRIu16 "\n", ehdr->e_phnum);
	f.Printf("ehdr->e_shentsize = 0x%04" PRIx16 "\n", ehdr->e_shentsize);
	f.Printf("ehdr->e_shnum = %" PRIu16 "\n", ehdr->e_shnum);
	f.Printf("ehdr->e_shstrndx = %" PRIu16 "\n", ehdr->e_shstrndx);
}

static void dbg_print_phdr_64(Elf64_Phdr* phdr, Core::File& f)
{
	f.Printf("phdr->p_type = 0x%08" PRIx32 "\n", phdr->p_type);
	f.Printf("phdr->p_flags = 0x%08" PRIx32 "\n", phdr->p_flags);
	f.Printf("phdr->p_offset = 0x%016" PRIx64 "\n", phdr->p_offset);
	f.Printf("phdr->p_vaddr = 0x%016" PRIx64 "\n", phdr->p_vaddr);
	f.Printf("phdr->p_paddr = 0x%016" PRIx64 "\n", phdr->p_paddr);
	f.Printf("phdr->p_filesz = 0x%016" PRIx64 "\n", phdr->p_filesz);
	f.Printf("phdr->p_memsz = 0x%016" PRIx64 "\n", phdr->p_memsz);
	f.Printf("phdr->p_align = 0x%016" PRIx64 "\n", phdr->p_align);
}

static void dbg_print_shdr_64(Elf64_Shdr* shdr, Core::File& f)
{
	f.Printf("shdr->sh_name = %d\n", shdr->sh_name);
	f.Printf("shdr->sh_type = 0x%08" PRIx32 "\n", shdr->sh_type);
	f.Printf("shdr->sh_flags = 0x%016" PRIx64 "\n", shdr->sh_flags);
	f.Printf("shdr->sh_addr = 0x%016" PRIx64 "\n", shdr->sh_addr);
	f.Printf("shdr->sh_offset = 0x%016" PRIx64 "\n", shdr->sh_offset);
	f.Printf("shdr->sh_size = 0x%016" PRIx64 "\n", shdr->sh_size);
	f.Printf("shdr->sh_link = %" PRId32 "\n", shdr->sh_link);
	f.Printf("shdr->sh_info = 0x%08" PRIx32 "\n", shdr->sh_info);
	f.Printf("shdr->sh_addralign = 0x%016" PRIx64 "\n", shdr->sh_addralign);
	f.Printf("shdr->sh_entsize = 0x%016" PRIx64 "\n", shdr->sh_entsize);
}

// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define DBG_NAME(tag)                                                                                                                      \
	case tag: name = #tag; break;

static void dbg_print_dynamic_64(const Elf64_Dyn* dyn, Core::File& f)
{
	const char* name = "Unknown";
	switch (dyn->d_tag)
	{
		DBG_NAME(DT_OS_HASH)
		DBG_NAME(DT_HASH)
		DBG_NAME(DT_OS_STRTAB)
		DBG_NAME(DT_OS_STRSZ)
		DBG_NAME(DT_STRTAB)
		DBG_NAME(DT_STRSZ)
		DBG_NAME(DT_OS_SYMTAB)
		DBG_NAME(DT_SYMTAB)
		DBG_NAME(DT_OS_HASHSZ)
		DBG_NAME(DT_OS_SYMTABSZ)
		DBG_NAME(DT_INIT)
		DBG_NAME(DT_FINI)
		DBG_NAME(DT_OS_PLTGOT)
		DBG_NAME(DT_PLTGOT)
		DBG_NAME(DT_OS_JMPREL)
		DBG_NAME(DT_JMPREL)
		DBG_NAME(DT_OS_PLTRELSZ)
		DBG_NAME(DT_PLTRELSZ)
		DBG_NAME(DT_OS_PLTREL)
		DBG_NAME(DT_PLTREL)
		DBG_NAME(DT_OS_RELA)
		DBG_NAME(DT_RELA)
		DBG_NAME(DT_OS_RELASZ)
		DBG_NAME(DT_RELASZ)
		DBG_NAME(DT_OS_RELAENT)
		DBG_NAME(DT_RELAENT)
		DBG_NAME(DT_INIT_ARRAY)
		DBG_NAME(DT_INIT_ARRAYSZ)
		DBG_NAME(DT_FINI_ARRAY)
		DBG_NAME(DT_FINI_ARRAYSZ)
		DBG_NAME(DT_PREINIT_ARRAY)
		DBG_NAME(DT_PREINIT_ARRAYSZ)
		DBG_NAME(DT_OS_SYMENT)
		DBG_NAME(DT_SYMENT)
		DBG_NAME(DT_DEBUG)
		DBG_NAME(DT_TEXTREL)
		DBG_NAME(DT_FLAGS)
		DBG_NAME(DT_NEEDED)
		DBG_NAME(DT_OS_NEEDED_MODULE)
		DBG_NAME(DT_OS_NEEDED_MODULE_1)
		DBG_NAME(DT_OS_IMPORT_LIB)
		DBG_NAME(DT_OS_IMPORT_LIB_1)
		DBG_NAME(DT_OS_IMPORT_LIB_ATTR)
		DBG_NAME(DT_OS_FINGERPRINT)
		DBG_NAME(DT_OS_ORIGINAL_FILENAME)
		DBG_NAME(DT_OS_ORIGINAL_FILENAME_1)
		DBG_NAME(DT_OS_MODULE_INFO)
		DBG_NAME(DT_OS_MODULE_INFO_1)
		DBG_NAME(DT_OS_MODULE_ATTR)
		DBG_NAME(DT_SONAME)
		DBG_NAME(DT_OS_EXPORT_LIB)
		DBG_NAME(DT_OS_EXPORT_LIB_1)
		DBG_NAME(DT_OS_EXPORT_LIB_ATTR)
		DBG_NAME(DT_RELACOUNT)
		DBG_NAME(DT_NULL)
	}
	f.Printf("d_tag = 0x%016" PRIx64 ", d_val = 0x%016" PRIx64 ", name = %s\n", dyn->d_tag, dyn->d_un.d_val, name);
}

Elf64::~Elf64()
{
	Clear();
}

bool Elf64::ResolveSegmentFileOffset(uint64_t file_offset, uint64_t size, uint64_t* physical_offset) const
{
	if (m_f == nullptr || physical_offset == nullptr) { return false; }

	if (m_self == nullptr)
	{
		if (!IsRangeWithin(file_offset, size, m_f->Size())) { return false; }
		*physical_offset = file_offset;
		return true;
	}

	if (m_self->segments_num == 0)
	{
		// Minimal SELF fixtures may carry an unsegmented ELF directly after
		// the container header. In that form the embedded ELF offsets are
		// relative to the payload start, so translate them once and keep the
		// same checked bounds as a regular segment.
		if (m_self->file_size > m_f->Size() || file_offset > m_f->Size() - m_self->file_size ||
		    size > m_f->Size() - m_self->file_size - file_offset)
		{
			return false;
		}
		*physical_offset = m_self->file_size + file_offset;
		return true;
	}

	if (m_self_segments == nullptr || m_phdr == nullptr || m_ehdr == nullptr) { return false; }

	for (uint16_t i = 0; i < m_self->segments_num; i++)
	{
		const auto& seg = m_self_segments[i];
		if ((seg.type & 0x800u) == 0) { continue; }

		const auto phdr_id = ((seg.type >> 20u) & 0xFFFu);
		if (phdr_id >= m_ehdr->e_phnum) { return false; }

		const auto& phdr = m_phdr[phdr_id];
		if (file_offset < phdr.p_offset) { continue; }

		const uint64_t offset = file_offset - phdr.p_offset;
		if (offset > phdr.p_filesz || size > phdr.p_filesz - offset) { continue; }
		if (offset > seg.decompressed_size || size > seg.decompressed_size - offset ||
		    offset > seg.compressed_size || size > seg.compressed_size - offset || seg.offset > m_f->Size() ||
		    offset > m_f->Size() - seg.offset || size > m_f->Size() - seg.offset - offset)
		{
			return false;
		}

		*physical_offset = seg.offset + offset;
		return true;
	}

	return false;
}

bool Elf64::LoadSegment(uint64_t vaddr, uint64_t file_offset, uint64_t size)
{
	uint64_t physical_offset = 0;
	if ((size != 0 && vaddr == 0) || !ResolveSegmentFileOffset(file_offset, size, &physical_offset)) { return false; }
	return ReadExact(*m_f, physical_offset, reinterpret_cast<void*>(static_cast<uintptr_t>(vaddr)), size);
}

const Elf64_Dyn* Elf64::GetDynValue(Elf64_Sxword tag) const
{
	if (m_dynamic == nullptr || m_dynamic_size % sizeof(Elf64_Dyn) != 0) { return nullptr; }
	const auto* dynamic = GetDynamic();
	const uint64_t count = m_dynamic_size / sizeof(Elf64_Dyn);
	for (uint64_t i = 0; i < count; i++)
	{
		const auto* dyn = dynamic + i;
		if (dyn->d_tag == DT_NULL) { break; }
		if (dyn->d_tag == tag)
		{
			return dyn;
		}
	}
	return nullptr;
}

Vector<const Elf64_Dyn*> Elf64::GetDynList(Elf64_Sxword tag) const
{
	Vector<const Elf64_Dyn*> ret;
	if (m_dynamic == nullptr || m_dynamic_size % sizeof(Elf64_Dyn) != 0) { return ret; }
	const auto* dynamic = GetDynamic();
	const uint64_t count = m_dynamic_size / sizeof(Elf64_Dyn);
	for (uint64_t i = 0; i < count; i++)
	{
		const auto* dyn = dynamic + i;
		if (dyn->d_tag == DT_NULL) { break; }
		if (dyn->d_tag == tag)
		{
			ret.Add(dyn);
		}
	}
	return ret;
}

bool Elf64::IsShared() const
{
	return (m_ehdr->e_type == ET_DYNAMIC);
}

GuestPlatform Elf64::GetGuestPlatform() const
{
	EXIT_IF(m_ehdr == nullptr);
	switch (m_ehdr->e_ident[EI_ABIVERSION])
	{
		case 0: return GuestPlatform::Ps4;
		case 2: return GuestPlatform::Ps5;
		default:
			EXIT("unsupported guest ABI version: 0x%x\n", m_ehdr->e_ident[EI_ABIVERSION]);
			return GuestPlatform::Unknown;
	}
}

void Elf64::Clear()
{
	if (m_f != nullptr)
	{
		m_f->Close();
		delete m_f;
	}
	delete m_self;
	delete m_ehdr;
	delete[] m_self_segments;
	delete[] m_phdr;
	delete[] m_shdr;
	delete[] m_str_table;
	delete[] static_cast<uint8_t*>(m_dynamic);
	delete[] static_cast<uint8_t*>(m_dynamic_data);

	m_f                 = nullptr;
	m_self              = nullptr;
	m_self_segments     = nullptr;
	m_ehdr              = nullptr;
	m_phdr              = nullptr;
	m_shdr              = nullptr;
	m_str_table         = nullptr;
	m_dynamic           = nullptr;
	m_dynamic_size      = 0;
	m_dynamic_data      = nullptr;
	m_dynamic_data_size = 0;
	m_valid             = false;
}

void Elf64::DbgDump(const String& folder)
{
	if (m_f == nullptr || m_ehdr == nullptr || !CanExportElf(*this, m_f->Size(), false))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: refusing ELF dump with invalid or unsupported ranges\n");
		return;
	}
	for (uint16_t i = 0; i < m_ehdr->e_phnum; i++)
	{
		uint64_t physical_offset = 0;
		if (m_phdr[i].p_filesz != 0 && !ResolveSegmentFileOffset(m_phdr[i].p_offset, m_phdr[i].p_filesz, &physical_offset))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: refusing ELF dump with unreadable segment ranges\n");
			return;
		}
	}

	auto folder_str = folder.FixDirectorySlash();

	Core::File::CreateDirectories(folder_str);

	for (uint16_t i = 0; m_phdr != nullptr && i < m_ehdr->e_phnum; i++)
	{
		if (m_phdr[i].p_filesz == 0u)
		{
			continue;
		}

		char str[512];
		int  s = snprintf(str, 512, "phdr_%03d", i);
		if (s >= 512) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

		Core::File fout;
		fout.Create(folder_str + str);

		auto* buf = new char[static_cast<uint32_t>(m_phdr[i].p_filesz)];

		// m_f->Seek(m_phdr[i].p_offset);
		// m_f->Read(buf, static_cast<uint32_t>(m_phdr[i].p_filesz));

		if (!LoadSegment(reinterpret_cast<uint64_t>(buf), m_phdr[i].p_offset, m_phdr[i].p_filesz))
		{
			delete[] buf;
			continue;
		}

		fout.Write(buf, static_cast<uint32_t>(m_phdr[i].p_filesz));

		delete[] buf;

		fout.Close();
	}

	for (uint16_t i = 0; m_shdr != nullptr && i < m_ehdr->e_shnum; i++)
	{
		if (m_shdr[i].sh_size == 0u || m_shdr[i].sh_type == kSectionTypeNoBits)
		{
			continue;
		}

		char str[512];
		int  s = snprintf(str, 512, "shdr_%03d", i);
		if (s >= 512) { KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: condition ignored (continuing)\n"); }

		Core::File fout;
		fout.Create(folder_str + str);

		auto* buf = new char[static_cast<uint32_t>(m_shdr[i].sh_size)];

		if (!ReadExact(*m_f, m_shdr[i].sh_offset, buf, m_shdr[i].sh_size))
		{
			delete[] buf;
			continue;
		}
		fout.Write(buf, static_cast<uint32_t>(m_shdr[i].sh_size));

		delete[] buf;

		fout.Close();
	}

	Core::File fout;

	fout.Create(folder_str + U"ehdr.txt");
	dbg_print_ehdr_64(m_ehdr, fout);
	fout.Close();

	fout.Create(folder_str + U"phdr.txt");
	for (uint16_t i = 0; m_phdr != nullptr && i < m_ehdr->e_phnum; i++)
	{
		fout.Printf("--- phdr [%d] ---\n", i);
		dbg_print_phdr_64(m_phdr + i, fout);
	}
	fout.Close();

	fout.Create(folder_str + U"shdr.txt");
	for (uint16_t i = 0; m_shdr != nullptr && i < m_ehdr->e_shnum; i++)
	{
		fout.Printf("--- shdr [%d] %s ---\n", i, GetSectionName(i));
		dbg_print_shdr_64(m_shdr + i, fout);
	}
	fout.Close();

	fout.Create(folder_str + U"dynamic.txt");
	const uint64_t dynamic_count = (m_dynamic != nullptr ? m_dynamic_size / sizeof(Elf64_Dyn) : 0);
	for (uint64_t i = 0; i < dynamic_count; i++)
	{
		const auto* dyn = GetDynamic() + i;
		if (dyn->d_tag == DT_NULL) { break; }
		dbg_print_dynamic_64(dyn, fout);
	}
	fout.Close();
}

uint64_t Elf64::GetEntry()
{
	return m_ehdr->e_entry;
}

bool Elf64::IsSelf() const
{
	if (m_f == nullptr || m_f->IsInvalid())
	{
		return false;
	}

	if (m_self == nullptr)
	{
		return false;
	}

	const bool primary_signature =
	    m_self->ident[0] == 0x4f && m_self->ident[1] == 0x15 && m_self->ident[2] == 0x3d && m_self->ident[3] == 0x1d;
	const bool alternate_gen5_signature =
	    m_self->ident[0] == 0x54 && m_self->ident[1] == 0x14 && m_self->ident[2] == 0xf5 && m_self->ident[3] == 0xee;
	if (!primary_signature && !alternate_gen5_signature)
	{
		return false;
	}

	if (m_self->ident[5] != 0x01 || m_self->ident[6] != 0x01 || m_self->ident[7] != 0x12)
	{
		return false;
	}

	const uint32_t key_type = static_cast<uint32_t>(m_self->ident[8]) | (static_cast<uint32_t>(m_self->ident[9]) << 8u) |
	                          (static_cast<uint32_t>(m_self->ident[10]) << 16u) | (static_cast<uint32_t>(m_self->ident[11]) << 24u);
	const bool base_metadata = m_self->ident[4] == 0x00 && key_type == 0x00000101u && m_self->unknown == 0x22u;
	const bool extended_metadata = m_self->ident[4] == 0x10 && key_type == 0x10000101u && m_self->unknown == 0x32u;
	if (!base_metadata && !extended_metadata)
	{
		return false;
	}

	const uint64_t table_size = static_cast<uint64_t>(sizeof(SelfSegment)) * m_self->segments_num;
	if (m_f->Size() < sizeof(SelfHeader) + table_size + sizeof(Elf64_Ehdr))
	{
		return false;
	}

	// The dwords at header offsets 0x0C/0x0E are SDK version fields, not a
	// size pair. Comparing them as sizes rejects valid metadata produced by
	// newer SDKs. The declared file size counts the last segment's alignment
	// padding, which a copied image may lack; Open bounds every segment instead.
	return m_self->file_size != 0;
}

bool Elf64::IsHeaderValid() const
{
	if (m_f == nullptr || m_f->IsInvalid())
	{
		return false;
	}

	if (m_ehdr == nullptr)
	{
		return false;
	}

	if (m_ehdr->e_ident[EI_MAG0] != '\x7f' || m_ehdr->e_ident[EI_MAG1] != 'E' || m_ehdr->e_ident[EI_MAG2] != 'L' ||
	    m_ehdr->e_ident[EI_MAG3] != 'F')
	{
		KYTY_LOG_DEBUG("Not an ELF file\n");
		return false;
	}

	if (m_ehdr->e_ident[EI_CLASS] != ELFCLASS64)
	{
		KYTY_LOG_DEBUG("ehdr->e_ident[EI_CLASS] (0x%x) != ELFCLASS64\n", m_ehdr->e_ident[EI_CLASS]);
		return false;
	}

	if (m_ehdr->e_ident[EI_DATA] != ELFDATA2LSB)
	{
		KYTY_LOG_DEBUG("ehdr->e_ident[EI_DATA] (0x%x) != ELFDATA2LSB\n", m_ehdr->e_ident[EI_DATA]);
		return false;
	}

	if (m_ehdr->e_ident[EI_VERSION] != EV_CURRENT)
	{
		KYTY_LOG_DEBUG("ehdr->e_ident[EI_VERSION] != EV_CURRENT\n");
		return false;
	}

	if (m_ehdr->e_ident[EI_OSABI] != ELFOSABI_FREEBSD)
	{
		KYTY_LOG_DEBUG("ehdr->e_ident[EI_OSABI] (0x%x) != ELFOSABI_FREEBSD\n", m_ehdr->e_ident[EI_OSABI]);
		return false;
	}

	if (m_ehdr->e_ident[EI_ABIVERSION] != 0 && m_ehdr->e_ident[EI_ABIVERSION] != 2)
	{
		KYTY_LOG_DEBUG("ehdr->e_ident[EI_ABIVERSION] (0x%x) != (0 or 2)\n", m_ehdr->e_ident[EI_ABIVERSION]);
		return false;
	}

	if (m_ehdr->e_type != ET_DYNEXEC && m_ehdr->e_type != ET_DYNAMIC)
	{
		KYTY_LOG_DEBUG("ehdr->e_type (%04x) != ET_DYNEXEC && m_ehdr->e_type != ET_DYNAMIC\n", m_ehdr->e_type);
		return false;
	}

	if (m_ehdr->e_machine != EM_X86_64)
	{
		KYTY_LOG_DEBUG("ehdr->e_machine (%04x) != EM_X86_64\n", m_ehdr->e_machine);
		return false;
	}

	if (m_ehdr->e_version != EV_CURRENT)
	{
		KYTY_LOG_DEBUG("ehdr->e_version != EV_CURRENT\n");
		return false;
	}

	if (m_ehdr->e_phentsize != sizeof(Elf64_Phdr))
	{
		KYTY_LOG_DEBUG("ehdr->e_phentsize != sizeof(Elf64_Phdr)\n");
		return false;
	}

	if ((m_ehdr->e_shnum > 0 && m_ehdr->e_shentsize != sizeof(Elf64_Shdr)) ||
	    (m_ehdr->e_shentsize > 0 && m_ehdr->e_shentsize != sizeof(Elf64_Shdr)))
	{
		KYTY_LOG_DEBUG("ehdr->e_shentsize (%d) != sizeof(Elf64_Shdr)\n", m_ehdr->e_shentsize);
		return false;
	}

	return true;
}

bool Elf64::IsValid() const
{
	return m_valid && IsHeaderValid();
}

bool Elf64::ValidateDynamicRanges() const
{
	if (m_dynamic == nullptr)
	{
		return m_dynamic_size == 0;
	}
	if (m_dynamic_size == 0 || m_dynamic_size % sizeof(Elf64_Dyn) != 0) { return false; }

	const auto* dynamic = GetDynamic();
	const uint64_t count = m_dynamic_size / sizeof(Elf64_Dyn);
	bool terminated = false;
	for (uint64_t i = 0; i < count; i++)
	{
		if (dynamic[i].d_tag == DT_NULL)
		{
			terminated = true;
			break;
		}
	}
	if (!terminated) { return false; }

	auto is_mapped_range = [this](uint64_t address, uint64_t size) {
		if (m_ehdr == nullptr || m_phdr == nullptr || size > std::numeric_limits<uint64_t>::max() - address) { return false; }
		for (Elf64_Half i = 0; i < m_ehdr->e_phnum; i++)
		{
			const auto& phdr = m_phdr[i];
			if ((phdr.p_type != PT_LOAD && phdr.p_type != PT_OS_RELRO) || phdr.p_memsz == 0 || address < phdr.p_vaddr)
			{
				continue;
			}
			const uint64_t offset = address - phdr.p_vaddr;
			if (offset <= phdr.p_memsz && size <= phdr.p_memsz - offset) { return true; }
		}
		return false;
	};

	auto validate_rela = [this, &is_mapped_range](Elf64_Sxword pointer_tag, Elf64_Sxword size_tag, bool os_data) {
		const auto* pointer = GetDynValue(pointer_tag);
		const auto* table_size = GetDynValue(size_tag);
		const uint64_t address = (pointer != nullptr ? pointer->d_un.d_ptr : 0);
		const uint64_t size = (table_size != nullptr ? table_size->d_un.d_val : 0);
		if (size % sizeof(Elf64_Rela) != 0 || (pointer == nullptr && size != 0)) { return false; }
		if (pointer == nullptr) { return true; }
		if (os_data)
		{
			return (m_dynamic_data != nullptr && address <= m_dynamic_data_size && size <= m_dynamic_data_size - address) ||
			       (m_dynamic_data == nullptr && m_dynamic_data_size == 0 && address == 0 && size == 0);
		}
		return is_mapped_range(address, size);
	};

	auto valid_stride = [this](Elf64_Sxword tag) {
		const auto* entry = GetDynValue(tag);
		return entry == nullptr || entry->d_un.d_val == sizeof(Elf64_Rela);
	};
	if (!valid_stride(DT_RELAENT) || !valid_stride(DT_OS_RELAENT)) { return false; }
	if (!validate_rela(DT_RELA, DT_RELASZ, false) || !validate_rela(DT_OS_RELA, DT_OS_RELASZ, true)) { return false; }

	auto validate_jmprel = [this, &is_mapped_range](Elf64_Sxword pointer_tag, Elf64_Sxword size_tag,
	                                               Elf64_Sxword type_tag, bool os_data) {
		const auto* pointer = GetDynValue(pointer_tag);
		const auto* table_size = GetDynValue(size_tag);
		const auto* type = GetDynValue(type_tag);
		const uint64_t address = (pointer != nullptr ? pointer->d_un.d_ptr : 0);
		const uint64_t size = (table_size != nullptr ? table_size->d_un.d_val : 0);
		if (pointer == nullptr && size != 0) { return false; }
		if (pointer != nullptr)
		{
			const bool in_range = os_data ? ((m_dynamic_data != nullptr && address <= m_dynamic_data_size &&
			                                  size <= m_dynamic_data_size - address) ||
			                                 (m_dynamic_data == nullptr && m_dynamic_data_size == 0 && address == 0 && size == 0)) :
			                               is_mapped_range(address, size);
			if (!in_range) { return false; }
		}
		return type == nullptr || type->d_un.d_val != DT_RELA || size % sizeof(Elf64_Rela) == 0;
	};
	return validate_jmprel(DT_JMPREL, DT_PLTRELSZ, DT_PLTREL, false) &&
	       validate_jmprel(DT_OS_JMPREL, DT_OS_PLTRELSZ, DT_OS_PLTREL, true);
}

void Elf64::Open(const String& file_name)
{
	Clear();

	m_f = new (std::nothrow) Core::File;
	if (m_f == nullptr) { return; }
	m_f->Open(file_name, Core::File::Mode::Read);

	if (m_f->IsInvalid())
	{
		EXIT("Can't open %s\n", file_name.C_Str());
	}

	m_self = load_self(*m_f);

	if (!IsSelf())
	{
		delete m_self;
		m_self = nullptr;
		if (!m_f->Seek(0))
		{
			Clear();
			return;
		}
	} else if (m_self->segments_num != 0)
	{
		m_self_segments = load_self_segments(*m_f, m_self->segments_num);
		if (!IsSelfSegmentTableValid(*m_self, m_self_segments, m_f->Size()))
		{
			Clear();
			return;
		}
	}

	const uint64_t ehdr_pos = m_f->Tell();

	m_ehdr = load_ehdr_64(*m_f);
	if (m_ehdr == nullptr || !IsHeaderValid())
	{
		Clear();
		return;
	}

	const uint64_t file_size = m_f->Size();
	if (m_ehdr->e_phoff > std::numeric_limits<uint64_t>::max() - ehdr_pos)
	{
		Clear();
		return;
	}
	const uint64_t phdr_offset = ehdr_pos + m_ehdr->e_phoff;
	const uint64_t phdr_size   = static_cast<uint64_t>(sizeof(Elf64_Phdr)) * m_ehdr->e_phnum;
	if (!IsRangeWithin(phdr_offset, phdr_size, file_size))
	{
		Clear();
		return;
	}
	if (m_ehdr->e_phnum > 0)
	{
		m_phdr = load_phdr_64(*m_f, phdr_offset, m_ehdr->e_phnum);
		if (m_phdr == nullptr)
		{
			Clear();
			return;
		}
	}

	uint64_t retained_eager_metadata_bytes = 0;

	// SELF program loading is defined by its embedded ELF program headers and
	// segment table. Some valid containers retain ELF section metadata whose
	// payload is not stored in the file; sections are debug/link metadata and
	// are not required for runtime segment loading.
	if (m_self == nullptr && m_ehdr->e_shnum > 0)
	{
		if (m_ehdr->e_shoff > std::numeric_limits<uint64_t>::max() - ehdr_pos)
		{
			Clear();
			return;
		}
		const uint64_t shdr_offset = ehdr_pos + m_ehdr->e_shoff;
		const uint64_t shdr_size   = static_cast<uint64_t>(sizeof(Elf64_Shdr)) * m_ehdr->e_shnum;
		if (!IsRangeWithin(shdr_offset, shdr_size, file_size))
		{
			Clear();
			return;
		}
		m_shdr = load_shdr_64(*m_f, shdr_offset, m_ehdr->e_shnum);
		if (m_shdr == nullptr)
		{
			Clear();
			return;
		}
	}

	if (m_shdr != nullptr && m_ehdr->e_shstrndx < m_ehdr->e_shnum)
	{
		const auto& str_header = m_shdr[m_ehdr->e_shstrndx];
		if (str_header.sh_size > std::numeric_limits<uint32_t>::max() ||
		    !IsRangeWithin(str_header.sh_offset, str_header.sh_size, file_size))
		{
			Clear();
			return;
		}
		const uint64_t string_table_allocation_size = (str_header.sh_size == 0 ? 1 : str_header.sh_size);
		if (!IsEagerElfMetadataWithinBudget(retained_eager_metadata_bytes, 0, string_table_allocation_size))
		{
			Clear();
			return;
		}
		m_str_table = load_str_table(*m_f, str_header.sh_offset, static_cast<uint32_t>(str_header.sh_size));
		if (m_str_table == nullptr)
		{
			Clear();
			return;
		}
		retained_eager_metadata_bytes += string_table_allocation_size;
	}

	uint64_t mapped_end = 0;
	for (Elf64_Half i = 0; i < m_ehdr->e_phnum; i++)
	{
		const auto& phdr = m_phdr[i];
		uint64_t physical_offset = 0;
		const bool segment_data_used = phdr.p_type == PT_LOAD || phdr.p_type == PT_OS_RELRO || phdr.p_type == PT_TLS ||
		                               phdr.p_type == PT_DYNAMIC || phdr.p_type == PT_OS_DYNLIBDATA;
		if (segment_data_used && phdr.p_filesz != 0 && !ResolveSegmentFileOffset(phdr.p_offset, phdr.p_filesz, &physical_offset))
		{
			Clear();
			return;
		}
		if ((phdr.p_type == PT_LOAD || phdr.p_type == PT_OS_RELRO || phdr.p_type == PT_TLS) &&
		    phdr.p_filesz > phdr.p_memsz)
		{
			Clear();
			return;
		}
		if (phdr.p_type == PT_LOAD || phdr.p_type == PT_OS_RELRO)
		{
			if (phdr.p_align != 0 && phdr.p_memsz > std::numeric_limits<uint64_t>::max() - (phdr.p_align - 1))
			{
				Clear();
				return;
			}
			const uint64_t aligned_size = (phdr.p_align != 0 ?
			                                    (phdr.p_memsz + (phdr.p_align - 1)) & ~(phdr.p_align - 1) :
			                                    phdr.p_memsz);
			if (phdr.p_vaddr > std::numeric_limits<uint64_t>::max() - aligned_size)
			{
				Clear();
				return;
			}
			mapped_end = std::max(mapped_end, phdr.p_vaddr + aligned_size);
		}
		if (phdr.p_type == PT_DYNAMIC)
		{
			if (phdr.p_filesz % sizeof(Elf64_Dyn) != 0)
			{
				Clear();
				return;
			}
			const uint64_t replaced_bytes = (m_dynamic != nullptr ? m_dynamic_size : 0);
			if (!IsEagerElfMetadataWithinBudget(retained_eager_metadata_bytes, replaced_bytes, phdr.p_filesz))
			{
				Clear();
				return;
			}
			delete[] static_cast<uint8_t*>(m_dynamic);
			m_dynamic = nullptr;
			retained_eager_metadata_bytes -= replaced_bytes;
			m_dynamic_size = phdr.p_filesz;
			if (!load_dynamic_64(this, phdr.p_offset, phdr.p_filesz, &m_dynamic))
			{
				m_dynamic_size = 0;
				Clear();
				return;
			}
			retained_eager_metadata_bytes += phdr.p_filesz;
		}
		if (phdr.p_type == PT_OS_DYNLIBDATA)
		{
			const uint64_t replaced_bytes = (m_dynamic_data != nullptr ? m_dynamic_data_size : 0);
			if (!IsEagerElfMetadataWithinBudget(retained_eager_metadata_bytes, replaced_bytes, phdr.p_filesz))
			{
				Clear();
				return;
			}
			delete[] static_cast<uint8_t*>(m_dynamic_data);
			m_dynamic_data = nullptr;
			retained_eager_metadata_bytes -= replaced_bytes;
			m_dynamic_data_size = phdr.p_filesz;
			if (!load_dynamic_64(this, phdr.p_offset, phdr.p_filesz, &m_dynamic_data))
			{
				m_dynamic_data_size = 0;
				Clear();
				return;
			}
			retained_eager_metadata_bytes += phdr.p_filesz;
		}
	}
	if ((mapped_end & ~static_cast<uint64_t>(0xfff)) + 0x1000 < mapped_end)
	{
		Clear();
		return;
	}

	m_valid = true;
	if (!ValidateDynamicRanges())
	{
		Clear();
	}
}

void Elf64::Save(const String& file_name)
{
	EXIT_IF(!IsValid());
	if (!IsValid() || m_f == nullptr || !CanExportElf(*this, m_f->Size(), true))
	{
		KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: refusing ELF save with invalid or unsupported ranges\n");
		return;
	}
	for (uint16_t i = 0; i < m_ehdr->e_phnum; i++)
	{
		uint64_t physical_offset = 0;
		if (m_phdr[i].p_filesz != 0 && !ResolveSegmentFileOffset(m_phdr[i].p_offset, m_phdr[i].p_filesz, &physical_offset))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: refusing ELF save with unreadable segment ranges\n");
			return;
		}
	}

	Core::File f;
	if (!f.Create(file_name))
	{
		EXIT("Can't create %s\n", file_name.C_Str());
		return;
	}

	save_ehdr_64(f, m_ehdr);

	save_phdr_64(f, m_ehdr->e_phoff, m_ehdr->e_phnum, m_phdr);
	save_shdr_64(f, m_ehdr->e_shoff, m_ehdr->e_shnum, m_shdr);

	for (uint16_t i = 0; i < m_ehdr->e_phnum; i++)
	{
		if (m_phdr[i].p_filesz == 0u)
		{
			continue;
		}

		auto* buf = new char[static_cast<uint32_t>(m_phdr[i].p_filesz)];

		EXIT_IF(!LoadSegment(reinterpret_cast<uint64_t>(buf), m_phdr[i].p_offset, m_phdr[i].p_filesz));

		uint32_t bytes_written = 0;

		f.Seek(m_phdr[i].p_offset);
		f.Write(buf, static_cast<uint32_t>(m_phdr[i].p_filesz), &bytes_written);

		EXIT_IF(bytes_written != m_phdr[i].p_filesz);

		delete[] buf;
	}

	for (uint16_t i = 0; i < m_ehdr->e_shnum; i++)
	{
		if (m_shdr[i].sh_size == 0u || m_shdr[i].sh_type == kSectionTypeNoBits)
		{
			continue;
		}

		auto* buf = new char[static_cast<uint32_t>(m_shdr[i].sh_size)];

		EXIT_IF(!ReadExact(*m_f, m_shdr[i].sh_offset, buf, m_shdr[i].sh_size));

		uint32_t bytes_written = 0;

		f.Seek(m_shdr[i].sh_offset);
		f.Write(buf, static_cast<uint32_t>(m_shdr[i].sh_size), &bytes_written);

		EXIT_IF(bytes_written != m_shdr[i].sh_size);

		delete[] buf;
	}

	f.Close();
}

} // namespace Kyty::Loader

#endif // KYTY_EMU_ENABLED
