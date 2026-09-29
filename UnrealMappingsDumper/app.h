#pragma once

#include "log.h"

namespace App
{
	struct FGameInfo
	{
		std::string ExecutableName;
		std::string FileVersion;
		float MajorMinor = 0;
	};

	bool Init(const FGameInfo& Game);
}
