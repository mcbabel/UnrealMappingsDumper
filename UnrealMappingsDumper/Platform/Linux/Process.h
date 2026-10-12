#pragma once

#include "../../process.h"

namespace Process
{
	struct FMapping
	{
		uintptr_t Start = 0;
		uintptr_t End = 0;
		std::string Permissions;
		uint64_t Offset = 0;
		std::string Path;
	};

	std::vector<FMapping> ReadMaps(int Pid);
}
