#include "Kyty/UnitTest.h"

#include "Emulator/Dialog.h"
#include "Emulator/Libs/Libs.h"
#include "Emulator/Loader/SymbolDatabase.h"
#include "Emulator/Ports/KeyboardTextPort.h"

#include <cstring>

UT_BEGIN(EmulatorImeDialog);

using namespace Libs::Dialog;

namespace {

Loader::SymbolResolve ResolveFor(const char16_t* nid)
{
	Loader::SymbolResolve query {};
	query.name                 = nid;
	query.library              = U"ImeDialog";
	query.library_version      = 1;
	query.module               = U"ImeDialog";
	query.module_version_major = 1;
	query.module_version_minor = 1;
	query.type                 = Loader::SymbolType::Func;
	return query;
}

} // namespace

TEST(EmulatorImeDialog, HostKeysEditTheTitlesTextUntilEnterConfirmsIt)
{
	using Emulator::Ports::KeyboardTextKey;
	using Emulator::Ports::KeyboardTextPort;
	char16_t text[8] = u"Ab";
	ImeDialog::ImeDialogParam param {};
	param.user_id           = 1;
	param.max_text_length   = 4;
	param.input_text_buffer = text;
	ASSERT_EQ(ImeDialog::ImeDialogGetStatus(), ImeDialog::STATUS_NONE);
	ASSERT_EQ(ImeDialog::ImeDialogInit(&param, nullptr), 0);
	EXPECT_EQ(ImeDialog::ImeDialogInit(&param, nullptr), static_cast<int>(0x80BC0001u));
	EXPECT_EQ(ImeDialog::ImeDialogGetStatus(), ImeDialog::STATUS_RUNNING);
	// Backspace, then three characters for the two free places, then Enter.
	for (const KeyboardTextKey key: {KeyboardTextKey {0x2a, 0, true, false}, KeyboardTextKey {0x06, u'c', true, false},
	                                 KeyboardTextKey {0x07, u'd', true, false}, KeyboardTextKey {0x08, u'e', true, false},
	                                 KeyboardTextKey {0x09, u'f', true, false}, KeyboardTextKey {0x28, 0, true, false}})
	{
		ASSERT_TRUE(KeyboardTextPort::Push(key));
	}
	EXPECT_EQ(ImeDialog::ImeDialogGetStatus(), ImeDialog::STATUS_FINISHED);
	EXPECT_FALSE(KeyboardTextPort::DialogOpen());
	ImeDialog::ImeDialogResult result {};
	std::memset(&result, 0x5a, sizeof(result));
	ASSERT_EQ(ImeDialog::ImeDialogGetResult(&result), 0);
	EXPECT_EQ(result.end_status, ImeDialog::END_STATUS_OK);
	EXPECT_EQ(std::memcmp(text, u"Acde", sizeof(u"Acde")), 0);
	EXPECT_EQ(ImeDialog::ImeDialogTerm(), 0);
	EXPECT_EQ(ImeDialog::ImeDialogGetStatus(), ImeDialog::STATUS_NONE);
}

TEST(EmulatorImeDialog, AnAbortedDialogReportsAbort)
{
	char16_t text[4] = u"";
	ImeDialog::ImeDialogParam param {};
	param.user_id           = 1;
	param.max_text_length   = 3;
	param.input_text_buffer = text;
	ASSERT_EQ(ImeDialog::ImeDialogInit(&param, nullptr), 0);
	EXPECT_EQ(ImeDialog::ImeDialogAbort(), 0);
	ImeDialog::ImeDialogResult result {};
	ASSERT_EQ(ImeDialog::ImeDialogGetResult(&result), 0);
	EXPECT_EQ(result.end_status, ImeDialog::END_STATUS_ABORTED);
	EXPECT_EQ(ImeDialog::ImeDialogTerm(), 0);
}

TEST(EmulatorImeDialog, EveryImportedImeDialogEntryResolves)
{
	Loader::SymbolDatabase symbols;
	ASSERT_TRUE(Libs::Init(U"libImeDialog_1", &symbols));
	for (const char16_t* nid: {u"NUeBrN7hzf0", u"IADmD4tScBY", u"x01jxu+vxlc", u"oBmw4xrmfKs", u"gyTyVn+bXMw"})
	{
		EXPECT_NE(symbols.Find(ResolveFor(nid)), nullptr);
	}
}

UT_END();
