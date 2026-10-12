#include "pch.h"

#include "process.h"

std::string Process::ToLower(std::string Str)
{
	for (auto& C : Str)
		C = static_cast<char>(tolower(static_cast<unsigned char>(C)));

	return Str;
}

bool Process::IsUnrealName(const std::string& Name)
{
	auto LowerName = ToLower(Name);

	for (auto Marker : { "-win64-", "-wingdk-" })
	{
		if (LowerName.find(Marker) != std::string::npos && LowerName.ends_with(".exe"))
			return true;
	}

	for (auto Platform : { "-linux-", "-linuxarm64-" })
	{
		for (auto Configuration : { "shipping", "test", "development", "debuggame", "debug" })
		{
			if (LowerName.ends_with(std::string(Platform) + Configuration))
				return true;
		}
	}

	return false;
}

std::vector<Process::FInfo> Process::Find(const std::string& Pattern)
{
	std::vector<FInfo> Ret;
	auto LowerPattern = ToLower(Pattern);

	for (auto& Info : List())
	{
		bool bMatches = LowerPattern.empty()
			? IsUnrealName(Info.Name)
			: ToLower(Info.Name).find(LowerPattern) != std::string::npos;

		if (bMatches)
			Ret.push_back(Info);
	}

	return Ret;
}
