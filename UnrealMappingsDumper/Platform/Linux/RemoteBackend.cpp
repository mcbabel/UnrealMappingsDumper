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

std::unique_ptr<FRemoteBackend> FRemoteBackend::Open(const Process::FInfo& Target, std::string& OutError, Process::EOpenError* OutCode)
{
	auto SetCode = [&](Process::EOpenError Code)
		{
			if (OutCode)
				*OutCode = Code;
		};

	SetCode(Process::EOpenError::None);

	auto Backend = std::unique_ptr<FRemoteBackend>(new FRemoteBackend());
	Backend->Pid = Target.Pid;

	auto Maps = Process::ReadMaps(Target.Pid);

	if (Maps.empty())
	{
		OutError = std::format("Could not read the memory map of process {}.", Target.Pid);
		SetCode(Process::EOpenError::NotFound);
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
		SetCode(Process::EOpenError::Other);
		return nullptr;
	}

	uint64_t Probe = 0;

	if (!Backend->ReadProcess(Backend->ModuleHeader, &Probe, sizeof(Probe)))
	{
		if (errno == EPERM || errno == EACCES)
		{
			OutError = std::format(
				"Not allowed to read the memory of process {} ({}). Run as root (sudo), or give the dumper the permission once with:\n"
				"  sudo setcap cap_sys_ptrace=eip <path to unreal-mappings-dumper>",
				Target.Pid, strerror(errno));
			SetCode(Process::EOpenError::PermissionDenied);
		}
		else
		{
			OutError = std::format("Could not read the memory of process {} ({}).", Target.Pid, strerror(errno));
			SetCode(Process::EOpenError::Other);
		}

		return nullptr;
	}

	Backend->Module.Name = Target.Name;
	Backend->Module.Path = ModulePath;

	return Backend;
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
