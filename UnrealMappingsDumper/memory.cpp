#include "pch.h"

#include "memory.h"

namespace
{
	std::unique_ptr<Memory::IBackend> CurrentBackend;
}

void Memory::SetBackend(std::unique_ptr<IBackend> Backend)
{
	CurrentBackend = std::move(Backend);
}

Memory::IBackend& Memory::Get()
{
	return *CurrentBackend;
}

bool Memory::HasBackend()
{
	return CurrentBackend != nullptr;
}

std::vector<uint8_t> Memory::ReadBytes(uintptr_t Address, size_t Size)
{
	std::vector<uint8_t> Ret(Size);

	if (!ReadRaw(Address, Ret.data(), Size))
		Ret.clear();

	return Ret;
}

bool Memory::ParsePeModule(uintptr_t Base, Module& Out)
{
	constexpr uint32_t ScnCntCode = 0x00000020;
	constexpr uint32_t ScnMemExecute = 0x20000000;
	constexpr uint32_t ScnMemWrite = 0x80000000;

	uint16_t DosMagic = 0;
	int32_t NtOffset = 0;

	if (!Read(Base, DosMagic) || DosMagic != 0x5A4D || !Read(Base + 0x3C, NtOffset) || NtOffset <= 0 || NtOffset > 0x1000)
		return false;

	auto Nt = Base + NtOffset;

	uint32_t Signature = 0;
	uint16_t NumberOfSections = 0;
	uint16_t SizeOfOptionalHeader = 0;
	uint16_t OptionalMagic = 0;
	uint32_t SizeOfImage = 0;

	if (!Read(Nt, Signature) || Signature != 0x4550 ||
		!Read(Nt + 6, NumberOfSections) ||
		!Read(Nt + 20, SizeOfOptionalHeader) ||
		!Read(Nt + 24, OptionalMagic) || OptionalMagic != 0x20B ||
		!Read(Nt + 24 + 0x38, SizeOfImage))
	{
		return false;
	}

	Out.Base = Base;
	Out.Size = SizeOfImage;
	Out.Sections.clear();

	auto SectionHeader = Nt + 24 + SizeOfOptionalHeader;

	for (uint16_t i = 0; i < NumberOfSections; i++, SectionHeader += 40)
	{
		char Name[9] = {};
		uint32_t VirtualSize = 0, VirtualAddress = 0, SizeOfRawData = 0, Characteristics = 0;

		if (!ReadRaw(SectionHeader, Name, 8) ||
			!Read(SectionHeader + 8, VirtualSize) ||
			!Read(SectionHeader + 12, VirtualAddress) ||
			!Read(SectionHeader + 16, SizeOfRawData) ||
			!Read(SectionHeader + 36, Characteristics))
		{
			return false;
		}

		Section S;
		S.Name = Name;
		S.Start = Base + VirtualAddress;
		S.Size = VirtualSize ? VirtualSize : SizeOfRawData;
		S.bExecutable = Characteristics & (ScnMemExecute | ScnCntCode);
		S.bWritable = Characteristics & ScnMemWrite;

		Out.Sections.push_back(std::move(S));
	}

	return true;
}

bool Memory::ParseElfModule(uintptr_t Base, Module& Out)
{
	constexpr uint32_t PtLoad = 1;
	constexpr uint32_t PfExecute = 1;
	constexpr uint32_t PfWrite = 2;

	uint8_t Ident[16] = {};

	if (!ReadRaw(Base, Ident, sizeof(Ident)) || memcmp(Ident, "\x7F" "ELF", 4) != 0 || Ident[4] != 2)
		return false;

	uint64_t PhOffset = 0;
	uint16_t PhEntrySize = 0, PhCount = 0;

	if (!Read(Base + 32, PhOffset) || !Read(Base + 54, PhEntrySize) || !Read(Base + 56, PhCount) || PhEntrySize < 56)
		return false;

	struct FSegment
	{
		uint32_t Flags;
		uint64_t VirtualAddress;
		uint64_t MemorySize;
	};

	std::vector<FSegment> Segments;
	uint64_t MinAddress = UINT64_MAX;

	for (uint16_t i = 0; i < PhCount; i++)
	{
		auto Header = Base + PhOffset + i * PhEntrySize;

		uint32_t Type = 0;
		FSegment Segment{};

		if (!Read(Header, Type) || !Read(Header + 4, Segment.Flags) || !Read(Header + 16, Segment.VirtualAddress) || !Read(Header + 40, Segment.MemorySize))
			return false;

		if (Type != PtLoad)
			continue;

		Segments.push_back(Segment);
		MinAddress = std::min(MinAddress, Segment.VirtualAddress & ~uint64_t(0xFFF));
	}

	if (Segments.empty())
		return false;

	auto Bias = Base - MinAddress;

	Out.Base = Base;
	Out.Sections.clear();

	uintptr_t End = Base;

	for (size_t i = 0; i < Segments.size(); i++)
	{
		auto& Segment = Segments[i];

		Section S;
		S.Name = std::format("LOAD{}{}{}", i, Segment.Flags & PfExecute ? "x" : "", Segment.Flags & PfWrite ? "w" : "");
		S.Start = Bias + Segment.VirtualAddress;
		S.Size = Segment.MemorySize;
		S.bExecutable = Segment.Flags & PfExecute;
		S.bWritable = Segment.Flags & PfWrite;

		End = std::max<uintptr_t>(End, S.Start + S.Size);
		Out.Sections.push_back(std::move(S));
	}

	Out.Size = End - Base;
	return true;
}

Memory::RegionSnapshot::RegionSnapshot()
{
	for (auto& R : Get().GetReadableRegions())
	{
		if (!Regions.empty() && Regions.back().End == R.Start)
			Regions.back().End = R.End;
		else
			Regions.push_back(R);
	}
}

bool Memory::RegionSnapshot::Contains(uintptr_t Address, size_t Size) const
{
	if (Address < 0x10000 || Address + Size < Address)
		return false;

	auto It = std::upper_bound(Regions.begin(), Regions.end(), Address,
		[](uintptr_t Value, const Region& R) { return Value < R.Start; });

	if (It == Regions.begin())
		return false;

	--It;

	return Address >= It->Start && Address + Size <= It->End;
}

std::string Memory::DescribeAddress(uintptr_t Address)
{
	if (HasBackend() && IsInModule(Address))
		return std::format("{}+0x{:X}", Get().GetModule().Name, Address - GetModuleBase());

	return std::format("0x{:X}", Address);
}

std::string Memory::PathToUtf8(const std::filesystem::path& Path)
{
	auto Utf8 = Path.u8string();
	return std::string(Utf8.begin(), Utf8.end());
}

#if defined(_MSC_VER)

static int FaultFilter(EXCEPTION_POINTERS* Pointers, Memory::FaultInfo* OutFault)
{
	if (OutFault)
	{
		OutFault->Code = Pointers->ExceptionRecord->ExceptionCode;
		OutFault->Address = reinterpret_cast<uintptr_t>(Pointers->ExceptionRecord->ExceptionAddress);
	}

	return EXCEPTION_EXECUTE_HANDLER;
}

bool Memory::TryCall(void (*Function)(void*), void* Context, FaultInfo* OutFault)
{
	__try
	{
		Function(Context);
		return true;
	}
	__except (FaultFilter(GetExceptionInformation(), OutFault))
	{
		return false;
	}
}

#elif defined(_WIN32)

namespace
{
	struct FGuard
	{
		void* JumpBuffer[5];
		Memory::FaultInfo Fault;
	};

	thread_local FGuard* CurrentGuard = nullptr;

	bool IsHardwareFault(DWORD Code)
	{
		switch (Code)
		{
		case EXCEPTION_ACCESS_VIOLATION:
		case EXCEPTION_IN_PAGE_ERROR:
		case EXCEPTION_ILLEGAL_INSTRUCTION:
		case EXCEPTION_PRIV_INSTRUCTION:
		case EXCEPTION_INT_DIVIDE_BY_ZERO:
		case EXCEPTION_INT_OVERFLOW:
		case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
		case EXCEPTION_DATATYPE_MISALIGNMENT:
			return true;
		default:
			return false;
		}
	}

	LONG CALLBACK GuardHandler(EXCEPTION_POINTERS* Pointers)
	{
		auto Guard = CurrentGuard;

		if (!Guard || !IsHardwareFault(Pointers->ExceptionRecord->ExceptionCode))
			return EXCEPTION_CONTINUE_SEARCH;

		Guard->Fault.Code = Pointers->ExceptionRecord->ExceptionCode;
		Guard->Fault.Address = reinterpret_cast<uintptr_t>(Pointers->ExceptionRecord->ExceptionAddress);

		__builtin_longjmp(Guard->JumpBuffer, 1);
	}
}

bool Memory::TryCall(void (*Function)(void*), void* Context, FaultInfo* OutFault)
{
	static PVOID Handler = AddVectoredExceptionHandler(1, GuardHandler);
	(void)Handler;

	FGuard Guard{};
	FGuard* volatile Previous = CurrentGuard;

	if (__builtin_setjmp(Guard.JumpBuffer))
	{
		CurrentGuard = Previous;

		if (OutFault)
			*OutFault = Guard.Fault;

		return false;
	}

	CurrentGuard = &Guard;

	try
	{
		Function(Context);
	}
	catch (...)
	{
		CurrentGuard = Previous;
		throw;
	}

	CurrentGuard = Previous;

	return true;
}

#else

bool Memory::TryCall(void (*Function)(void*), void* Context, FaultInfo* OutFault)
{
	(void)OutFault;

	Function(Context);
	return true;
}

#endif
