#pragma once

#include "app.h"
#include "unrealTypes.h"
#include "scanning.h"

/*
* The idea here is to make something that can easily be overriden for engine or game versions with different types
* that need to be overriden or handled differently.
*/

struct OffsetFinder
{
	static bool FindGObjects(const std::vector<std::shared_ptr<IScanObject>>& Scans);

	static bool FindOffsets();

	static void LogOffsets();
};

struct IUnrealVersion
{
public:

	template <typename Version>
	static bool InitTypes()
	{
		ApplyDefaults<Version>();

		if (!OffsetFinder::FindGObjects(Version::GetGObjectsPatterns()))
		{
			UE_LOG("Could not find GObjects. Try overriding it or adding the correct sig for it.");
			return false;
		}

		if (!Names::Init(Version::GetFNameStringPatterns(), Version::GetNamePoolPatterns()))
		{
			UE_LOG("Could not find FName::ToString or the name pool. Try overriding it or adding the correct sig for it.");
			return false;
		}

		if (!OffsetFinder::FindOffsets())
			return false;

		OffsetFinder::LogOffsets();

		return true;
	}

private:

	template <typename Version>
	static void ApplyDefaults()
	{
		using UObjectImpl = Version::Offsets::UObject;
		using UStructImpl = Version::Offsets::UStruct;

		UObject::NameOffset = UObjectImpl::NameOffset;
		UObject::ClassOffset = UObjectImpl::ClassOffset;
		UObject::OuterOffset = UObjectImpl::OuterOffset;

		UStruct::SuperOffset = UStructImpl::SuperOffset;
		UStruct::ChildPropertiesOffset = UStructImpl::ChildPropertiesOffset;

		FName::IsOptimized = Version::HasOptimizedFName;
		FProperty::FPropertySize = Version::FPropertySize;

		FProperty::ArrayDimOffset = Version::HasOptimizedFName ? 0x30 : 0x38;
	}
};

/*
* Yes, it's true, UObject should pretty much always be the same across all games,
* however there are some games that use a custom UObject, so should they ever need mappings,
* this design pattern makes it easier to override the offsets.
*/

struct UnrealVersionBase : IUnrealVersion
{
	static constexpr int FPropertySize = 0x78;
	static constexpr bool HasOptimizedFName = false;

	struct Offsets
	{
		struct UObject
		{
			static constexpr int NameOffset = 0x18;
			static constexpr int ClassOffset = 0x10;
			static constexpr int OuterOffset = 0x20;
		};

		struct UStruct
		{
			static constexpr int SuperOffset = 0x40;
			static constexpr int ChildPropertiesOffset = 0x50;
		};
	};

	static std::vector<std::shared_ptr<IScanObject>> GetFNameStringPatterns()
	{
		return
		{
			std::make_shared<PatternScanObject>("E8 ? ? ? ? 83 7D C8 00 48 8D 15 ? ? ? ? 0F 5A DE", 1, true),
			std::make_shared<PatternScanObject>("E8 ? ? ? ? 48 8B 4C 24 ? 8B FD 48 85 C9", 1, true),// 4.12 - 5.0 EA
			std::make_shared<PatternScanObject>("E8 ? ? ? ? BD 01 00 00 00 41 39 6E ? 0F 8E", 1, true),// 4.25+ Backup
			std::make_shared<StringRefScanObject<std::wstring>>(
				L"%s %s SetTimer passed a negative or zero time. The associated timer may fail to be created/fire! If using InitialStartDelayVariance, be sure it is smaller than (Time + InitialStartDelay).",
				true, 1, true, 0xE8),
			std::make_shared<StringRefCallScanObject>(L"ForwardShadingQuality_", std::vector<std::string>
			{
				"48 8D ? ? 48 8D ? ? E8",
				"48 8D ? ? ? 48 8D ? ? E8",
				"48 8D ? ? 49 8B ? E8",
				"48 8D ? ? ? 49 8B ? E8",
				"48 8D ? ? 48 8B ? E8",
				"48 8D ? ? ? 48 8B ? E8",
			})
		};
	}

	static std::vector<std::shared_ptr<IScanObject>> GetNamePoolPatterns()
	{
		return
		{
			std::make_shared<PatternScanObject>("48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05 ? ? ? ? 01", 3, true),
			std::make_shared<PatternScanObject>("48 8D 05 ? ? ? ? EB ? 48 8D 0D ? ? ? ? E8 ? ? ? ? C6 05", 3, true),
		};
	}

	static std::vector<std::shared_ptr<IScanObject>> GetGObjectsPatterns()
	{
		return
		{
			std::make_shared<PatternScanObject>("48 8B 05 ? ? ? ? 48 8B 0C C8 48 8D 04 D1", 3, true),
			std::make_shared<PatternScanObject>("48 8B 05 ? ? ? ? 48 8B 0C C8", 3, true),
			std::make_shared<PatternScanObject>("48 89 05 ? ? ? ? E8 ? ? ? ? ? ? ? 0F 84", 3, true),
			std::make_shared<PatternScanObject>("48 8B 05 ? ? ? ? 48 8B 0C 07 48 85 C9 74 20", 3, true),
			std::make_shared<PatternScanObject>("48 8B 05 ? ? ? ? 48 8B 0C", 3, true),
			std::make_shared<PatternScanObject>("48 03 ? ? ? ? ? ? ? ? ? ? 48 8B 10 48 85 D2 74 07", 3, true),
		};
	}
};

/*
* Use this if the games engine has UE_FNAME_OUTLINE_NUMBER defined as 1
*/
struct Version_OptimizedFName : UnrealVersionBase
{
	static constexpr int FPropertySize = 0x70;
	static constexpr bool HasOptimizedFName = true;
};

struct Version_FortniteLatest : Version_OptimizedFName
{
};
