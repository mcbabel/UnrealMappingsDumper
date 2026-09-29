#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <filesystem>

namespace
{
	struct FProcess
	{
		DWORD Pid;
		std::wstring Name;
	};

	std::wstring ToLower(std::wstring Str)
	{
		for (auto& C : Str)
			C = towlower(C);

		return Str;
	}

	std::vector<FProcess> GetProcesses()
	{
		std::vector<FProcess> Ret;

		auto Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);

		if (Snapshot == INVALID_HANDLE_VALUE)
			return Ret;

		PROCESSENTRY32W Entry{};
		Entry.dwSize = sizeof(Entry);

		if (Process32FirstW(Snapshot, &Entry))
		{
			do
			{
				Ret.push_back({ Entry.th32ProcessID, Entry.szExeFile });
			} while (Process32NextW(Snapshot, &Entry));
		}

		CloseHandle(Snapshot);
		return Ret;
	}

	std::vector<FProcess> FindProcesses(const std::wstring& Pattern)
	{
		std::vector<FProcess> Ret;
		auto LowerPattern = ToLower(Pattern);

		for (auto& Process : GetProcesses())
		{
			auto LowerName = ToLower(Process.Name);

			bool bMatches = LowerPattern.empty()
				? (LowerName.find(L"-win64-") != std::wstring::npos || LowerName.find(L"-wingdk-") != std::wstring::npos)
				: LowerName == LowerPattern || LowerName.find(LowerPattern) != std::wstring::npos;

			if (bMatches)
				Ret.push_back(Process);
		}

		return Ret;
	}

	std::filesystem::path GetDefaultDllPath()
	{
		wchar_t Path[MAX_PATH];
		GetModuleFileNameW(nullptr, Path, MAX_PATH);

		return std::filesystem::path(Path).parent_path() / L"UnrealMappingsDumper.dll";
	}

	bool Is64BitProcess(HANDLE Process)
	{
		BOOL bWow64 = FALSE;

		if (!IsWow64Process(Process, &bWow64))
			return true;

		return !bWow64;
	}

	bool IsModuleLoaded(DWORD Pid, const std::wstring& ModuleName)
	{
		auto Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, Pid);

		if (Snapshot == INVALID_HANDLE_VALUE)
			return false;

		MODULEENTRY32W Entry{};
		Entry.dwSize = sizeof(Entry);

		bool bFound = false;

		if (Module32FirstW(Snapshot, &Entry))
		{
			do
			{
				if (ToLower(Entry.szModule) == ToLower(ModuleName))
				{
					bFound = true;
					break;
				}
			} while (Module32NextW(Snapshot, &Entry));
		}

		CloseHandle(Snapshot);
		return bFound;
	}

	int Inject(DWORD Pid, const std::filesystem::path& DllPath)
	{
		auto Process = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, Pid);

		if (!Process)
		{
			wprintf(L"Could not open process %lu (error %lu). Try running this as administrator.\n", Pid, GetLastError());
			return 1;
		}

		if (!Is64BitProcess(Process))
		{
			wprintf(L"Process %lu is a 32 bit process. UnrealMappingsDumper only supports 64 bit games.\n", Pid);
			CloseHandle(Process);
			return 1;
		}

		if (IsModuleLoaded(Pid, DllPath.filename().wstring()))
			wprintf(L"Warning: %ls is already loaded in process %lu, it has to finish before it can run again.\n", DllPath.filename().c_str(), Pid);

		auto PathString = DllPath.wstring();
		auto PathSize = (PathString.size() + 1) * sizeof(wchar_t);

		auto RemotePath = VirtualAllocEx(Process, nullptr, PathSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

		if (!RemotePath || !WriteProcessMemory(Process, RemotePath, PathString.c_str(), PathSize, nullptr))
		{
			wprintf(L"Could not write to process %lu (error %lu).\n", Pid, GetLastError());

			if (RemotePath)
				VirtualFreeEx(Process, RemotePath, 0, MEM_RELEASE);

			CloseHandle(Process);
			return 1;
		}

		auto LoadLibraryAddress = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
		auto Thread = CreateRemoteThread(Process, nullptr, 0, LoadLibraryAddress, RemotePath, 0, nullptr);

		if (!Thread)
		{
			wprintf(L"Could not create a thread in process %lu (error %lu).\n", Pid, GetLastError());
			VirtualFreeEx(Process, RemotePath, 0, MEM_RELEASE);
			CloseHandle(Process);
			return 1;
		}

		WaitForSingleObject(Thread, 30000);

		DWORD ExitCode = 0;
		GetExitCodeThread(Thread, &ExitCode);

		CloseHandle(Thread);
		VirtualFreeEx(Process, RemotePath, 0, MEM_RELEASE);
		CloseHandle(Process);

		if (!ExitCode)
		{
			wprintf(L"LoadLibrary failed in process %lu. Check that the dll exists and matches the game (64 bit).\n", Pid);
			return 1;
		}

		wprintf(L"Injected %ls into process %lu.\n", PathString.c_str(), Pid);
		wprintf(L"The mappings and UnrealMappingsDumper.log are written next to the game executable.\n");
		return 0;
	}

	void PrintUsage()
	{
		wprintf(
			L"Usage: UnrealMappingsInjector.exe [options] [process name]\n"
			L"\n"
			L"  process name     Name (or part of it) of the game process, e.g. MyGame-Win64-Shipping.exe.\n"
			L"                   Without it the only running *-Win64-Shipping.exe process is used.\n"
			L"  --pid <pid>      Inject into this process id instead.\n"
			L"  --dll <path>     Dll to inject, defaults to UnrealMappingsDumper.dll next to the injector.\n"
			L"  --list           List the running processes.\n");
	}
}

int wmain(int argc, wchar_t** argv)
{
	std::wstring Pattern;
	DWORD Pid = 0;
	auto DllPath = GetDefaultDllPath();

	for (int i = 1; i < argc; i++)
	{
		std::wstring Arg = argv[i];

		if (Arg == L"--help" || Arg == L"-h" || Arg == L"/?")
		{
			PrintUsage();
			return 0;
		}
		else if (Arg == L"--list")
		{
			for (auto& Process : GetProcesses())
				wprintf(L"%8lu  %ls\n", Process.Pid, Process.Name.c_str());

			return 0;
		}
		else if (Arg == L"--pid" && i + 1 < argc)
		{
			Pid = static_cast<DWORD>(wcstoul(argv[++i], nullptr, 10));
		}
		else if (Arg == L"--dll" && i + 1 < argc)
		{
			DllPath = argv[++i];
		}
		else
		{
			Pattern = Arg;
		}
	}

	std::error_code Error;
	DllPath = std::filesystem::absolute(DllPath, Error);

	if (!std::filesystem::exists(DllPath, Error))
	{
		wprintf(L"%ls doesn't exist.\n", DllPath.c_str());
		return 1;
	}

	if (!Pid)
	{
		auto Matches = FindProcesses(Pattern);

		if (Matches.empty())
		{
			wprintf(L"No matching game process found. Start the game first, or pass its process name or --pid.\n");
			return 1;
		}

		if (Matches.size() > 1)
		{
			wprintf(L"More than one process matches, pass the exact name or --pid:\n");

			for (auto& Process : Matches)
				wprintf(L"%8lu  %ls\n", Process.Pid, Process.Name.c_str());

			return 1;
		}

		Pid = Matches[0].Pid;
		wprintf(L"Found %ls (%lu)\n", Matches[0].Name.c_str(), Pid);
	}

	return Inject(Pid, DllPath);
}
