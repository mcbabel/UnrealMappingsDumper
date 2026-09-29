#pragma once

namespace Memory
{
	struct Section
	{
		std::string Name;
		uintptr_t Start = 0;
		size_t Size = 0;
		bool bExecutable = false;
		bool bWritable = false;

		bool IsExecutable() const { return bExecutable; }
		bool IsWritable() const { return bWritable; }
		bool Contains(uintptr_t Address) const { return Address >= Start && Address < Start + Size; }
	};

	struct Region
	{
		uintptr_t Start = 0;
		uintptr_t End = 0;
	};

	struct Module
	{
		std::string Name;
		std::filesystem::path Path;
		uintptr_t Base = 0;
		size_t Size = 0;
		std::vector<Section> Sections;
	};

	class IBackend
	{
	public:

		virtual ~IBackend() = default;

		virtual bool ReadRaw(uintptr_t Address, void* Out, size_t Size) = 0;

		virtual void ReadUnchecked(uintptr_t Address, void* Out, size_t Size)
		{
			if (!ReadRaw(Address, Out, Size))
				memset(Out, 0, Size);
		}

		virtual bool IsReadable(uintptr_t Address, size_t Size) = 0;

		virtual std::vector<Region> GetReadableRegions() = 0;

		virtual const Module& GetModule() = 0;

		virtual bool IsLocal() const { return false; }
	};

	void SetBackend(std::unique_ptr<IBackend> Backend);
	IBackend& Get();
	bool HasBackend();

	bool ParsePeModule(uintptr_t Base, Module& Out);
	bool ParseElfModule(uintptr_t Base, Module& Out);

	inline bool ReadRaw(uintptr_t Address, void* Out, size_t Size)
	{
		return Get().ReadRaw(Address, Out, Size);
	}

	inline bool IsReadable(uintptr_t Address, size_t Size = sizeof(void*))
	{
		return Get().IsReadable(Address, Size);
	}

	inline bool IsReadable(const void* Address, size_t Size = sizeof(void*))
	{
		return IsReadable(reinterpret_cast<uintptr_t>(Address), Size);
	}

	template <typename T>
	bool Read(uintptr_t Address, T& Out)
	{
		return Get().ReadRaw(Address, &Out, sizeof(T));
	}

	template <typename T>
	T ReadOr(uintptr_t Address, T Default)
	{
		T Out{};
		return Read(Address, Out) ? Out : Default;
	}

	template <typename T>
	T Fetch(uintptr_t Address)
	{
		T Out{};
		Get().ReadUnchecked(Address, &Out, sizeof(T));
		return Out;
	}

	std::vector<uint8_t> ReadBytes(uintptr_t Address, size_t Size);

	inline const std::vector<Section>& GetSections()
	{
		return Get().GetModule().Sections;
	}

	inline uintptr_t GetModuleBase()
	{
		return Get().GetModule().Base;
	}

	inline size_t GetModuleSize()
	{
		return Get().GetModule().Size;
	}

	inline bool IsInModule(uintptr_t Address)
	{
		return Address >= GetModuleBase() && Address < GetModuleBase() + GetModuleSize();
	}

	template <typename Callback>
	void ForEachSectionChunk(const Section& S, size_t Overlap, Callback&& Func)
	{
		constexpr size_t ChunkSize = 4 * 1024 * 1024;

		std::vector<uint8_t> Buffer;

		for (size_t Offset = 0; Offset < S.Size; Offset += ChunkSize)
		{
			auto Size = std::min(ChunkSize + Overlap, S.Size - Offset);
			auto Owned = std::min(ChunkSize, S.Size - Offset);
			Buffer.resize(Size);

			if (!Get().ReadRaw(S.Start + Offset, Buffer.data(), Size))
			{
				for (size_t Page = 0; Page < Size; Page += 0x1000)
				{
					auto PageSize = std::min<size_t>(0x1000, Size - Page);

					if (!Get().ReadRaw(S.Start + Offset + Page, Buffer.data() + Page, PageSize))
						memset(Buffer.data() + Page, 0, PageSize);
				}
			}

			Func(S.Start + Offset, Buffer.data(), Size, Owned);
		}
	}

	class RegionSnapshot
	{
		std::vector<Region> Regions;

	public:

		RegionSnapshot();

		bool Contains(uintptr_t Address, size_t Size = sizeof(void*)) const;
	};

	std::string DescribeAddress(uintptr_t Address);

	struct FaultInfo
	{
		uint32_t Code = 0;
		uintptr_t Address = 0;
	};

	bool TryCall(void (*Function)(void*), void* Context, FaultInfo* OutFault = nullptr);

	template <typename Callable>
	bool TryInvoke(Callable&& Function, FaultInfo* OutFault = nullptr)
	{
		return TryCall([](void* Ctx) { (*static_cast<std::remove_reference_t<Callable>*>(Ctx))(); }, &Function, OutFault);
	}

	std::string PathToUtf8(const std::filesystem::path& Path);
}
