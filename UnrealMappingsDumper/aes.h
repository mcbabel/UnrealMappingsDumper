#pragma once

namespace Aes
{
	using FKey = std::array<uint8_t, 32>;
	using FBlock = std::array<uint8_t, 16>;

	class FCipher
	{
		std::array<uint8_t, 240> RoundKeys{};

	public:

		explicit FCipher(const uint8_t* Key);

		FBlock Encrypt(const FBlock& In) const;
		FBlock Decrypt(const FBlock& In) const;

		void DecryptInPlace(uint8_t* Data, size_t Size) const;
		void EncryptInPlace(uint8_t* Data, size_t Size) const;
	};

	std::string ToHex(const FKey& Key);
	std::optional<FKey> FromHex(std::string_view Hex);
}
