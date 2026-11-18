#include "ShaderSpirvSelectionCut.h"

#include "Kyty/Core/DbgAssert.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

constexpr size_t   kMinFunctionVars  = 60;
constexpr size_t   kMinBodyBlocks    = 100;
constexpr size_t   kMinParkedScalars = 32;
constexpr uint32_t kInvalidBlock     = UINT32_MAX;

using Tokens = std::vector<std::string>;

Tokens Tokenize(const std::string& line)
{
	Tokens             tokens;
	std::istringstream words(line.substr(0, line.find(';')));
	for (std::string word; words >> word;)
	{
		tokens.push_back(word);
	}
	return tokens;
}

class Bitset
{
public:
	explicit Bitset(size_t bits = 0): m_words((bits + 63u) / 64u, 0u) {}

	void Set(size_t bit) { m_words[bit >> 6u] |= (uint64_t {1} << (bit & 63u)); }
	void Reset(size_t bit) { m_words[bit >> 6u] &= ~(uint64_t {1} << (bit & 63u)); }
	[[nodiscard]] bool Test(size_t bit) const { return (m_words[bit >> 6u] & (uint64_t {1} << (bit & 63u))) != 0u; }

	void FillOnes(size_t bits)
	{
		std::fill(m_words.begin(), m_words.end(), ~uint64_t {0});
		const size_t rem = bits & 63u;
		if (!m_words.empty() && rem != 0u)
		{
			m_words.back() = (uint64_t {1} << rem) - 1u;
		}
	}

	void OrWith(const Bitset& other)
	{
		for (size_t i = 0; i < m_words.size(); ++i)
		{
			m_words[i] |= other.m_words[i];
		}
	}

	void AndWith(const Bitset& other)
	{
		for (size_t i = 0; i < m_words.size(); ++i)
		{
			m_words[i] &= other.m_words[i];
		}
	}

	void AssignLiveStep(const Bitset& use, const Bitset& out, const Bitset& kill)
	{
		for (size_t i = 0; i < m_words.size(); ++i)
		{
			m_words[i] = use.m_words[i] | (out.m_words[i] & ~kill.m_words[i]);
		}
	}

	[[nodiscard]] bool operator==(const Bitset& other) const { return m_words == other.m_words; }
	[[nodiscard]] bool operator!=(const Bitset& other) const { return m_words != other.m_words; }

private:
	std::vector<uint64_t> m_words;
};

enum class ScalarKind : uint8_t
{
	Uint,
	Int,
	Float
};

struct ScalarVar
{
	std::string name;
	ScalarKind  kind;
	bool        has_initializer;
};

struct BasicBlock
{
	std::string           label;
	size_t                start_line        = 0;
	size_t                end_line          = 0;
	size_t                first_non_phi     = 0;
	std::vector<uint32_t> succs;
	std::vector<uint32_t> preds;
};

struct ParsedLine
{
	std::string text;
	Tokens      tokens;
	bool        has_result = false;
	std::string opcode;
	uint32_t    block      = kInvalidBlock;
};

const char* ScalarTypeName(ScalarKind kind)
{
	switch (kind)
	{
		case ScalarKind::Uint: return "%uint";
		case ScalarKind::Int: return "%int";
		case ScalarKind::Float: return "%float";
	}
	return "%uint";
}

bool TryClassifyScalarPointer(const std::string& ptr_type, ScalarKind* out_kind)
{
	if (ptr_type == "%_ptr_Function_uint")
	{
		*out_kind = ScalarKind::Uint;
		return true;
	}
	if (ptr_type == "%_ptr_Function_int")
	{
		*out_kind = ScalarKind::Int;
		return true;
	}
	if (ptr_type == "%_ptr_Function_float")
	{
		*out_kind = ScalarKind::Float;
		return true;
	}
	return false;
}

std::string BuildCutSequence(const char* tag, const std::vector<ScalarVar>& vars, const std::vector<size_t>& parked)
{
	std::string out;
	out.reserve(parked.size() * 192u);
	for (size_t k = 0; k < parked.size(); ++k)
	{
		const auto& var  = vars[parked[k]];
		const auto* ty   = ScalarTypeName(var.kind);
		const auto  base = "%lrc_" + std::string(tag) + "_" + std::to_string(k);
		out += base + "_a = OpLoad " + ty + " " + var.name + "\n";
		if (var.kind == ScalarKind::Uint)
		{
			out += base + "_b = OpIAdd %uint " + base + "_a %lrc_z\n";
			out += "OpStore " + var.name + " " + base + "_b\n";
		} else
		{
			out += base + "_u = OpBitcast %uint " + base + "_a\n";
			out += base + "_b = OpIAdd %uint " + base + "_u %lrc_z\n";
			out += base + "_r = OpBitcast " + std::string(ty) + " " + base + "_b\n";
			out += "OpStore " + var.name + " " + base + "_r\n";
		}
	}
	return out;
}

} // namespace

bool SpirvCutSelectionLiveRanges(const String8& main_body, const SpirvSelectionCutConfig& config, String8* out_constants,
                                 String8* out_main_body)
{
	EXIT_IF(out_constants == nullptr || out_main_body == nullptr);
	out_constants->Clear();
	out_main_body->Clear();

	// Fast pre-check before allocating line vectors/maps on small shaders.
	if (main_body.FindIndex("OpSelectionMerge") == Core::STRING8_INVALID_INDEX ||
	    main_body.FindIndex("OpLoopMerge") == Core::STRING8_INVALID_INDEX)
	{
		return false;
	}

	std::vector<ParsedLine> lines;
	{
		std::istringstream stream(main_body.c_str());
		for (std::string raw; std::getline(stream, raw);)
		{
			ParsedLine pl;
			pl.text       = std::move(raw);
			pl.tokens     = Tokenize(pl.text);
			pl.has_result = pl.tokens.size() >= 3u && pl.tokens[1] == "=";
			pl.opcode     = pl.has_result ? pl.tokens[2] : (pl.tokens.empty() ? std::string() : pl.tokens[0]);
			lines.push_back(std::move(pl));
		}
	}

	std::vector<BasicBlock>                   blocks;
	std::unordered_map<std::string, uint32_t> label_to_block;
	std::vector<ScalarVar>                    scalar_vars;
	std::unordered_map<std::string, size_t>   var_index;
	size_t                                    total_fn_vars       = 0;
	size_t                                    last_entry_var_line = 0;
	bool                                      in_function         = false;
	uint32_t                                  cur_block           = kInvalidBlock;

	for (size_t i = 0; i < lines.size(); ++i)
	{
		auto& l = lines[i];
		if (!in_function)
		{
			if (l.has_result && l.opcode == "OpFunction")
			{
				in_function = true;
			}
			continue;
		}
		if (l.opcode == "OpFunctionEnd")
		{
			break;
		}
		if (l.has_result && l.opcode == "OpLabel")
		{
			cur_block                = static_cast<uint32_t>(blocks.size());
			label_to_block[l.tokens[0]] = cur_block;
			BasicBlock bb;
			bb.label         = l.tokens[0];
			bb.start_line    = i;
			bb.end_line      = i;
			bb.first_non_phi = i + 1u;
			blocks.push_back(std::move(bb));
			l.block = cur_block;
			continue;
		}
		if (cur_block == kInvalidBlock)
		{
			continue;
		}
		l.block                    = cur_block;
		blocks[cur_block].end_line = i;
		if (i == blocks[cur_block].first_non_phi && (l.opcode == "OpPhi" || l.tokens.empty()))
		{
			blocks[cur_block].first_non_phi = i + 1u;
		}
		if (l.has_result && l.opcode == "OpVariable" && l.tokens.size() >= 5u && l.tokens[4] == "Function")
		{
			++total_fn_vars;
			if (cur_block == 0u)
			{
				last_entry_var_line = i;
			}
			ScalarKind kind = ScalarKind::Uint;
			if (TryClassifyScalarPointer(l.tokens[3], &kind))
			{
				var_index[l.tokens[0]] = scalar_vars.size();
				scalar_vars.push_back({l.tokens[0], kind, l.tokens.size() >= 6u});
			}
		}
	}

	if (blocks.empty() || total_fn_vars < kMinFunctionVars || scalar_vars.size() < kMinParkedScalars)
	{
		return false;
	}

	// Build CFG edges and collect loop headers.
	std::unordered_set<uint32_t> loop_headers;
	std::vector<std::string>     term_targets;
	for (uint32_t b = 0; b < blocks.size(); ++b)
	{
		for (size_t i = blocks[b].start_line; i <= blocks[b].end_line; ++i)
		{
			const auto& t  = lines[i].tokens;
			const auto& op = lines[i].opcode;
			if (op == "OpLoopMerge")
			{
				loop_headers.insert(b);
			}
			if (op == "OpBranch" && t.size() >= 2u)
			{
				term_targets = {t[1]};
			} else if (op == "OpBranchConditional" && t.size() >= 4u)
			{
				term_targets = {t[2], t[3]};
			} else if (op == "OpSwitch" && t.size() >= 3u)
			{
				term_targets.clear();
				for (size_t k = 2u; k < t.size(); ++k)
				{
					if (!t[k].empty() && t[k][0] == '%')
					{
						term_targets.push_back(t[k]);
					}
				}
			} else
			{
				continue;
			}
			for (const auto& lbl: term_targets)
			{
				const auto it = label_to_block.find(lbl);
				if (it != label_to_block.end())
				{
					blocks[b].succs.push_back(it->second);
					blocks[it->second].preds.push_back(b);
				}
			}
			break;
		}
	}

	if (loop_headers.empty())
	{
		return false;
	}

	const size_t n_blocks = blocks.size();
	const size_t n_vars   = scalar_vars.size();

	std::vector<Bitset> use(n_blocks, Bitset(n_vars));
	std::vector<Bitset> kill(n_blocks, Bitset(n_vars));
	std::vector<Bitset> mentions(n_blocks, Bitset(n_vars));
	std::vector<Bitset> defs(n_blocks, Bitset(n_vars));

	for (uint32_t b = 0; b < n_blocks; ++b)
	{
		for (size_t i = blocks[b].start_line; i <= blocks[b].end_line; ++i)
		{
			const auto& l = lines[i];
			if (l.opcode == "OpVariable" || l.tokens.empty())
			{
				continue;
			}
			if (!l.has_result && l.opcode == "OpStore" && l.tokens.size() >= 3u)
			{
				const auto it = var_index.find(l.tokens[1]);
				if (it != var_index.end())
				{
					const size_t v = it->second;
					mentions[b].Set(v);
					defs[b].Set(v);
					if (!use[b].Test(v))
					{
						kill[b].Set(v);
					}
					continue;
				}
			}
			if (l.has_result && l.opcode == "OpLoad" && l.tokens.size() >= 5u)
			{
				const auto it = var_index.find(l.tokens[4]);
				if (it != var_index.end())
				{
					const size_t v = it->second;
					mentions[b].Set(v);
					if (!kill[b].Test(v))
					{
						use[b].Set(v);
					}
				}
				continue;
			}
			const size_t start = l.has_result ? 3u : 1u;
			for (size_t k = start; k < l.tokens.size(); ++k)
			{
				const auto it = var_index.find(l.tokens[k]);
				if (it != var_index.end())
				{
					const size_t v = it->second;
					mentions[b].Set(v);
					// Template helpers take Function pointers for outputs as well as inputs.
					defs[b].Set(v);
					if (!kill[b].Test(v))
					{
						use[b].Set(v);
					}
					kill[b].Reset(v);
				}
			}
		}
	}

	// Backward liveness over CFG.
	std::vector<Bitset> live_in = use;
	Bitset              scratch_out(n_vars);
	Bitset              scratch_next(n_vars);
	bool                changed = true;
	while (changed)
	{
		changed = false;
		for (size_t idx = n_blocks; idx-- > 0;)
		{
			scratch_out = Bitset(n_vars);
			for (uint32_t s: blocks[idx].succs)
			{
				scratch_out.OrWith(live_in[s]);
			}
			scratch_next.AssignLiveStep(use[idx], scratch_out, kill[idx]);
			if (scratch_next != live_in[idx])
			{
				live_in[idx] = scratch_next;
				changed      = true;
			}
		}
	}

	// Forward must-initialized analysis over reachable blocks from block 0.
	std::vector<uint8_t> reachable(n_blocks, 0);
	{
		std::vector<uint32_t> work = {0u};
		reachable[0]               = 1;
		while (!work.empty())
		{
			const uint32_t b = work.back();
			work.pop_back();
			for (uint32_t s: blocks[b].succs)
			{
				if (!reachable[s])
				{
					reachable[s] = 1;
					work.push_back(s);
				}
			}
		}
	}

	std::vector<Bitset> must_init_out(n_blocks, Bitset(n_vars));
	Bitset              entry_inits(n_vars);
	for (size_t v = 0; v < n_vars; ++v)
	{
		if (scalar_vars[v].has_initializer)
		{
			entry_inits.Set(v);
		}
	}
	must_init_out[0] = entry_inits;
	must_init_out[0].OrWith(defs[0]);
	for (size_t b = 1; b < n_blocks; ++b)
	{
		must_init_out[b].FillOnes(n_vars);
	}
	changed = true;
	while (changed)
	{
		changed = false;
		for (size_t b = 1; b < n_blocks; ++b)
		{
			if (!reachable[b])
			{
				continue;
			}
			bool has_reach_pred = false;
			for (uint32_t p: blocks[b].preds)
			{
				if (!reachable[p])
				{
					continue;
				}
				if (!has_reach_pred)
				{
					scratch_next   = must_init_out[p];
					has_reach_pred = true;
				} else
				{
					scratch_next.AndWith(must_init_out[p]);
				}
			}
			if (!has_reach_pred)
			{
				scratch_next = Bitset(n_vars);
			}
			scratch_next.OrWith(defs[b]);
			if (scratch_next != must_init_out[b])
			{
				must_init_out[b] = scratch_next;
				changed          = true;
			}
		}
	}

	// Score candidate one-armed selection constructs enclosing at least one loop.
	struct Candidate
	{
		size_t              sel_line    = 0;
		uint32_t            merge_block = kInvalidBlock;
		size_t              score       = 0;
		std::vector<size_t> parked;
	};
	Candidate             best;
	std::vector<uint8_t>  in_body(n_blocks);
	std::vector<uint32_t> bfs;
	for (size_t i = 0; i + 1u < lines.size(); ++i)
	{
		if (lines[i].opcode != "OpSelectionMerge" || lines[i].block == kInvalidBlock || !reachable[lines[i].block] ||
		    lines[i].tokens.size() < 2u || lines[i + 1u].opcode != "OpBranchConditional" || lines[i + 1u].tokens.size() < 4u)
		{
			continue;
		}
		const uint32_t hdr     = lines[i].block;
		const auto     mg_it   = label_to_block.find(lines[i].tokens[1]);
		if (mg_it == label_to_block.end() || mg_it->second == hdr)
		{
			continue;
		}
		const uint32_t    mg = mg_it->second;
		const std::string t0 = lines[i + 1u].tokens[2];
		const std::string t1 = lines[i + 1u].tokens[3];
		if (t0 == t1 || (t0 != lines[i].tokens[1] && t1 != lines[i].tokens[1]))
		{
			continue;
		}
		const auto be_it = label_to_block.find(t0 == lines[i].tokens[1] ? t1 : t0);
		if (be_it == label_to_block.end() || be_it->second == mg)
		{
			continue;
		}

		std::fill(in_body.begin(), in_body.end(), 0);
		bfs.clear();
		bfs.push_back(be_it->second);
		in_body[be_it->second] = 1;
		size_t body_count      = 0;
		bool   has_loop        = false;
		Bitset in_reg(n_vars);
		for (size_t q = 0; q < bfs.size(); ++q)
		{
			const uint32_t b = bfs[q];
			++body_count;
			if (loop_headers.count(b) != 0u)
			{
				has_loop = true;
			}
			in_reg.OrWith(mentions[b]);
			for (uint32_t s: blocks[b].succs)
			{
				if (s != mg && !in_body[s])
				{
					in_body[s] = 1;
					bfs.push_back(s);
				}
			}
		}
		if (!has_loop || body_count < kMinBodyBlocks)
		{
			continue;
		}

		std::vector<size_t> parked;
		for (size_t v = 0; v < n_vars; ++v)
		{
			if (!in_reg.Test(v) && live_in[mg].Test(v) && must_init_out[hdr].Test(v))
			{
				parked.push_back(v);
			}
		}
		if (parked.size() < kMinParkedScalars)
		{
			continue;
		}
		const size_t score = parked.size() * body_count;
		if (score > best.score || (score == best.score && parked.size() > best.parked.size()))
		{
			best.sel_line    = i;
			best.merge_block = mg;
			best.score       = score;
			best.parked      = std::move(parked);
		}
	}

	if (best.merge_block == kInvalidBlock)
	{
		return false;
	}

	*out_constants = String8::FromPrintf(
	    "%%lrc_u15 = OpConstant %%uint 15\n"
	    "%%lrc_i0 = OpConstant %%int 0\n"
	    "%%lrc_blk = OpConstant %%int %u\n",
	    config.device_address_block);

	const char* vsharp_ptr_ty = config.vsharp_uniform_buffer ? "%_ptr_Uniform_uint" : "%_ptr_PushConstant_uint";
	const auto  init_zero     = String8::FromPrintf(
        "%%lrc_zp = OpAccessChain %s %%vsharp %%lrc_i0 %%lrc_blk %%lrc_i0\n"
        "%%lrc_tl = OpLoad %%uint %%lrc_zp\n"
        "%%lrc_z = OpBitwiseAnd %%uint %%lrc_tl %%lrc_u15\n",
        vsharp_ptr_ty);
	const std::string pre_cut  = BuildCutSequence("pre", scalar_vars, best.parked);
	const std::string post_cut = BuildCutSequence("post", scalar_vars, best.parked);
	const size_t      mg_line  = blocks[best.merge_block].first_non_phi;

	std::string rebuilt;
	rebuilt.reserve(main_body.Size() + pre_cut.size() + post_cut.size() + 256u);
	for (size_t i = 0; i < lines.size(); ++i)
	{
		if (i == best.sel_line)
		{
			rebuilt += pre_cut;
		}
		if (i == mg_line)
		{
			rebuilt += post_cut;
		}
		rebuilt += lines[i].text;
		rebuilt.push_back('\n');
		if (i == last_entry_var_line)
		{
			rebuilt += init_zero.c_str();
		}
	}
	*out_main_body = String8(rebuilt.c_str());
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
