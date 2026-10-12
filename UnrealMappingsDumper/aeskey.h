#pragma once

#include "aes.h"

namespace AesKey
{
	using FGuid = std::array<uint8_t, 16>;

	struct FContainer
	{
		std::filesystem::path Path;
		bool bIoStore = false;
		bool bEncrypted = false;
		bool bHasBlock = false;
		FGuid Guid{};
		Aes::FBlock FirstBlock{};
	};

	struct FCandidate
	{
		Aes::FKey Key{};
		uintptr_t Address = 0;
		bool bReturns = false;
	};

	struct FFoundKey
	{
		Aes::FKey Key{};
		FGuid Guid{};
		bool bVerified = false;
		std::string Source;
		size_t Containers = 0;
	};

	struct FResult
	{
		std::filesystem::path PakDirectory;
		std::vector<FContainer> Containers;
		std::vector<FFoundKey> Keys;
	};

	using FReadFunction = std::function<bool(uintptr_t Address, void* Out, size_t Size)>;

	std::vector<FCandidate> ScanCode(const uint8_t* Code, size_t Size, size_t Owned, uintptr_t Address, const FReadFunction& Read);

	std::optional<FContainer> ReadContainer(const std::filesystem::path& File);
	std::vector<FContainer> ReadContainers(const std::filesystem::path& Directory);
	std::filesystem::path GuessPakDirectory(const std::filesystem::path& Executable);

	bool IsMountPoint(const Aes::FBlock& Block);
	bool Decrypts(const Aes::FKey& Key, const FContainer& Container);

	std::string GuidToString(const FGuid& Guid);
	bool IsZeroGuid(const FGuid& Guid);

	FResult Find(const std::filesystem::path& PakDirectory);
}
