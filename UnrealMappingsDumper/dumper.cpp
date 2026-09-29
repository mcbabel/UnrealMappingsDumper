#include "pch.h"

#include "dumper.h"
#include "writer.h"
#ifdef _WIN32
#include "oodle.h"
#endif
#include "app.h"

namespace
{
	constexpr EUsmapVersion UsmapVersion = EUsmapVersion::ExplicitEnumValues;

	constexpr int32_t InvalidNameIndex = -1;

	constexpr int MaxPropertiesPerStruct = 0x4000;

	EPropertyType TypeFromClassName(const std::string& Name)
	{
		static const std::unordered_map<std::string, EPropertyType> Types =
		{
			{ "ByteProperty", EPropertyType::ByteProperty },
			{ "BoolProperty", EPropertyType::BoolProperty },
			{ "IntProperty", EPropertyType::IntProperty },
			{ "Int8Property", EPropertyType::Int8Property },
			{ "Int16Property", EPropertyType::Int16Property },
			{ "Int64Property", EPropertyType::Int64Property },
			{ "UInt16Property", EPropertyType::UInt16Property },
			{ "UInt32Property", EPropertyType::UInt32Property },
			{ "UInt64Property", EPropertyType::UInt64Property },
			{ "FloatProperty", EPropertyType::FloatProperty },
			{ "DoubleProperty", EPropertyType::DoubleProperty },
			{ "LargeWorldCoordinatesRealProperty", EPropertyType::DoubleProperty },
			{ "NameProperty", EPropertyType::NameProperty },
			{ "StrProperty", EPropertyType::StrProperty },
			{ "TextProperty", EPropertyType::TextProperty },
			{ "ObjectProperty", EPropertyType::ObjectProperty },
			{ "ClassProperty", EPropertyType::ObjectProperty },
			{ "ObjectPtrProperty", EPropertyType::ObjectProperty },
			{ "ClassPtrProperty", EPropertyType::ObjectProperty },
			{ "WeakObjectProperty", EPropertyType::WeakObjectProperty },
			{ "LazyObjectProperty", EPropertyType::LazyObjectProperty },
			{ "SoftObjectProperty", EPropertyType::SoftObjectProperty },
			{ "SoftClassProperty", EPropertyType::SoftObjectProperty },
			{ "AssetObjectProperty", EPropertyType::AssetObjectProperty },
			{ "AssetClassProperty", EPropertyType::AssetObjectProperty },
			{ "InterfaceProperty", EPropertyType::InterfaceProperty },
			{ "StructProperty", EPropertyType::StructProperty },
			{ "ArrayProperty", EPropertyType::ArrayProperty },
			{ "MapProperty", EPropertyType::MapProperty },
			{ "SetProperty", EPropertyType::SetProperty },
			{ "EnumProperty", EPropertyType::EnumProperty },
			{ "DelegateProperty", EPropertyType::DelegateProperty },
			{ "MulticastDelegateProperty", EPropertyType::MulticastDelegateProperty },
			{ "MulticastInlineDelegateProperty", EPropertyType::MulticastDelegateProperty },
			{ "MulticastSparseDelegateProperty", EPropertyType::MulticastDelegateProperty },
			{ "FieldPathProperty", EPropertyType::FieldPathProperty },
			{ "OptionalProperty", EPropertyType::OptionalProperty },
			{ "Utf8StrProperty", EPropertyType::Utf8StrProperty },
			{ "AnsiStrProperty", EPropertyType::AnsiStrProperty },
		};

		auto It = Types.find(Name);
		return It != Types.end() ? It->second : EPropertyType::Unknown;
	}

	EPropertyType TypeFromCastFlags(uint64_t Flags)
	{
		static const std::pair<uint64_t, EPropertyType> Types[] =
		{
			{ CASTCLASS_FObjectProperty | CASTCLASS_FClassProperty | CASTCLASS_FObjectPtrProperty | CASTCLASS_FClassPtrProperty, EPropertyType::ObjectProperty },
			{ CASTCLASS_FStructProperty, EPropertyType::StructProperty },
			{ CASTCLASS_FByteProperty, EPropertyType::ByteProperty },
			{ CASTCLASS_FInt8Property, EPropertyType::Int8Property },
			{ CASTCLASS_FInt16Property, EPropertyType::Int16Property },
			{ CASTCLASS_FIntProperty, EPropertyType::IntProperty },
			{ CASTCLASS_FInt64Property, EPropertyType::Int64Property },
			{ CASTCLASS_FUInt16Property, EPropertyType::UInt16Property },
			{ CASTCLASS_FUInt32Property, EPropertyType::UInt32Property },
			{ CASTCLASS_FUInt64Property, EPropertyType::UInt64Property },
			{ CASTCLASS_FArrayProperty, EPropertyType::ArrayProperty },
			{ CASTCLASS_FFloatProperty, EPropertyType::FloatProperty },
			{ CASTCLASS_FDoubleProperty | CASTCLASS_FLargeWorldCoordinatesRealProperty, EPropertyType::DoubleProperty },
			{ CASTCLASS_FBoolProperty, EPropertyType::BoolProperty },
			{ CASTCLASS_FStrProperty, EPropertyType::StrProperty },
			{ CASTCLASS_FNameProperty, EPropertyType::NameProperty },
			{ CASTCLASS_FTextProperty, EPropertyType::TextProperty },
			{ CASTCLASS_FEnumProperty, EPropertyType::EnumProperty },
			{ CASTCLASS_FInterfaceProperty, EPropertyType::InterfaceProperty },
			{ CASTCLASS_FMapProperty, EPropertyType::MapProperty },
			{ CASTCLASS_FMulticastDelegateProperty | CASTCLASS_FMulticastInlineDelegateProperty | CASTCLASS_FMulticastSparseDelegateProperty, EPropertyType::MulticastDelegateProperty },
			{ CASTCLASS_FDelegateProperty, EPropertyType::DelegateProperty },
			{ CASTCLASS_FSoftObjectProperty | CASTCLASS_FSoftClassProperty, EPropertyType::SoftObjectProperty },
			{ CASTCLASS_FWeakObjectProperty, EPropertyType::WeakObjectProperty },
			{ CASTCLASS_FLazyObjectProperty, EPropertyType::LazyObjectProperty },
			{ CASTCLASS_FSetProperty, EPropertyType::SetProperty },
			{ CASTCLASS_FFieldPathProperty, EPropertyType::FieldPathProperty },
		};

		for (auto& [Mask, Type] : Types)
		{
			if (Flags & Mask)
				return Type;
		}

		return EPropertyType::Unknown;
	}

	EPropertyType GetPropertyType(FProperty Prop)
	{
		static std::unordered_map<uintptr_t, EPropertyType> Cache;

		auto Class = Prop.GetClass();

		if (!Class)
			return EPropertyType::Unknown;

		EPropertyType Type;

		if (auto It = Cache.find(Class.GetAddress()); It != Cache.end())
		{
			Type = It->second;
		}
		else
		{
			auto ClassName = Class.GetName();

			Type = TypeFromClassName(ClassName);

			if (Type == EPropertyType::Unknown)
			{
				Type = TypeFromCastFlags(Class.GetCastFlags());

				UE_LOG("Property class \"%s\" isn't known, %s.", ClassName.c_str(),
					Type == EPropertyType::Unknown ? "writing it as unknown" : "using the type it inherits from");
			}

			Cache[Class.GetAddress()] = Type;
		}

		if (Type == EPropertyType::ByteProperty && Prop.Cast<FByteProperty>().GetEnum())
			return EPropertyType::EnumAsByteProperty;

		return Type;
	}

	struct FNameTable
	{
		phmap::parallel_flat_hash_map<FName, int32_t> Indices;
		std::vector<FName> Names;

		void Add(FName Name)
		{
			if (Indices.try_emplace(Name, static_cast<int32_t>(Names.size())).second)
				Names.push_back(Name);
		}

		int32_t Get(FName Name) const
		{
			auto It = Indices.find(Name);
			return It != Indices.end() ? It->second : InvalidNameIndex;
		}
	};

	void CollectPropertyNames(FProperty Prop, FNameTable& Table, int Depth = 0)
	{
		if (!Prop || Depth > 8)
			return;

		switch (GetPropertyType(Prop))
		{
		case EPropertyType::EnumProperty:
		{
			auto EnumProp = Prop.Cast<FEnumProperty>();

			if (EnumProp.GetEnum())
				Table.Add(EnumProp.GetEnum().GetFName());

			CollectPropertyNames(EnumProp.GetUnderlying(), Table, Depth + 1);
			break;
		}
		case EPropertyType::EnumAsByteProperty:
		{
			Table.Add(Prop.Cast<FByteProperty>().GetEnum().GetFName());
			break;
		}
		case EPropertyType::StructProperty:
		{
			if (auto Struct = Prop.Cast<FStructProperty>().GetStruct())
				Table.Add(Struct.GetFName());

			break;
		}
		case EPropertyType::SetProperty:
		case EPropertyType::ArrayProperty:
		case EPropertyType::OptionalProperty:
		{
			CollectPropertyNames(Prop.Cast<FArrayProperty>().GetInner(), Table, Depth + 1);
			break;
		}
		case EPropertyType::MapProperty:
		{
			CollectPropertyNames(Prop.Cast<FMapProperty>().GetKey(), Table, Depth + 1);
			CollectPropertyNames(Prop.Cast<FMapProperty>().GetValue(), Table, Depth + 1);
			break;
		}
		default:
			break;
		}
	}

	void WriteProperty(StreamWriter& Buffer, const FNameTable& Table, FProperty Prop, int Depth = 0)
	{
		auto Type = Prop && Depth <= 8 ? GetPropertyType(Prop) : EPropertyType::Unknown;

		Buffer.Write(Type == EPropertyType::EnumAsByteProperty ? EPropertyType::EnumProperty : Type);

		switch (Type)
		{
		case EPropertyType::EnumProperty:
		{
			auto EnumProp = Prop.Cast<FEnumProperty>();
			auto Enum = EnumProp.GetEnum();

			WriteProperty(Buffer, Table, EnumProp.GetUnderlying(), Depth + 1);
			Buffer.Write<int32_t>(Enum ? Table.Get(Enum.GetFName()) : InvalidNameIndex);
			break;
		}
		case EPropertyType::EnumAsByteProperty:
		{
			Buffer.Write(EPropertyType::ByteProperty);
			Buffer.Write<int32_t>(Table.Get(Prop.Cast<FByteProperty>().GetEnum().GetFName()));
			break;
		}
		case EPropertyType::StructProperty:
		{
			auto Struct = Prop.Cast<FStructProperty>().GetStruct();

			Buffer.Write<int32_t>(Struct ? Table.Get(Struct.GetFName()) : InvalidNameIndex);
			break;
		}
		case EPropertyType::SetProperty:
		case EPropertyType::ArrayProperty:
		case EPropertyType::OptionalProperty:
		{
			WriteProperty(Buffer, Table, Prop.Cast<FArrayProperty>().GetInner(), Depth + 1);
			break;
		}
		case EPropertyType::MapProperty:
		{
			WriteProperty(Buffer, Table, Prop.Cast<FMapProperty>().GetKey(), Depth + 1);
			WriteProperty(Buffer, Table, Prop.Cast<FMapProperty>().GetValue(), Depth + 1);
			break;
		}
		default:
			break;
		}
	}

	template <typename Callback>
	void ForEachProperty(UStruct Struct, Callback&& Func)
	{
		auto Prop = Struct.ChildProperties();

		for (int i = 0; Prop && i < MaxPropertiesPerStruct; i++)
		{
			Func(Prop);
			Prop = Prop.GetNextProperty();
		}
	}

	std::string_view StripEnumPrefix(std::string_view Name)
	{
		auto Find = Name.find("::");
		return Find != std::string_view::npos ? Name.substr(Find + 2) : Name;
	}
}

bool Dumper::Run(ECompressionMethod CompressionMethod, const std::filesystem::path& OutputPath)
{
	auto ClassClass = UClass::StaticClass();
	auto ScriptStructClass = UScriptStruct::StaticClass();
	auto EnumClass = UEnum::StaticClass();

	if (!ClassClass || !ScriptStructClass || !EnumClass)
	{
		UE_LOG("Could not find the Class, ScriptStruct and Enum classes. The object paths can't be resolved, the Outer offset is probably wrong.");
		return false;
	}

	FNameTable NameTable;

	std::vector<UEnum> Enums;
	std::vector<UStruct> Structs;

	ObjObjects::ForEach([&](UObject Object)
		{
			auto Class = Object.Class();

			if (Class == ClassClass || Class == ScriptStructClass)
			{
				Structs.push_back(Object.Cast<UStruct>());
			}
			else if (Class == EnumClass)
			{
				Enums.push_back(Object.Cast<UEnum>());
			}
		});

	UE_LOG("Found %zu classes/structs and %zu enums in %d objects.", Structs.size(), Enums.size(), ObjObjects::Num());

	if (Structs.empty())
	{
		UE_LOG("No classes or structs found, not writing an empty mappings file.");
		return false;
	}

	for (auto Enum : Enums)
	{
		NameTable.Add(Enum.GetFName());

		for (auto& [Key, Value] : Enum.Names())
			NameTable.Add(Key);
	}

	size_t PropertyCount = 0;

	for (auto Struct : Structs)
	{
		NameTable.Add(Struct.GetFName());

		if (auto Super = Struct.Super())
			NameTable.Add(Super.GetFName());

		ForEachProperty(Struct, [&](FProperty Prop)
			{
				NameTable.Add(Prop.GetFName());
				CollectPropertyNames(Prop, NameTable);
				PropertyCount++;
			});
	}

	UE_LOG("Found %zu properties and %zu unique names.", PropertyCount, NameTable.Names.size());

	StreamWriter Buffer;

	Buffer.Write<uint32_t>(static_cast<uint32_t>(NameTable.Names.size()));

	size_t UnresolvedNames = 0;

	for (auto& N : NameTable.Names)
	{
		std::string Utf8;

		if (!Names::TryToString(N, Utf8))
			UnresolvedNames++;

		auto View = StripEnumPrefix(Utf8);

		if (View.size() > UINT16_MAX)
			View = View.substr(0, UINT16_MAX);

		Buffer.Write<uint16_t>(static_cast<uint16_t>(View.size()));
		Buffer.WriteString(View);
	}

	if (UnresolvedNames)
		UE_LOG("Warning: %zu names could not be resolved and were written empty.", UnresolvedNames);

	Buffer.Write<uint32_t>(static_cast<uint32_t>(Enums.size()));

	for (auto Enum : Enums)
	{
		Buffer.Write<int32_t>(NameTable.Get(Enum.GetFName()));

		auto EnumNames = Enum.Names();
		auto Count = static_cast<uint16_t>(std::min<size_t>(EnumNames.size(), UINT16_MAX));

		Buffer.Write<uint16_t>(Count);

		for (int i = 0; i < Count; i++)
		{
			Buffer.Write<int64_t>(EnumNames[i].second);
			Buffer.Write<int32_t>(NameTable.Get(EnumNames[i].first));
		}
	}

	Buffer.Write<uint32_t>(static_cast<uint32_t>(Structs.size()));

	for (auto Struct : Structs)
	{
		Buffer.Write<int32_t>(NameTable.Get(Struct.GetFName()));
		Buffer.Write<int32_t>(Struct.Super() ? NameTable.Get(Struct.Super().GetFName()) : InvalidNameIndex);

		std::vector<FProperty> Properties;
		uint16_t PropCount = 0;

		ForEachProperty(Struct, [&](FProperty Prop)
			{
				Properties.push_back(Prop);
				PropCount += static_cast<uint8_t>(Prop.GetArrayDim());
			});

		Buffer.Write<uint16_t>(PropCount);
		Buffer.Write<uint16_t>(static_cast<uint16_t>(Properties.size()));

		uint16_t Index = 0;

		for (auto Prop : Properties)
		{
			auto ArrayDim = static_cast<uint8_t>(Prop.GetArrayDim());

			Buffer.Write<uint16_t>(Index);
			Buffer.Write<uint8_t>(ArrayDim);
			Buffer.Write<int32_t>(NameTable.Get(Prop.GetFName()));

			WriteProperty(Buffer, NameTable, Prop);

			Index += ArrayDim;
		}
	}

	auto Uncompressed = Buffer.GetBuffer().str();
	std::vector<uint8_t> UsmapData;

	if (CompressionMethod == ECompressionMethod::Oodle)
	{
#ifdef _WIN32
		UsmapData = Oodle::Compress(Uncompressed);
#endif

		if (UsmapData.empty())
		{
			UE_LOG("Oodle isn't available (place oo2core_9_win64.dll next to the dumper), writing the mappings uncompressed.");
			CompressionMethod = ECompressionMethod::None;
		}
	}
	else if (CompressionMethod != ECompressionMethod::None)
	{
		UE_LOG("Compression method %d isn't supported, writing the mappings uncompressed.", static_cast<int>(CompressionMethod));
		CompressionMethod = ECompressionMethod::None;
	}

	if (CompressionMethod == ECompressionMethod::None)
		UsmapData.assign(Uncompressed.begin(), Uncompressed.end());

	FileWriter FileOutput(OutputPath);

	if (!FileOutput.IsOpen())
	{
		UE_LOG("Could not open %s for writing.", Memory::PathToUtf8(OutputPath).c_str());
		return false;
	}

	FileOutput.Write<uint16_t>(0x30C4); //magic
	FileOutput.Write(UsmapVersion); //version
	FileOutput.Write<int32_t>(0);
	FileOutput.Write(CompressionMethod); //compression
	FileOutput.Write<uint32_t>(static_cast<uint32_t>(UsmapData.size())); //compressed size
	FileOutput.Write<uint32_t>(static_cast<uint32_t>(Uncompressed.size())); //decompressed size

	FileOutput.Write(UsmapData.data(), UsmapData.size());

	if (!FileOutput.Close())
	{
		UE_LOG("Failed to write %s.", Memory::PathToUtf8(OutputPath).c_str());
		return false;
	}

	UE_LOG("Wrote %s (%zu bytes)", Memory::PathToUtf8(OutputPath).c_str(), UsmapData.size() + 16);

	return true;
}
