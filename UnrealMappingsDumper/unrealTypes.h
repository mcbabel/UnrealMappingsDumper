#pragma once

#include "unrealEnums.h"
#include "unrealContainers.h"
#include "names.h"
#include "memory.h"

#define DECLARE_STATIC_CLASS(PATH) \
	static class UClass StaticClass(); \

class FName
{
private:

	uint32_t ComparisonIndex = 0;

	uint32_t Number = 0;

public:

	static inline bool IsOptimized = false;

	FName() = default;

	explicit FName(uint32_t InIndex, uint32_t InNumber = 0) : ComparisonIndex(InIndex), Number(InNumber)
	{
	}

	FORCEINLINE uint32_t GetComparisonIndex() const
	{
		return ComparisonIndex;
	}

	FORCEINLINE uint32_t GetNumber() const
	{
		return IsOptimized ? 0 : Number;
	}

	bool operator==(const FName& Other) const
	{
		return ComparisonIndex == Other.ComparisonIndex && GetNumber() == Other.GetNumber();
	}

	friend size_t hash_value(const FName& p)
	{
		return (size_t(p.GetNumber()) << 32) | p.ComparisonIndex;
	}

	std::string ToString() const
	{
		return Names::ToString(*this);
	}
};

class FRemoteObject
{
protected:

	uintptr_t Address = 0;

	template <typename T>
	FORCEINLINE T Get(int Offset) const
	{
		return Memory::Fetch<T>(Address + Offset);
	}

public:

	FRemoteObject() = default;

	explicit FRemoteObject(uintptr_t InAddress) : Address(InAddress)
	{
	}

	FORCEINLINE uintptr_t GetAddress() const
	{
		return Address;
	}

	explicit operator bool() const
	{
		return Address != 0;
	}

	bool operator==(const FRemoteObject& Other) const
	{
		return Address == Other.Address;
	}

	template <typename T>
	FORCEINLINE T Cast() const
	{
		return T(Address);
	}
};

class UObject : public FRemoteObject
{
private:

	static inline int IndexOffset = 0x0C;
	static inline int ClassOffset = 0x10;
	static inline int NameOffset = 0x18;
	static inline int OuterOffset = 0x20;

	friend struct IUnrealVersion;
	friend struct OffsetFinder;

public:

	using FRemoteObject::FRemoteObject;

	void GetPathName(std::string& Result, UObject StopOuter = UObject()) const
	{
		if (*this == StopOuter)
		{
			Result += "None";
			return;
		}

		auto OuterObj = Outer();

		if (OuterObj && OuterObj != StopOuter)
		{
			OuterObj.GetPathName(Result, StopOuter);
			Result += ".";
		}

		Result += GetName();
	}

	FORCEINLINE int32_t GetIndex() const
	{
		return Get<int32_t>(IndexOffset);
	}

	FORCEINLINE FName GetFName() const
	{
		return Get<FName>(NameOffset);
	}

	FORCEINLINE std::string GetName() const
	{
		return GetFName().ToString();
	}

	FORCEINLINE std::string GetPath() const
	{
		std::string Ret;

		GetPathName(Ret);

		return Ret;
	}

	FORCEINLINE UObject ClassObject() const
	{
		return UObject(Get<uintptr_t>(ClassOffset));
	}

	class UClass Class() const;

	FORCEINLINE UObject Outer() const
	{
		return UObject(Get<uintptr_t>(OuterOffset));
	}

	bool IsA(const class UClass& Cls) const;
};

class ObjObjects
{
	enum
	{
		NumElementsPerChunk = 64 * 1024,
	};

	struct FHeader
	{
		uintptr_t Objects;
		uintptr_t PreAllocatedObjects;
		int32_t MaxElements;
		int32_t NumElements;
		int32_t MaxChunks;
		int32_t NumChunks;
	};

	static inline uintptr_t Inst = 0;

	static inline int ItemSize = 0x18;

	friend struct OffsetFinder;

	static FHeader GetHeader()
	{
		return Memory::Fetch<FHeader>(Inst);
	}

	static UObject GetObjectByIndex(const FHeader& Header, int Index)
	{
		int ChunkIndex = Index / NumElementsPerChunk;
		int WithinChunkIndex = Index % NumElementsPerChunk;

		if (Index < Header.NumElements &&
			Index >= 0 &&
			ChunkIndex < Header.NumChunks &&
			Index < Header.MaxElements)
		{
			auto Chunk = Memory::Fetch<uintptr_t>(Header.Objects + ChunkIndex * sizeof(uintptr_t));

			if (Chunk)
				return UObject(Memory::Fetch<uintptr_t>(Chunk + static_cast<uintptr_t>(WithinChunkIndex) * ItemSize));
		}

		return UObject();
	}

public:

	static UObject GetObjectByIndex(int Index)
	{
		return GetObjectByIndex(GetHeader(), Index);
	}

	static FORCEINLINE int Num()
	{
		return GetHeader().NumElements;
	}

	static FORCEINLINE uintptr_t GetInstance()
	{
		return Inst;
	}

	static void SetInstance(uintptr_t Val)
	{
		if (Val)
			Inst = Val;
	}

	template <typename Callback>
	static void ForEach(Callback&& Action)
	{
		auto Header = GetHeader();

		for (int i = 0; i < Header.NumElements; i++)
		{
			auto Obj = GetObjectByIndex(Header, i);

			if (!Obj) continue;

			Action(Obj);
		}
	}

	static UObject FindObjectByName(std::string_view ObjectName, std::string_view ClassName = {})
	{
		auto Header = GetHeader();

		for (int i = 0; i < Header.NumElements; i++)
		{
			auto Obj = GetObjectByIndex(Header, i);

			if (!Obj || Obj.GetName() != ObjectName)
				continue;

			if (!ClassName.empty() && (!Obj.ClassObject() || Obj.ClassObject().GetName() != ClassName))
				continue;

			return Obj;
		}

		return UObject();
	}

	static UObject FindObject(const std::string& FullName)
	{
		auto ShortName = FullName.substr(FullName.find_last_of('.') + 1);

		auto Header = GetHeader();

		for (int i = 0; i < Header.NumElements; i++)
		{
			auto Obj = GetObjectByIndex(Header, i);

			if (!Obj || Obj.GetName() != ShortName)
				continue;

			if (Obj.GetPath() == FullName)
				return Obj;
		}

		return UObject();
	}
};

class FProperty;

class UStruct : public UObject
{
private:

	static inline int SuperOffset = 0x40;
	static inline int ChildPropertiesOffset = 0x50;

	friend struct IUnrealVersion;
	friend struct OffsetFinder;

public:

	using UObject::UObject;

	FORCEINLINE UStruct Super() const
	{
		return UStruct(Get<uintptr_t>(SuperOffset));
	}

	FORCEINLINE int32_t PropertiesSize() const
	{
		return Get<int32_t>(ChildPropertiesOffset + sizeof(void*));
	}

	FProperty ChildProperties() const;

	bool IsChildOf(const UStruct& Other) const
	{
		auto Current = *this;

		for (int i = 0; Current && i < 256; i++, Current = Current.Super())
		{
			if (Current == Other)
				return true;
		}

		return false;
	}
};

class UClass : public UStruct
{
public:

	using UStruct::UStruct;

	DECLARE_STATIC_CLASS("/Script/CoreUObject.Class");
};

class UScriptStruct : public UStruct
{
public:

	using UStruct::UStruct;

	DECLARE_STATIC_CLASS("/Script/CoreUObject.ScriptStruct");
};

inline UClass UObject::Class() const
{
	return UClass(Get<uintptr_t>(ClassOffset));
}

inline bool UObject::IsA(const UClass& Cls) const
{
	return Class() && Class().IsChildOf(Cls);
}

class FFieldClass : public FRemoteObject
{
public:

	using FRemoteObject::FRemoteObject;

	FORCEINLINE FName GetFName() const
	{
		return Get<FName>(0);
	}

	FORCEINLINE std::string GetName() const
	{
		return GetFName().ToString();
	}

	FORCEINLINE uint64_t GetId() const
	{
		return Get<uint64_t>(0x08);
	}

	FORCEINLINE uint64_t GetCastFlags() const
	{
		return Get<uint64_t>(0x10);
	}
};

class FField : public FRemoteObject
{
protected:

	static inline int ClassOffset = 0x08;
	static inline int NextOffset = 0x20;
	static inline int NameOffset = 0x28;

	friend struct IUnrealVersion;
	friend struct OffsetFinder;

public:

	using FRemoteObject::FRemoteObject;

	FORCEINLINE FName GetFName() const
	{
		return Get<FName>(NameOffset);
	}

	FORCEINLINE FField GetNext() const
	{
		return FField(Get<uintptr_t>(NextOffset));
	}

	FORCEINLINE FFieldClass GetClass() const
	{
		return FFieldClass(Get<uintptr_t>(ClassOffset));
	}
};

class FProperty : public FField
{
protected:

	static inline int ArrayDimOffset = 0x38;
	static inline int FPropertySize = 0x78;

	friend struct IUnrealVersion;
	friend struct OffsetFinder;

	FORCEINLINE uintptr_t Member(int Index) const
	{
		return Get<uintptr_t>(FPropertySize + Index * static_cast<int>(sizeof(uintptr_t)));
	}

public:

	using FField::FField;

	FORCEINLINE FProperty GetNextProperty() const
	{
		return FProperty(Get<uintptr_t>(NextOffset));
	}

	FORCEINLINE int32_t GetArrayDim() const
	{
		return Get<int32_t>(ArrayDimOffset);
	}

	FORCEINLINE int32_t GetElementSize() const
	{
		return Get<int32_t>(ArrayDimOffset + sizeof(int32_t));
	}
};

inline FProperty UStruct::ChildProperties() const
{
	return FProperty(Get<uintptr_t>(ChildPropertiesOffset));
}

class UEnum : public UObject
{
	static inline int NamesOffset = 0x40;

	friend struct IUnrealVersion;
	friend struct OffsetFinder;

public:

	using UObject::UObject;

	std::vector<std::pair<FName, int64_t>> Names() const
	{
		struct FEntry
		{
			FName Key;
			int64_t Value;
		};

		auto Data = Get<uintptr_t>(NamesOffset);
		auto Num = Get<int32_t>(NamesOffset + sizeof(uintptr_t));

		std::vector<std::pair<FName, int64_t>> Ret;

		if (!Data || Num <= 0 || Num > 0x10000)
			return Ret;

		std::vector<FEntry> Entries(Num);

		if (!Memory::ReadRaw(Data, Entries.data(), Entries.size() * sizeof(FEntry)))
			return Ret;

		for (auto& Entry : Entries)
			Ret.emplace_back(Entry.Key, Entry.Value);

		return Ret;
	}

	DECLARE_STATIC_CLASS("/Script/CoreUObject.Enum");
};

class FStructProperty : public FProperty
{
public:

	using FProperty::FProperty;

	FORCEINLINE UScriptStruct GetStruct() const
	{
		return UScriptStruct(Member(0));
	}
};

class FByteProperty : public FProperty
{
public:

	using FProperty::FProperty;

	FORCEINLINE UEnum GetEnum() const
	{
		return UEnum(Member(0));
	}
};

class FArrayProperty : public FProperty
{
public:

	using FProperty::FProperty;

	FORCEINLINE FProperty GetInner() const
	{
		return FProperty(Member(0));
	}
};

class FMapProperty : public FProperty
{
public:

	using FProperty::FProperty;

	FORCEINLINE FProperty GetKey() const
	{
		return FProperty(Member(0));
	}

	FORCEINLINE FProperty GetValue() const
	{
		return FProperty(Member(1));
	}
};

class FEnumProperty : public FProperty
{
public:

	using FProperty::FProperty;

	FORCEINLINE FProperty GetUnderlying() const
	{
		return FProperty(Member(0));
	}

	FORCEINLINE UEnum GetEnum() const
	{
		return UEnum(Member(1));
	}
};

inline UClass UClass::StaticClass()
{
	static auto Inst = ObjObjects::FindObject("/Script/CoreUObject.Class").Cast<UClass>();
	return Inst;
}

inline UClass UScriptStruct::StaticClass()
{
	static auto Inst = ObjObjects::FindObject("/Script/CoreUObject.ScriptStruct").Cast<UClass>();
	return Inst;
}

inline UClass UEnum::StaticClass()
{
	static auto Inst = ObjObjects::FindObject("/Script/CoreUObject.Enum").Cast<UClass>();
	return Inst;
}
