#include "pch.h"

#include "names.h"
#include "memory.h"
#include "log.h"
#include "unrealTypes.h"

namespace
{
	enum class EMode
	{
		None,
		Function,
		NamePool
	};

	EMode Mode = EMode::None;

	void AppendUtf8(std::string& Out, uint32_t CodePoint)
	{
		if (CodePoint < 0x80)
		{
			Out += static_cast<char>(CodePoint);
		}
		else if (CodePoint < 0x800)
		{
			Out += static_cast<char>(0xC0 | (CodePoint >> 6));
			Out += static_cast<char>(0x80 | (CodePoint & 0x3F));
		}
		else if (CodePoint < 0x10000)
		{
			Out += static_cast<char>(0xE0 | (CodePoint >> 12));
			Out += static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F));
			Out += static_cast<char>(0x80 | (CodePoint & 0x3F));
		}
		else
		{
			Out += static_cast<char>(0xF0 | (CodePoint >> 18));
			Out += static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3F));
			Out += static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F));
			Out += static_cast<char>(0x80 | (CodePoint & 0x3F));
		}
	}

#ifdef _WIN32
	using FNameToStringFn = void (*)(const void* Name, FString& Out);

	FNameToStringFn ToStringFunc = nullptr;
	FString ToStringBuffer;

	struct CallContext
	{
		FNameToStringFn Function;
		const void* Name;
		FString* Out;
	};

	bool CallToString(FNameToStringFn Function, const void* Name, std::string& Out)
	{
		ToStringBuffer.ResetNumKeepAllocation();

		CallContext Ctx{ Function, Name, &ToStringBuffer };

		bool bOk = Memory::TryCall([](void* Context)
			{
				auto C = static_cast<CallContext*>(Context);
				C->Function(C->Name, *C->Out);
			}, &Ctx);

		if (!bOk)
			return false;

		if (ToStringBuffer.Num() < 0 || ToStringBuffer.Num() > ToStringBuffer.Max() || ToStringBuffer.Num() > 0x10000)
			return false;

		auto View = ToStringBuffer.AsString();
		Out = Names::Utf16ToUtf8(reinterpret_cast<const char16_t*>(View.data()), View.size());
		return true;
	}
#endif

	constexpr int BlocksOffset = 0x10;
	constexpr int MaxBlocks = 8192;
	constexpr int BlockOffsetBits = 16;
	constexpr int EntryStride = 2;

	struct FNamePoolReader
	{
		uintptr_t Pool = 0;

		int LenShift = 6;

		uintptr_t GetBlock(uint32_t Block)
		{
			if (Block >= MaxBlocks)
				return 0;

			return Memory::ReadOr<uintptr_t>(Pool + BlocksOffset + Block * sizeof(void*), 0);
		}

		bool Decode(uint32_t Index, std::string& Out, int Depth = 0)
		{
			auto Block = GetBlock(Index >> BlockOffsetBits);

			if (!Block || Depth > 2)
				return false;

			auto Entry = Block + (Index & ((1u << BlockOffsetBits) - 1)) * EntryStride;

			uint16_t Header = 0;

			if (!Memory::Read(Entry, Header))
				return false;

			bool bIsWide = Header & 1;
			uint32_t Len = Header >> LenShift;

			if (Len == 0)
			{
				uint32_t Id = 0, Number = 0;

				if (!Memory::Read(Entry + 2, Id) || !Memory::Read(Entry + 6, Number) || Id == Index)
					return false;

				if (!Decode(Id, Out, Depth + 1))
					return false;

				if (Number)
					Out += "_" + std::to_string(Number - 1);

				return true;
			}

			if (Len > 1024)
				return false;

			if (bIsWide)
			{
				std::vector<char16_t> Chars(Len);

				if (!Memory::ReadRaw(Entry + 2, Chars.data(), Len * sizeof(char16_t)))
					return false;

				Out = Names::Utf16ToUtf8(Chars.data(), Len);
			}
			else
			{
				std::vector<uint8_t> Chars(Len);

				if (!Memory::ReadRaw(Entry + 2, Chars.data(), Len))
					return false;

				Out.clear();

				for (auto C : Chars)
					AppendUtf8(Out, C);
			}

			return true;
		}
	};

	FNamePoolReader PoolReader;

	bool IsNamePoolAt(uintptr_t Block0, int LenShift, const Memory::RegionSnapshot* Snapshot)
	{
		auto Check = [&](uintptr_t Entry, std::string_view Expected)
			{
				auto Size = 2 + Expected.size();

				if (Snapshot && !Snapshot->Contains(Entry, Size))
					return false;

				uint8_t Data[16] = {};

				if (!Memory::ReadRaw(Entry, Data, Size))
					return false;

				uint16_t Header = 0;
				memcpy(&Header, Data, 2);

				if ((Header & 1) || (Header >> LenShift) != Expected.size())
					return false;

				return memcmp(Data + 2, Expected.data(), Expected.size()) == 0;
			};

		return Check(Block0, "None") && Check(Block0 + 6, "ByteProperty");
	}

	bool TryUseNamePool(uintptr_t Pool, uintptr_t Block0, const Memory::RegionSnapshot* Snapshot)
	{
		for (int LenShift : { 6, 1 })
		{
			if (IsNamePoolAt(Block0, LenShift, Snapshot))
			{
				PoolReader = {};
				PoolReader.Pool = Pool;
				PoolReader.LenShift = LenShift;
				return true;
			}
		}

		return false;
	}

	bool TryUseNamePool(uintptr_t Pool)
	{
		uintptr_t Block0 = 0;
		return Memory::Read(Pool + BlocksOffset, Block0) && TryUseNamePool(Pool, Block0, nullptr);
	}

	uintptr_t BruteForceNamePool()
	{
		Memory::RegionSnapshot Snapshot;
		uintptr_t Found = 0;

		for (auto& Section : Memory::GetSections())
		{
			if (!Section.IsWritable() || Section.IsExecutable() || Found)
				continue;

			Memory::ForEachSectionChunk(Section, BlocksOffset + 8, [&](uintptr_t Start, const uint8_t* Data, size_t Size, size_t Owned)
				{
					for (size_t Offset = 0; !Found && Offset < Owned && Offset + BlocksOffset + 8 <= Size; Offset += 8)
					{
						uintptr_t Block0 = 0;
						memcpy(&Block0, Data + Offset + BlocksOffset, sizeof(Block0));

						if (!Snapshot.Contains(Block0, 32))
							continue;

						if (TryUseNamePool(Start + Offset, Block0, &Snapshot))
							Found = Start + Offset;
					}
				});
		}

		return Found;
	}
}

bool Names::Init(
	const std::vector<std::shared_ptr<IScanObject>>& ToStringScans,
	const std::vector<std::shared_ptr<IScanObject>>& NamePoolScans)
{
	for (auto& Scan : NamePoolScans)
	{
		for (auto Candidate : Scan->FindAll())
		{
			if (TryUseNamePool(Candidate))
			{
				UE_LOG("Found FNamePool at %s (%s)", Memory::DescribeAddress(Candidate).c_str(), Scan->Describe().c_str());

				Mode = EMode::NamePool;
				return true;
			}
		}
	}

	if (auto Pool = BruteForceNamePool())
	{
		UE_LOG("Found FNamePool at %s (data section scan)", Memory::DescribeAddress(Pool).c_str());

		Mode = EMode::NamePool;
		return true;
	}

#ifdef _WIN32
	if (Memory::Get().IsLocal())
	{
		UE_LOG("No standard FNamePool found, looking for FName::ToString instead.");

		const FName NoneName(0);

		for (auto& Scan : ToStringScans)
		{
			for (auto Candidate : Scan->FindAll())
			{
				if (!Memory::IsInModule(Candidate))
					continue;

				std::string Result;

				if (CallToString(reinterpret_cast<FNameToStringFn>(Candidate), &NoneName, Result) && Result == "None")
				{
					UE_LOG("Found FName::ToString at %s (%s)", Memory::DescribeAddress(Candidate).c_str(), Scan->Describe().c_str());

					ToStringFunc = reinterpret_cast<FNameToStringFn>(Candidate);
					Mode = EMode::Function;
					return true;
				}
			}
		}
	}
#else
	(void)ToStringScans;
#endif

	return false;
}

bool Names::TryToString(const FName& Name, std::string& Out)
{
	switch (Mode)
	{
#ifdef _WIN32
	case EMode::Function:
	{
		return CallToString(ToStringFunc, &Name, Out);
	}
#endif
	case EMode::NamePool:
	{
		if (!PoolReader.Decode(Name.GetComparisonIndex(), Out))
			return false;

		if (Name.GetNumber())
			Out += "_" + std::to_string(Name.GetNumber() - 1);

		return true;
	}
	default:
	{
		return false;
	}
	}
}

std::string Names::ToString(const FName& Name)
{
	std::string Ret;

	if (!TryToString(Name, Ret))
		Ret.clear();

	return Ret;
}

std::string Names::Utf16ToUtf8(const char16_t* Data, size_t Length)
{
	std::string Ret;
	Ret.reserve(Length);

	for (size_t i = 0; i < Length; i++)
	{
		uint32_t CodePoint = Data[i];

		if (CodePoint >= 0xD800 && CodePoint <= 0xDBFF && i + 1 < Length && Data[i + 1] >= 0xDC00 && Data[i + 1] <= 0xDFFF)
		{
			CodePoint = 0x10000 + ((CodePoint - 0xD800) << 10) + (Data[i + 1] - 0xDC00);
			i++;
		}

		AppendUtf8(Ret, CodePoint);
	}

	return Ret;
}
