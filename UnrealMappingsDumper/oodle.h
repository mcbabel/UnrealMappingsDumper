#pragma once

enum class OodleFormat
{
	Invalid = -1,
	None = 3,
	Kraken = 8,
	Leviathan = 13,
	Mermaid = 9,
	Selkie = 11,
	Hydra = 12,
	Count = 14,
	Force32 = 0x40000000
};

enum class OodleCompressionLevel : uint32_t
{
	None,
	SuperFast,
	VeryFast,
	Fast,
	Normal,
	Optimal1,
	Optimal2,
	Optimal3,
	Optimal4,
	Optimal5
};

typedef int64_t(*_OodleCompressFunc)(
	OodleFormat format,
	const void* buffer,
	int64_t bufferSize,
	void* outputBuffer,
	OodleCompressionLevel level,
	void* options,
	void* dictionaryBase,
	void* lrm,
	void* scratchMem,
	int64_t scratchSize);

class Oodle
{
	static inline _OodleCompressFunc OodleLZ_Compress = nullptr;

	static constexpr const wchar_t* DllNames[] =
	{
		L"oo2core_9_win64.dll",
		L"oo2core_8_win64.dll",
		L"oo2core_7_win64.dll",
		L"oo2core_6_win64.dll",
		L"oo2core_5_win64.dll",
		L"Oodle.dll",
	};

	static std::filesystem::path GetModuleDirectory(HMODULE Module)
	{
		wchar_t Path[MAX_PATH];
		GetModuleFileNameW(Module, Path, MAX_PATH);
		return std::filesystem::path(Path).parent_path();
	}

	static HMODULE FindOodle()
	{
		for (auto Name : DllNames)
		{
			if (auto Module = GetModuleHandleW(Name))
				return Module;
		}

		HMODULE Self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&FindOodle), &Self);

		for (auto& Directory : { GetModuleDirectory(Self), GetModuleDirectory(nullptr) })
		{
			for (auto Name : DllNames)
			{
				auto Path = Directory / Name;

				if (std::filesystem::exists(Path))
				{
					if (auto Module = LoadLibraryW(Path.c_str()))
						return Module;
				}
			}
		}

		return nullptr;
	}

	static __forceinline int64_t GetCompressionBound(int64_t uncompressedSize)
	{
		return uncompressedSize + 274 * ((uncompressedSize + 0x3FFFF) / 0x40000);
	}

public:

	static bool IsAvailable()
	{
		static bool bInitialized = false;

		if (!bInitialized)
		{
			bInitialized = true;

			if (auto Module = FindOodle())
				OodleLZ_Compress = reinterpret_cast<_OodleCompressFunc>(GetProcAddress(Module, "OodleLZ_Compress"));
		}

		return OodleLZ_Compress != nullptr;
	}

	static std::vector<uint8_t> Compress(const std::string& uncompressed, OodleFormat format = OodleFormat::Kraken, OodleCompressionLevel level = OodleCompressionLevel::Optimal5)
	{
		if (!IsAvailable() || uncompressed.empty())
			return {};

		std::vector<uint8_t> compressed(GetCompressionBound(uncompressed.size()));

		auto compressedSize = OodleLZ_Compress(
			format,
			uncompressed.data(),
			uncompressed.size(),
			compressed.data(),
			level,
			nullptr, nullptr, nullptr, nullptr, 0);

		if (compressedSize <= 0 || compressedSize > static_cast<int64_t>(compressed.size()))
			return {};

		compressed.resize(compressedSize);
		return compressed;
	}
};
