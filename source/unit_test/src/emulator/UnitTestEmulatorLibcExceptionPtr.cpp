#include "Kyty/UnitTest.h"

#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"

#include <exception>

UT_BEGIN(EmulatorLibcExceptionPtr);

namespace {

using PrimaryFunc = void* (*)();
using ObjectFunc  = void (*)(void*);

int g_destroyed = 0;

struct Counted
{
	int value = 7;
	~Counted() { g_destroyed++; }
};

uint64_t Export(const Kyty::Loader::SymbolDatabase& symbols, const char16_t* nid)
{
	Kyty::Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"libc";
	query.library_version      = 1;
	query.module               = U"libc";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Kyty::Loader::SymbolType::Func;
	const auto* record         = symbols.Find(query);
	return record != nullptr ? record->vaddr : 0;
}

} // namespace

// std::exception_ptr keeps a thrown object alive across catch blocks and
// rethrows; the object is destroyed exactly once, when its last reference goes.
TEST(EmulatorLibcExceptionPtr, SharesThrownObjectUntilLastReference)
{
	Kyty::Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Kyty::Libs::Init(U"libc_1", &symbols));
	auto* current   = reinterpret_cast<PrimaryFunc>(Export(symbols, u"RY8mQlhg7mI"));
	auto* rethrow   = reinterpret_cast<ObjectFunc>(Export(symbols, u"qKQiNX91IGo"));
	auto* increment = reinterpret_cast<ObjectFunc>(Export(symbols, u"PsrRUg671K0"));
	auto* decrement = reinterpret_cast<ObjectFunc>(Export(symbols, u"MQFPAqQPt1s"));
	ASSERT_TRUE(current != nullptr && rethrow != nullptr && increment != nullptr && decrement != nullptr);

	EXPECT_EQ(current(), nullptr);

	g_destroyed   = 0;
	void* primary = nullptr;
	try
	{
		throw Counted {};
	} catch (...)
	{
		primary = current();
	}
	ASSERT_NE(primary, nullptr);
	EXPECT_EQ(g_destroyed, 0);
	EXPECT_EQ(static_cast<Counted*>(primary)->value, 7);

	increment(primary);
	decrement(primary);
	EXPECT_EQ(g_destroyed, 0);

	void* caught = nullptr;
	try
	{
		rethrow(primary);
	} catch (...)
	{
		caught = current();
	}
	EXPECT_EQ(caught, primary);
	EXPECT_EQ(std::uncaught_exceptions(), 0);
	EXPECT_EQ(g_destroyed, 0);

	decrement(caught);
	EXPECT_EQ(g_destroyed, 0);
	decrement(primary);
	EXPECT_EQ(g_destroyed, 1);
}

UT_END();
