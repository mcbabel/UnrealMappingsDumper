#include "pch.h"

#include "remote.h"

const uint8_t* FCachedRemoteBackend::GetPage(uintptr_t Page)
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

bool FCachedRemoteBackend::ReadRaw(uintptr_t Address, void* Out, size_t Size)
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

bool FCachedRemoteBackend::IsReadable(uintptr_t Address, size_t Size)
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

void FCachedRemoteBackend::ClearCache()
{
	Pages.clear();
	BadPages.clear();
}
