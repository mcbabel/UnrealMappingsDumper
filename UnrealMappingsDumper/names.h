#pragma once

#include "scanning.h"

class FName;

namespace Names
{
	bool Init(
		const std::vector<std::shared_ptr<IScanObject>>& ToStringScans,
		const std::vector<std::shared_ptr<IScanObject>>& NamePoolScans);

	bool TryToString(const FName& Name, std::string& Out);

	std::string ToString(const FName& Name);

	std::string Utf16ToUtf8(const char16_t* Data, size_t Length);
}
