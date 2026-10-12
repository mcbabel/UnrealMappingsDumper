#pragma once

#include "../../remote.h"
#include "Process.h"

class FRemoteBackend : public FCachedRemoteBackend
{
	int Pid = 0;
	Memory::Module Module;
	uintptr_t ModuleHeader = 0;
	bool bModuleIsPe = false;
	bool bModuleParsed = false;

protected:

	bool ReadProcess(uintptr_t Address, void* Out, size_t Size) override;

public:

	static std::unique_ptr<FRemoteBackend> Open(const Process::FInfo& Target, std::string& OutError, Process::EOpenError* OutCode = nullptr);

	std::vector<Memory::Region> GetReadableRegions() override;
	const Memory::Module& GetModule() override;
};
