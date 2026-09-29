#pragma once

#include "../../memory.h"
#include "Process.h"

class FRemoteBackend : public Memory::IBackend
{
	static constexpr uintptr_t PageSize = 0x1000;
	static constexpr size_t MaxCachedPages = 512 * 1024;
	static constexpr size_t DirectReadThreshold = 64 * 1024;

	int Pid = 0;
	Memory::Module Module;
	uintptr_t ModuleHeader = 0;
	bool bModuleIsPe = false;
	bool bModuleParsed = false;

	std::unordered_map<uintptr_t, std::unique_ptr<uint8_t[]>> Pages;
	std::unordered_map<uintptr_t, bool> BadPages;

	bool ReadProcess(uintptr_t Address, void* Out, size_t Size);
	const uint8_t* GetPage(uintptr_t Page);

public:

	static std::unique_ptr<FRemoteBackend> Open(const Process::FInfo& Target, std::string& OutError);

	bool ReadRaw(uintptr_t Address, void* Out, size_t Size) override;
	bool IsReadable(uintptr_t Address, size_t Size) override;
	std::vector<Memory::Region> GetReadableRegions() override;
	const Memory::Module& GetModule() override;

	void ClearCache();
};
