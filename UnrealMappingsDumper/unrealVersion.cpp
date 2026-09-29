#include "pch.h"

#include "unrealVersion.h"
#include "memory.h"

#pragma comment(lib, "Version.lib")

namespace
{
	constexpr int ElementsPerChunk = 64 * 1024;

	constexpr int ObjectIndexOffset = 0x0C;

	struct FObjectArrayHeader
	{
		uintptr_t Objects;
		uintptr_t PreAllocatedObjects;
		int32_t MaxElements;
		int32_t NumElements;
		int32_t MaxChunks;
		int32_t NumChunks;
	};

	bool LooksLikeObjectArray(const FObjectArrayHeader& H)
	{
		return H.Objects &&
			H.NumElements > 0 &&
			H.NumElements <= H.MaxElements &&
			H.MaxElements <= 0x10000000 &&
			H.NumChunks > 0 &&
			H.NumChunks <= H.MaxChunks &&
			H.MaxChunks <= 0x4000 &&
			H.NumChunks >= (H.NumElements + ElementsPerChunk - 1) / ElementsPerChunk;
	}

	int DetectItemSize(uintptr_t Chunk0, int NumElements)
	{
		const int ToCheck = std::min(NumElements, 0x200);

		for (int Size : { 0x18, 0x10, 0x20, 0x28, 0x30 })
		{
			int Checked = 0;
			int Matched = 0;

			for (int i = 0; i < ToCheck; i++)
			{
				uintptr_t Obj = 0;

				if (!Memory::Read(Chunk0 + i * Size, Obj))
					break;

				if (!Obj)
					continue;

				Checked++;

				int32_t Index = -1;

				if (Memory::Read(Obj + ObjectIndexOffset, Index) && Index == i)
					Matched++;

				if (Checked >= 16 && Matched == 0)
					break;
			}

			if (Checked >= 16 && Matched * 10 >= Checked * 9)
				return Size;
		}

		int Checked = 0;
		int Matched = 0;

		for (int i = 0; i < ToCheck; i++)
		{
			uintptr_t Obj = 0;

			if (!Memory::Read(Chunk0 + i * 0x18, Obj))
				break;

			if (!Obj)
				continue;

			Checked++;

			uintptr_t VTable = 0;

			if (Memory::Read(Obj, VTable) && Memory::IsInModule(VTable))
				Matched++;
		}

		if (Checked >= 16 && Matched * 10 >= Checked * 9)
		{
			UE_LOG("GObjects candidate has objects with an unexpected InternalIndex, assuming the default FUObjectItem size.");
			return 0x18;
		}

		return 0;
	}

	int ValidateObjectArray(uintptr_t Candidate)
	{
		FObjectArrayHeader H{};

		if (!Memory::Read(Candidate, H) || !LooksLikeObjectArray(H))
			return 0;

		if (!Memory::IsReadable(H.Objects, H.NumChunks * sizeof(void*)))
			return 0;

		uintptr_t Chunk0 = 0;

		if (!Memory::Read(H.Objects, Chunk0) || !Chunk0)
			return 0;

		return DetectItemSize(Chunk0, H.NumElements);
	}

	uintptr_t ReadPtr(uintptr_t Address)
	{
		return Memory::ReadOr<uintptr_t>(Address, 0);
	}

	std::string NameAt(uintptr_t Address)
	{
		uint32_t Index = 0;

		if (!Memory::Read(Address, Index))
			return {};

		std::string Ret;

		if (!Names::TryToString(FName(Index), Ret))
			return {};

		return Ret;
	}

	bool IsSensibleName(const std::string& Name)
	{
		if (Name.empty() || Name.size() > 1024)
			return false;

		for (unsigned char C : Name)
		{
			if (C < 0x20 || C == 0x7F)
				return false;
		}

		return true;
	}

	struct FCoreObjects
	{
		std::unordered_map<std::string, std::vector<uintptr_t>> ByName;

		uintptr_t Class = 0;
		uintptr_t Package = 0;
		uintptr_t CoreUObject = 0;
		uintptr_t Struct = 0;
		uintptr_t ScriptStruct = 0;
		uintptr_t Enum = 0;
		uintptr_t Vector = 0;
		uintptr_t Quat = 0;
		uintptr_t Transform = 0;

		void Collect(int NameOffset)
		{
			static const std::string Wanted[] =
			{
				"Class", "Package", "/Script/CoreUObject", "Struct", "ScriptStruct", "Enum", "Vector", "Quat", "Transform"
			};

			for (int i = 0; i < ObjObjects::Num(); i++)
			{
				auto Obj = ObjObjects::GetObjectByIndex(i).GetAddress();

				if (!Obj)
					continue;

				auto Name = NameAt(Obj + NameOffset);

				for (auto& W : Wanted)
				{
					if (Name == W && ByName[W].size() < 16)
						ByName[W].push_back(Obj);
				}
			}
		}

		uintptr_t Find(const char* Name, uintptr_t ClassObj, int ClassOffset) const
		{
			auto It = ByName.find(Name);

			if (It == ByName.end())
				return 0;

			for (auto Obj : It->second)
			{
				if (ReadPtr(Obj + ClassOffset) == ClassObj)
					return Obj;
			}

			return 0;
		}
	};

	bool DetectFieldLayout(uintptr_t FirstField, const char* FirstName, const char* SecondName, int& OutNameOffset, int& OutNextOffset)
	{
		for (int NameOffset = 0x18; NameOffset <= 0x40; NameOffset += 4)
		{
			if (NameAt(FirstField + NameOffset) != FirstName)
				continue;

			for (int NextOffset = 0x10; NextOffset < NameOffset; NextOffset += 8)
			{
				auto Next = ReadPtr(FirstField + NextOffset);

				if (!Memory::IsReadable(Next, NameOffset + 8))
					continue;

				if (NameAt(Next + NameOffset) == SecondName)
				{
					OutNameOffset = NameOffset;
					OutNextOffset = NextOffset;
					return true;
				}
			}
		}

		return false;
	}
}

bool OffsetFinder::FindGObjects(const std::vector<std::shared_ptr<IScanObject>>& Scans)
{
	auto TryCandidate = [](uintptr_t Candidate, const std::string& Source, std::initializer_list<int> Offsets)
		{
			for (auto Offset : Offsets)
			{
				if (int ItemSize = ValidateObjectArray(Candidate + Offset))
				{
					ObjObjects::SetInstance(Candidate + Offset);
					ObjObjects::ItemSize = ItemSize;

					UE_LOG("Found GObjects at %s (%s) with %d objects, FUObjectItem size 0x%X",
						Memory::DescribeAddress(Candidate + Offset).c_str(), Source.c_str(), ObjObjects::Num(), ItemSize);

					return true;
				}
			}

			return false;
		};

	for (auto& Scan : Scans)
	{
		for (auto Candidate : Scan->FindAll())
		{
			if (TryCandidate(Candidate, Scan->Describe(), { 0, 0x10 }))
				return true;
		}
	}

	UE_LOG("None of the GObjects signatures matched, scanning the data sections instead...");

	Memory::RegionSnapshot Snapshot;
	bool bFound = false;

	for (auto& Section : Memory::GetSections())
	{
		if (!Section.IsWritable() || Section.IsExecutable() || bFound)
			continue;

		Memory::ForEachSectionChunk(Section, sizeof(FObjectArrayHeader), [&](uintptr_t Start, const uint8_t* Data, size_t Size, size_t Owned)
			{
				for (size_t Offset = 0; !bFound && Offset < Owned && Offset + sizeof(FObjectArrayHeader) <= Size; Offset += 8)
				{
					FObjectArrayHeader Header;
					memcpy(&Header, Data + Offset, sizeof(Header));

					if (!LooksLikeObjectArray(Header) || !Snapshot.Contains(Header.Objects, Header.NumChunks * sizeof(void*)))
						continue;

					bFound = TryCandidate(Start + Offset, "data section scan", { 0 });
				}
			});
	}

	return bFound;
}

bool OffsetFinder::FindOffsets()
{
	auto ScoreNameOffset = [](int Offset)
		{
			int Good = 0;
			int Total = 0;

			for (int i = 0; i < ObjObjects::Num() && Total < 200; i++)
			{
				auto Obj = ObjObjects::GetObjectByIndex(i).GetAddress();

				if (!Obj)
					continue;

				Total++;

				if (IsSensibleName(NameAt(Obj + Offset)))
					Good++;
			}

			return Total ? Good * 100 / Total : 0;
		};

	if (ScoreNameOffset(UObject::NameOffset) < 90)
	{
		bool bFound = false;

		for (int Offset = 0x8; Offset <= 0x40; Offset += 4)
		{
			if (ScoreNameOffset(Offset) >= 90)
			{
				UObject::NameOffset = Offset;
				bFound = true;
				break;
			}
		}

		if (!bFound)
		{
			UE_LOG("Could not resolve the names of the objects in GObjects. The game might use a custom UObject layout or encrypted names.");
			return false;
		}
	}

	FCoreObjects Core;
	Core.Collect(UObject::NameOffset);

	for (auto Obj : Core.ByName["Class"])
	{
		for (int Offset = 0x8; Offset < 0x40; Offset += 8)
		{
			if (ReadPtr(Obj + Offset) == Obj)
			{
				UObject::ClassOffset = Offset;
				Core.Class = Obj;
				break;
			}
		}

		if (Core.Class)
			break;
	}

	if (!Core.Class)
	{
		UE_LOG("Could not find the UClass \"Class\". GObjects or the names are wrong.");
		return false;
	}

	Core.Package = Core.Find("Package", Core.Class, UObject::ClassOffset);
	Core.CoreUObject = Core.Find("/Script/CoreUObject", Core.Package, UObject::ClassOffset);

	if (Core.CoreUObject)
	{
		for (int Offset = 0x8; Offset < 0x48; Offset += 8)
		{
			if (Offset != UObject::ClassOffset && ReadPtr(Core.Class + Offset) == Core.CoreUObject)
			{
				UObject::OuterOffset = Offset;
				break;
			}
		}
	}
	else
	{
		UE_LOG("Could not find \"/Script/CoreUObject\", using the default Outer offset.");
	}

	Core.Struct = Core.Find("Struct", Core.Class, UObject::ClassOffset);
	Core.ScriptStruct = Core.Find("ScriptStruct", Core.Class, UObject::ClassOffset);
	Core.Enum = Core.Find("Enum", Core.Class, UObject::ClassOffset);

	if (Core.Struct && Core.ScriptStruct)
	{
		for (int Offset = 0x28; Offset < 0x80; Offset += 8)
		{
			if (ReadPtr(Core.Class + Offset) == Core.Struct && ReadPtr(Core.ScriptStruct + Offset) == Core.Struct)
			{
				UStruct::SuperOffset = Offset;
				break;
			}
		}
	}
	else
	{
		UE_LOG("Could not find \"Struct\"/\"ScriptStruct\", using the default Super offset.");
	}

	if (Core.ScriptStruct)
	{
		Core.Vector = Core.Find("Vector", Core.ScriptStruct, UObject::ClassOffset);
		Core.Quat = Core.Find("Quat", Core.ScriptStruct, UObject::ClassOffset);
		Core.Transform = Core.Find("Transform", Core.ScriptStruct, UObject::ClassOffset);
	}

	uintptr_t VectorX = 0;

	if (Core.Vector)
	{
		std::vector<int> Candidates = { UStruct::SuperOffset + 0x10 };

		for (int Offset = UStruct::SuperOffset + 0x8; Offset <= UStruct::SuperOffset + 0x48; Offset += 8)
		{
			if (Offset != Candidates[0])
				Candidates.push_back(Offset);
		}

		for (auto Offset : Candidates)
		{
			auto First = ReadPtr(Core.Vector + Offset);

			if (!Memory::IsReadable(First, 0x40))
				continue;

			int NameOffset = 0, NextOffset = 0;

			if (DetectFieldLayout(First, "X", "Y", NameOffset, NextOffset))
			{
				UStruct::ChildPropertiesOffset = Offset;
				FField::NameOffset = NameOffset;
				FField::NextOffset = NextOffset;
				VectorX = First;
				break;
			}
		}
	}

	if (VectorX)
	{
		for (int Offset = 0x8; Offset < FField::NextOffset; Offset += 8)
		{
			auto FieldClass = ReadPtr(VectorX + Offset);
			auto ClassName = FieldClass ? NameAt(FieldClass) : std::string();

			if (ClassName == "FloatProperty" || ClassName == "DoubleProperty" || ClassName == "LargeWorldCoordinatesRealProperty")
			{
				FField::ClassOffset = Offset;
				break;
			}
		}

		auto CheckArrayDim = [&](int Offset)
			{
				int32_t ArrayDim = 0, ElementSize = 0;

				return Memory::Read(VectorX + Offset, ArrayDim) &&
					Memory::Read(VectorX + Offset + 4, ElementSize) &&
					ArrayDim == 1 &&
					(ElementSize == 4 || ElementSize == 8);
			};

		if (CheckArrayDim(FField::NameOffset + 0x10))
		{
			FProperty::ArrayDimOffset = FField::NameOffset + 0x10;
			FName::IsOptimized = false;
		}
		else if (CheckArrayDim(FField::NameOffset + 0x8))
		{
			FProperty::ArrayDimOffset = FField::NameOffset + 0x8;
			FName::IsOptimized = true;
		}
		else
		{
			FProperty::ArrayDimOffset = FField::NameOffset + (FName::IsOptimized ? 0x8 : 0x10);
			UE_LOG("Could not detect FProperty::ArrayDim, assuming 0x%X.", FProperty::ArrayDimOffset);
		}

		FProperty::FPropertySize = FProperty::ArrayDimOffset + 0x40;
	}
	else
	{
		UE_LOG("Could not detect the FField layout, using the hardcoded offsets. The dump will probably fail.");
	}

	if (VectorX && Core.Transform && (Core.Quat || Core.Vector))
	{
		int Detected = 0;
		bool bConsistent = true;

		auto Prop = ReadPtr(Core.Transform + UStruct::ChildPropertiesOffset);

		for (int i = 0; Prop && i < 16; i++, Prop = ReadPtr(Prop + FField::NextOffset))
		{
			for (int Offset = FProperty::ArrayDimOffset + 0x20; Offset < 0x100; Offset += 8)
			{
				auto Ptr = ReadPtr(Prop + Offset);

				if (Ptr && (Ptr == Core.Quat || Ptr == Core.Vector))
				{
					if (Detected && Detected != Offset)
						bConsistent = false;

					Detected = Offset;
					break;
				}
			}
		}

		if (Detected && bConsistent)
			FProperty::FPropertySize = Detected;
		else
			UE_LOG("Could not detect sizeof(FProperty), assuming 0x%X.", FProperty::FPropertySize);
	}

	if (Core.Enum)
	{
		std::vector<uintptr_t> Enums;

		for (int i = 0; i < ObjObjects::Num() && Enums.size() < 64; i++)
		{
			auto Obj = ObjObjects::GetObjectByIndex(i).GetAddress();

			if (Obj && ReadPtr(Obj + UObject::ClassOffset) == Core.Enum)
				Enums.push_back(Obj);
		}

		int BestOffset = 0;
		int BestScore = 0;

		for (int Offset = 0x28; Offset <= 0x90; Offset += 8)
		{
			int Score = 0;

			for (auto Enum : Enums)
			{
				struct { uintptr_t Data; int32_t Num; int32_t Max; } Array;

				if (!Memory::Read(Enum + Offset, Array) || Array.Num <= 0 || Array.Num > Array.Max || Array.Max > 0x10000)
					continue;

				if (!Memory::IsReadable(Array.Data, Array.Num * 0x10))
					continue;

				auto FirstName = NameAt(Array.Data);
				auto LastName = NameAt(Array.Data + (Array.Num - 1) * 0x10);

				if (!IsSensibleName(FirstName) || !IsSensibleName(LastName))
					continue;

				if (LastName.ends_with("_MAX") || FirstName.find("::") != std::string::npos)
					Score++;
			}

			if (Score > BestScore)
			{
				BestScore = Score;
				BestOffset = Offset;
			}
		}

		if (BestScore >= 3)
			UEnum::NamesOffset = BestOffset;
		else
			UE_LOG("Could not detect UEnum::Names, assuming 0x%X.", UEnum::NamesOffset);
	}

	if (Core.Vector)
	{
		std::string FieldNames;

		auto Field = UStruct(Core.Vector).ChildProperties();

		for (int i = 0; Field && i < 8 && Memory::IsReadable(Field.GetAddress(), FProperty::FPropertySize); i++)
		{
			FieldNames += Field.GetFName().ToString() + " ";
			Field = Field.GetNextProperty();
		}

		UE_LOG("FVector properties: %s", FieldNames.c_str());
	}

	return true;
}

void OffsetFinder::LogOffsets()
{
	UE_LOG("Offsets:");
	UE_LOG("  FUObjectItem size:        0x%X", ObjObjects::ItemSize);
	UE_LOG("  UObject::ClassPrivate:    0x%X", UObject::ClassOffset);
	UE_LOG("  UObject::NamePrivate:     0x%X", UObject::NameOffset);
	UE_LOG("  UObject::OuterPrivate:    0x%X", UObject::OuterOffset);
	UE_LOG("  UStruct::SuperStruct:     0x%X", UStruct::SuperOffset);
	UE_LOG("  UStruct::ChildProperties: 0x%X", UStruct::ChildPropertiesOffset);
	UE_LOG("  UEnum::Names:             0x%X", UEnum::NamesOffset);
	UE_LOG("  FField::ClassPrivate:     0x%X", FField::ClassOffset);
	UE_LOG("  FField::Next:             0x%X", FField::NextOffset);
	UE_LOG("  FField::NamePrivate:      0x%X", FField::NameOffset);
	UE_LOG("  FProperty::ArrayDim:      0x%X", FProperty::ArrayDimOffset);
	UE_LOG("  sizeof(FProperty):        0x%X", FProperty::FPropertySize);
	UE_LOG("  FName outline number:     %s", FName::IsOptimized ? "yes" : "no");
}
