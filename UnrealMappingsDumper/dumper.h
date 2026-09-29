#pragma once

#include "unrealTypes.h"

namespace Dumper
{
	bool Run(ECompressionMethod CompressionMethod, const std::filesystem::path& OutputPath);
};