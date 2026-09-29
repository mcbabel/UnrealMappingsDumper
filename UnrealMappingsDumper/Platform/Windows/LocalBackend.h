#pragma once

#include "../../memory.h"

class FLocalBackend : public Memory::IBackend
{
	Memory::Module Module;
	bool bModuleParsed = false;

public:

	bool ReadRaw(uintptr_t Address, void* Out, size_t Size) override;
	void ReadUnchecked(uintptr_t Address, void* Out, size_t Size) override;
	bool IsReadable(uintptr_t Address, size_t Size) override;
	std::vector<Memory::Region> GetReadableRegions() override;
	const Memory::Module& GetModule() override;

	bool IsLocal() const override { return true; }
};
