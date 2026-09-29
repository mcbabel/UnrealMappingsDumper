#include "../../pch.h"

#include "RemoteBackend.h"

#include <sys/uio.h>
#include <cerrno>

bool FRemoteBackend::ReadProcess(uintptr_t Address, void* Out, size_t Size)
{
	iovec Local{ Out, Size };
	iovec Remote{ reinterpret_cast<void*>(Address), Size };

	auto Read = process_vm_readv(Pid, &Local, 1, &Remote, 1, 0);
	return Read == static_cast<ssize_t>(Size);
}

const uint8_t* FRemoteBackend::GetPage(uintptr_t Page)
{
	if (auto It = Pages.find(Page); It != Pages.end())
		return It->second.get();

	if (BadPages.contains(Page))
		return nullptr;

	if (Pages.size() >= MaxCachedPages)
		ClearCache();

	auto Data = std::make_unique<uint8_t[]>(PageSize);

	if (!ReadProcess(Page, Data.get(), PageSize))
	{
		BadPages[Page] = true;
		return nullptr;
	}

	return Pages.emplace(Page, std::move(Data)).first->second.get();
}

std::unique_ptr<FRemoteBackend> FRemoteBackend::Open(const Process::FInfo& Target, std::string& OutError)
{
	auto Backend = std::unique_ptr<FRemoteBackend>(new FRemoteBackend());
	Backend->Pid = Target.Pid;

	uint64_t Probe = 0;
	iovec Local{ &Probe, sizeof(Probe) };

	auto Maps = Process::ReadMaps(Target.Pid);

	if (Maps.empty())
	{
		OutError = std::format("Could not read the memory map of process {}.", Target.Pid);
		return nullptr;
	}

	std::string ModulePath;

	if (Target.bWine)
	{
		auto WantedName = Process::ToLower(Target.Name);

		for (auto& Map : Maps)
		{
			if (Map.Offset != 0 || Map.Path.empty())
				continue;

			auto Name = Process::ToLower(std::filesystem::path(Map.Path).filename().string());

			if (Name == WantedName)
			{
				Backend->ModuleHeader = Map.Start;
				ModulePath = Map.Path;
				break;
			}
		}

		Backend->bModuleIsPe = true;
	}
	else
	{
		for (auto& Map : Maps)
		{
			if (Map.Offset == 0 && Map.Path == Target.Executable.string())
			{
				Backend->ModuleHeader = Map.Start;
				ModulePath = Map.Path;
				break;
			}
		}
	}

	if (!Backend->ModuleHeader)
	{
		OutError = std::format("Could not find the executable {} in the memory of process {}.", Target.Name, Target.Pid);
		return nullptr;
	}

	iovec Remote{ reinterpret_cast<void*>(Backend->ModuleHeader), sizeof(Probe) };

	if (process_vm_readv(Target.Pid, &Local, 1, &Remote, 1, 0) != sizeof(Probe))
	{
		if (errno == EPERM || errno == EACCES)
		{
			OutError = std::format(
				"Not allowed to read the memory of process {} ({}). Run as root (sudo), or give the dumper the permission once with:\n"
				"  sudo setcap cap_sys_ptrace=eip <path to unreal-mappings-dumper>",
				Target.Pid, strerror(errno));
		}
		else
		{
			OutError = std::format("Could not read the memory of process {} ({}).", Target.Pid, strerror(errno));
		}

		return nullptr;
	}

	Backend->Module.Name = Target.Name;
	Backend->Module.Path = ModulePath;

	return Backend;
}

bool FRemoteBackend::ReadRaw(uintptr_t Address, void* Out, size_t Size)
{
	if (Size == 0)
		return true;

	if (Address < 0x10000 || Address + Size < Address)
		return false;

	if (Size >= DirectReadThreshold)
		return ReadProcess(Address, Out, Size);

	auto Dest = static_cast<uint8_t*>(Out);
	auto Current = Address;
	auto End = Address + Size;

	while (Current < End)
	{
		auto Page = Current & ~(PageSize - 1);
		auto Data = GetPage(Page);

		if (!Data)
			return false;

		auto Offset = Current - Page;
		auto Count = std::min<uintptr_t>(PageSize - Offset, End - Current);

		memcpy(Dest, Data + Offset, Count);

		Dest += Count;
		Current += Count;
	}

	return true;
}

bool FRemoteBackend::IsReadable(uintptr_t Address, size_t Size)
{
	if (Address < 0x10000 || Address + Size < Address)
		return false;

	for (auto Page = Address & ~(PageSize - 1); Page < Address + Size; Page += PageSize)
	{
		if (!GetPage(Page))
			return false;
	}

	return true;
}

std::vector<Memory::Region> FRemoteBackend::GetReadableRegions()
{
	std::vector<Memory::Region> Ret;

	for (auto& Map : Process::ReadMaps(Pid))
	{
		if (!Map.Permissions.empty() && Map.Permissions[0] == 'r')
			Ret.push_back({ Map.Start, Map.End });
	}

	return Ret;
}

const Memory::Module& FRemoteBackend::GetModule()
{
	if (!bModuleParsed)
	{
		bModuleParsed = true;

		if (bModuleIsPe)
			Memory::ParsePeModule(ModuleHeader, Module);
		else
			Memory::ParseElfModule(ModuleHeader, Module);
	}

	return Module;
}

void FRemoteBackend::ClearCache()
{
	Pages.clear();
	BadPages.clear();
}
