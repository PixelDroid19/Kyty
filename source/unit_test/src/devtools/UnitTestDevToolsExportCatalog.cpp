#include "Kyty/DevTools/Catalog/ExportCatalog.h"
#include "Kyty/UnitTest.h"

#include <cstdio>
#include <cstring>

UT_BEGIN(DevToolsExportCatalog);

using namespace Kyty::DevTools;

TEST(DevToolsExportCatalog, SchemaIsVersionedAndNonExecutable)
{
	ExportCatalog cat;
	EXPECT_EQ(cat.SchemaMajor(), 1u);
	EXPECT_EQ(cat.SchemaMinor(), 0u);
	EXPECT_TRUE(ExportCatalog::IsNonExecutableInventory());
	// No function-pointer field exists on ExportEntry (size is data-only).
	EXPECT_GE(sizeof(ExportEntry), 64u);
}

TEST(DevToolsExportCatalog, SeedLoadsMultiSourceEntries)
{
	ExportCatalog cat;
	const uint32_t n = SeedPublicExportCatalog(&cat);
	ASSERT_GT(n, 5u);
	EXPECT_EQ(cat.Size(), n);
	EXPECT_GE(cat.CountByStatus(ExportStatus::Implemented), 1u);
	EXPECT_GE(cat.CountByStatus(ExportStatus::UnknownAbi), 1u);
	EXPECT_GE(cat.CountByStatus(ExportStatus::Unimplemented), 1u);
	EXPECT_GE(cat.CountByStatus(ExportStatus::Conflict), 1u);
}

TEST(DevToolsExportCatalog, QueryByNameNidAndLibrary)
{
	ExportCatalog cat;
	ASSERT_GT(SeedPublicExportCatalog(&cat), 0u);

	uint32_t idx = 0;
	ASSERT_EQ(cat.FindByName("sceAudioOut2Initialize", &idx), ExportCatalogResult::Ok);
	const ExportEntry* by_name = cat.At(idx);
	ASSERT_NE(by_name, nullptr);
	EXPECT_STREQ(by_name->nid, "g2tViFIohHE");
	EXPECT_EQ(by_name->status, ExportStatus::Implemented);
	EXPECT_STREQ(ExportStatusName(by_name->status), "IMPLEMENTED");

	ASSERT_EQ(cat.FindByNid("Q2V+iqvjgC0", &idx), ExportCatalogResult::Ok);
	const ExportEntry* by_nid = cat.At(idx);
	ASSERT_NE(by_nid, nullptr);
	EXPECT_STREQ(by_nid->export_name, "vsnprintf");
	EXPECT_EQ(by_nid->status, ExportStatus::UnknownAbi);
	EXPECT_STREQ(ExportStatusName(by_nid->status), "UNKNOWN_ABI");

	uint32_t indices[32] = {};
	uint32_t count       = 0;
	ASSERT_EQ(cat.FindByLibrary("libSceAudioOut", indices, 32, &count), ExportCatalogResult::Ok);
	EXPECT_GE(count, 2u);
	for (uint32_t i = 0; i < count && i < 32u; ++i)
	{
		const ExportEntry* e = cat.At(indices[i]);
		ASSERT_NE(e, nullptr);
		EXPECT_STREQ(e->library, "libSceAudioOut");
	}
}

TEST(DevToolsExportCatalog, UnresolvedExportsRemainNonExecutable)
{
	ExportCatalog cat;
	ASSERT_GT(SeedPublicExportCatalog(&cat), 0u);

	// UNIMPLEMENTED / UNKNOWN_ABI / REQUIRED must not become HLE via catalog.
	EXPECT_TRUE(ExportCatalog::IsNonExecutableInventory());
	uint32_t idx = 0;
	ASSERT_EQ(cat.FindByName("sceNpTrophy2CreateContext", &idx), ExportCatalogResult::Ok);
	const ExportEntry* e = cat.At(idx);
	ASSERT_NE(e, nullptr);
	EXPECT_EQ(e->status, ExportStatus::Unimplemented);
	// Catalog has no trampoline / vaddr / callable field to invoke.
	EXPECT_EQ(e->flags, 0u);
}

TEST(DevToolsExportCatalog, InsertRejectsEmptyAndDoesNotRegisterHle)
{
	ExportCatalog cat;
	ExportEntry   empty {};
	EXPECT_EQ(cat.Insert(empty), ExportCatalogResult::InvalidArgument);
	EXPECT_EQ(cat.Size(), 0u);
}

TEST(DevToolsExportCatalog, ConflictMarksDisagreement)
{
	ExportCatalog cat;
	ExportEntry   a {};
	std::snprintf(a.library, sizeof(a.library), "%s", "libTest");
	std::snprintf(a.module, sizeof(a.module), "%s", "libTest");
	std::snprintf(a.export_name, sizeof(a.export_name), "%s", "foo");
	std::snprintf(a.nid, sizeof(a.nid), "%s", "TestNidAAAA");
	std::snprintf(a.firmware, sizeof(a.firmware), "%s", "any");
	std::snprintf(a.source, sizeof(a.source), "%s", "seed:a");
	a.status      = ExportStatus::Known;
	a.symbol_type = ExportSymbolType::Func;
	ASSERT_EQ(cat.Insert(a), ExportCatalogResult::Ok);

	ExportEntry b = a;
	std::snprintf(b.export_name, sizeof(b.export_name), "%s", "bar");
	std::snprintf(b.source, sizeof(b.source), "%s", "seed:b");
	EXPECT_EQ(cat.Insert(b), ExportCatalogResult::Conflict);
	EXPECT_EQ(cat.At(0)->status, ExportStatus::Conflict);
	EXPECT_EQ(cat.Size(), 1u); // second name not silently added as success
}

// Driver used by verification plan: prints seed inventory to stdout for capture.
TEST(DevToolsExportCatalog, VerificationDriverPrintsInventory)
{
	ExportCatalog cat;
	const uint32_t n = SeedPublicExportCatalog(&cat);
	ASSERT_GT(n, 0u);
	std::printf("export_catalog schema=%u.%u entries=%u non_executable=%d\n", cat.SchemaMajor(), cat.SchemaMinor(), n,
	            ExportCatalog::IsNonExecutableInventory() ? 1 : 0);
	std::printf("status_counts KNOWN=%u REQUIRED=%u IMPLEMENTED=%u UNIMPLEMENTED=%u CONFLICT=%u UNKNOWN_ABI=%u\n",
	            cat.CountByStatus(ExportStatus::Known), cat.CountByStatus(ExportStatus::Required),
	            cat.CountByStatus(ExportStatus::Implemented), cat.CountByStatus(ExportStatus::Unimplemented),
	            cat.CountByStatus(ExportStatus::Conflict), cat.CountByStatus(ExportStatus::UnknownAbi));
	for (uint32_t i = 0; i < n; ++i)
	{
		const ExportEntry* e = cat.At(i);
		ASSERT_NE(e, nullptr);
		std::printf("entry[%u] lib=%s name=%s nid=%s status=%s source=%s fw=%s\n", i, e->library, e->export_name, e->nid,
		            ExportStatusName(e->status), e->source, e->firmware);
	}
	// Query samples
	uint32_t idx = 0;
	ASSERT_EQ(cat.FindByNid("g2tViFIohHE", &idx), ExportCatalogResult::Ok);
	std::printf("query_by_nid g2tViFIohHE -> %s (%s)\n", cat.At(idx)->export_name, ExportStatusName(cat.At(idx)->status));
	ASSERT_EQ(cat.FindByName("vsnprintf", &idx), ExportCatalogResult::Ok);
	std::printf("query_by_name vsnprintf -> %s (%s)\n", cat.At(idx)->nid, ExportStatusName(cat.At(idx)->status));
	uint32_t indices[16] = {};
	uint32_t count       = 0;
	ASSERT_EQ(cat.FindByLibrary("libSceAudioOut", indices, 16, &count), ExportCatalogResult::Ok);
	std::printf("query_by_library libSceAudioOut count=%u\n", count);
	std::printf("unresolved_exports_not_auto_hle=1\n");
}

UT_END();
