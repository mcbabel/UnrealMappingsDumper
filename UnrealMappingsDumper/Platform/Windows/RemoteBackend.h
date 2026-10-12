#pragma once

#include "../../remote.h"
#include "../../process.h"

class FWindowsRemoteBackend : public FCachedRemoteBackend
{
	HANDLE Process = nullptr;
	Memory::Module Module;
	uintptr_t ModuleBase = 0;
	bool bModuleParsed = false;

protected:

	bool ReadProcess(uintptr_t Address, void* Out, size_t Size) override;

public:

	~FWindowsRemoteBackend() override;

	static std::unique_ptr<FWindowsRemoteBackend> Open(const Process::FInfo& Target, std::string& OutError, Process::EOpenError* OutCode = nullptr);

	std::vector<Memory::Region> GetReadableRegions() override;
	const Memory::Module& GetModule() override;
};
