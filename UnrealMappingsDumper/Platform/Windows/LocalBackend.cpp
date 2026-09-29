#include "../../pch.h"

#include "LocalBackend.h"

namespace
{
	constexpr DWORD ReadableProtection =
		PAGE_READONLY |
		PAGE_READWRITE |
		PAGE_WRITECOPY |
		PAGE_EXECUTE_READ |
		PAGE_EXECUTE_READWRITE |
		PAGE_EXECUTE_WRITECOPY;

	constexpr uintptr_t MinUserAddress = 0x10000;
	constexpr uintptr_t MaxUserAddress = 0x7FFFFFFFFFFF;

	bool IsReadableRegion(const MEMORY_BASIC_INFORMATION& Info)
	{
		return Info.State == MEM_COMMIT &&
			!(Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
			(Info.Protect & ReadableProtection);
	}
}

bool FLocalBackend::ReadRaw(uintptr_t Address, void* Out, size_t Size)
{
	if (!IsReadable(Address, Size))
		return false;

	memcpy(Out, reinterpret_cast<const void*>(Address), Size);
	return true;
}

void FLocalBackend::ReadUnchecked(uintptr_t Address, void* Out, size_t Size)
{
	memcpy(Out, reinterpret_cast<const void*>(Address), Size);
}

bool FLocalBackend::IsReadable(uintptr_t Address, size_t Size)
{
	auto End = Address + Size;

	if (Address < MinUserAddress || End > MaxUserAddress || End < Address)
		return false;

	auto Current = Address;

	while (Current < End)
	{
		MEMORY_BASIC_INFORMATION Info;

		if (!VirtualQuery(reinterpret_cast<void*>(Current), &Info, sizeof(Info)) || !IsReadableRegion(Info))
			return false;

		Current = reinterpret_cast<uintptr_t>(Info.BaseAddress) + Info.RegionSize;
	}

	return true;
}

std::vector<Memory::Region> FLocalBackend::GetReadableRegions()
{
	std::vector<Memory::Region> Ret;

	uintptr_t Current = MinUserAddress;
	MEMORY_BASIC_INFORMATION Info;

	while (Current < MaxUserAddress && VirtualQuery(reinterpret_cast<void*>(Current), &Info, sizeof(Info)))
	{
		auto RegionStart = reinterpret_cast<uintptr_t>(Info.BaseAddress);
		auto RegionEnd = RegionStart + Info.RegionSize;

		if (IsReadableRegion(Info))
			Ret.push_back({ RegionStart, RegionEnd });

		if (RegionEnd <= Current)
			break;

		Current = RegionEnd;
	}

	return Ret;
}

const Memory::Module& FLocalBackend::GetModule()
{
	if (!bModuleParsed)
	{
		bModuleParsed = true;

		wchar_t Path[MAX_PATH];
		GetModuleFileNameW(nullptr, Path, MAX_PATH);

		Module.Path = Path;
		Module.Name = Memory::PathToUtf8(Module.Path.filename());

		Memory::ParsePeModule(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), Module);
	}

	return Module;
}
