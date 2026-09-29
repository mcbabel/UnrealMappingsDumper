#pragma once

namespace Process
{
	struct FInfo
	{
		int Pid = 0;
		std::string Name;
		std::filesystem::path Executable;
		bool bWine = false;
	};

	struct FMapping
	{
		uintptr_t Start = 0;
		uintptr_t End = 0;
		std::string Permissions;
		uint64_t Offset = 0;
		std::string Path;
	};

	std::optional<FInfo> Get(int Pid);
	std::vector<FInfo> List();
	std::vector<FInfo> Find(const std::string& Pattern);
	std::vector<FMapping> ReadMaps(int Pid);

	std::string ToLower(std::string Str);
}
