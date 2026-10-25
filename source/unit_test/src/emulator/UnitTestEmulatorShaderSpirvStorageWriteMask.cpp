#include "Kyty/UnitTest.h"

#include "../../../emulator/src/Graphics/ShaderSpirvStorageWriteMask.h"

UT_BEGIN(EmulatorShaderSpirvStorageWriteMask);

using namespace Kyty::Libs::Graphics;

static bool Contains(const String8& text, const char* part)
{
	return text.FindIndex(part) != Kyty::Core::STRING8_INVALID_INDEX;
}

TEST(EmulatorShaderSpirvStorageWriteMask, FlagsThePageBeforeEachStoreAndAtomic)
{
	const String8 body = R"(
         %p = OpAccessChain %_ptr_StorageBuffer_float %buf %bi %int_0 %word
              OpStore %p %value ; buf[bi].data[word] = value
     %block = OpAccessChain %_ptr_StorageBuffer_BufferObjectUint %buf_uint %bi2
         %q = OpAccessChain %_ptr_StorageBuffer_uint %block %int_0 %word2
     %prior = OpAtomicIAdd %uint %q %scope %uint_0 %add
      %read = OpLoad %float %p
)";
	String8       marked;
	uint32_t      writes = 0;
	ASSERT_TRUE(SpirvMarkStorageWrites(body, &marked, &writes));
	EXPECT_EQ(writes, 2u);
	EXPECT_TRUE(Contains(marked, "%wmask_page_0 = OpShiftRightLogical %uint %word %wmask_page_shift\n"
	                             "%wmask_ptr_0 = OpAccessChain %_ptr_StorageBuffer_uint %buf_write_mask %bi %int_0 %wmask_page_0\n"
	                             "OpStore %wmask_ptr_0 %wmask_written\n"
	                             "              OpStore %p %value"));
	EXPECT_TRUE(Contains(marked, "%wmask_page_1 = OpShiftRightLogical %uint %word2 %wmask_page_shift\n"
	                             "%wmask_ptr_1 = OpAccessChain %_ptr_StorageBuffer_uint %buf_write_mask %bi2 %int_0 %wmask_page_1\n"
	                             "OpStore %wmask_ptr_1 %wmask_written\n"
	                             "     %prior = OpAtomicIAdd"));
}

TEST(EmulatorShaderSpirvStorageWriteMask, LeavesReadOnlyModulesUnchanged)
{
	const String8 body = R"(
         %p = OpAccessChain %_ptr_StorageBuffer_float %buf %bi %int_0 %word
      %read = OpLoad %float %p
         %q = OpAccessChain %_ptr_StorageBuffer_uint %buf_uint %bi %int_0 %word
    %atomic = OpAtomicLoad %uint %q %scope %uint_0
              OpStore %local %read
)";
	String8       marked;
	uint32_t      writes = 1;
	ASSERT_TRUE(SpirvMarkStorageWrites(body, &marked, &writes));
	EXPECT_EQ(writes, 0u);
	EXPECT_FALSE(Contains(marked, "%wmask"));
}

TEST(EmulatorShaderSpirvStorageWriteMask, RefusesPointersItCannotFollow)
{
	const char* bodies[] = {
	    // A copied pointer would carry an unmarked store.
	    "%p = OpAccessChain %_ptr_StorageBuffer_float %buf %bi %int_0 %word\n%c = OpCopyObject %_ptr_StorageBuffer_float %p\n",
	    // A pointer passed to a function.
	    "%p = OpAccessChain %_ptr_StorageBuffer_float %buf %bi %int_0 %word\n%r = OpFunctionCall %void %f %p\n",
	    // A store through a partial chain has no single word.
	    "%p = OpAccessChain %_ptr_StorageBuffer_BufferObject %buf %bi\nOpStore %p %object\n",
	    // A storage pointer stored as a value.
	    "%p = OpAccessChain %_ptr_StorageBuffer_float %buf %bi %int_0 %word\nOpStore %local %p\n",
	};
	for (const char* body: bodies)
	{
		String8  marked;
		uint32_t writes = 0;
		EXPECT_FALSE(SpirvMarkStorageWrites(body, &marked, &writes)) << body;
	}
}

UT_END();
