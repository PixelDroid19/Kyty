#include "ShaderSpirvStorageWriteMask.h"

#include "Kyty/Core/DbgAssert.h"

#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

namespace {

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

// Access-chain indices of each pointer derived from a storage root.
class StoragePointers
{
public:
	StoragePointers(): m_indices {{"%buf", {}}, {"%buf_uint", {}}} {}

	[[nodiscard]] const Tokens* Find(const std::string& id) const
	{
		const auto found = m_indices.find(id);
		return found == m_indices.end() ? nullptr : &found->second;
	}
	void Add(const std::string& id, Tokens indices) { m_indices[id] = std::move(indices); }

private:
	std::unordered_map<std::string, Tokens> m_indices;
};

bool IsWriteAtomic(const std::string& opcode)
{
	return opcode.rfind("OpAtomic", 0) == 0 && opcode != "OpAtomicLoad";
}

std::string MarkPage(const Tokens& element, uint32_t index)
{
	const auto page = "%wmask_page_" + std::to_string(index);
	const auto ptr  = "%wmask_ptr_" + std::to_string(index);
	return page + " = OpShiftRightLogical %uint " + element[2] + " %wmask_page_shift\n" + ptr +
	       " = OpAccessChain %_ptr_StorageBuffer_uint %buf_write_mask " + element[0] + " " + element[1] + " " + page + "\n" + "OpStore " +
	       ptr + " %wmask_written\n";
}

} // namespace

bool SpirvMarkStorageWrites(const String8& body, String8* marked, uint32_t* writes)
{
	EXIT_IF(marked == nullptr || writes == nullptr);

	*writes = 0;
	if (body.FindIndex("%buf") == Core::STRING8_INVALID_INDEX)
	{
		*marked = body;
		return true;
	}

	StoragePointers    pointers;
	std::string        output;
	std::istringstream lines(body.c_str());
	for (std::string line; std::getline(lines, line);)
	{
		const auto    tokens  = Tokenize(line);
		const bool    result  = tokens.size() >= 3 && tokens[1] == "=";
		const auto    opcode  = result ? tokens[2] : (tokens.empty() ? std::string() : tokens[0]);
		const auto    pointer = result ? size_t {4} : size_t {1};
		const Tokens* element = nullptr;
		for (size_t i = result ? 3 : 1; i < tokens.size(); i++)
		{
			const auto* indices = pointers.Find(tokens[i]);
			if (indices == nullptr)
			{
				continue;
			}
			const bool read_use  = result && (opcode == "OpLoad" || opcode == "OpAtomicLoad" || opcode == "OpArrayLength");
			const bool write_use = (!result && (opcode == "OpStore" || opcode == "OpAtomicStore")) || (result && IsWriteAtomic(opcode));
			if (opcode == "OpAccessChain" && i == pointer)
			{
				Tokens chain = *indices;
				chain.insert(chain.end(), tokens.begin() + 5, tokens.end());
				pointers.Add(tokens[0], std::move(chain));
				continue;
			}
			if (i != pointer || !(read_use || write_use) || (write_use && indices->size() != 3))
			{
				return false;
			}
			element = write_use ? indices : nullptr;
		}
		if (element != nullptr)
		{
			output += MarkPage(*element, (*writes)++);
		}
		output += line;
		output += '\n';
	}
	*marked = String8(output.c_str());
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
