#include "../../pch.h"

#include "../../process.h"
#include "RemoteBackend.h"

#include <tlhelp32.h>

namespace
{
	std::filesystem::path GetExecutablePath(DWORD Pid)
	{
		auto Handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, Pid);

		if (!Handle)
			return {};

		wchar_t Path[MAX_PATH * 4];
		DWORD Size = static_cast<DWORD>(std::size(Path));

		std::filesystem::path Ret;

		if (QueryFullProcessImageNameW(Handle, 0, Path, &Size))
			Ret = std::wstring(Path, Size);

		CloseHandle(Handle);
		return Ret;
	}
}

std::vector<Process::FInfo> Process::List()
{
	std::vector<FInfo> Ret;

	auto Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

	if (Snapshot == INVALID_HANDLE_VALUE)
		return Ret;

	PROCESSENTRY32W Entry{};
	Entry.dwSize = sizeof(Entry);

	if (Process32FirstW(Snapshot, &Entry))
	{
		do
		{
			if (Entry.th32ProcessID == 0)
				continue;

			FInfo Info;
			Info.Pid = static_cast<int>(Entry.th32ProcessID);
			Info.Name = Memory::PathToUtf8(std::filesystem::path(Entry.szExeFile));
			Ret.push_back(std::move(Info));
		} while (Process32NextW(Snapshot, &Entry));
	}

	CloseHandle(Snapshot);

	for (auto& Info : Ret)
	{
		if (IsUnrealName(Info.Name))
			Info.Executable = GetExecutablePath(static_cast<DWORD>(Info.Pid));
	}

	std::sort(Ret.begin(), Ret.end(), [](const FInfo& A, const FInfo& B) { return A.Pid < B.Pid; });
	return Ret;
}

std::optional<Process::FInfo> Process::Get(int Pid)
{
	for (auto& Info : List())
	{
		if (Info.Pid == Pid)
		{
			if (Info.Executable.empty())
				Info.Executable = GetExecutablePath(static_cast<DWORD>(Pid));

			return Info;
		}
	}

	return std::nullopt;
}

std::unique_ptr<Memory::IBackend> Process::OpenBackend(const FInfo& Target, std::string& OutError, EOpenError* OutCode)
{
	auto Info = Target;

	if (Info.Executable.empty())
		Info.Executable = GetExecutablePath(static_cast<DWORD>(Info.Pid));

	return FWindowsRemoteBackend::Open(Info, OutError, OutCode);
}

std::filesystem::path Process::GetSelfPath()
{
	wchar_t Path[MAX_PATH * 4];
	auto Size = GetModuleFileNameW(nullptr, Path, static_cast<DWORD>(std::size(Path)));
	return std::filesystem::path(std::wstring(Path, Size));
}
