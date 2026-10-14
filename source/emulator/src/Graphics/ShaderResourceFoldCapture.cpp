#include "ShaderResourceFoldCapture.h"

#include "Emulator/Config.h"
#include "Emulator/Log.h"

#include "Kyty/Core/VirtualMemory.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <utility>

namespace Kyty::Libs::Graphics {

namespace {

// Schema bounds. Every element count is checked before the owned container grows.
constexpr uint32_t k_max_instructions   = 4096;
constexpr uint32_t k_max_labels         = 4096;
constexpr uint32_t k_max_metadata_slots = 256; // per direct or sharp metadata array
constexpr uint32_t k_max_nesting        = 16;
constexpr uint32_t k_max_integer_digits = 10;
constexpr int64_t  k_u32_max            = 0xffffffffLL;
constexpr uint32_t k_mimg_address_slots = 13;
constexpr uint32_t k_src_slots          = 4;
constexpr int      k_user_sgprs         = HW::UserSgprInfo::SGPRS_MAX;
constexpr uint16_t k_unused_direct      = 0xffffu;
constexpr uint16_t k_unused_sharp       = 0x7fffu;

std::atomic<uint32_t> g_capture_count {0};
thread_local bool     g_capture_active = false; // set while a capture or a replay is running

std::atomic<ShaderResourceFoldCaptureTestSink> g_test_sink {nullptr};
std::mutex                                     g_test_sink_mutex;
void*                                          g_test_sink_context = nullptr;

bool Fail(std::string* error, const char* message)
{
	if (error != nullptr)
	{
		*error = message;
	}
	return false;
}

// ---- Writer -----------------------------------------------------------------

// Compact writer for the schema: objects, arrays, integers and the one canonical
// output string. Keys are emitted in the fixed order the strict reader expects.
class DocumentWriter
{
public:
	void BeginObject()
	{
		BeginValue();
		m_text += '{';
		m_first.push_back(true);
	}
	void EndObject()
	{
		m_first.pop_back();
		m_text += '}';
	}
	void BeginArray()
	{
		BeginValue();
		m_text += '[';
		m_first.push_back(true);
	}
	void EndArray()
	{
		m_first.pop_back();
		m_text += ']';
	}
	void Key(const char* key)
	{
		Separate();
		m_text += '"';
		m_text += key;
		m_text += "\":";
		m_after_key = true;
	}
	void Int(int64_t value)
	{
		BeginValue();
		m_text += std::to_string(value);
	}
	void Bool(bool value) { Int(value ? 1 : 0); }
	// A value produced by this writer in an earlier step.
	void Raw(const std::string& value)
	{
		BeginValue();
		m_text += value;
	}
	// Printable ASCII and newlines only; the newline is the single escape.
	[[nodiscard]] bool String(const std::string& value)
	{
		BeginValue();
		m_text += '"';
		for (const char c: value)
		{
			if (c == '\n')
			{
				m_text += "\\n";
				continue;
			}
			if (c < 0x20 || c > 0x7e || c == '"' || c == '\\')
			{
				return false;
			}
			m_text += c;
		}
		m_text += '"';
		return true;
	}
	[[nodiscard]] const std::string& Text() const { return m_text; }

private:
	void Separate()
	{
		if (!m_first.empty())
		{
			if (!m_first.back())
			{
				m_text += ',';
			}
			m_first.back() = false;
		}
	}
	void BeginValue()
	{
		if (m_after_key)
		{
			m_after_key = false;
		}
		else
		{
			Separate();
		}
	}

	std::string       m_text;
	std::vector<bool> m_first;
	bool              m_after_key = false;
};

void WriteOperand(DocumentWriter& w, const ShaderOperand& op)
{
	uint32_t multiplier_bits = 0;
	std::memcpy(&multiplier_bits, &op.multiplier, sizeof(multiplier_bits));
	w.BeginArray();
	w.Int(static_cast<int64_t>(op.type));
	w.Int(op.constant.u);
	w.Int(op.register_id);
	w.Int(op.size);
	w.Int(multiplier_bits);
	w.Bool(op.absolute);
	w.Bool(op.negate);
	w.Bool(op.clamp);
	w.Int(op.swizzle);
	w.Bool(op.dpp);
	w.Int(op.dpp_ctrl);
	w.Int(op.dpp_row_mask);
	w.Int(op.dpp_bank_mask);
	w.Bool(op.dpp_fetch_inactive);
	w.Bool(op.dpp_bound_ctrl);
	w.EndArray();
}

// Field order here is the schema contract; ReadInstruction must mirror it.
void WriteInstruction(DocumentWriter& w, const ShaderInstruction& inst)
{
	const auto format = static_cast<uint64_t>(inst.format);
	w.BeginArray();
	w.Int(inst.pc);
	w.Int(static_cast<int64_t>(inst.type));
	w.Int(static_cast<int64_t>(format & 0xffffffffu));
	w.Int(static_cast<int64_t>(format >> 32u));
	w.Int(inst.sopp_opcode);
	w.Int(inst.raw_word);
	w.BeginArray();
	for (const auto& op: inst.src)
	{
		WriteOperand(w, op);
	}
	w.EndArray();
	w.Int(inst.src_num);
	w.Int(inst.exp_control);
	WriteOperand(w, inst.dst);
	WriteOperand(w, inst.dst2);
	w.Int(inst.vop3_op_sel);
	w.Int(inst.vop3_omod);
	w.Int(inst.vop3p_op_sel_hi);
	w.Bool(inst.vop_sdwa);
	w.Int(inst.vop_sdwa_ctrl);
	w.BeginArray();
	for (const auto& op: inst.mimg_address)
	{
		WriteOperand(w, op);
	}
	w.EndArray();
	w.Int(inst.mimg_address_num);
	w.Int(inst.mimg_dmask);
	w.Int(inst.mimg_dimension);
	w.Bool(inst.mimg_explicit_lod);
	w.Bool(inst.mimg_offset);
	w.Bool(inst.mimg_return_old_value);
	w.Int(inst.smem_imm_offset);
	w.Int(inst.flat_offset);
	w.Int(inst.smem_flags);
	w.Int(inst.buffer_imm_offset);
	w.Bool(inst.buffer_idxen);
	w.Bool(inst.buffer_offen);
	w.Bool(inst.buffer_return_old_value);
	w.Int(inst.buffer_flags);
	w.Int(inst.mtbuf_format);
	w.Int(inst.mtbuf_components);
	w.Bool(inst.mtbuf_format_is_gen5);
	w.Int(inst.ds_offset);
	w.Int(inst.ds_encoding_control);
	w.Int(inst.ds_encoding_registers);
	w.Int(inst.exp_enable_mask);
	w.EndArray();
}

void WriteLabels(DocumentWriter& w, const char* key, const Vector<ShaderLabel>& labels)
{
	w.Key(key);
	w.BeginArray();
	for (const auto& label: labels)
	{
		w.BeginArray();
		w.Int(label.GetDst());
		w.Int(label.GetSrc());
		w.EndArray();
	}
	w.EndArray();
}

bool WriteCode(DocumentWriter& w, const ShaderCode& code)
{
	if (!code.GetDebugPrintfs().IsEmpty() || code.GetInstructions().Size() > k_max_instructions ||
	    code.GetLabels().Size() > k_max_labels || code.GetIndirectLabels().Size() > k_max_labels)
	{
		return false; // debug printf payloads and over-bound code are not representable
	}
	w.BeginObject();
	w.Key("type");
	w.Int(static_cast<int64_t>(code.GetType()));
	w.Key("vs_embedded");
	w.Bool(code.IsVsEmbedded());
	w.Key("vs_embedded_id");
	w.Int(code.GetVsEmbeddedId());
	w.Key("ps_embedded");
	w.Bool(code.IsPsEmbedded());
	w.Key("ps_embedded_id");
	w.Int(code.GetPsEmbeddedId());
	w.Key("continuation_pc");
	w.Int(code.GetContinuationPc());
	w.Key("hash0");
	w.Int(code.GetHash0());
	w.Key("crc32");
	w.Int(code.GetCrc32());
	WriteLabels(w, "labels", code.GetLabels());
	WriteLabels(w, "indirect_labels", code.GetIndirectLabels());
	w.Key("instructions");
	w.BeginArray();
	for (const auto& inst: code.GetInstructions())
	{
		WriteInstruction(w, inst);
	}
	w.EndArray();
	w.EndObject();
	return true;
}

bool WriteUserData(DocumentWriter& w, const ShaderUserData& data)
{
	// The evaluator dereferences these arrays for every counted slot.
	if (static_cast<uint32_t>(data.direct_resource_count) > k_max_metadata_slots ||
	    (data.direct_resource_count != 0 && data.direct_resource_offset == nullptr))
	{
		return false;
	}
	w.BeginObject();
	w.Key("eud_size_dw");
	w.Int(data.eud_size_dw);
	w.Key("srt_size_dw");
	w.Int(data.srt_size_dw);
	w.Key("direct_count");
	w.Int(data.direct_resource_count);
	w.Key("direct_present");
	w.Bool(data.direct_resource_offset != nullptr);
	w.Key("direct");
	w.BeginArray();
	for (uint16_t i = 0; data.direct_resource_offset != nullptr && i < data.direct_resource_count; ++i)
	{
		w.Int(data.direct_resource_offset[i]);
	}
	w.EndArray();
	w.Key("sharp");
	w.BeginArray();
	for (int category = 0; category < 4; ++category)
	{
		const uint16_t count   = data.sharp_resource_count[category];
		const bool     present = data.sharp_resource_offset[category] != nullptr;
		if (static_cast<uint32_t>(count) > k_max_metadata_slots || (count != 0 && !present))
		{
			return false;
		}
		w.BeginObject();
		w.Key("count");
		w.Int(count);
		w.Key("present");
		w.Bool(present);
		w.Key("entries");
		w.BeginArray();
		for (uint16_t slot = 0; present && slot < count; ++slot)
		{
			const auto& sharp = data.sharp_resource_offset[category][slot];
			w.BeginArray();
			w.Int(sharp.offset_dw);
			w.Int(sharp.size);
			w.EndArray();
		}
		w.EndArray();
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	return true;
}

// ShaderParseUsage2 only adds to the caller's bind. The callers seed its layout
// (push constant offset and size, descriptor set slot); any other prepopulated
// field would be evaluator input that the schema does not carry. The comparison
// uses the canonical text, which lists every bind field.
bool InitialBindHoldsOnlyLayoutSeeds(const ShaderBindResources& initial)
{
	const auto fresh    = std::make_unique<ShaderBindResources>();
	auto       stripped = std::make_unique<ShaderBindResources>(initial);
	stripped->push_constant_offset = fresh->push_constant_offset;
	stripped->push_constant_size   = fresh->push_constant_size;
	stripped->descriptor_set_slot  = fresh->descriptor_set_slot;
	const ShaderParsedUsage usage {};
	return ShaderResourceFoldCanonicalOutput(usage, *stripped) == ShaderResourceFoldCanonicalOutput(usage, *fresh);
}

// The evaluator resets every usage field at entry except vertex_attrib and
// vertex_attrib_reg, so those two and the three bind layout seeds are the whole
// initial output state.
void WriteInitialOutput(DocumentWriter& w, const ShaderParsedUsage& usage, const ShaderBindResources& bind)
{
	w.BeginObject();
	w.Key("push_constant_offset");
	w.Int(bind.push_constant_offset);
	w.Key("push_constant_size");
	w.Int(bind.push_constant_size);
	w.Key("descriptor_set_slot");
	w.Int(bind.descriptor_set_slot);
	w.Key("vertex_attrib");
	w.Bool(usage.vertex_attrib);
	w.Key("vertex_attrib_reg");
	w.Int(usage.vertex_attrib_reg);
	w.EndObject();
}

bool WriteParse(DocumentWriter& w, const ShaderResourceFoldInputs& in, GuestPlatform platform)
{
	w.BeginObject();
	w.Key("guest_platform");
	w.Int(static_cast<int64_t>(platform));
	w.Key("user_sgpr_num");
	w.Int(in.user_sgpr_num);
	w.Key("user_data_register_base");
	w.Int(in.user_data_register_base);
	w.Key("vertex_resource_types");
	w.Bool(in.vertex_resource_types);
	w.Key("initial_output");
	WriteInitialOutput(w, *in.initial_usage, *in.initial_bind);
	w.Key("user_sgpr");
	w.BeginObject();
	w.Key("count");
	w.Int(in.user_sgpr->count);
	w.Key("value");
	w.BeginArray();
	for (const auto value: in.user_sgpr->value)
	{
		w.Int(value);
	}
	w.EndArray();
	w.Key("type");
	w.BeginArray();
	for (const auto type: in.user_sgpr->type)
	{
		w.Int(static_cast<int64_t>(type));
	}
	w.EndArray();
	w.EndObject();
	w.Key("user_data");
	if (!WriteUserData(w, *in.user_data))
	{
		return false;
	}
	w.Key("code_present");
	w.Bool(in.code != nullptr);
	if (in.code != nullptr)
	{
		w.Key("code");
		if (!WriteCode(w, *in.code))
		{
			return false;
		}
	}
	w.EndObject();
	return true;
}

bool WriteTranscript(DocumentWriter& w, const ShaderResourceFoldTranscript& transcript)
{
	w.BeginObject();
	w.Key("reads");
	w.BeginArray();
	for (const auto& read: transcript.reads)
	{
		if (read.dwords == 0 || read.dwords > static_cast<uint32_t>(SHADER_GEN5_EUD_MAX_DWORDS) ||
		    read.words.size() != (read.success ? read.dwords : 0u))
		{
			return false;
		}
		w.BeginObject();
		w.Key("address_hi");
		w.Int(static_cast<int64_t>(read.address >> 32u));
		w.Key("address_lo");
		w.Int(static_cast<int64_t>(read.address & 0xffffffffu));
		w.Key("dwords");
		w.Int(read.dwords);
		w.Key("success");
		w.Bool(read.success);
		w.Key("words");
		w.BeginArray();
		for (const uint32_t word: read.words)
		{
			w.Int(word);
		}
		w.EndArray();
		w.EndObject();
	}
	w.EndArray();
	w.EndObject();
	return true;
}

// ---- Strict reader ----------------------------------------------------------

// Schema-directed reader over the exact document grammar: objects with keys in
// the fixed schema order, arrays, decimal integers without fraction or exponent,
// and the canonical output string. It allocates nothing itself; callers grow
// owned containers one checked element at a time. Unknown, duplicate, missing
// or reordered keys, trailing data and incomplete input are all rejected.
class StrictReader
{
public:
	StrictReader(const std::string& text, std::string* error): m_text(text), m_error(error) {}

	bool BeginObject() { return Open('{'); }
	bool EndObject() { return Close('}'); }
	bool BeginArray() { return Open('['); }

	// Advances to the next array element; *more is false once ']' is consumed.
	bool Next(bool* more)
	{
		SkipSpace();
		if (m_first.empty())
		{
			return Reject("array element outside an array");
		}
		if (Peek() == ']')
		{
			++m_pos;
			m_first.pop_back();
			*more = false;
			return true;
		}
		if (!m_first.back() && !Expect(','))
		{
			return Reject("array elements must be separated by commas");
		}
		m_first.back() = false;
		*more          = true;
		return true;
	}

	bool Key(const char* expected)
	{
		SkipSpace();
		if (m_first.empty())
		{
			return Reject("object key outside an object");
		}
		if (!m_first.back() && !Expect(','))
		{
			return Reject("unexpected, duplicate or missing object key");
		}
		m_first.back() = false;
		SkipSpace();
		if (!Expect('"'))
		{
			return Reject("object key expected");
		}
		const size_t length = std::strlen(expected);
		if (m_text.compare(m_pos, length, expected) != 0 || m_pos + length >= m_text.size() || m_text[m_pos + length] != '"')
		{
			return Reject("unexpected, duplicate or missing object key");
		}
		m_pos += length + 1;
		SkipSpace();
		return Expect(':') || Reject("':' expected after an object key");
	}

	bool Int(int64_t min, int64_t max, int64_t* value)
	{
		SkipSpace();
		const bool negative = Peek() == '-';
		if (negative)
		{
			++m_pos;
		}
		if (Peek() < '0' || Peek() > '9' || (negative && Peek() == '0'))
		{
			return Reject("integer expected");
		}
		const bool leading_zero = Peek() == '0';
		int64_t    magnitude    = 0;
		uint32_t   digits       = 0;
		while (Peek() >= '0' && Peek() <= '9')
		{
			if (++digits > k_max_integer_digits || (leading_zero && digits > 1))
			{
				return Reject("integer has leading zeros or too many digits");
			}
			magnitude = magnitude * 10 + (Peek() - '0');
			++m_pos;
		}
		const char next = Peek();
		if (next == '.' || next == 'e' || next == 'E' || next == '+' || next == '-')
		{
			return Reject("fractional or exponent numbers are not allowed");
		}
		const int64_t result = negative ? -magnitude : magnitude;
		if (result < min || result > max)
		{
			return Reject("integer outside the schema range");
		}
		*value = result;
		return true;
	}

	// The canonical output string: printable ASCII with "\n" as the only escape.
	bool String(size_t max_bytes, std::string* value)
	{
		SkipSpace();
		if (!Expect('"'))
		{
			return Reject("string expected");
		}
		value->clear();
		while (m_pos < m_text.size())
		{
			char c = m_text[m_pos++];
			if (c == '"')
			{
				return true;
			}
			if (c == '\\')
			{
				if (m_pos >= m_text.size() || m_text[m_pos] != 'n')
				{
					return Reject("only the \\n escape is allowed");
				}
				++m_pos;
				c = '\n';
			} else if (c < 0x20 || c > 0x7e)
			{
				return Reject("string contains a control or non-ASCII byte");
			}
			if (value->size() >= max_bytes)
			{
				return Reject("string longer than the schema bound");
			}
			value->push_back(c);
		}
		return Reject("unterminated string");
	}

	// Only whitespace may follow the single top-level value.
	bool Finish()
	{
		SkipSpace();
		return m_pos == m_text.size() || Reject("trailing data after the document");
	}

	bool Reject(const char* message) { return Fail(m_error, message); }

private:
	[[nodiscard]] char Peek() const { return m_pos < m_text.size() ? m_text[m_pos] : '\0'; }
	bool               Expect(char c)
	{
		if (Peek() != c || m_pos >= m_text.size())
		{
			return false;
		}
		++m_pos;
		return true;
	}
	void SkipSpace()
	{
		while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
		{
			++m_pos;
		}
	}
	bool Open(char c)
	{
		SkipSpace();
		if (!Expect(c))
		{
			return Reject(c == '{' ? "object expected" : "array expected");
		}
		if (m_first.size() >= k_max_nesting)
		{
			return Reject("document nesting exceeds the schema bound");
		}
		m_first.push_back(true);
		return true;
	}
	bool Close(char c)
	{
		SkipSpace();
		if (m_first.empty() || !Expect(c))
		{
			return Reject("unexpected, duplicate or missing object key");
		}
		m_first.pop_back();
		return true;
	}

	const std::string& m_text;
	std::string*       m_error;
	size_t             m_pos = 0;
	std::vector<bool>  m_first;
};

template <typename T>
bool ReadValue(StrictReader& r, T* out, int64_t min, int64_t max)
{
	int64_t value = 0;
	if (!r.Int(min, max, &value))
	{
		return false;
	}
	*out = static_cast<T>(value);
	return true;
}

template <typename T>
bool ReadField(StrictReader& r, const char* key, T* out, int64_t min, int64_t max)
{
	return r.Key(key) && ReadValue(r, out, min, max);
}

// The next element of a fixed-length array.
bool Element(StrictReader& r)
{
	bool more = false;
	return r.Next(&more) && (more || r.Reject("array is shorter than the schema"));
}

bool EndOfArray(StrictReader& r)
{
	bool more = false;
	return r.Next(&more) && (!more || r.Reject("array is longer than the schema"));
}

template <typename T>
bool ReadElement(StrictReader& r, T* out, int64_t min, int64_t max)
{
	return Element(r) && ReadValue(r, out, min, max);
}

bool ReadOperand(StrictReader& r, ShaderOperand* op)
{
	uint32_t multiplier_bits = 0;
	if (!r.BeginArray() || !ReadElement(r, &op->type, 0, static_cast<int64_t>(ShaderOperandType::Null)) ||
	    !ReadElement(r, &op->constant.u, 0, k_u32_max) || !ReadElement(r, &op->register_id, INT32_MIN, INT32_MAX) ||
	    !ReadElement(r, &op->size, INT32_MIN, INT32_MAX) || !ReadElement(r, &multiplier_bits, 0, k_u32_max) ||
	    !ReadElement(r, &op->absolute, 0, 1) || !ReadElement(r, &op->negate, 0, 1) || !ReadElement(r, &op->clamp, 0, 1) ||
	    !ReadElement(r, &op->swizzle, 0, 255) || !ReadElement(r, &op->dpp, 0, 1) || !ReadElement(r, &op->dpp_ctrl, 0, 0xffff) ||
	    !ReadElement(r, &op->dpp_row_mask, 0, 255) || !ReadElement(r, &op->dpp_bank_mask, 0, 255) ||
	    !ReadElement(r, &op->dpp_fetch_inactive, 0, 1) || !ReadElement(r, &op->dpp_bound_ctrl, 0, 1) || !EndOfArray(r))
	{
		return false;
	}
	std::memcpy(&op->multiplier, &multiplier_bits, sizeof(multiplier_bits));
	return true;
}

bool ReadOperands(StrictReader& r, ShaderOperand* operands, uint32_t count)
{
	if (!Element(r) || !r.BeginArray())
	{
		return false;
	}
	for (uint32_t i = 0; i < count; ++i)
	{
		if (!Element(r) || !ReadOperand(r, &operands[i]))
		{
			return false;
		}
	}
	return EndOfArray(r);
}

bool ReadInstruction(StrictReader& r, ShaderInstruction* inst)
{
	uint64_t      format_lo  = 0;
	uint64_t      format_hi  = 0;
	const int64_t last_type  = static_cast<int64_t>(ShaderInstructionType::ZMax) - 1;
	if (!r.BeginArray() || !ReadElement(r, &inst->pc, 0, k_u32_max) || !ReadElement(r, &inst->type, 0, last_type) ||
	    !ReadElement(r, &format_lo, 0, k_u32_max) || !ReadElement(r, &format_hi, 0, k_u32_max) ||
	    !ReadElement(r, &inst->sopp_opcode, 0, 255) || !ReadElement(r, &inst->raw_word, 0, k_u32_max) ||
	    !ReadOperands(r, inst->src, k_src_slots) || !ReadElement(r, &inst->src_num, 0, k_src_slots) ||
	    !ReadElement(r, &inst->exp_control, 0, 255) || !Element(r) || !ReadOperand(r, &inst->dst) || !Element(r) ||
	    !ReadOperand(r, &inst->dst2) || !ReadElement(r, &inst->vop3_op_sel, 0, 255) || !ReadElement(r, &inst->vop3_omod, 0, 255) ||
	    !ReadElement(r, &inst->vop3p_op_sel_hi, 0, 255) || !ReadElement(r, &inst->vop_sdwa, 0, 1) ||
	    !ReadElement(r, &inst->vop_sdwa_ctrl, 0, k_u32_max) || !ReadOperands(r, inst->mimg_address, k_mimg_address_slots) ||
	    !ReadElement(r, &inst->mimg_address_num, 0, k_mimg_address_slots) || !ReadElement(r, &inst->mimg_dmask, 0, 255) ||
	    !ReadElement(r, &inst->mimg_dimension, 0, 255) || !ReadElement(r, &inst->mimg_explicit_lod, 0, 1) ||
	    !ReadElement(r, &inst->mimg_offset, 0, 1) || !ReadElement(r, &inst->mimg_return_old_value, 0, 1) ||
	    !ReadElement(r, &inst->smem_imm_offset, INT32_MIN, INT32_MAX) || !ReadElement(r, &inst->flat_offset, INT16_MIN, INT16_MAX) ||
	    !ReadElement(r, &inst->smem_flags, 0, 255) || !ReadElement(r, &inst->buffer_imm_offset, 0, 0xffff) ||
	    !ReadElement(r, &inst->buffer_idxen, 0, 1) || !ReadElement(r, &inst->buffer_offen, 0, 1) ||
	    !ReadElement(r, &inst->buffer_return_old_value, 0, 1) || !ReadElement(r, &inst->buffer_flags, 0, 255) ||
	    !ReadElement(r, &inst->mtbuf_format, 0, 255) || !ReadElement(r, &inst->mtbuf_components, 0, 255) ||
	    !ReadElement(r, &inst->mtbuf_format_is_gen5, 0, 1) || !ReadElement(r, &inst->ds_offset, 0, 0xffff) ||
	    !ReadElement(r, &inst->ds_encoding_control, 0, k_u32_max) || !ReadElement(r, &inst->ds_encoding_registers, 0, k_u32_max) ||
	    !ReadElement(r, &inst->exp_enable_mask, 0, 255) || !EndOfArray(r))
	{
		return false;
	}
	inst->format = static_cast<ShaderInstructionFormat::Format>(format_lo | (format_hi << 32u));
	return true;
}

bool ReadLabels(StrictReader& r, const char* key, Vector<ShaderLabel>* labels)
{
	if (!r.Key(key) || !r.BeginArray())
	{
		return false;
	}
	for (uint32_t count = 0;; ++count)
	{
		bool more = false;
		if (!r.Next(&more))
		{
			return false;
		}
		if (!more)
		{
			return true;
		}
		uint32_t dst = 0;
		uint32_t src = 0;
		if (count >= k_max_labels)
		{
			return r.Reject("label list longer than the schema bound");
		}
		if (!r.BeginArray() || !ReadElement(r, &dst, 0, k_u32_max) || !ReadElement(r, &src, 0, k_u32_max) || !EndOfArray(r))
		{
			return false;
		}
		labels->Add(ShaderLabel(dst, src));
	}
}

bool ReadCode(StrictReader& r, ShaderCode* code)
{
	int64_t type           = 0;
	bool    vs_embedded    = false;
	bool    ps_embedded    = false;
	int64_t vs_embedded_id = 0;
	int64_t ps_embedded_id = 0;
	int64_t continuation   = 0;
	int64_t hash0          = 0;
	int64_t crc32          = 0;
	if (!r.BeginObject() || !ReadField(r, "type", &type, 0, static_cast<int64_t>(ShaderType::Compute)) ||
	    !ReadField(r, "vs_embedded", &vs_embedded, 0, 1) || !ReadField(r, "vs_embedded_id", &vs_embedded_id, 0, k_u32_max) ||
	    !ReadField(r, "ps_embedded", &ps_embedded, 0, 1) || !ReadField(r, "ps_embedded_id", &ps_embedded_id, 0, k_u32_max) ||
	    !ReadField(r, "continuation_pc", &continuation, 0, k_u32_max) || !ReadField(r, "hash0", &hash0, 0, k_u32_max) ||
	    !ReadField(r, "crc32", &crc32, 0, k_u32_max) || !ReadLabels(r, "labels", &code->GetLabels()) ||
	    !ReadLabels(r, "indirect_labels", &code->GetIndirectLabels()) || !r.Key("instructions") || !r.BeginArray())
	{
		return false;
	}
	code->SetType(static_cast<ShaderType>(type));
	code->SetVsEmbedded(vs_embedded);
	code->SetVsEmbeddedId(static_cast<uint32_t>(vs_embedded_id));
	code->SetPsEmbedded(ps_embedded);
	code->SetPsEmbeddedId(static_cast<uint32_t>(ps_embedded_id));
	code->SetContinuationPc(static_cast<uint32_t>(continuation));
	code->SetHash0(static_cast<uint32_t>(hash0));
	code->SetCrc32(static_cast<uint32_t>(crc32));
	for (uint32_t count = 0;; ++count)
	{
		bool more = false;
		if (!r.Next(&more))
		{
			return false;
		}
		if (!more)
		{
			break;
		}
		if (count >= k_max_instructions)
		{
			return r.Reject("instruction list longer than the schema bound");
		}
		ShaderInstruction inst {};
		if (!ReadInstruction(r, &inst))
		{
			return false;
		}
		code->GetInstructions().Add(inst);
	}
	return r.EndObject();
}

// Owned storage behind the pointers of one reconstructed ShaderParseUsage2 call.
struct ReplayCase
{
	GuestPlatform                guest_platform = GuestPlatform::Unknown;
	HW::UserSgprInfo             user_sgpr {};
	ShaderUserData               user_data {};
	std::vector<uint16_t>        direct;
	std::vector<ShaderSharp>     sharp[4];
	std::unique_ptr<ShaderCode>  code;
	int                          user_sgpr_num           = 0;
	int                          user_data_register_base = 0;
	bool                         vertex_resource_types   = false;
	uint32_t                     push_constant_offset    = 0; // initial output seeds
	uint32_t                     push_constant_size      = 0;
	uint32_t                     descriptor_set_slot     = 0;
	bool                         vertex_attrib           = false;
	int                          vertex_attrib_reg       = 0;
	ShaderResourceFoldTranscript transcript;
	std::string                  canonical_output;
	uint64_t                     fingerprint = 0;
};

bool ReadUserSgpr(StrictReader& r, HW::UserSgprInfo* sgpr)
{
	if (!r.BeginObject() || !ReadField(r, "count", &sgpr->count, 0, k_u32_max) || !r.Key("value") || !r.BeginArray())
	{
		return false;
	}
	for (auto& value: sgpr->value)
	{
		if (!ReadElement(r, &value, 0, k_u32_max))
		{
			return false;
		}
	}
	if (!EndOfArray(r) || !r.Key("type") || !r.BeginArray())
	{
		return false;
	}
	for (auto& type: sgpr->type)
	{
		if (!ReadElement(r, &type, 0, static_cast<int64_t>(HW::UserSgprType::Vsharp)))
		{
			return false;
		}
	}
	return EndOfArray(r) && r.EndObject();
}

// Reads `count` entries exactly. A present array keeps non-empty storage so a
// zero-count array still maps to a non-null pointer, as in the captured run.
bool ReadDirect(StrictReader& r, ReplayCase* replay, uint16_t count, bool present)
{
	if (!r.Key("direct") || !r.BeginArray())
	{
		return false;
	}
	if (present)
	{
		replay->direct.resize(std::max<uint32_t>(count, 1u));
	}
	for (uint32_t i = 0;; ++i)
	{
		bool more = false;
		if (!r.Next(&more))
		{
			return false;
		}
		if (!more)
		{
			return i == (present ? count : 0u) || r.Reject("direct resource list does not match its count");
		}
		if (!present || i >= static_cast<uint32_t>(count) || !ReadValue(r, &replay->direct[i], 0, 0xffff))
		{
			return r.Reject("direct resource list does not match its count");
		}
	}
}

bool ReadSharpCategory(StrictReader& r, ReplayCase* replay, int category)
{
	auto&    data    = replay->user_data;
	uint16_t count   = 0;
	bool     present = false;
	if (!r.BeginObject() || !ReadField(r, "count", &count, 0, k_max_metadata_slots) || !ReadField(r, "present", &present, 0, 1))
	{
		return false;
	}
	if (count != 0 && !present)
	{
		return r.Reject("sharp metadata count without an array");
	}
	if (!r.Key("entries") || !r.BeginArray())
	{
		return false;
	}
	auto& entries = replay->sharp[category];
	if (present)
	{
		entries.resize(std::max<uint32_t>(count, 1u));
	}
	for (uint32_t slot = 0;; ++slot)
	{
		bool more = false;
		if (!r.Next(&more))
		{
			return false;
		}
		if (!more)
		{
			if (slot != (present ? count : 0u))
			{
				return r.Reject("sharp entries do not match their count");
			}
			break;
		}
		uint16_t offset_dw = 0;
		uint16_t size      = 0;
		if (!present || slot >= static_cast<uint32_t>(count))
		{
			return r.Reject("sharp entries do not match their count");
		}
		if (!r.BeginArray() || !ReadElement(r, &offset_dw, 0, k_unused_sharp) || !ReadElement(r, &size, 0, 1) || !EndOfArray(r))
		{
			return false;
		}
		entries[slot].offset_dw = offset_dw;
		entries[slot].size      = size;
	}
	data.sharp_resource_count[category]  = count;
	data.sharp_resource_offset[category] = present ? entries.data() : nullptr;
	return r.EndObject();
}

bool ReadUserData(StrictReader& r, ReplayCase* replay)
{
	auto& data    = replay->user_data;
	bool  present = false;
	if (!r.BeginObject() || !ReadField(r, "eud_size_dw", &data.eud_size_dw, 0, 0xffff) ||
	    !ReadField(r, "srt_size_dw", &data.srt_size_dw, 0, 0xffff) ||
	    !ReadField(r, "direct_count", &data.direct_resource_count, 0, k_max_metadata_slots) ||
	    !ReadField(r, "direct_present", &present, 0, 1))
	{
		return false;
	}
	// The evaluator dereferences the direct array for every counted slot.
	if (data.direct_resource_count != 0 && !present)
	{
		return r.Reject("direct metadata count without an array");
	}
	if (!ReadDirect(r, replay, data.direct_resource_count, present))
	{
		return false;
	}
	data.direct_resource_offset = present ? replay->direct.data() : nullptr;
	if (!r.Key("sharp") || !r.BeginArray())
	{
		return false;
	}
	for (int category = 0; category < 4; ++category)
	{
		if (!Element(r) || !ReadSharpCategory(r, replay, category))
		{
			return false;
		}
	}
	return EndOfArray(r) && r.EndObject();
}

bool ReadInitialOutput(StrictReader& r, ReplayCase* replay)
{
	return r.BeginObject() && ReadField(r, "push_constant_offset", &replay->push_constant_offset, 0, k_u32_max) &&
	       ReadField(r, "push_constant_size", &replay->push_constant_size, 0, k_u32_max) &&
	       ReadField(r, "descriptor_set_slot", &replay->descriptor_set_slot, 0, k_u32_max) &&
	       ReadField(r, "vertex_attrib", &replay->vertex_attrib, 0, 1) &&
	       ReadField(r, "vertex_attrib_reg", &replay->vertex_attrib_reg, INT32_MIN, INT32_MAX) && r.EndObject();
}

bool ReadParse(StrictReader& r, ReplayCase* replay)
{
	bool code_present = false;
	if (!r.BeginObject() ||
	    !ReadField(r, "guest_platform", &replay->guest_platform, static_cast<int64_t>(GuestPlatform::Ps4),
	               static_cast<int64_t>(GuestPlatform::Ps5)) ||
	    !ReadField(r, "user_sgpr_num", &replay->user_sgpr_num, 0, k_user_sgprs) ||
	    !ReadField(r, "user_data_register_base", &replay->user_data_register_base, 0, 255) ||
	    !ReadField(r, "vertex_resource_types", &replay->vertex_resource_types, 0, 1) || !r.Key("initial_output") ||
	    !ReadInitialOutput(r, replay) || !r.Key("user_sgpr") ||
	    !ReadUserSgpr(r, &replay->user_sgpr) || !r.Key("user_data") || !ReadUserData(r, replay) ||
	    !ReadField(r, "code_present", &code_present, 0, 1))
	{
		return false;
	}
	if (code_present)
	{
		replay->code = std::make_unique<ShaderCode>();
		if (!r.Key("code") || !ReadCode(r, replay->code.get()))
		{
			return false;
		}
	}
	return r.EndObject();
}

bool ReadRead(StrictReader& r, ShaderResourceFoldRead* read)
{
	uint32_t hi = 0;
	uint32_t lo = 0;
	if (!r.BeginObject() || !ReadField(r, "address_hi", &hi, 0, k_u32_max) || !ReadField(r, "address_lo", &lo, 0, k_u32_max) ||
	    !ReadField(r, "dwords", &read->dwords, 1, SHADER_GEN5_EUD_MAX_DWORDS) || !ReadField(r, "success", &read->success, 0, 1) ||
	    !r.Key("words") || !r.BeginArray())
	{
		return false;
	}
	read->address        = (static_cast<uint64_t>(hi) << 32u) | lo;
	const uint32_t count = read->success ? read->dwords : 0u;
	read->words.reserve(count);
	for (;;)
	{
		bool more = false;
		if (!r.Next(&more))
		{
			return false;
		}
		if (!more)
		{
			break;
		}
		uint32_t word = 0;
		if (read->words.size() >= count || !ReadValue(r, &word, 0, k_u32_max))
		{
			return r.Reject("read word count must match its success flag");
		}
		read->words.push_back(word);
	}
	if (read->words.size() != count)
	{
		return r.Reject("read word count must match its success flag");
	}
	return r.EndObject();
}

bool ReadTranscript(StrictReader& r, ShaderResourceFoldTranscript* transcript)
{
	if (!r.BeginObject() || !r.Key("reads") || !r.BeginArray())
	{
		return false;
	}
	for (;;)
	{
		bool more = false;
		if (!r.Next(&more))
		{
			return false;
		}
		if (!more)
		{
			break;
		}
		if (transcript->reads.size() >= SHADER_RESOURCE_FOLD_MAX_READS)
		{
			return r.Reject("transcript longer than the read bound");
		}
		transcript->reads.emplace_back();
		if (!ReadRead(r, &transcript->reads.back()))
		{
			return false;
		}
	}
	return r.EndObject();
}

bool ReadDocument(const std::string& text, ReplayCase* replay, std::string* error)
{
	if (text.size() > SHADER_RESOURCE_FOLD_MAX_DOCUMENT_BYTES)
	{
		return Fail(error, "document exceeds the byte bound");
	}
	StrictReader r(text, error);
	uint32_t     schema         = 0;
	uint32_t     fingerprint_hi = 0;
	uint32_t     fingerprint_lo = 0;
	if (!r.BeginObject() ||
	    !ReadField(r, "schema_version", &schema, SHADER_RESOURCE_FOLD_SCHEMA_VERSION, SHADER_RESOURCE_FOLD_SCHEMA_VERSION) ||
	    !r.Key("parse") || !ReadParse(r, replay) || !r.Key("transcript") || !ReadTranscript(r, &replay->transcript) ||
	    !r.Key("canonical_output") || !r.String(SHADER_RESOURCE_FOLD_MAX_CANONICAL_BYTES, &replay->canonical_output) ||
	    !ReadField(r, "fingerprint_hi", &fingerprint_hi, 0, k_u32_max) || !ReadField(r, "fingerprint_lo", &fingerprint_lo, 0, k_u32_max) ||
	    !r.EndObject() || !r.Finish())
	{
		return false;
	}
	replay->fingerprint = (static_cast<uint64_t>(fingerprint_hi) << 32u) | fingerprint_lo;
	if (replay->fingerprint != ShaderResourceFoldFingerprint(replay->canonical_output))
	{
		return Fail(error, "fingerprint does not match the canonical output");
	}
	return true;
}

// ---- Evaluator bounds -------------------------------------------------------

// Rejects metadata the evaluator would consume outside a fixed array on a path
// that still warns and continues: a direct register whose minimal descriptor does
// not fit the user-SGPR window (the EUD pointer pair reads it unchecked), and
// sampler metadata that certainly exceeds the sampler array. No shader-level
// classification is repeated here. The texture, sampler, storage,
// zero-scalar-buffer and GDS writers refuse an out-of-window start or a full array
// themselves, so code- and content-dependent cases are refused during replay.
bool ValidateEvaluatorBounds(const ReplayCase& replay, std::string* error)
{
	const auto& data          = replay.user_data;
	const bool  has_code      = replay.code != nullptr;
	const bool  vertex_types  = replay.vertex_resource_types;
	const bool  has_eud_ptr   = Gen5HasEudPointer(&data);
	int         direct10      = 0;
	bool        eud_ptr_valid = false;
	for (uint16_t type = 0; type < data.direct_resource_count; ++type)
	{
		const uint16_t reg = data.direct_resource_offset[type];
		if (reg == k_unused_direct)
		{
			continue;
		}
		// Minimal descriptor width per metadata type: the default slot is a 4-dword
		// V#; type 8 is a T# and type 10 an S# unless they name vertex tables.
		int dwords = 4;
		if (type == 8u)
		{
			dwords = vertex_types ? 0 : 8;
		} else if (type == 10u)
		{
			dwords = vertex_types ? 0 : 4;
			direct10 += vertex_types ? 0 : 1;
		} else if (type == k_gen5_eud_direct_type && has_eud_ptr)
		{
			dwords = 2;
		}
		if (reg + dwords > k_user_sgprs)
		{
			return Fail(error, "direct resource register outside the user-SGPR window");
		}
		if (type == k_gen5_eud_direct_type && has_eud_ptr)
		{
			ShaderExtendedResource pointer;
			pointer.fields[0] = replay.user_sgpr.value[reg];
			pointer.fields[1] = replay.user_sgpr.value[reg + 1];
			eud_ptr_valid     = pointer.Base() != 0;
		}
	}

	// Sampler writes the metadata alone guarantees; at most one direct slot can
	// be diverted to the GDS pointer.
	int sharp_samplers = 0;
	for (uint16_t slot = 0; slot < data.sharp_resource_count[2]; ++slot)
	{
		const auto& sharp = data.sharp_resource_offset[2][slot];
		if (sharp.offset_dw != k_unused_sharp && (eud_ptr_valid || !Gen5SharpNeedsEud(sharp.offset_dw, 4, replay.user_sgpr_num)))
		{
			++sharp_samplers;
		}
	}
	const int diverted = (!vertex_types && has_code && direct10 > 0) ? 1 : 0;
	if (sharp_samplers + direct10 - diverted > ShaderSamplerResources::RES_MAX)
	{
		return Fail(error, "sampler metadata exceeds the evaluator sampler capacity");
	}
	return true;
}

bool LoadDocument(const std::string& text, ReplayCase* replay, std::string* error)
{
	return ReadDocument(text, replay, error) && ValidateEvaluatorBounds(*replay, error);
}

// ---- Canonical output -------------------------------------------------------

class CanonicalText
{
public:
	template <typename T>
	void Put(const std::string& key, T value)
	{
		m_text += key;
		m_text += '=';
		m_text += std::to_string(static_cast<int64_t>(value));
		m_text += '\n';
	}
	[[nodiscard]] const std::string& Text() const { return m_text; }

private:
	std::string m_text;
};

std::string Item(const char* name, int index)
{
	return std::string(name) + "[" + std::to_string(index) + "].";
}

void PutUsage(CanonicalText& out, const ShaderParsedUsage& usage)
{
	out.Put("usage.fetch", usage.fetch);
	out.Put("usage.fetch_reg", usage.fetch_reg);
	out.Put("usage.vertex_buffer", usage.vertex_buffer);
	out.Put("usage.vertex_buffer_reg", usage.vertex_buffer_reg);
	out.Put("usage.vertex_attrib", usage.vertex_attrib);
	out.Put("usage.vertex_attrib_reg", usage.vertex_attrib_reg);
	out.Put("usage.storage_buffers_readwrite", usage.storage_buffers_readwrite);
	out.Put("usage.storage_buffers_readonly", usage.storage_buffers_readonly);
	out.Put("usage.storage_buffers_constant", usage.storage_buffers_constant);
	out.Put("usage.textures2D_readonly", usage.textures2D_readonly);
	out.Put("usage.textures2D_readwrite", usage.textures2D_readwrite);
	out.Put("usage.extended_buffer", usage.extended_buffer);
	out.Put("usage.samplers", usage.samplers);
	out.Put("usage.gds_pointers", usage.gds_pointers);
	out.Put("usage.direct_sgprs", usage.direct_sgprs);
}

void PutStorage(CanonicalText& out, const ShaderStorageResources& s)
{
	out.Put("bind.storage_buffers.buffers_num", s.buffers_num);
	out.Put("bind.storage_buffers.binding_index", s.binding_index);
	for (int i = 0; i < ShaderStorageResources::BUFFERS_MAX; ++i)
	{
		const std::string p = Item("bind.storage_buffers.buffers", i);
		for (int field = 0; field < 4; ++field)
		{
			out.Put(p + "fields" + std::to_string(field), s.buffers[i].fields[field]);
		}
		out.Put(p + "usage", s.usages[i]);
		out.Put(p + "access", s.accesses[i]);
		out.Put(p + "source", s.sources[i]);
		out.Put(p + "unknown_reason", s.unknown_reasons[i]);
		out.Put(p + "code_available", s.code_available[i]);
		out.Put(p + "exact_match", s.exact_matches[i]);
		out.Put(p + "unbased_match", s.unbased_matches[i]);
		out.Put(p + "decoded_unknown", s.decoded_unknown[i]);
		out.Put(p + "indirect_descriptor_use", s.indirect_descriptor_use[i]);
		out.Put(p + "raw_vmem_oob_guarded", s.raw_vmem_oob_guarded[i]);
		out.Put(p + "raw_smem_use", s.raw_smem_use[i]);
		out.Put(p + "raw_tbuffer_use", s.raw_tbuffer_use[i]);
		out.Put(p + "raw_smem_required_bytes", s.raw_smem_required_bytes[i]);
		out.Put(p + "raw_smem_dynamic_offset", s.raw_smem_dynamic_offset[i]);
		out.Put(p + "dynamic_sload", s.dynamic_sload[i]);
		out.Put(p + "slot", s.slots[i]);
		out.Put(p + "start_register", s.start_register[i]);
		out.Put(p + "extended", s.extended[i]);
	}
}

void PutZeroSBuffers(CanonicalText& out, const ShaderZeroSBufferResources& z)
{
	out.Put("bind.zero_sbuffer.buffers_num", z.buffers_num);
	for (int i = 0; i < ShaderZeroSBufferResources::BUFFERS_MAX; ++i)
	{
		out.Put("bind.zero_sbuffer.start_register" + std::to_string(i), z.start_register[i]);
	}
}

void PutTextures(CanonicalText& out, const ShaderTextureResources& t)
{
	out.Put("bind.textures2D.textures_num", t.textures_num);
	out.Put("bind.textures2D.sampled_num", t.textures2d_sampled_num);
	out.Put("bind.textures2D.sampled_depth_num", t.textures2d_sampled_depth_num);
	out.Put("bind.textures2D.array_sampled_num", t.textures2d_array_sampled_num);
	out.Put("bind.textures2D.sampled3d_num", t.textures3d_sampled_num);
	out.Put("bind.textures2D.sampled_uint_num", t.textures2d_sampled_uint_num);
	out.Put("bind.textures2D.array_sampled_uint_num", t.textures2d_array_sampled_uint_num);
	out.Put("bind.textures2D.sampled3d_uint_num", t.textures3d_sampled_uint_num);
	out.Put("bind.textures2D.storage_num", t.textures2d_storage_num);
	out.Put("bind.textures2D.binding_sampled_index", t.binding_sampled_index);
	out.Put("bind.textures2D.binding_sampled_depth_index", t.binding_sampled_depth_index);
	out.Put("bind.textures2D.binding_sampled_array_index", t.binding_sampled_array_index);
	out.Put("bind.textures2D.binding_sampled_3d_index", t.binding_sampled_3d_index);
	out.Put("bind.textures2D.binding_sampled_uint_index", t.binding_sampled_uint_index);
	out.Put("bind.textures2D.binding_sampled_array_uint_index", t.binding_sampled_array_uint_index);
	out.Put("bind.textures2D.binding_sampled_3d_uint_index", t.binding_sampled_3d_uint_index);
	out.Put("bind.textures2D.binding_storage_index", t.binding_storage_index);
	for (int i = 0; i < ShaderTextureResources::RES_MAX; ++i)
	{
		const auto&       d = t.desc[i];
		const std::string p = Item("bind.textures2D.desc", i);
		for (int field = 0; field < 8; ++field)
		{
			out.Put(p + "fields" + std::to_string(field), d.texture.fields[field]);
		}
		out.Put(p + "usage", d.usage);
		out.Put(p + "sample_operation", d.sample_operation);
		out.Put(p + "sampled_shape", d.sampled_shape);
		out.Put(p + "slot", d.slot);
		out.Put(p + "start_register", d.start_register);
		out.Put(p + "extended", d.extended);
		out.Put(p + "dynamic_sload", d.dynamic_sload);
		out.Put(p + "sampler_indices_mask", d.sampler_indices_mask);
		out.Put(p + "without_sampler", d.textures2d_without_sampler);
		out.Put(p + "shape_from_instruction", d.sampled_shape_from_instruction);
		out.Put(p + "storage_write_only", d.storage_image_write_only);
	}
}

void PutSamplers(CanonicalText& out, const ShaderSamplerResources& s)
{
	out.Put("bind.samplers.samplers_num", s.samplers_num);
	out.Put("bind.samplers.binding_index", s.binding_index);
	for (int i = 0; i < ShaderSamplerResources::RES_MAX; ++i)
	{
		const std::string p = Item("bind.samplers.samplers", i);
		for (int field = 0; field < 4; ++field)
		{
			out.Put(p + "fields" + std::to_string(field), s.samplers[i].fields[field]);
		}
		out.Put(p + "operation", s.operations[i]);
		out.Put(p + "slot", s.slots[i]);
		out.Put(p + "start_register", s.start_register[i]);
		out.Put(p + "extended", s.extended[i]);
		out.Put(p + "dynamic_sload", s.dynamic_sload[i]);
	}
}

void PutGdsDirectExtended(CanonicalText& out, const ShaderBindResources& bind)
{
	out.Put("bind.gds.pointers_num", bind.gds_pointers.pointers_num);
	out.Put("bind.gds.binding_index", bind.gds_pointers.binding_index);
	for (int i = 0; i < ShaderGdsResources::POINTERS_MAX; ++i)
	{
		const std::string p = Item("bind.gds.pointers", i);
		out.Put(p + "field", bind.gds_pointers.pointers[i].field);
		out.Put(p + "slot", bind.gds_pointers.slots[i]);
		out.Put(p + "start_register", bind.gds_pointers.start_register[i]);
		out.Put(p + "extended", bind.gds_pointers.extended[i]);
	}
	out.Put("bind.direct_sgprs.sgprs_num", bind.direct_sgprs.sgprs_num);
	for (int i = 0; i < ShaderDirectSgprsResources::SGPRS_MAX; ++i)
	{
		const std::string p = Item("bind.direct_sgprs.sgprs", i);
		out.Put(p + "field", bind.direct_sgprs.sgprs[i].field);
		out.Put(p + "start_register", bind.direct_sgprs.start_register[i]);
		out.Put(p + "absolute_register", bind.direct_sgprs.absolute_register[i]);
	}
	const auto& ext = bind.extended;
	out.Put("bind.extended.used", ext.used);
	out.Put("bind.extended.slot", ext.slot);
	out.Put("bind.extended.start_register", ext.start_register);
	out.Put("bind.extended.eud_user_sgpr_num", ext.eud_user_sgpr_num);
	out.Put("bind.extended.eud_size_dw", ext.eud_size_dw);
	out.Put("bind.extended.eud_offset_base", ext.eud_offset_base);
	out.Put("bind.extended.data0", ext.data.fields[0]);
	out.Put("bind.extended.data1", ext.data.fields[1]);
}

void PutDynamicAndAssembled(CanonicalText& out, const ShaderBindResources& bind)
{
	out.Put("bind.dynamic_sloads.count", bind.dynamic_sloads.records.Size());
	int index = 0;
	for (const auto& record: bind.dynamic_sloads.records)
	{
		const std::string p = Item("bind.dynamic_sloads.records", index++);
		out.Put(p + "kind", record.kind);
		out.Put(p + "resource_index", record.resource_index);
		out.Put(p + "destination_register", record.destination_register);
		out.Put(p + "instruction_pc", record.instruction_pc);
		out.Put(p + "offset_dw", record.offset_dw);
		out.Put(p + "dword_count", record.dword_count);
		out.Put(p + "resource_field_offset", record.resource_field_offset);
		out.Put(p + "last_consumer_pc", record.last_consumer_pc);
		out.Put(p + "raw_vmem_oob_guarded", record.raw_vmem_oob_guarded);
	}
	out.Put("bind.assembled_descriptors.count", bind.assembled_descriptors.Size());
	index = 0;
	for (const auto& descriptor: bind.assembled_descriptors)
	{
		const std::string p = Item("bind.assembled_descriptors", index++);
		out.Put(p + "consumer_pc", descriptor.consumer_pc);
		out.Put(p + "register_id", descriptor.register_id);
		out.Put(p + "resource_index", descriptor.resource_index);
	}
}

// ---- Capture output ---------------------------------------------------------

const std::filesystem::path& CaptureDirectory()
{
	static const std::filesystem::path directory = []
	{
		const char*     value = std::getenv("KYTY_RESOURCE_FOLD_CAPTURE_DIR");
		std::error_code error;
		if (value == nullptr || value[0] == '\0' || !std::filesystem::is_directory(value, error))
		{
			return std::filesystem::path();
		}
		return std::filesystem::path(value);
	}();
	return directory;
}

// Admits one environment-gated capture: at most SHADER_RESOURCE_FOLD_MAX_CAPTURES per process.
bool AdmitCapture(uint32_t* file_index)
{
	if (CaptureDirectory().empty())
	{
		return false;
	}
	uint32_t current = g_capture_count.load();
	do
	{
		if (current >= SHADER_RESOURCE_FOLD_MAX_CAPTURES)
		{
			return false;
		}
	} while (!g_capture_count.compare_exchange_weak(current, current + 1u));
	*file_index = current + 1u;
	return true;
}

bool WriteCaptureDocument(uint32_t file_index, const std::string& document)
{
	const std::string path = (CaptureDirectory() / ("resource-fold-" + std::to_string(file_index) + ".json")).string();
	// "x" fails when the path already exists, so no existing file, symlink or FIFO is ever replaced.
	FILE* file = std::fopen(path.c_str(), "wbx");
	if (file == nullptr)
	{
		return false;
	}
	const bool written = std::fwrite(document.data(), 1, document.size(), file) == document.size();
	const bool closed  = std::fclose(file) == 0;
	if (written && closed)
	{
		return true;
	}
	std::remove(path.c_str()); // created exclusively by this call, so removing it is safe
	return false;
}

bool ReadDocumentFile(const std::string& path, std::string* text, std::string* error)
{
	std::error_code ec;
	const auto      status = std::filesystem::status(path, ec);
	if (ec || !std::filesystem::is_regular_file(status))
	{
		return Fail(error, "document is not a regular file");
	}
	const auto size = std::filesystem::file_size(path, ec);
	if (ec || size > SHADER_RESOURCE_FOLD_MAX_DOCUMENT_BYTES)
	{
		return Fail(error, "document exceeds the byte bound");
	}
	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		return Fail(error, "document cannot be opened");
	}
	text->resize(static_cast<size_t>(size));
	file.read(text->data(), static_cast<std::streamsize>(size));
	if (file.gcount() != static_cast<std::streamsize>(size) || file.peek() != std::ifstream::traits_type::eof())
	{
		return Fail(error, "document changed while it was read");
	}
	return true;
}

// Keeps the replay's own ShaderParseUsage2 call from starting a nested capture.
class ReplayCaptureSuppression
{
public:
	ReplayCaptureSuppression(): m_previous(g_capture_active) { g_capture_active = true; }
	ReplayCaptureSuppression(const ReplayCaptureSuppression&)            = delete;
	ReplayCaptureSuppression& operator=(const ReplayCaptureSuppression&) = delete;
	~ReplayCaptureSuppression() { g_capture_active = m_previous; }

private:
	bool m_previous;
};

} // namespace

// ---- Public API -------------------------------------------------------------

ShaderResourceFoldRecorder::ShaderResourceFoldRecorder(uint32_t max_reads): m_max_reads(max_reads) {}

bool ShaderResourceFoldRecorder::Read(uint64_t guest_address, uint32_t dwords, uint32_t* words)
{
	const bool success = Core::VirtualMemory::CopyFromGuest(words, guest_address, static_cast<uint64_t>(dwords) * sizeof(uint32_t));
	if (m_transcript.reads.size() >= m_max_reads)
	{
		m_overflowed = true;
		return success;
	}
	ShaderResourceFoldRead read;
	read.address = guest_address;
	read.dwords  = dwords;
	read.success = success;
	if (success)
	{
		read.words.assign(words, words + dwords);
	}
	m_transcript.reads.push_back(std::move(read));
	return success;
}

ShaderResourceFoldReplayReader::ShaderResourceFoldReplayReader(const ShaderResourceFoldTranscript& transcript): m_transcript(transcript) {}

bool ShaderResourceFoldReplayReader::Read(uint64_t guest_address, uint32_t dwords, uint32_t* words)
{
	if (m_status != ShaderResourceFoldReplayStatus::Ok)
	{
		return false;
	}
	if (m_cursor >= m_transcript.reads.size())
	{
		Fail(ShaderResourceFoldReplayStatus::Missing, guest_address, dwords);
		return false;
	}
	const ShaderResourceFoldRead& read = m_transcript.reads[m_cursor];
	if (read.address != guest_address || read.dwords != dwords)
	{
		Fail(ShaderResourceFoldReplayStatus::Order, guest_address, dwords);
		return false;
	}
	++m_cursor;
	if (!read.success)
	{
		Fail(ShaderResourceFoldReplayStatus::Unreadable, guest_address, dwords);
		return false;
	}
	if (read.words.size() != dwords)
	{
		Fail(ShaderResourceFoldReplayStatus::Invalid, guest_address, dwords);
		return false;
	}
	std::copy(read.words.begin(), read.words.end(), words);
	return true;
}

ShaderResourceFoldReplayStatus ShaderResourceFoldReplayReader::Status() const
{
	if (m_status != ShaderResourceFoldReplayStatus::Ok)
	{
		return m_status;
	}
	return m_cursor == m_transcript.reads.size() ? ShaderResourceFoldReplayStatus::Ok : ShaderResourceFoldReplayStatus::Unused;
}

void ShaderResourceFoldReplayReader::Fail(ShaderResourceFoldReplayStatus status, uint64_t guest_address, uint32_t dwords)
{
	m_status          = status;
	const char* token = "unknown";
	switch (status)
	{
		case ShaderResourceFoldReplayStatus::Missing: token = "missing"; break;
		case ShaderResourceFoldReplayStatus::Order: token = "order"; break;
		case ShaderResourceFoldReplayStatus::Unreadable: token = "unreadable"; break;
		case ShaderResourceFoldReplayStatus::Invalid: token = "invalid"; break;
		case ShaderResourceFoldReplayStatus::Unused: token = "unused"; break;
		case ShaderResourceFoldReplayStatus::Ok: break;
	}
	std::fprintf(stderr, "resource-fold-replay status=%s cursor=%zu address=0x%016llx dwords=%u\n", token, m_cursor,
	             static_cast<unsigned long long>(guest_address), static_cast<unsigned>(dwords));
}

std::string ShaderResourceFoldCanonicalOutput(const ShaderParsedUsage& usage, const ShaderBindResources& bind)
{
	CanonicalText out;
	PutUsage(out, usage);
	out.Put("bind.push_constant_offset", bind.push_constant_offset);
	out.Put("bind.push_constant_size", bind.push_constant_size);
	out.Put("bind.descriptor_set_slot", bind.descriptor_set_slot);
	out.Put("bind.vsharp_uniform_buffer", bind.vsharp_uniform_buffer);
	out.Put("bind.vsharp_binding_index", bind.vsharp_binding_index);
	out.Put("bind.program_base_used", bind.program_base_used);
	out.Put("bind.program_base_offset_dw", bind.program_base_offset_dw);
	out.Put("bind.program_base", bind.program_base);
	out.Put("bind.device_address_used", bind.device_address_used);
	out.Put("bind.device_address_offset_dw", bind.device_address_offset_dw);
	out.Put("bind.thread_limits_used", bind.thread_limits_used);
	out.Put("bind.thread_limits_offset_dw", bind.thread_limits_offset_dw);
	for (int i = 0; i < 3; ++i)
	{
		out.Put("bind.thread_limits" + std::to_string(i), bind.thread_limits[i]);
	}
	PutStorage(out, bind.storage_buffers);
	PutZeroSBuffers(out, bind.zero_sbuffer_resources);
	PutTextures(out, bind.textures2D);
	PutSamplers(out, bind.samplers);
	PutGdsDirectExtended(out, bind);
	PutDynamicAndAssembled(out, bind);
	return out.Text();
}

uint64_t ShaderResourceFoldFingerprint(const std::string& text)
{
	uint64_t hash = 14695981039346656037ull; // FNV-1a, 64-bit
	for (const char c: text)
	{
		hash ^= static_cast<uint8_t>(c);
		hash *= 1099511628211ull;
	}
	return hash;
}

bool ShaderResourceFoldSerializeInputs(const ShaderResourceFoldInputs& inputs, std::string* parse_section)
{
	if (parse_section == nullptr || inputs.user_data == nullptr || inputs.user_sgpr == nullptr || inputs.initial_usage == nullptr ||
	    inputs.initial_bind == nullptr || !Config::IsInitialized() || !InitialBindHoldsOnlyLayoutSeeds(*inputs.initial_bind))
	{
		return false;
	}
	const GuestPlatform platform = Config::GetGuestPlatform();
	if (platform != GuestPlatform::Ps4 && platform != GuestPlatform::Ps5)
	{
		return false;
	}
	DocumentWriter w;
	if (!WriteParse(w, inputs, platform))
	{
		return false;
	}
	*parse_section = w.Text();
	return true;
}

bool ShaderResourceFoldAssembleDocument(const std::string& parse_section, const ShaderResourceFoldTranscript& transcript,
                                        const std::string& canonical_output, std::string* document)
{
	if (document == nullptr || parse_section.empty() || transcript.reads.size() > SHADER_RESOURCE_FOLD_MAX_READS ||
	    canonical_output.size() > SHADER_RESOURCE_FOLD_MAX_CANONICAL_BYTES)
	{
		return false;
	}
	const uint64_t fingerprint = ShaderResourceFoldFingerprint(canonical_output);
	DocumentWriter w;
	w.BeginObject();
	w.Key("schema_version");
	w.Int(SHADER_RESOURCE_FOLD_SCHEMA_VERSION);
	w.Key("parse");
	w.Raw(parse_section);
	w.Key("transcript");
	if (!WriteTranscript(w, transcript))
	{
		return false;
	}
	w.Key("canonical_output");
	if (!w.String(canonical_output))
	{
		return false;
	}
	w.Key("fingerprint_hi");
	w.Int(static_cast<int64_t>(fingerprint >> 32u));
	w.Key("fingerprint_lo");
	w.Int(static_cast<int64_t>(fingerprint & 0xffffffffu));
	w.EndObject();
	if (w.Text().size() > SHADER_RESOURCE_FOLD_MAX_DOCUMENT_BYTES)
	{
		return false;
	}
	*document = w.Text();
	return true;
}

bool ShaderResourceFoldSerializeDocument(const ShaderResourceFoldInputs& inputs, const ShaderResourceFoldTranscript& transcript,
                                         const ShaderParsedUsage& usage, const ShaderBindResources& bind, std::string* document)
{
	std::string parse_section;
	return ShaderResourceFoldSerializeInputs(inputs, &parse_section) &&
	       ShaderResourceFoldAssembleDocument(parse_section, transcript, ShaderResourceFoldCanonicalOutput(usage, bind), document);
}

bool ShaderResourceFoldValidateDocument(const std::string& document, std::string* error)
{
	ReplayCase replay;
	return LoadDocument(document, &replay, error);
}

bool ShaderResourceFoldReplayDocument(const std::string& document, ShaderResourceFoldReplayReport* report, std::string* error)
{
	if (report == nullptr)
	{
		return Fail(error, "report output is required");
	}
	ReplayCase replay;
	if (!LoadDocument(document, &replay, error))
	{
		return false;
	}
	if (!Config::IsInitialized() || Config::GetGuestPlatform() != replay.guest_platform)
	{
		return Fail(error, "configured guest platform does not match the document");
	}

	// Replay answers from the transcript only; the scope is the production read seam.
	ShaderResourceFoldReplayReader reader(replay.transcript);
	auto                           usage = std::make_unique<ShaderParsedUsage>();
	auto                           bind  = std::make_unique<ShaderBindResources>();
	bind->push_constant_offset           = replay.push_constant_offset;
	bind->push_constant_size             = replay.push_constant_size;
	bind->descriptor_set_slot            = replay.descriptor_set_slot;
	usage->vertex_attrib                 = replay.vertex_attrib;
	usage->vertex_attrib_reg             = replay.vertex_attrib_reg;
	{
		const ReplayCaptureSuppression          suppression;
		const ScopedShaderGuestDescriptorReader reader_scope(&reader);
		ShaderParseUsage2(&replay.user_data, usage.get(), bind.get(), replay.user_sgpr, replay.user_sgpr_num, replay.code.get(),
		                  replay.user_data_register_base, replay.vertex_resource_types);
	}
	report->status                  = reader.Status();
	report->canonical_output        = ShaderResourceFoldCanonicalOutput(*usage, *bind);
	report->fingerprint             = ShaderResourceFoldFingerprint(report->canonical_output);
	report->output_matches_document = report->status == ShaderResourceFoldReplayStatus::Ok && report->canonical_output == replay.canonical_output;
	return true;
}

ShaderResourceFoldToolResult ShaderResourceFoldRunFile(const std::string& path, bool validate_only, ShaderResourceFoldReplayReport* report,
                                                       std::string* error)
{
	std::string document;
	if (!ReadDocumentFile(path, &document, error))
	{
		return ShaderResourceFoldToolResult::Malformed;
	}
	if (validate_only)
	{
		return ShaderResourceFoldValidateDocument(document, error) ? ShaderResourceFoldToolResult::Ok
		                                                            : ShaderResourceFoldToolResult::Malformed;
	}
	ShaderResourceFoldReplayReport local;
	auto*                          out = report != nullptr ? report : &local;
	if (!ShaderResourceFoldReplayDocument(document, out, error))
	{
		return ShaderResourceFoldToolResult::Malformed;
	}
	if (out->status == ShaderResourceFoldReplayStatus::Unused)
	{
		Fail(error, "the transcript holds reads that production never requested");
		return ShaderResourceFoldToolResult::Unused;
	}
	return out->output_matches_document ? ShaderResourceFoldToolResult::Ok : ShaderResourceFoldToolResult::OutputMismatch;
}

void ShaderResourceFoldSetCaptureTestSink(ShaderResourceFoldCaptureTestSink sink, void* context)
{
	std::scoped_lock lock(g_test_sink_mutex);
	g_test_sink.store(nullptr, std::memory_order_release);
	g_test_sink_context = context;
	g_test_sink.store(sink, std::memory_order_release);
}

struct ShaderResourceFoldCaptureScope::Session
{
	// The output objects are copied at entry, before the evaluator writes them, so
	// the exit check compares the borrowed inputs only and never the evaluated bind.
	Session(const ShaderResourceFoldInputs& in, const ShaderParsedUsage* usage_out, const ShaderBindResources* bind_out, uint32_t index,
	        ShaderResourceFoldCaptureTestSink test_sink, void* test_context)
	    : initial_usage(*usage_out), initial_bind(*bind_out), inputs(WithInitialOutput(in, &initial_usage, &initial_bind)),
	      usage(usage_out), bind(bind_out), file_index(index), sink(test_sink), sink_context(test_context),
	      entry_representable(ShaderResourceFoldSerializeInputs(inputs, &entry_parse)), recorder(SHADER_RESOURCE_FOLD_MAX_READS),
	      reader_scope(&recorder)
	{
	}

	static ShaderResourceFoldInputs WithInitialOutput(ShaderResourceFoldInputs in, const ShaderParsedUsage* usage_entry,
	                                                  const ShaderBindResources* bind_entry)
	{
		in.initial_usage = usage_entry;
		in.initial_bind  = bind_entry;
		return in;
	}

	// The document is built from the inputs as they were at entry. Unless they
	// still serialize identically at exit, the capture is not an exact record.
	ShaderResourceFoldCaptureOutcome Finish(std::string* document) const
	{
		if (!entry_representable)
		{
			return ShaderResourceFoldCaptureOutcome::DroppedUnrepresentable;
		}
		std::string exit_parse;
		if (!ShaderResourceFoldSerializeInputs(inputs, &exit_parse) || exit_parse != entry_parse)
		{
			return ShaderResourceFoldCaptureOutcome::DroppedUnstableInput;
		}
		if (recorder.Overflowed())
		{
			return ShaderResourceFoldCaptureOutcome::DroppedOverflow;
		}
		if (!ShaderResourceFoldAssembleDocument(entry_parse, recorder.Transcript(), ShaderResourceFoldCanonicalOutput(*usage, *bind),
		                                        document))
		{
			return ShaderResourceFoldCaptureOutcome::DroppedUnrepresentable;
		}
		return ShaderResourceFoldCaptureOutcome::Captured;
	}

	void Deliver(ShaderResourceFoldCaptureOutcome outcome, const std::string& document) const
	{
		if (sink != nullptr)
		{
			sink(sink_context, outcome, document);
			return;
		}
		if (outcome != ShaderResourceFoldCaptureOutcome::Captured)
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: resource-fold capture dropped: outcome=%u\n", static_cast<unsigned>(outcome));
			return;
		}
		if (!WriteCaptureDocument(file_index, document))
		{
			KYTY_LOG_LIMIT(Log::Level::Warn, 8, "WARNING: resource-fold capture file could not be created exclusively\n");
		}
	}

	ShaderParsedUsage                 initial_usage;
	ShaderBindResources               initial_bind;
	ShaderResourceFoldInputs          inputs;
	const ShaderParsedUsage*          usage;
	const ShaderBindResources*        bind;
	uint32_t                          file_index;
	ShaderResourceFoldCaptureTestSink sink;
	void*                             sink_context;
	std::string                       entry_parse;
	bool                              entry_representable;
	ShaderResourceFoldRecorder        recorder;
	ScopedShaderGuestDescriptorReader reader_scope;
};

ShaderResourceFoldCaptureScope::ShaderResourceFoldCaptureScope(const ShaderUserData* user_data, const ShaderParsedUsage* info,
                                                               const ShaderBindResources* bind, const HW::UserSgprInfo& user_sgpr,
                                                               int user_sgpr_num, const ShaderCode* code, int user_data_register_base,
                                                               bool vertex_resource_types)
{
	if (g_capture_active || user_data == nullptr || info == nullptr || bind == nullptr)
	{
		return;
	}
	ShaderResourceFoldCaptureTestSink sink         = nullptr;
	void*                             sink_context = nullptr;
	uint32_t                          file_index   = 0;
	if (g_test_sink.load(std::memory_order_acquire) != nullptr)
	{
		std::scoped_lock lock(g_test_sink_mutex);
		sink         = g_test_sink.load(std::memory_order_acquire);
		sink_context = g_test_sink_context;
	}
	if (sink == nullptr && !AdmitCapture(&file_index))
	{
		return;
	}
	ShaderResourceFoldInputs inputs;
	inputs.user_data               = user_data;
	inputs.user_sgpr               = &user_sgpr;
	inputs.user_sgpr_num           = user_sgpr_num;
	inputs.code                    = code;
	inputs.user_data_register_base = user_data_register_base;
	inputs.vertex_resource_types   = vertex_resource_types;
	m_session        = std::make_unique<Session>(inputs, info, bind, file_index, sink, sink_context);
	g_capture_active = true;
}

ShaderResourceFoldCaptureScope::~ShaderResourceFoldCaptureScope()
{
	if (!m_session)
	{
		return;
	}
	g_capture_active = false;
	std::string document;
	const auto  outcome = m_session->Finish(&document);
	m_session->Deliver(outcome, document);
}

} // namespace Kyty::Libs::Graphics
