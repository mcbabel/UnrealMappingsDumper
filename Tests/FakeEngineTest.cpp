
#include "../UnrealMappingsDumper/pch.h"

#include "../UnrealMappingsDumper/app.h"
#include "../UnrealMappingsDumper/dumper.h"
#include "../UnrealMappingsDumper/memory.h"

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

int main(int argc, char** argv)
{
	std::vector<std::string> Args(argv + 1, argv + argc);

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
