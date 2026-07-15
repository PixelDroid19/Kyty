#include "Emulator/Agent/EventRing.h"
#include "Emulator/Agent/FrameScore.h"
#include "Emulator/Agent/Protocol.h"
#include "Emulator/Agent/StallWatch.h"
#include "Emulator/Controller.h"
#include "Emulator/Graphics/Window.h"
#include "Kyty/UnitTest.h"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

UT_BEGIN(AgentTools);

using namespace Kyty::Emulator::Agent;

namespace {

void WriteBmp32(const char* path, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b)
{
	const uint32_t row_bytes   = width * 4u;
	const uint32_t pixel_bytes = row_bytes * height;
	std::vector<uint8_t> file(54u + pixel_bytes, 0);
	file[0]                    = 'B';
	file[1]                    = 'M';
	const uint32_t file_size   = static_cast<uint32_t>(file.size());
	file[2]                    = static_cast<uint8_t>(file_size);
	file[3]                    = static_cast<uint8_t>(file_size >> 8);
	file[4]                    = static_cast<uint8_t>(file_size >> 16);
	file[5]                    = static_cast<uint8_t>(file_size >> 24);
	file[10]                   = 54;
	file[14]                   = 40;
	file[18]                   = static_cast<uint8_t>(width);
	file[19]                   = static_cast<uint8_t>(width >> 8);
	file[20]                   = static_cast<uint8_t>(width >> 16);
	file[21]                   = static_cast<uint8_t>(width >> 24);
	file[22]                   = static_cast<uint8_t>(height);
	file[23]                   = static_cast<uint8_t>(height >> 8);
	file[24]                   = static_cast<uint8_t>(height >> 16);
	file[25]                   = static_cast<uint8_t>(height >> 24);
	file[26]                   = 1;
	file[28]                   = 32;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t off = 54u + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4u;
			file[off + 0]    = b;
			file[off + 1]    = g;
			file[off + 2]    = r;
			file[off + 3]    = 255;
		}
	}
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

void WriteBmp32HotBlocks(const char* path, uint32_t width, uint32_t height)
{
	const uint32_t row_bytes   = width * 4u;
	const uint32_t pixel_bytes = row_bytes * height;
	std::vector<uint8_t> file(54u + pixel_bytes, 0);
	file[0]                  = 'B';
	file[1]                  = 'M';
	const uint32_t file_size = static_cast<uint32_t>(file.size());
	file[2]                  = static_cast<uint8_t>(file_size);
	file[3]                  = static_cast<uint8_t>(file_size >> 8);
	file[4]                  = static_cast<uint8_t>(file_size >> 16);
	file[5]                  = static_cast<uint8_t>(file_size >> 24);
	file[10]                 = 54;
	file[14]                 = 40;
	file[18]                 = static_cast<uint8_t>(width);
	file[19]                 = static_cast<uint8_t>(width >> 8);
	file[22]                 = static_cast<uint8_t>(height);
	file[23]                 = static_cast<uint8_t>(height >> 8);
	file[26]                 = 1;
	file[28]                 = 32;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t off = 54u + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4u;
			const bool   hot = (x < width / 3u) || (x > (2u * width) / 3u);
			file[off + 0]    = hot ? 20 : static_cast<uint8_t>(40 + (x % 40));
			file[off + 1]    = hot ? 240 : static_cast<uint8_t>(50 + (y % 40));
			file[off + 2]    = hot ? 255 : static_cast<uint8_t>(30 + ((x + y) % 50));
			file[off + 3]    = 255;
		}
	}
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
}

} // namespace

TEST(AgentTools, ProtocolParsesRequestAndFormatsResponse)
{
	Request   req {};
	ErrorInfo error {};
	ASSERT_TRUE(ParseRequestLine("{\"id\":7,\"tool\":\"status\",\"args\":{\"last\":3}}", &req, &error));
	EXPECT_EQ(req.id, 7u);
	EXPECT_EQ(req.tool, "status");
	EXPECT_EQ(req.args_json, "{\"last\":3}");

	uint32_t last = 0;
	EXPECT_TRUE(ArgsGetU32(req.args_json, "last", &last));
	EXPECT_EQ(last, 3u);

	const std::string ok = FormatOk(7, "{\"alive\":true}");
	EXPECT_NE(ok.find("\"ok\":true"), std::string::npos);
	EXPECT_NE(ok.find("\"alive\":true"), std::string::npos);

	const std::string err = FormatErr(7, "timeout", "wait timed out");
	EXPECT_NE(err.find("\"ok\":false"), std::string::npos);
	EXPECT_NE(err.find("timeout"), std::string::npos);
}

TEST(AgentTools, ProtocolRejectsUnknownShape)
{
	Request   req {};
	ErrorInfo error {};
	EXPECT_FALSE(ParseRequestLine("{\"tool\":\"status\"}", &req, &error));
	EXPECT_EQ(error.code, "invalid_args");
}

TEST(AgentTools, EventRingKeepsNewestAndLastError)
{
	EventRing ring;
	ring.Push(EventKind::Info, "boot", "hello");
	ring.Push(EventKind::Error, "boom", "failed");
	ring.Push(EventKind::Capture, "capture_ok", "/tmp/a.bmp");

	EventRecord err {};
	ASSERT_TRUE(ring.LastError(&err));
	EXPECT_STREQ(err.code, "boom");

	EventRecord out[4] {};
	const uint32_t count = ring.CopySince(0, out, 4);
	EXPECT_EQ(count, 3u);
	EXPECT_EQ(out[0].kind, EventKind::Capture);
}

TEST(AgentTools, EventRingOverflowStaysBounded)
{
	EventRing ring;
	for (uint32_t i = 0; i < kAgentEventRingCapacity + 32u; ++i)
	{
		char code[32];
		std::snprintf(code, sizeof(code), "c%u", i);
		ring.Push(EventKind::Info, code, "x");
	}
	EXPECT_EQ(ring.Size(), kAgentEventRingCapacity);

	EventRecord newest[1] {};
	ASSERT_EQ(ring.CopySince(0, newest, 1), 1u);
	EXPECT_STREQ(newest[0].code, "c543");
}

TEST(AgentTools, AgentPadOverlayMergesButtonsAndAxes)
{
	Kyty::Libs::Controller::AgentPadClear();
	uint32_t button = 0;
	ASSERT_TRUE(Kyty::Libs::Controller::AgentPadButtonFromName("cross", &button));
	Kyty::Libs::Controller::AgentPadSetButton(button, true);

	Kyty::Libs::Controller::Axis axis {};
	ASSERT_TRUE(Kyty::Libs::Controller::AgentPadAxisFromName("left_x", &axis));
	Kyty::Libs::Controller::AgentPadSetAxis(axis, 200);

	uint32_t buttons = 0;
	uint8_t  axes[6] {};
	Kyty::Libs::Controller::AgentPadGetState(&buttons, axes);
	EXPECT_EQ(buttons & Kyty::Libs::Controller::PAD_BUTTON_CROSS, Kyty::Libs::Controller::PAD_BUTTON_CROSS);
	EXPECT_EQ(axes[0], 200);

	Kyty::Libs::Controller::AgentPadClear();
	Kyty::Libs::Controller::AgentPadGetState(&buttons, axes);
	EXPECT_EQ(buttons, 0u);
}

TEST(AgentTools, AgentPadTapIsReleasePressReleaseOnGuestSamples)
{
	using Kyty::Libs::Controller::AgentPadApplyReadStateSample;
	using Kyty::Libs::Controller::AgentPadClear;
	using Kyty::Libs::Controller::AgentPadGetReadStats;
	using Kyty::Libs::Controller::AgentPadScheduleTap;
	using Kyty::Libs::Controller::AgentPadReadStats;
	using Kyty::Libs::Controller::PAD_BUTTON_CROSS;

	AgentPadClear();
	ASSERT_TRUE(AgentPadScheduleTap(PAD_BUTTON_CROSS));

	AgentPadReadStats stats {};
	AgentPadGetReadStats(&stats);
	EXPECT_TRUE(stats.tap_pending);
	EXPECT_EQ(stats.delivered_taps, 0u);

	uint32_t sample = 0;
	AgentPadApplyReadStateSample(&sample);
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, 0u);

	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, PAD_BUTTON_CROSS);

	sample = 0;
	AgentPadApplyReadStateSample(&sample);
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, 0u);

	AgentPadGetReadStats(&stats);
	EXPECT_FALSE(stats.tap_pending);
	EXPECT_EQ(stats.delivered_taps, 1u);

	// PadRead-style samples must not advance a new tap by themselves.
	ASSERT_TRUE(AgentPadScheduleTap(PAD_BUTTON_CROSS));
	sample = 0;
	AgentPadApplyReadStateSample(&sample); // release
	EXPECT_EQ(sample & PAD_BUTTON_CROSS, 0u);
	AgentPadClear();
}

TEST(AgentTools, CaptureRequestFailsCleanlyWithoutWindow)
{
	Kyty::Libs::Graphics::WindowPresentStats stats {};
	ASSERT_TRUE(Kyty::Libs::Graphics::WindowGetPresentStats(&stats));
	EXPECT_FALSE(stats.graphic_ready);
	EXPECT_FALSE(stats.capture_ready);

	Kyty::Libs::Graphics::WindowNativeCaptureResult error {};
	uint64_t                                        request_id = 0;
	EXPECT_FALSE(Kyty::Libs::Graphics::WindowRequestNativeCapture(&request_id, &error));
	EXPECT_EQ(error.error_code, "not_ready");
}

TEST(AgentTools, StallWatchClassifiesPresentHang)
{
	StallSample start {};
	start.graphic_ready    = true;
	start.frame            = 2594;
	start.present          = 100;
	start.fps              = 11.0;
	start.ms_since_present = 100;
	start.ms_since_frame   = 100;

	StallSample end = start;
	end.fps              = 0.9;
	end.ms_since_present = 8000;
	end.ms_since_frame   = 8000;

	StallWatchArgs args {};
	args.window_ms        = 10000;
	args.present_stall_ms = 5000;
	args.frame_stall_ms   = 5000;
	args.min_fps          = 2.0;

	const StallWatchResult result = ClassifyStall(start, end, args);
	EXPECT_FALSE(result.healthy);
	EXPECT_EQ(result.code, StallCode::PresentStalled);
	EXPECT_STREQ(result.code_name, "present_stalled");
}

TEST(AgentTools, StallWatchHealthyWhenPresentAdvances)
{
	StallSample start {};
	start.graphic_ready = true;
	start.frame         = 100;
	start.present       = 50;
	start.fps           = 12.0;

	StallSample end = start;
	end.frame   = 220;
	end.present = 170;
	end.fps     = 11.5;

	StallWatchArgs args {};
	const StallWatchResult result = ClassifyStall(start, end, args);
	EXPECT_TRUE(result.healthy);
	EXPECT_EQ(result.code, StallCode::Healthy);
}

TEST(AgentTools, FrameScoreFlagsHotCorruption)
{
	const char* path = "/tmp/kyty_agent_score_hot.bmp";
	WriteBmp32HotBlocks(path, 128, 96);
	FrameScoreMetrics metrics {};
	ASSERT_TRUE(ScoreNativeBmp(path, &metrics));
	EXPECT_EQ(metrics.verdict, FrameVerdict::HotCorruption);
	EXPECT_STREQ(metrics.verdict_name, "hot_corruption");
	EXPECT_GE(metrics.hot_block_ratio, 0.08);
	const std::string json = FrameScoreToJson(metrics, path);
	EXPECT_NE(json.find("\"healthy\":false"), std::string::npos);
	EXPECT_NE(json.find("hot_corruption"), std::string::npos);
	std::remove(path);
}

TEST(AgentTools, FrameScoreFlagsWhiteWorld)
{
	const char* path = "/tmp/kyty_agent_score_white.bmp";
	WriteBmp32(path, 96, 64, 252, 252, 252);
	FrameScoreMetrics metrics {};
	ASSERT_TRUE(ScoreNativeBmp(path, &metrics));
	EXPECT_EQ(metrics.verdict, FrameVerdict::WhiteWorld);
	EXPECT_GE(metrics.white_ratio, 0.35);
	std::remove(path);
}

TEST(AgentTools, FrameScoreAcceptsVariedGameplayLikeFrame)
{
	const char*    path      = "/tmp/kyty_agent_score_ok.bmp";
	const uint32_t width     = 96;
	const uint32_t height    = 64;
	const uint32_t row_bytes = width * 4u;
	std::vector<uint8_t> file(54u + row_bytes * height, 0);
	file[0]                  = 'B';
	file[1]                  = 'M';
	const uint32_t file_size = static_cast<uint32_t>(file.size());
	file[2]                  = static_cast<uint8_t>(file_size);
	file[3]                  = static_cast<uint8_t>(file_size >> 8);
	file[4]                  = static_cast<uint8_t>(file_size >> 16);
	file[5]                  = static_cast<uint8_t>(file_size >> 24);
	file[10]                 = 54;
	file[14]                 = 40;
	file[18]                 = static_cast<uint8_t>(width);
	file[22]                 = static_cast<uint8_t>(height);
	file[26]                 = 1;
	file[28]                 = 32;
	for (uint32_t y = 0; y < height; ++y)
	{
		for (uint32_t x = 0; x < width; ++x)
		{
			const size_t off = 54u + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4u;
			file[off + 0]    = static_cast<uint8_t>((x * 3 + y * 5) % 180);
			file[off + 1]    = static_cast<uint8_t>((x * 7 + y * 2) % 160);
			file[off + 2]    = static_cast<uint8_t>((x * 11 + y * 13) % 140);
			file[off + 3]    = 255;
		}
	}
	std::ofstream out(path, std::ios::binary);
	out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
	out.close();

	FrameScoreMetrics metrics {};
	ASSERT_TRUE(ScoreNativeBmp(path, &metrics));
	EXPECT_EQ(metrics.verdict, FrameVerdict::Healthy);
	EXPECT_NE(FrameScoreToJson(metrics, path).find("\"healthy\":true"), std::string::npos);
	std::remove(path);
}

UT_END();
