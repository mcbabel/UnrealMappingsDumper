#pragma once

#include "memory.h"

namespace Process
{
	struct FInfo
	{
		int Pid = 0;
		std::string Name;
		std::filesystem::path Executable;
		bool bWine = false;
	};

	enum class EOpenError
	{
		None,
		NotFound,
		PermissionDenied,
		Other
	};

	std::optional<FInfo> Get(int Pid);
	std::vector<FInfo> List();
	std::vector<FInfo> Find(const std::string& Pattern);

	bool IsUnrealName(const std::string& Name);
	std::string ToLower(std::string Str);

	std::unique_ptr<Memory::IBackend> OpenBackend(const FInfo& Target, std::string& OutError, EOpenError* OutCode = nullptr);

	std::filesystem::path GetSelfPath();
}
