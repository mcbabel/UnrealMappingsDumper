#include "../../pch.h"

#include "RemoteBackend.h"

#include <tlhelp32.h>

namespace
{
	constexpr DWORD ReadableProtection =
		PAGE_READONLY |
		PAGE_READWRITE |
		PAGE_WRITECOPY |
		PAGE_EXECUTE_READ |
		PAGE_EXECUTE_READWRITE |
		PAGE_EXECUTE_WRITECOPY;

	uintptr_t FindMainModule(DWORD Pid)
	{
		auto Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, Pid);

		if (Snapshot == INVALID_HANDLE_VALUE)
			return 0;

		MODULEENTRY32W Entry{};
		Entry.dwSize = sizeof(Entry);

		uintptr_t Base = 0;

		if (Module32FirstW(Snapshot, &Entry))
			Base = reinterpret_cast<uintptr_t>(Entry.modBaseAddr);

		CloseHandle(Snapshot);
		return Base;
	}
}

FWindowsRemoteBackend::~FWindowsRemoteBackend()
{
	if (Process)
		CloseHandle(Process);
}

bool FWindowsRemoteBackend::ReadProcess(uintptr_t Address, void* Out, size_t Size)
{
	SIZE_T Read = 0;
	return ReadProcessMemory(Process, reinterpret_cast<LPCVOID>(Address), Out, Size, &Read) && Read == Size;
}

std::unique_ptr<FWindowsRemoteBackend> FWindowsRemoteBackend::Open(const Process::FInfo& Target, std::string& OutError, Process::EOpenError* OutCode)
{
	auto SetCode = [&](Process::EOpenError Code)
		{
			if (OutCode)
				*OutCode = Code;
		};

	SetCode(Process::EOpenError::None);

	auto Handle = OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(Target.Pid));

	if (!Handle)
	{
		auto Error = GetLastError();

		if (Error == ERROR_ACCESS_DENIED)
		{
			OutError = std::format("Not allowed to read the memory of process {}. Run UnrealMappingsDumper as administrator.", Target.Pid);
			SetCode(Process::EOpenError::PermissionDenied);
		}
		else
		{
			OutError = std::format("Could not open process {} (error {}).", Target.Pid, Error);
			SetCode(Process::EOpenError::NotFound);
		}

		return nullptr;
	}

	BOOL bWow64 = FALSE;

	if (IsWow64Process(Handle, &bWow64) && bWow64)
	{
		CloseHandle(Handle);
		OutError = std::format("Process {} is a 32 bit process. UnrealMappingsDumper only supports 64 bit games.", Target.Pid);
		SetCode(Process::EOpenError::Other);
		return nullptr;
	}

	auto Backend = std::unique_ptr<FWindowsRemoteBackend>(new FWindowsRemoteBackend());
	Backend->Process = Handle;
	Backend->ModuleBase = FindMainModule(static_cast<DWORD>(Target.Pid));

	if (!Backend->ModuleBase)
	{
		OutError = std::format("Could not find the executable of process {}.", Target.Pid);
		SetCode(Process::EOpenError::Other);
		return nullptr;
	}

	uint16_t Probe = 0;

	if (!Backend->ReadProcess(Backend->ModuleBase, &Probe, sizeof(Probe)))
	{
		OutError = std::format("Could not read the memory of process {} (error {}).", Target.Pid, GetLastError());
		SetCode(Process::EOpenError::Other);
		return nullptr;
	}

	Backend->Module.Name = Target.Name;
	Backend->Module.Path = Target.Executable;

	return Backend;
}

std::vector<Memory::Region> FWindowsRemoteBackend::GetReadableRegions()
{
	std::vector<Memory::Region> Ret;

	uintptr_t Current = 0x10000;
	MEMORY_BASIC_INFORMATION Info;

	while (Current < 0x7FFFFFFFFFFF && VirtualQueryEx(Process, reinterpret_cast<LPCVOID>(Current), &Info, sizeof(Info)))
	{
		auto RegionStart = reinterpret_cast<uintptr_t>(Info.BaseAddress);
		auto RegionEnd = RegionStart + Info.RegionSize;

		if (Info.State == MEM_COMMIT && !(Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) && (Info.Protect & ReadableProtection))
			Ret.push_back({ RegionStart, RegionEnd });

		if (RegionEnd <= Current)
			break;

		Current = RegionEnd;
	}

	return Ret;
}

const Memory::Module& FWindowsRemoteBackend::GetModule()
{
	if (!bModuleParsed)
	{
		bModuleParsed = true;
		Memory::ParsePeModule(ModuleBase, Module);
	}

	return Module;
}
