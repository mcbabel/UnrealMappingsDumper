
#include "../UnrealMappingsDumper/pch.h"

#include "../UnrealMappingsDumper/aes.h"
#include "../UnrealMappingsDumper/aeskey.h"
#include "../UnrealMappingsDumper/app.h"
#include "../UnrealMappingsDumper/cli.h"
#include "../UnrealMappingsDumper/dumper.h"
#include "../UnrealMappingsDumper/memory.h"
#include "../UnrealMappingsDumper/process.h"

#include <map>
#include <thread>

#ifdef _WIN32
#include "../UnrealMappingsDumper/Platform/Windows/LocalBackend.h"
#else
#include "../UnrealMappingsDumper/Platform/Linux/RemoteBackend.h"
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace
{
	struct FLayout
	{
		const char* Name;
		int FieldVariantSize;
		bool bOptimizedNames;
		int ItemSize;
		int StructShift;
		int EnumNamesOffset;
	};

	constexpr FLayout Layouts[] =
	{
		{ "classic", 16, false, 0x18, 0, 0x40 },
		{ "newfield", 8, false, 0x18, 0, 0x40 },
		{ "optimized", 8, true, 0x20, 8, 0x48 },
	};

	const FLayout* Layout = nullptr;

	struct FFakeNamePool
	{
		uint64_t Lock;
		uint32_t CurrentBlock;
		uint32_t CurrentByteCursor;
		uint8_t* Blocks[8192];
	};

	struct FFakeObjectArray
	{
		uint8_t** Objects;
		uint8_t* PreAllocatedObjects;
		int32_t MaxElements;
		int32_t NumElements;
		int32_t MaxChunks;
		int32_t NumChunks;
	};

	FFakeNamePool NamePool;
	FFakeObjectArray ObjectArray;
	uintptr_t FakeVTable[16];

	std::unordered_map<std::string, uint32_t> NameIds;

	uint32_t AddName(const std::string& Name)
	{
		if (auto It = NameIds.find(Name); It != NameIds.end())
			return It->second;

		if (!NamePool.Blocks[0])
			NamePool.Blocks[0] = static_cast<uint8_t*>(calloc(1, 0x20000));

		auto Cursor = NamePool.CurrentByteCursor;
		auto Entry = NamePool.Blocks[0] + Cursor;

		uint16_t Header = static_cast<uint16_t>(Name.size() << 6);
		memcpy(Entry, &Header, 2);
		memcpy(Entry + 2, Name.data(), Name.size());

		uint32_t Id = Cursor / 2;
		NamePool.CurrentByteCursor = (Cursor + 2 + static_cast<uint32_t>(Name.size()) + 1) & ~1u;

		NameIds[Name] = Id;
		return Id;
	}

	uint32_t AddNumberedName(const std::string& Name, uint32_t Number)
	{
		auto BaseId = AddName(Name);

		auto Cursor = NamePool.CurrentByteCursor;
		auto Entry = NamePool.Blocks[0] + Cursor;

		uint16_t Header = 0;
		memcpy(Entry, &Header, 2);
		memcpy(Entry + 2, &BaseId, 4);
		memcpy(Entry + 6, &Number, 4);

		NamePool.CurrentByteCursor = Cursor + 10;
		return Cursor / 2;
	}

	void WriteName(uint8_t* At, const std::string& Name, uint32_t Number = 0)
	{
		if (Layout->bOptimizedNames)
		{
			auto Id = Number ? AddNumberedName(Name, Number) : AddName(Name);
			memcpy(At, &Id, 4);
			return;
		}

		auto Id = AddName(Name);
		memcpy(At, &Id, 4);
		memcpy(At + 4, &Number, 4);
	}

	template <typename T>
	void Put(uint8_t* Base, int Offset, T Value)
	{
		memcpy(Base + Offset, &Value, sizeof(T));
	}

	constexpr int ObjectSize = 0x100;

	std::vector<uint8_t*> AllObjects;

	int SuperOffset() { return 0x40 + Layout->StructShift; }
	int ChildPropertiesOffset() { return SuperOffset() + 0x10; }

	uint8_t* NewObject(const std::string& Name, uint8_t* Class, uint8_t* Outer)
	{
		auto Obj = static_cast<uint8_t*>(calloc(1, ObjectSize));

		Put(Obj, 0x00, &FakeVTable[0]);
		Put<int32_t>(Obj, 0x0C, static_cast<int32_t>(AllObjects.size()));
		Put(Obj, 0x10, Class);
		WriteName(Obj + 0x18, Name);
		Put(Obj, 0x20, Outer);

		AllObjects.push_back(Obj);
		return Obj;
	}

	void SetClass(uint8_t* Obj, uint8_t* Class)
	{
		Put(Obj, 0x10, Class);
	}

	void SetSuper(uint8_t* Struct, uint8_t* Super)
	{
		Put(Struct, SuperOffset(), Super);
	}

	int FieldNextOffset() { return 0x10 + Layout->FieldVariantSize; }
	int FieldNameOffset() { return FieldNextOffset() + 8; }
	int ArrayDimOffset() { return FieldNameOffset() + (Layout->bOptimizedNames ? 8 : 16); }
	int PropertySize() { return ArrayDimOffset() + 0x40; }

	std::unordered_map<std::string, uint8_t*> FieldClasses;

	uint8_t* GetFieldClass(const std::string& Name, uint64_t CastFlags = 0)
	{
		auto& Class = FieldClasses[Name];

		if (!Class)
		{
			Class = static_cast<uint8_t*>(calloc(1, 0x40));
			WriteName(Class, Name);
			Put<uint64_t>(Class, 0x8, CastFlags);
			Put<uint64_t>(Class, 0x10, CastFlags);
		}

		return Class;
	}

	uint8_t* NewProperty(const std::string& ClassName, const std::string& Name, int ElementSize, int ArrayDim = 1, uint32_t Number = 0, uint64_t CastFlags = 0)
	{
		auto Prop = static_cast<uint8_t*>(calloc(1, 0x100));

		Put(Prop, 0x00, &FakeVTable[1]);
		Put(Prop, 0x08, GetFieldClass(ClassName, CastFlags));
		WriteName(Prop + FieldNameOffset(), Name, Number);
		Put<uint32_t>(Prop, FieldNameOffset() + (Layout->bOptimizedNames ? 4 : 8), 0x8000009);
		Put<int32_t>(Prop, ArrayDimOffset(), ArrayDim);
		Put<int32_t>(Prop, ArrayDimOffset() + 4, ElementSize);

		return Prop;
	}

	void SetMember(uint8_t* Prop, int Index, void* Value)
	{
		Put(Prop, PropertySize() + Index * 8, Value);
	}

	void SetProperties(uint8_t* Struct, std::initializer_list<uint8_t*> Props)
	{
		uint8_t* Previous = nullptr;

		for (auto Prop : Props)
		{
			if (Previous)
				Put(Previous, FieldNextOffset(), Prop);
			else
				Put(Struct, ChildPropertiesOffset(), Prop);

			Previous = Prop;
		}
	}

	void SetEnumNames(uint8_t* Enum, const std::string& EnumName, std::initializer_list<std::pair<std::string, int64_t>> Entries)
	{
		auto Data = static_cast<uint8_t*>(calloc(Entries.size(), 0x10));
		int i = 0;

		for (auto& [Name, Value] : Entries)
		{
			WriteName(Data + i * 0x10, EnumName + "::" + Name);
			Put<int64_t>(Data, i * 0x10 + 8, Value);
			i++;
		}

		Put(Enum, Layout->EnumNamesOffset, Data);
		Put<int32_t>(Enum, Layout->EnumNamesOffset + 8, static_cast<int32_t>(Entries.size()));
		Put<int32_t>(Enum, Layout->EnumNamesOffset + 12, static_cast<int32_t>(Entries.size()));
	}

	void BuildEngine()
	{
		AddName("None");
		AddName("ByteProperty");

		auto CoreUObject = NewObject("/Script/CoreUObject", nullptr, nullptr);
		auto Engine = NewObject("/Script/Engine", nullptr, nullptr);

		auto Object = NewObject("Object", nullptr, CoreUObject);
		auto Class = NewObject("Class", nullptr, CoreUObject);
		auto Field = NewObject("Field", Class, CoreUObject);
		auto Struct = NewObject("Struct", Class, CoreUObject);
		auto ScriptStruct = NewObject("ScriptStruct", Class, CoreUObject);
		auto Enum = NewObject("Enum", Class, CoreUObject);
		auto Package = NewObject("Package", Class, CoreUObject);

		SetClass(Object, Class);
		SetClass(Class, Class);
		SetClass(CoreUObject, Package);
		SetClass(Engine, Package);

		SetSuper(Field, Object);
		SetSuper(Struct, Field);
		SetSuper(Class, Struct);
		SetSuper(ScriptStruct, Struct);
		SetSuper(Enum, Field);
		SetSuper(Package, Object);

		auto Vector = NewObject("Vector", ScriptStruct, CoreUObject);
		auto Quat = NewObject("Quat", ScriptStruct, CoreUObject);
		auto Transform = NewObject("Transform", ScriptStruct, CoreUObject);

		SetProperties(Vector, { NewProperty("DoubleProperty", "X", 8), NewProperty("DoubleProperty", "Y", 8), NewProperty("DoubleProperty", "Z", 8) });
		SetProperties(Quat, { NewProperty("DoubleProperty", "X", 8), NewProperty("DoubleProperty", "Y", 8), NewProperty("DoubleProperty", "Z", 8), NewProperty("DoubleProperty", "W", 8) });

		auto Rotation = NewProperty("StructProperty", "Rotation", 32);
		auto Translation = NewProperty("StructProperty", "Translation", 24);
		auto Scale3D = NewProperty("StructProperty", "Scale3D", 24);
		SetMember(Rotation, 0, Quat);
		SetMember(Translation, 0, Vector);
		SetMember(Scale3D, 0, Vector);
		SetProperties(Transform, { Rotation, Translation, Scale3D });

		uint8_t* TestEnum = nullptr;

		for (auto& Name : { "ETestEnum", "EOtherEnum", "EThirdEnum", "EFourthEnum" })
		{
			auto E = NewObject(Name, Enum, CoreUObject);
			SetEnumNames(E, Name, { { "First", 0 }, { "Flag", 4 }, { "Big", 0x100000000 }, { std::string(Name) + "_MAX", 0x100000001 } });

			if (!TestEnum)
				TestEnum = E;
		}

		auto Actor = NewObject("Actor", Class, Engine);
		SetSuper(Actor, Object);

		for (int i = 0; i < 40; i++)
			NewObject("Dummy" + std::to_string(i), Object, Engine);

		auto TestActor = NewObject("TestActor", Class, Engine);
		SetSuper(TestActor, Actor);

		auto Points = NewProperty("ArrayProperty", "Points", 16);
		auto PointsInner = NewProperty("StructProperty", "Points", 24);
		SetMember(Points, 0, PointsInner);
		SetMember(PointsInner, 0, Vector);

		auto Scores = NewProperty("MapProperty", "Scores", 80);
		SetMember(Scores, 0, NewProperty("NameProperty", "Scores_Key", 8));
		SetMember(Scores, 1, NewProperty("IntProperty", "Scores", 4));

		auto Mode = NewProperty("EnumProperty", "Mode", 1);
		SetMember(Mode, 0, NewProperty("ByteProperty", "UnderlyingType", 1));
		SetMember(Mode, 1, TestEnum);

		auto OldMode = NewProperty("ByteProperty", "OldMode", 1);
		SetMember(OldMode, 0, TestEnum);

		auto Owner = NewProperty("ObjectProperty", "Owner", 8);
		SetMember(Owner, 0, Actor);

		auto Custom = NewProperty("MyGameStructProperty", "Custom", 24, 1, 0, 0x0000000000100000);
		SetMember(Custom, 0, Vector);

		auto Maybe = NewProperty("OptionalProperty", "Maybe", 8);
		SetMember(Maybe, 0, NewProperty("IntProperty", "Maybe", 4));

		SetProperties(TestActor,
			{
				NewProperty("IntProperty", "Health", 4),
				Points,
				Scores,
				Mode,
				OldMode,
				NewProperty("IntProperty", "Slots", 4, 3),
				NewProperty("BoolProperty", "bAlive", 1),
				Owner,
				NewProperty("StrProperty", "Title", 16),
				NewProperty("IntProperty", "Prop", 4, 1, 2),
				Custom,
				Maybe,
			});

		auto Chunk = static_cast<uint8_t*>(calloc(64 * 1024, Layout->ItemSize));

		for (size_t i = 0; i < AllObjects.size(); i++)
			Put(Chunk, static_cast<int>(i * Layout->ItemSize), AllObjects[i]);

		static uint8_t* Chunks[33] = {};
		Chunks[0] = Chunk;

		ObjectArray.Objects = Chunks;
		ObjectArray.MaxElements = 0x200000;
		ObjectArray.NumElements = static_cast<int32_t>(AllObjects.size());
		ObjectArray.MaxChunks = 33;
		ObjectArray.NumChunks = 1;
	}

	struct Reader
	{
		std::vector<uint8_t> Data;
		size_t Pos = 0;

		template <typename T>
		T Read()
		{
			if (Pos + sizeof(T) > Data.size())
				throw std::runtime_error("read past the end of the usmap");

			T Ret;
			memcpy(&Ret, Data.data() + Pos, sizeof(T));
			Pos += sizeof(T);
			return Ret;
		}

		std::string ReadString(size_t Size)
		{
			if (Pos + Size > Data.size())
				throw std::runtime_error("read past the end of the usmap");

			std::string Ret(reinterpret_cast<const char*>(Data.data() + Pos), Size);
			Pos += Size;
			return Ret;
		}
	};

	struct FProp
	{
		uint16_t Index;
		uint8_t ArrayDim;
		std::string Name;
		std::string Type;
	};

	struct FParsed
	{
		std::map<std::string, std::vector<std::pair<int64_t, std::string>>> Enums;
		std::map<std::string, std::pair<std::string, std::vector<FProp>>> Structs;
	};

	const char* TypeNames[] =
	{
		"ByteProperty", "BoolProperty", "IntProperty", "FloatProperty", "ObjectProperty", "NameProperty", "DelegateProperty",
		"DoubleProperty", "ArrayProperty", "StructProperty", "StrProperty", "TextProperty", "InterfaceProperty",
		"MulticastDelegateProperty", "WeakObjectProperty", "LazyObjectProperty", "AssetObjectProperty", "SoftObjectProperty",
		"UInt64Property", "UInt32Property", "UInt16Property", "Int64Property", "Int16Property", "Int8Property", "MapProperty",
		"SetProperty", "EnumProperty", "FieldPathProperty", "OptionalProperty", "Utf8StrProperty", "AnsiStrProperty",
	};

	std::string ReadType(Reader& R, const std::vector<std::string>& Names)
	{
		auto Type = R.Read<uint8_t>();

		if (Type >= std::size(TypeNames))
			return "Unknown(" + std::to_string(Type) + ")";

		std::string Ret = TypeNames[Type];

		auto Name = [&]() -> std::string
			{
				auto Index = R.Read<int32_t>();
				return Index >= 0 && Index < static_cast<int32_t>(Names.size()) ? Names[Index] : "<invalid>";
			};

		switch (Type)
		{
		case 26:
		{
			auto Inner = ReadType(R, Names);
			Ret += "<" + Inner + "," + Name() + ">";
			break;
		}
		case 9:
			Ret += "<" + Name() + ">";
			break;
		case 8:
		case 25:
		case 28:
			Ret += "<" + ReadType(R, Names) + ">";
			break;
		case 24:
		{
			auto Key = ReadType(R, Names);
			auto Value = ReadType(R, Names);
			Ret += "<" + Key + "," + Value + ">";
			break;
		}
		}

		return Ret;
	}

	FParsed ParseUsmap(const std::filesystem::path& Path)
	{
		std::ifstream File(Path, std::ios::binary);

		if (!File)
			throw std::runtime_error("Mappings.usmap wasn't written");

		Reader Header;
		Header.Data.assign(std::istreambuf_iterator<char>(File), {});

		if (Header.Read<uint16_t>() != 0x30C4)
			throw std::runtime_error("bad magic");

		if (Header.Read<uint8_t>() != 4)
			throw std::runtime_error("unexpected usmap version");

		if (Header.Read<int32_t>() != 0)
			throw std::runtime_error("unexpected versioning info");

		if (Header.Read<uint8_t>() != 0)
			throw std::runtime_error("unexpected compression");

		auto CompressedSize = Header.Read<uint32_t>();
		auto DecompressedSize = Header.Read<uint32_t>();

		if (CompressedSize != DecompressedSize || Header.Pos + CompressedSize != Header.Data.size())
			throw std::runtime_error("bad sizes in the header");

		Reader R;
		R.Data.assign(Header.Data.begin() + Header.Pos, Header.Data.end());

		std::vector<std::string> Names(R.Read<uint32_t>());

		for (auto& Name : Names)
			Name = R.ReadString(R.Read<uint16_t>());

		FParsed Ret;

		auto EnumCount = R.Read<uint32_t>();

		for (uint32_t i = 0; i < EnumCount; i++)
		{
			auto& Entries = Ret.Enums[Names.at(R.Read<int32_t>())];
			auto Count = R.Read<uint16_t>();

			for (int j = 0; j < Count; j++)
			{
				auto Value = R.Read<int64_t>();
				Entries.emplace_back(Value, Names.at(R.Read<int32_t>()));
			}
		}

		auto StructCount = R.Read<uint32_t>();

		for (uint32_t i = 0; i < StructCount; i++)
		{
			auto Name = Names.at(R.Read<int32_t>());
			auto SuperIndex = R.Read<int32_t>();

			auto& [Super, Props] = Ret.Structs[Name];
			Super = SuperIndex >= 0 ? Names.at(SuperIndex) : "";

			R.Read<uint16_t>();
			auto Serializable = R.Read<uint16_t>();

			for (int j = 0; j < Serializable; j++)
			{
				FProp P;
				P.Index = R.Read<uint16_t>();
				P.ArrayDim = R.Read<uint8_t>();
				P.Name = Names.at(R.Read<int32_t>());
				P.Type = ReadType(R, Names);
				Props.push_back(P);
			}
		}

		if (R.Pos != R.Data.size())
			throw std::runtime_error("trailing data in the usmap");

		return Ret;
	}

	int Failures = 0;

	void Check(bool bCondition, const std::string& What)
	{
		fprintf(stderr, "[%s] %s\n", bCondition ? " OK " : "FAIL", What.c_str());

		if (!bCondition)
			Failures++;
	}
}

void CheckUsmap(const std::filesystem::path& OutputPath)
{
	try
	{
		auto Usmap = ParseUsmap(OutputPath);

		Check(Usmap.Enums.contains("ETestEnum"), "enum ETestEnum was dumped");

		auto& Entries = Usmap.Enums["ETestEnum"];
		Check(Entries.size() == 4, "ETestEnum has 4 entries");

		if (Entries.size() == 4)
		{
			Check(Entries[1] == std::make_pair<int64_t, std::string>(4, "Flag"), "explicit enum value and stripped prefix");
			Check(Entries[2].first == 0x100000000, "64 bit enum value");
			Check(Entries[3].second == "ETestEnum_MAX", "enum _MAX entry");
		}

		Check(Usmap.Structs.contains("Vector") && Usmap.Structs["Vector"].second.size() == 3, "struct Vector with 3 properties");
		Check(Usmap.Structs.contains("TestActor"), "class TestActor was dumped");

		auto& [Super, Props] = Usmap.Structs["TestActor"];
		Check(Super == "Actor", "TestActor inherits from Actor");

		std::vector<std::tuple<std::string, std::string, int, int>> Expected =
		{
			{ "Health", "IntProperty", 0, 1 },
			{ "Points", "ArrayProperty<StructProperty<Vector>>", 1, 1 },
			{ "Scores", "MapProperty<NameProperty,IntProperty>", 2, 1 },
			{ "Mode", "EnumProperty<ByteProperty,ETestEnum>", 3, 1 },
			{ "OldMode", "EnumProperty<ByteProperty,ETestEnum>", 4, 1 },
			{ "Slots", "IntProperty", 5, 3 },
			{ "bAlive", "BoolProperty", 8, 1 },
			{ "Owner", "ObjectProperty", 9, 1 },
			{ "Title", "StrProperty", 10, 1 },
			{ "Prop_1", "IntProperty", 11, 1 },
			{ "Custom", "StructProperty<Vector>", 12, 1 },
			{ "Maybe", "OptionalProperty<IntProperty>", 13, 1 },
		};

		Check(Props.size() == Expected.size(), std::format("TestActor has {} properties (got {})", Expected.size(), Props.size()));

		for (size_t i = 0; i < std::min(Props.size(), Expected.size()); i++)
		{
			auto& [Name, Type, Index, ArrayDim] = Expected[i];
			auto& P = Props[i];

			Check(P.Name == Name && P.Type == Type && P.Index == Index && P.ArrayDim == ArrayDim,
				std::format("{} {} index {} dim {} (got {} {} index {} dim {})", Name, Type, Index, ArrayDim, P.Name, P.Type, P.Index, P.ArrayDim));
		}
	}
	catch (const std::exception& E)
	{
		Check(false, std::string("parsing the usmap: ") + E.what());
	}
}

bool WaitForFile(const std::filesystem::path& Path, int Seconds)
{
	for (int i = 0; i < Seconds * 5; i++)
	{
		std::error_code Error;
		auto Size = std::filesystem::exists(Path, Error) ? std::filesystem::file_size(Path, Error) : 0;

		if (Size > 16)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1));
			return true;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(200));
	}

	return false;
}

std::filesystem::path GetExecutableDirectory()
{
#ifdef _WIN32
	wchar_t Path[MAX_PATH];
	GetModuleFileNameW(nullptr, Path, MAX_PATH);
	return std::filesystem::path(Path).parent_path();
#else
	return std::filesystem::read_symlink("/proc/self/exe").parent_path();
#endif
}

#ifndef _WIN32
int RunAgainstChild(const std::string& LayoutName, const std::filesystem::path& OutputPath)
{
	int Pipe[2];

	if (pipe(Pipe) != 0)
		return 1;

	fflush(stdout);

	auto Child = fork();

	if (Child == 0)
	{
		BuildEngine();

		printf("Fake GObjects at %p, fake FNamePool at %p\n", static_cast<void*>(&ObjectArray), static_cast<void*>(&NamePool));
		fflush(stdout);

		char Ready = 1;
		(void)!write(Pipe[1], &Ready, 1);

		for (;;)
			pause();
	}

	char Ready = 0;
	(void)!read(Pipe[0], &Ready, 1);

	auto Target = Process::Get(Child);
	Check(Target.has_value(), "the fake game process can be inspected");

	std::string Error;
	auto Backend = Target ? FRemoteBackend::Open(*Target, Error) : nullptr;
	Check(Backend != nullptr, "opening the fake game process " + Error);

	int Result = 1;

	if (Backend)
	{
		Memory::SetBackend(std::move(Backend));

		Log::OpenFile(OutputPath.parent_path() / ("UnrealMappingsDumper.Tests." + LayoutName + ".log"));

		App::FGameInfo Game;
		Game.ExecutableName = Target->Name;

		bool bInitialized = App::Init(Game);
		Check(bInitialized, "App::Init");

		if (bInitialized)
			Check(Dumper::Run(ECompressionMethod::None, OutputPath), "Dumper::Run");

		Log::CloseFile();

		if (bInitialized)
			CheckUsmap(OutputPath);

		Result = Failures ? 1 : 0;
	}

	kill(Child, SIGKILL);
	waitpid(Child, nullptr, 0);

	return Result;
}
#endif

namespace AesTest
{
	constexpr Aes::FKey MainKey =
	{
		0x1F, 0x8E, 0x52, 0xC4, 0x7A, 0x09, 0xB3, 0xE6, 0x2D, 0x91, 0x5C, 0xF0, 0x48, 0xA7, 0x13, 0x6B,
		0xD2, 0x3E, 0x85, 0x0F, 0xC9, 0x74, 0xEA, 0x21, 0x96, 0x5B, 0x07, 0xBD, 0x68, 0xF3, 0x4A, 0x1C,
	};

	constexpr Aes::FKey DynamicKey =
	{
		0x63, 0xA1, 0x0E, 0xD7, 0x39, 0xF4, 0x82, 0x5D, 0xB6, 0x17, 0xC8, 0x4F, 0xE3, 0x2A, 0x95, 0x70,
		0x0B, 0xDE, 0x56, 0x8C, 0x31, 0xFA, 0x47, 0xA9, 0x14, 0x6E, 0xC2, 0x9B, 0x25, 0x7D, 0xE8, 0x50,
	};

	constexpr AesKey::FGuid DynamicGuid =
	{
		0x4D, 0x3C, 0x2B, 0x1A, 0x8F, 0x7E, 0x6D, 0x5C, 0x13, 0x24, 0x35, 0x46, 0x97, 0xA8, 0xB9, 0xCA,
	};

#ifdef _MSC_VER
	__declspec(noinline)
#else
	[[gnu::noinline]]
#endif
	void RegisterEncryptionKeyCallback(unsigned char OutKey[32])
	{
		const unsigned char Key[32] =
		{
			0x1F, 0x8E, 0x52, 0xC4, 0x7A, 0x09, 0xB3, 0xE6, 0x2D, 0x91, 0x5C, 0xF0, 0x48, 0xA7, 0x13, 0x6B,
			0xD2, 0x3E, 0x85, 0x0F, 0xC9, 0x74, 0xEA, 0x21, 0x96, 0x5B, 0x07, 0xBD, 0x68, 0xF3, 0x4A, 0x1C,
		};

		for (int ByteIndex = 0; ByteIndex < 32; ++ByteIndex)
			OutKey[ByteIndex] = Key[ByteIndex];
	}

	void (*volatile KeyCallback)(unsigned char*) = &RegisterEncryptionKeyCallback;
	uint8_t* volatile KeyRegistry = nullptr;
	volatile uint8_t KeySalt = 0;

	template <typename T>
	void Append(std::vector<uint8_t>& Data, const T& Value)
	{
		auto Bytes = reinterpret_cast<const uint8_t*>(&Value);
		Data.insert(Data.end(), Bytes, Bytes + sizeof(T));
	}

	void Append(std::vector<uint8_t>& Data, size_t Count, uint8_t Value)
	{
		Data.insert(Data.end(), Count, Value);
	}

	template <typename T>
	void Put(std::vector<uint8_t>& Data, size_t Offset, const T& Value)
	{
		memcpy(Data.data() + Offset, &Value, sizeof(T));
	}

	std::vector<uint8_t> MakeIndex(size_t Size, const Aes::FKey* Key)
	{
		std::vector<uint8_t> Index(Size, 0xAB);
		Put<int32_t>(Index, 0, 10);
		memcpy(Index.data() + 4, "../../../", 10);

		if (Key)
			Aes::FCipher(Key->data()).EncryptInPlace(Index.data(), Index.size());

		return Index;
	}

	std::vector<uint8_t> MakePak(const Aes::FKey* Key, const AesKey::FGuid& Guid)
	{
		std::vector<uint8_t> Data(100, 0x11);
		auto Index = MakeIndex(48, Key);

		int64_t IndexOffset = static_cast<int64_t>(Data.size());
		int64_t IndexSize = static_cast<int64_t>(Index.size());
		Data.insert(Data.end(), Index.begin(), Index.end());

		Data.insert(Data.end(), Guid.begin(), Guid.end());
		Append<uint8_t>(Data, Key ? 1 : 0);
		Append<uint32_t>(Data, 0x5A6F12E1);
		Append<int32_t>(Data, 11);
		Append(Data, IndexOffset);
		Append(Data, IndexSize);
		Append(Data, 20, 0x5C);

		std::vector<uint8_t> Names(5 * 32, 0);
		memcpy(Names.data(), "Oodle", 5);
		Data.insert(Data.end(), Names.begin(), Names.end());

		return Data;
	}

	std::vector<uint8_t> MakeToc(const Aes::FKey& Key, const AesKey::FGuid& Guid, bool bSigned)
	{
		constexpr uint32_t EntryCount = 2;
		constexpr uint32_t BlockCount = 3;
		constexpr uint32_t SeedCount = 2;
		constexpr uint32_t OverflowCount = 1;

		std::vector<uint8_t> Data(144, 0);
		memcpy(Data.data(), "-==--==--==--==-", 16);
		Data[16] = 8;
		Put<uint32_t>(Data, 20, 144);
		Put<uint32_t>(Data, 24, EntryCount);
		Put<uint32_t>(Data, 28, BlockCount);
		Put<uint32_t>(Data, 32, 12);
		Put<uint32_t>(Data, 36, 1);
		Put<uint32_t>(Data, 40, 32);
		Put<uint32_t>(Data, 44, 0x10000);
		Put<uint32_t>(Data, 48, 32);
		Put<uint32_t>(Data, 52, 1);
		memcpy(Data.data() + 64, Guid.data(), 16);
		Data[80] = static_cast<uint8_t>(1 | 2 | 8 | (bSigned ? 4 : 0));
		Put<uint32_t>(Data, 84, SeedCount);
		Put<uint32_t>(Data, 96, OverflowCount);

		Append(Data, EntryCount * 12 + EntryCount * 10, 0x21);
		Append(Data, (SeedCount + OverflowCount) * 4, 0x22);
		Append(Data, BlockCount * 12, 0x23);
		Append(Data, 32, 0);

		if (bSigned)
		{
			Append<int32_t>(Data, 256);
			Append(Data, 512, 0x24);
			Append(Data, BlockCount * 20, 0x25);
		}

		auto Index = MakeIndex(32, &Key);
		Data.insert(Data.end(), Index.begin(), Index.end());
		Append(Data, 40, 0x26);

		return Data;
	}

	bool WriteBytes(const std::filesystem::path& Path, const std::vector<uint8_t>& Data)
	{
		std::ofstream File(Path, std::ios::binary | std::ios::trunc);
		File.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
		return static_cast<bool>(File);
	}

	std::vector<AesKey::FCandidate> Scan(const std::vector<uint8_t>& Code)
	{
		constexpr uintptr_t Base = 0x140001000;

		AesKey::FReadFunction Read = [&](uintptr_t Address, void* Out, size_t Size)
			{
				if (Address < Base || Address + Size > Base + Code.size())
					return false;

				memcpy(Out, Code.data() + (Address - Base), Size);
				return true;
			};

		return AesKey::ScanCode(Code.data(), Code.size(), Code.size(), Base, Read);
	}

	bool Contains(const std::vector<AesKey::FCandidate>& Candidates, const Aes::FKey& Key)
	{
		return std::any_of(Candidates.begin(), Candidates.end(), [&](const AesKey::FCandidate& C) { return C.Key == Key; });
	}

	void CheckCipher()
	{
		Aes::FKey Key;
		Aes::FBlock Plain;

		for (int i = 0; i < 32; i++)
			Key[i] = static_cast<uint8_t>(i);

		for (int i = 0; i < 16; i++)
			Plain[i] = static_cast<uint8_t>(i * 0x11);

		Aes::FCipher Cipher(Key.data());
		auto Encrypted = Cipher.Encrypt(Plain);

		constexpr Aes::FBlock Expected = { 0x8E, 0xA2, 0xB7, 0xCA, 0x51, 0x67, 0x45, 0xBF, 0xEA, 0xFC, 0x49, 0x90, 0x4B, 0x49, 0x60, 0x89 };

		Check(Encrypted == Expected, "AES-256 matches the FIPS-197 test vector");
		Check(Cipher.Decrypt(Encrypted) == Plain, "AES-256 decrypts what it encrypted");
		Check(Aes::FromHex(Aes::ToHex(MainKey)) == MainKey, "AES key hex round trip");
		Check(!Aes::FromHex("0x12345").has_value() && !Aes::FromHex(std::string(64, 'g')).has_value(), "invalid hex keys are rejected");
	}

	void CheckScanner()
	{
		std::vector<uint8_t> Code(16, 0xCC);

		for (int i = 0; i < 8; i++)
		{
			Code.push_back(0xC7);

			if (i == 0)
			{
				Code.push_back(0x01);
			}
			else
			{
				Code.push_back(0x41);
				Code.push_back(static_cast<uint8_t>(i * 4));
			}

			Append(Code, *reinterpret_cast<const uint32_t*>(MainKey.data() + i * 4));
		}

		Code.push_back(0xC3);

		auto Found = Scan(Code);
		Check(Found.size() == 1 && Found[0].Key == MainKey && Found[0].bReturns, "key stored with mov dword [rcx+n], imm32");

		Code.assign(8, 0xCC);

		for (int i = 0; i < 4; i++)
		{
			Code.insert(Code.end(), { 0x48, 0xB8 });
			Append(Code, *reinterpret_cast<const uint64_t*>(MainKey.data() + i * 8));

			if (i == 0)
				Code.insert(Code.end(), { 0x48, 0x89, 0x07 });
			else
				Code.insert(Code.end(), { 0x48, 0x89, 0x47, static_cast<uint8_t>(i * 8) });
		}

		Code.push_back(0xC3);
		Check(Contains(Scan(Code), MainKey), "key stored with movabs rax, imm64 / mov [rdi+n], rax");

		Code.assign(4, 0xCC);
		auto Load = [&](std::initializer_list<uint8_t> Prefix, size_t ConstantOffset)
			{
				Code.insert(Code.end(), Prefix);
				auto End = Code.size() + 4;
				Append<int32_t>(Code, static_cast<int32_t>(ConstantOffset - End));
			};

		Load({ 0x0F, 0x28, 0x05 }, 64);
		Code.insert(Code.end(), { 0x0F, 0x11, 0x07 });
		Load({ 0x66, 0x0F, 0x6F, 0x0D }, 80);
		Code.insert(Code.end(), { 0xF3, 0x0F, 0x7F, 0x4F, 0x10 });
		Code.push_back(0xC3);
		Code.resize(64, 0xCC);
		Code.insert(Code.end(), MainKey.begin(), MainKey.end());
		Check(Contains(Scan(Code), MainKey), "key loaded from constants with movaps/movdqa and stored with movups/movdqu");

		Code.assign(4, 0xCC);
		Load({ 0xC5, 0xFC, 0x10, 0x05 }, 64);
		Code.insert(Code.end(), { 0xC5, 0xFC, 0x11, 0x01, 0xC5, 0xF8, 0x77, 0xC3 });
		Code.resize(64, 0xCC);
		Code.insert(Code.end(), MainKey.begin(), MainKey.end());
		Check(Contains(Scan(Code), MainKey), "key copied with vmovups ymm");

		Code.assign(4, 0xCC);
		Code.insert(Code.end(), { 0x55, 0x48, 0x89, 0xE5 });

		for (int i = 0; i < 32; i++)
			Code.insert(Code.end(), { 0xC6, 0x45, static_cast<uint8_t>(0xC0 + i), MainKey[i] });

		Code.insert(Code.end(), { 0xC7, 0x45, 0xBC, 0, 0, 0, 0 });
		Check(Contains(Scan(Code), MainKey), "key stored byte by byte next to a zeroed loop counter");

		constexpr uint32_t Sha256[8] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19 };
		Code.assign(4, 0xCC);

		for (int i = 0; i < 8; i++)
		{
			Code.insert(Code.end(), { 0xC7, 0x41, static_cast<uint8_t>(8 + i * 4) });
			Append(Code, Sha256[i]);
		}

		Code.push_back(0xC3);
		Check(Scan(Code).empty(), "SHA-256 initial values aren't reported as a key");

		Code.assign(4, 0xCC);

		for (int i = 0; i < 12; i++)
		{
			Code.insert(Code.end(), { 0xC7, 0x41, static_cast<uint8_t>(i * 4) });
			Append(Code, *reinterpret_cast<const uint32_t*>(MainKey.data() + (i % 8) * 4) ^ static_cast<uint32_t>(i * 0x01010101));
		}

		Code.push_back(0xC3);
		Check(Scan(Code).empty(), "48 byte constants aren't reported as a key");
	}

	std::filesystem::path MakePaks(const std::filesystem::path& Directory)
	{
		std::error_code Error;
		std::filesystem::remove_all(Directory, Error);
		std::filesystem::create_directories(Directory / "Sub", Error);

		AesKey::FGuid Zero{};

		Check(WriteBytes(Directory / "pakchunk0-Windows.pak", MakePak(&MainKey, Zero)), "writing an encrypted pak");
		Check(WriteBytes(Directory / "pakchunk1-Windows.pak", MakePak(nullptr, Zero)), "writing an unencrypted pak");
		Check(WriteBytes(Directory / "pakchunk2-Windows.utoc", MakeToc(DynamicKey, DynamicGuid, false)), "writing an encrypted utoc");
		Check(WriteBytes(Directory / "Sub" / "pakchunk3-Windows.utoc", MakeToc(MainKey, Zero, true)), "writing a signed encrypted utoc");

		return Directory;
	}

	void CheckContainers(const std::filesystem::path& Directory)
	{
		auto Containers = AesKey::ReadContainers(Directory);
		Check(Containers.size() == 4, std::format("4 pak and utoc files were read (got {})", Containers.size()));

		if (Containers.size() != 4)
			return;

		auto Get = [&](const char* Name) -> const AesKey::FContainer&
			{
				for (auto& Container : Containers)
				{
					if (Container.Path.filename() == Name)
						return Container;
				}

				return Containers[0];
			};

		auto& Pak = Get("pakchunk0-Windows.pak");
		auto& Plain = Get("pakchunk1-Windows.pak");
		auto& Toc = Get("pakchunk2-Windows.utoc");
		auto& Signed = Get("pakchunk3-Windows.utoc");

		Check(Pak.bEncrypted && Pak.bHasBlock && AesKey::IsZeroGuid(Pak.Guid), "encrypted pak index detected");
		Check(!Plain.bEncrypted && !Plain.bHasBlock, "unencrypted pak detected");
		Check(Toc.bIoStore && Toc.bEncrypted && Toc.bHasBlock && Toc.Guid == DynamicGuid, "encrypted utoc directory index and its key guid detected");
		Check(Signed.bIoStore && Signed.bHasBlock, "signed utoc directory index found");

		Check(AesKey::Decrypts(MainKey, Pak) && !AesKey::Decrypts(DynamicKey, Pak), "the pak index decrypts only with its key");
		Check(AesKey::Decrypts(DynamicKey, Toc) && !AesKey::Decrypts(MainKey, Toc), "the utoc directory index decrypts only with its key");
		Check(AesKey::Decrypts(MainKey, Signed), "the signed utoc directory index decrypts");
	}

	void CheckResult(const AesKey::FResult& Result)
	{
		const AesKey::FFoundKey* Main = nullptr;
		const AesKey::FFoundKey* Dynamic = nullptr;

		for (auto& Key : Result.Keys)
		{
			if (Key.bVerified && AesKey::IsZeroGuid(Key.Guid))
				Main = &Key;

			if (Key.bVerified && Key.Guid == DynamicGuid)
				Dynamic = &Key;
		}

		Check(Main && Main->Key == MainKey, "main AES key found and verified");
		Check(Main && Main->Source == "code", std::format("main AES key found in the code (found in {})", Main ? Main->Source : "nothing"));
		Check(Main && Main->Containers == 2, "main AES key decrypts the pak and the signed utoc");
		Check(Dynamic && Dynamic->Key == DynamicKey, "dynamic AES key found and verified");
		Check(Dynamic && Dynamic->Source == "memory", std::format("dynamic AES key found in memory (found in {})", Dynamic ? Dynamic->Source : "nothing"));
	}

	void RegisterKeys()
	{
		KeyRegistry = new uint8_t[64];
		memcpy(KeyRegistry, DynamicGuid.data(), 16);

		for (int i = 0; i < 32; i++)
			KeyRegistry[16 + i] = DynamicKey[i] ^ KeySalt;
	}

	int Host()
	{
		RegisterKeys();

		auto Executable = GetExecutableDirectory();
		auto Directory = MakePaks(Executable.parent_path().parent_path() / "Content" / "Paks");
		auto LogPath = Executable / "UnrealMappingsDumper.log";

		std::error_code Error;
		std::filesystem::remove(LogPath, Error);

		printf("Fake game with encrypted paks in %s\n", Memory::PathToUtf8(Directory).c_str());
		fflush(stdout);

		std::string Log;

		for (int i = 0; i < 600 && Log.find("Searched for") == std::string::npos; i++)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(200));

			std::ifstream File(LogPath, std::ios::binary);
			Log.assign(std::istreambuf_iterator<char>(File), std::istreambuf_iterator<char>());
		}

		Check(Log.find("Searched for") != std::string::npos, "the AES key search finished within 120 seconds");
		Check(Log.find("AES key: " + Aes::ToHex(MainKey)) != std::string::npos, "the dumper found the main AES key");
		Check(Log.find("Dynamic AES key " + AesKey::GuidToString(DynamicGuid) + ": " + Aes::ToHex(DynamicKey)) != std::string::npos, "the dumper found the dynamic AES key");

		fprintf(stderr, "aes (external): %d failure(s)\n", Failures);
		return Failures ? 1 : 0;
	}

	int Run()
	{
		RegisterKeys();

		unsigned char Registered[32];
		KeyCallback(Registered);
		Check(memcmp(Registered, MainKey.data(), 32) == 0, "the fake key registration callback works");

		CheckCipher();
		CheckScanner();

		auto Directory = MakePaks(GetExecutableDirectory() / "AesTest" / "Content" / "Paks");
		CheckContainers(Directory);

#ifdef _WIN32
		int Pid = static_cast<int>(GetCurrentProcessId());
		Memory::SetBackend(std::make_unique<FLocalBackend>());
#else
		int Pipe[2];

		if (pipe(Pipe) != 0)
			return 1;

		fflush(stdout);
		fflush(stderr);

		auto Child = fork();

		if (Child == 0)
		{
			char Ready = 1;
			(void)!write(Pipe[1], &Ready, 1);

			for (;;)
				pause();
		}

		char Ready = 0;
		(void)!read(Pipe[0], &Ready, 1);

		int Pid = Child;

		auto Target = Process::Get(Pid);
		std::string Error;
		auto Backend = Target ? FRemoteBackend::Open(*Target, Error) : nullptr;
		Check(Backend != nullptr, "opening the fake game process " + Error);

		if (Backend)
			Memory::SetBackend(std::move(Backend));
#endif

		if (Memory::HasBackend())
		{
			Log::OpenFile(GetExecutableDirectory() / "UnrealMappingsDumper.Tests.aes.log");
			CheckResult(AesKey::Find(Directory));
			Log::CloseFile();

			auto ExitCode = Cli::Run({ "--pid", std::to_string(Pid), "--aes", "--paks", Memory::PathToUtf8(Directory) });
			Check(ExitCode == 0, std::format("unreal-mappings-dumper --aes exits with 0 (got {})", ExitCode));
		}

#ifndef _WIN32
		kill(Child, SIGKILL);
		waitpid(Child, nullptr, 0);
#endif

		fprintf(stderr, "aes: %d failure(s)\n", Failures);
		return Failures ? 1 : 0;
	}
}

int main(int argc, char** argv)
{
	std::vector<std::string> Args(argv + 1, argv + argc);

	if (!Args.empty() && Args[0] == "aes")
		return AesTest::Run();

	if (!Args.empty() && Args[0] == "host-aes")
		return AesTest::Host();

	bool bHost = !Args.empty() && Args[0] == "host";

	if (bHost)
		Args.erase(Args.begin());

	std::string LayoutName = !Args.empty() ? Args[0] : "classic";

	for (auto& L : Layouts)
	{
		if (LayoutName == L.Name)
			Layout = &L;
	}

	if (!Layout)
	{
		printf("Unknown layout %s\n", LayoutName.c_str());
		return 2;
	}

	printf("Testing layout \"%s\"%s\n", Layout->Name, bHost ? " as dump target" : "");

	auto OutputPath = Args.size() > 1 ? std::filesystem::path(Args[1]) : GetExecutableDirectory() / "Mappings.usmap";
	std::filesystem::remove(OutputPath);

	if (bHost)
	{
		BuildEngine();

		printf("Fake GObjects at %p, fake FNamePool at %p\n", static_cast<void*>(&ObjectArray), static_cast<void*>(&NamePool));
		fflush(stdout);

		if (!WaitForFile(OutputPath, 120))
		{
			fprintf(stderr, "Mappings.usmap wasn't written within 120 seconds\n");
			return 1;
		}

		CheckUsmap(OutputPath);

		fprintf(stderr, "%s (external): %d failure(s)\n", Layout->Name, Failures);
		return Failures ? 1 : 0;
	}

#ifdef _WIN32
	static volatile uintptr_t BadAddress = 0x10;

	Memory::FaultInfo Fault;
	bool bGuardReturned = Memory::TryInvoke([] { *reinterpret_cast<volatile int*>(BadAddress) = 1; }, &Fault);
	Check(!bGuardReturned && Fault.Code == EXCEPTION_ACCESS_VIOLATION, "access violations are caught");

	bool bNestedOk = Memory::TryInvoke([]
		{
			Memory::TryInvoke([] { *reinterpret_cast<volatile int*>(BadAddress + 0x10) = 1; });
		});
	Check(bNestedOk, "nested guards recover to the innermost guard");

	BuildEngine();

	printf("Fake GObjects at %p, fake FNamePool at %p\n", static_cast<void*>(&ObjectArray), static_cast<void*>(&NamePool));

	Memory::SetBackend(std::make_unique<FLocalBackend>());

	Log::OpenFile(OutputPath.parent_path() / ("UnrealMappingsDumper.Tests." + LayoutName + ".log"));

	App::FGameInfo Game;
	Game.ExecutableName = Memory::Get().GetModule().Name;

	bool bInitialized = App::Init(Game);
	Check(bInitialized, "App::Init");

	if (!bInitialized)
	{
		Log::CloseFile();
		return 1;
	}

	Check(Dumper::Run(ECompressionMethod::None, OutputPath), "Dumper::Run");

	Log::CloseFile();

	CheckUsmap(OutputPath);
#else
	RunAgainstChild(LayoutName, OutputPath);
#endif

	fprintf(stderr, "%s: %d failure(s)\n", Layout->Name, Failures);

	return Failures ? 1 : 0;
}
