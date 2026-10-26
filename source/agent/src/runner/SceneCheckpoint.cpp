#include "Kyty/Agent/Runner/SceneCheckpoint.h"

#include "Kyty/Agent/Runner/Sha256.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>

namespace Kyty::Agent::Runner {
namespace {

namespace fs = std::filesystem;

constexpr size_t   kMaxContractBytes  = 65536;
constexpr size_t   kMaxReferenceBytes = 32u * 1024u * 1024u;
constexpr uint64_t kMaxPixels         = 4096ull * 4096ull;

using Box = std::array<double, 4>;

bool Number(const Json& value, double low, double high, double* out = nullptr)
{
	if (!value.IsNumber() || !std::isfinite(value.AsNumber()) || value.AsNumber() < low || value.AsNumber() > high)
	{
		return false;
	}
	if (out != nullptr)
	{
		*out = value.AsNumber();
	}
	return true;
}

bool IntegerNumber(const Json& value, double low, double high)
{
	double number = 0.0;
	return Number(value, low, high, &number) && std::floor(number) == number;
}

bool Region(const Json& spec, Box* box)
{
	const Json& values = spec["box"];
	if (!values.IsArray() || values.Size() != 4)
	{
		return false;
	}
	for (size_t i = 0; i < 4; ++i)
	{
		if (!Number(values[i], 0, 1, &(*box)[i]))
		{
			return false;
		}
	}
	return (*box)[0] < (*box)[2] && (*box)[1] < (*box)[3] && Number(spec["max_mean_error"], 0, 64);
}

struct Difference
{
	double mean_error    = 0.0;
	double changed_ratio = 0.0;
};

// At most 16384 samples per region, independent of the source resolution.
Difference Compare(const RgbImage& a, const RgbImage& b, const Box& box, double pixel_delta = 1)
{
	const auto nx = static_cast<uint32_t>(
	    std::min<int64_t>(128, std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(std::min(a.width, b.width)) * (box[2] - box[0])))));
	const auto ny = static_cast<uint32_t>(
	    std::min<int64_t>(128, std::max<int64_t>(1, static_cast<int64_t>(static_cast<double>(std::min(a.height, b.height)) * (box[3] - box[1])))));
	auto sample = [](const RgbImage& image, double u, double v)
	{
		const auto x = std::min<int64_t>(image.width - 1, static_cast<int64_t>(u * image.width));
		const auto y = std::min<int64_t>(image.height - 1, static_cast<int64_t>(v * image.height));
		return image.Pixel(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
	};
	double error   = 0.0;
	double changed = 0.0;
	for (uint32_t y = 0; y < ny; ++y)
	{
		const double v = box[1] + (y + 0.5) * (box[3] - box[1]) / ny;
		for (uint32_t x = 0; x < nx; ++x)
		{
			const double   u  = box[0] + (x + 0.5) * (box[2] - box[0]) / nx;
			const uint8_t* pa = sample(a, u, v);
			const uint8_t* pb = sample(b, u, v);
			int            sum     = 0;
			int            largest = 0;
			for (int i = 0; i < 3; ++i)
			{
				const int delta = std::abs(static_cast<int>(pa[i]) - static_cast<int>(pb[i]));
				sum += delta;
				largest = std::max(largest, delta);
			}
			error += static_cast<double>(sum) / 3;
			changed += static_cast<double>(largest >= pixel_delta);
		}
	}
	const double samples = static_cast<double>(nx) * ny;
	return {error / samples, changed / samples};
}

bool LoadPng(const std::string& path, RgbImage* image, std::string* digest, std::string* error)
{
	std::vector<uint8_t> bytes;
	if (!ReadFileBytes(path, kMaxReferenceBytes, &bytes, error) || !DecodePng(bytes, kMaxPixels, image, error))
	{
		return false;
	}
	if (digest != nullptr)
	{
		*digest = Sha256Hex(bytes.data(), bytes.size());
	}
	return true;
}

bool FileDigest(const std::string& path, std::string* digest)
{
	std::vector<uint8_t> bytes;
	std::string          error;
	if (!ReadFileBytes(path, kMaxReferenceBytes, &bytes, &error))
	{
		return false;
	}
	*digest = Sha256Hex(bytes.data(), bytes.size());
	return true;
}

bool ValidStep(const Json& step)
{
	const std::string& tool = step["tool"].AsString();
	if (tool == "pad_tap" || tool == "pad_down" || tool == "pad_up")
	{
		if (!step["button"].IsString() || step["button"].AsString().empty())
		{
			return false;
		}
		return tool != "pad_down" || Number(step["hold_s"], 0.001, 30);
	}
	if (tool != "pad_axis")
	{
		return false;
	}
	const std::string& axis = step["axis"].AsString();
	if (axis != "left_x" && axis != "left_y" && axis != "right_x" && axis != "right_y")
	{
		return false;
	}
	const Json& value = step["value"];
	if (!value.IsInteger() || value.AsNumber() < 0 || value.AsNumber() > 255)
	{
		return false;
	}
	return !step.Has("hold_s") || Number(step["hold_s"], 0, 30);
}

bool ValidAction(const Json& action, std::string* error)
{
	const Json& sequence = action["pad_sequence"];
	if (!sequence.IsArray() || sequence.Size() < 1 || sequence.Size() > 64)
	{
		*error = "checkpoint needs a bounded action sequence";
		return false;
	}
	if (!IntegerNumber(action["min_present_delta"], 1, 10000) || !Number(action["min_settle_s"], 0, 30))
	{
		*error = "checkpoint action bounds out of range";
		return false;
	}
	const Json& milestones = action["milestones"];
	if (!milestones.IsObject())
	{
		*error = "checkpoint needs action milestones";
		return false;
	}
	for (const char* key: {"min_pad_taps", "min_guest_read_state_samples", "min_guest_read_samples"})
	{
		if (milestones.Has(key) && !IntegerNumber(milestones[key], 0, 100000))
		{
			*error = "checkpoint input counts must be integers";
			return false;
		}
	}
	if (!milestones.Has("min_pad_taps"))
	{
		*error = "checkpoint must declare its tap count (zero for holds)";
		return false;
	}
	int64_t taps = 0;
	for (const Json& step: sequence.Items())
	{
		if (!ValidStep(step))
		{
			*error = "invalid checkpoint action";
			return false;
		}
		taps += static_cast<int64_t>(step["tool"].AsString() == "pad_tap");
	}
	if (static_cast<double>(taps) != milestones["min_pad_taps"].AsNumber())
	{
		*error = "checkpoint tap count does not match its sequence";
		return false;
	}
	return true;
}

bool ValidContract(const Json& contract, std::string* error)
{
	if (contract["schema"].AsString() != "kyty_scene_checkpoint_v1" || contract["fixture"].AsString().empty())
	{
		*error = "invalid checkpoint identity";
		return false;
	}
	const std::string& kind = contract["kind"].AsString();
	if (kind != "gameplay" && kind != "menu" && kind != "boot" && kind != "non_gameplay")
	{
		*error = "invalid checkpoint kind";
		return false;
	}
	const Json& regions = contract["scene_regions"];
	if (!regions.IsArray() || regions.Size() < 1 || regions.Size() > 16)
	{
		*error = "checkpoint needs 1..16 scene regions";
		return false;
	}
	Box box {};
	for (const Json& spec: regions.Items())
	{
		if (!Region(spec, &box))
		{
			*error = "checkpoint region must have positive area and bounded error";
			return false;
		}
	}
	const Json& response = contract["response"];
	if (!Region(response, &box) || !Number(response["pixel_delta"], 1, 255) || !Number(response["min_changed_ratio"], 0.0001, 1))
	{
		*error = "invalid checkpoint response";
		return false;
	}
	return ValidAction(contract["action"], error);
}

Json RegionErrors(const RgbImage& image, const RgbImage& reference, const Json& regions, bool* matches)
{
	Json errors = Json::MakeArray();
	*matches    = true;
	for (const Json& spec: regions.Items())
	{
		Box box {};
		(void)Region(spec, &box);
		const double error = Compare(image, reference, box).mean_error;
		errors.Push(error);
		*matches = *matches && error <= spec["max_mean_error"].AsNumber();
	}
	return errors;
}

} // namespace

bool SceneCheckpoint::Load(const std::string& contract_path, std::string* error)
{
	std::vector<uint8_t> payload;
	if (!ReadFileBytes(contract_path, kMaxContractBytes, &payload, error))
	{
		return false;
	}
	if (!ParseJson(std::string_view(reinterpret_cast<const char*>(payload.data()), payload.size()), &m_contract, error) ||
	    !ValidContract(m_contract, error))
	{
		return false;
	}
	m_sha256 = Sha256Hex(payload.data(), payload.size());

	const fs::path base = fs::path(contract_path).parent_path();
	for (const char* role: {"before", "after"})
	{
		const Json& reference = m_contract[role];
		RgbImage*   image     = std::string(role) == "before" ? &m_before : &m_after;
		std::string digest;
		const auto  path = (base / reference["image"].AsString()).lexically_normal().string();
		if (!reference["image"].IsString() || !LoadPng(path, image, &digest, error) || digest != reference["sha256"].AsString())
		{
			*error = "checkpoint reference hash/size mismatch";
			return false;
		}
		(std::string(role) == "before" ? m_before_sha256 : m_after_sha256) = digest;
	}
	const Json& response = m_contract["response"];
	Box         box {};
	(void)Region(response, &box);
	const Difference reference_delta = Compare(m_before, m_after, box, response["pixel_delta"].AsNumber());
	if (reference_delta.changed_ratio < response["min_changed_ratio"].AsNumber() ||
	    reference_delta.mean_error <= 2 * response["max_mean_error"].AsNumber())
	{
		*error = "checkpoint references do not distinguish the expected response";
		return false;
	}
	return true;
}

Json SceneCheckpoint::Match(const std::string& path, const char* role) const
{
	RgbImage    image;
	std::string error;
	Json        out = Json::MakeObject();
	if (!LoadPng(path, &image, nullptr, &error))
	{
		out.Set("matches", false);
		out.Set("error", error);
		return out;
	}
	bool matches = false;
	Json errors  = RegionErrors(image, std::string(role) == "before" ? m_before : m_after, m_contract["scene_regions"], &matches);
	out.Set("matches", matches);
	out.Set("region_errors", std::move(errors));
	return out;
}

Json SceneCheckpoint::Evaluate(const Json& before, const Json& after, const Json& action) const
{
	Json result = Json::MakeObject();
	result.Set("contract_sha256", m_sha256);
	result.Set("fixture", m_contract["fixture"]);
	result.Set("kind", m_contract["kind"]);
	result.Set("reference_sha256", Json::MakeObject().Set("before", m_before_sha256).Set("after", m_after_sha256));
	result.Set("action", action);

	Json checks = Json::MakeObject();
	checks.Set("gameplay_checkpoint", m_contract["kind"].AsString() == "gameplay");
	bool scene_ok[2] = {false, false};
	int  index       = 0;
	for (const auto& [role, record]: {std::pair<const char*, const Json*> {"before", &before}, {"after", &after}})
	{
		const std::string path   = (*record)["path"].AsString();
		std::string       digest;
		std::error_code   ec;
		const bool        bound = !path.empty() && fs::is_regular_file(path, ec) && (*record)["source"].AsString() == "kyty_agent.capture" &&
		                   FileDigest(path, &digest) && digest == (*record)["sha256"].AsString();
		Json match = bound ? Match(path, role) : Json::MakeObject().Set("matches", false);
		scene_ok[index] = bound && match["matches"].AsBool();
		checks.Set(std::string(role) + "_scene", scene_ok[index]);
		Json merged = Json::MakeObject();
		for (const auto& [key, value]: record->Members())
		{
			if (key != "path")
			{
				merged.Set(key, value);
			}
		}
		for (const auto& [key, value]: match.Members())
		{
			merged.Set(key, value);
		}
		result.Set(role, std::move(merged));
		++index;
	}

	const int64_t before_present = before["present"].AsInt();
	const int64_t after_present  = after["present"].AsInt();
	const int64_t start_present  = action["start_present"].AsInt();
	const int64_t end_present    = action["end_present"].AsInt();
	checks.Set("capture_input_order",
	           before_present > 0 && after_present > end_present && end_present >= start_present && start_present >= before_present);
	bool delivered = action["delivery_checks"].IsObject() && action["delivery_checks"].Size() > 0;
	for (const auto& [name, value]: action["delivery_checks"].Members())
	{
		delivered = delivered && value.Truthy();
	}
	checks.Set("action_delivered", delivered);
	checks.Set("action_settled", action["settled"].Truthy());
	checks.Set("expected_response", false);

	if (scene_ok[0] && scene_ok[1])
	{
		RgbImage    a;
		RgbImage    b;
		std::string error;
		if (LoadPng(before["path"].AsString(), &a, nullptr, &error) && LoadPng(after["path"].AsString(), &b, nullptr, &error))
		{
			const Json&  spec      = m_contract["response"];
			const double max_error = spec["max_mean_error"].AsNumber();
			Box          box {};
			(void)Region(spec, &box);
			const double     before_error       = Compare(a, m_before, box).mean_error;
			const double     after_error        = Compare(b, m_after, box).mean_error;
			const double     wrong_before_error = Compare(a, m_after, box).mean_error;
			const double     wrong_after_error  = Compare(b, m_before, box).mean_error;
			const Difference delta              = Compare(a, b, box, spec["pixel_delta"].AsNumber());
			checks.Set("expected_response", before_error <= max_error && after_error <= max_error && wrong_before_error > max_error &&
			                                    wrong_after_error > max_error && delta.changed_ratio >= spec["min_changed_ratio"].AsNumber());
			result.Set("response", Json::MakeObject()
			                           .Set("mean_error", delta.mean_error)
			                           .Set("changed_ratio", delta.changed_ratio)
			                           .Set("before_error", before_error)
			                           .Set("after_error", after_error)
			                           .Set("wrong_before_error", wrong_before_error)
			                           .Set("wrong_after_error", wrong_after_error));
		}
	}
	result.Set("checks", std::move(checks));
	return result;
}

} // namespace Kyty::Agent::Runner
