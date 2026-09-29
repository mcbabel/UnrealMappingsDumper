#include "../../pch.h"

#include "Process.h"

namespace
{
	std::string ReadFile(const std::filesystem::path& Path)
	{
		std::ifstream File(Path, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(File), {});
	}

	std::string BaseName(const std::string& Path)
	{
		auto Slash = Path.find_last_of("/\\");
		return Slash == std::string::npos ? Path : Path.substr(Slash + 1);
	}

	bool IsUnrealName(const std::string& LowerName)
	{
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
}

std::string Process::ToLower(std::string Str)
{
	for (auto& C : Str)
		C = static_cast<char>(tolower(static_cast<unsigned char>(C)));

	return Str;
}

std::optional<Process::FInfo> Process::Get(int Pid)
{
	auto Dir = std::filesystem::path("/proc") / std::to_string(Pid);

	std::error_code Error;
	auto Exe = std::filesystem::read_symlink(Dir / "exe", Error);

	if (Error)
		return std::nullopt;

	FInfo Info;
	Info.Pid = Pid;
	Info.Executable = Exe;

	auto ExeName = Exe.filename().string();
	Info.bWine = ExeName.starts_with("wine") || ExeName.find("preloader") != std::string::npos;

	if (Info.bWine)
	{
		auto CmdLine = ReadFile(Dir / "cmdline");
		auto First = CmdLine.substr(0, CmdLine.find('\0'));

		Info.Name = BaseName(First);

		if (!ToLower(Info.Name).ends_with(".exe"))
			return std::nullopt;
	}
	else
	{
		Info.Name = ExeName;
	}

	return Info;
}

std::vector<Process::FInfo> Process::List()
{
	std::vector<FInfo> Ret;

	std::error_code Error;

	for (auto& Entry : std::filesystem::directory_iterator("/proc", Error))
	{
		auto Name = Entry.path().filename().string();

		if (Name.empty() || !std::all_of(Name.begin(), Name.end(), [](char C) { return isdigit(static_cast<unsigned char>(C)); }))
			continue;

		if (auto Info = Get(std::stoi(Name)))
			Ret.push_back(std::move(*Info));
	}

	std::sort(Ret.begin(), Ret.end(), [](const FInfo& A, const FInfo& B) { return A.Pid < B.Pid; });
	return Ret;
}

std::vector<Process::FInfo> Process::Find(const std::string& Pattern)
{
	std::vector<FInfo> Ret;
	auto LowerPattern = ToLower(Pattern);

	for (auto& Info : List())
	{
		auto LowerName = ToLower(Info.Name);

		bool bMatches = LowerPattern.empty()
			? IsUnrealName(LowerName)
			: LowerName.find(LowerPattern) != std::string::npos;

		if (bMatches)
			Ret.push_back(Info);
	}

	return Ret;
}

std::vector<Process::FMapping> Process::ReadMaps(int Pid)
{
	std::vector<FMapping> Ret;
	std::ifstream File(std::format("/proc/{}/maps", Pid));
	std::string Line;

	while (std::getline(File, Line))
	{
		FMapping Map;
		char Permissions[8] = {};
		unsigned long long Start = 0, End = 0, Offset = 0;
		int PathStart = 0;

		if (sscanf(Line.c_str(), "%llx-%llx %7s %llx %*s %*s %n", &Start, &End, Permissions, &Offset, &PathStart) < 4)
			continue;

		Map.Start = Start;
		Map.End = End;
		Map.Permissions = Permissions;
		Map.Offset = Offset;

		if (PathStart > 0 && PathStart < static_cast<int>(Line.size()))
			Map.Path = Line.substr(PathStart);

		Ret.push_back(std::move(Map));
	}

	return Ret;
}
