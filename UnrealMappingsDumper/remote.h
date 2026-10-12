#pragma once

#include "memory.h"

class FCachedRemoteBackend : public Memory::IBackend
{
	static constexpr uintptr_t PageSize = 0x1000;
	static constexpr size_t MaxCachedPages = 512 * 1024;
	static constexpr size_t DirectReadThreshold = 64 * 1024;

	std::unordered_map<uintptr_t, std::unique_ptr<uint8_t[]>> Pages;
	std::unordered_map<uintptr_t, bool> BadPages;

	const uint8_t* GetPage(uintptr_t Page);

protected:

	virtual bool ReadProcess(uintptr_t Address, void* Out, size_t Size) = 0;

public:

	bool ReadRaw(uintptr_t Address, void* Out, size_t Size) override;
	bool IsReadable(uintptr_t Address, size_t Size) override;

	void ClearCache();
};
