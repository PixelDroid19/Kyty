#ifndef EMULATOR_INCLUDE_EMULATOR_CONFIGSOURCE_H_
#define EMULATOR_INCLUDE_EMULATOR_CONFIGSOURCE_H_

#include "Kyty/Core/String.h"

#include <cstdint>

// Neutral keyed configuration source. The CLI's Lua host implements this
// adapter; the runtime never depends on the script engine. Missing keys
// report false from Has(); conversions follow the Lua host semantics
// (integer for numeric fields, boolean, string).
namespace Kyty::Config {

class ConfigSource
{
public:
	virtual ~ConfigSource() = default;

	[[nodiscard]] virtual bool           Has(const Core::String& key) const     = 0;
	[[nodiscard]] virtual int64_t        GetInteger(const Core::String& key) const = 0;
	[[nodiscard]] virtual bool           GetBool(const Core::String& key) const = 0;
	[[nodiscard]] virtual Core::String   GetString(const Core::String& key) const = 0;
};

} // namespace Kyty::Config

#endif /* EMULATOR_INCLUDE_EMULATOR_CONFIGSOURCE_H_ */
