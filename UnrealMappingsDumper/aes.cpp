#include "pch.h"

#include "aes.h"

namespace
{
	struct FTables
	{
		std::array<uint8_t, 256> SBox{};
		std::array<uint8_t, 256> InvSBox{};
		std::array<uint8_t, 256> Mul2{};
		std::array<uint8_t, 256> Mul3{};
		std::array<uint8_t, 256> Mul9{};
		std::array<uint8_t, 256> Mul11{};
		std::array<uint8_t, 256> Mul13{};
		std::array<uint8_t, 256> Mul14{};

		static uint8_t Multiply(uint8_t A, uint8_t B)
		{
			uint8_t Result = 0;

			while (B)
			{
				if (B & 1)
					Result ^= A;

				A = static_cast<uint8_t>((A << 1) ^ ((A & 0x80) ? 0x1B : 0));
				B >>= 1;
			}

			return Result;
		}

		FTables()
		{
			for (int i = 0; i < 256; i++)
			{
				uint8_t Inverse = 0;

				if (i)
				{
					for (int j = 1; j < 256; j++)
					{
						if (Multiply(static_cast<uint8_t>(i), static_cast<uint8_t>(j)) == 1)
						{
							Inverse = static_cast<uint8_t>(j);
							break;
						}
					}
				}

				uint8_t Value = Inverse;
				uint8_t Result = 0x63;

				for (int Bit = 0; Bit < 5; Bit++)
				{
					Result ^= Value;
					Value = static_cast<uint8_t>((Value << 1) | (Value >> 7));
				}

				SBox[i] = Result;
				InvSBox[Result] = static_cast<uint8_t>(i);

				Mul2[i] = Multiply(static_cast<uint8_t>(i), 2);
				Mul3[i] = Multiply(static_cast<uint8_t>(i), 3);
				Mul9[i] = Multiply(static_cast<uint8_t>(i), 9);
				Mul11[i] = Multiply(static_cast<uint8_t>(i), 11);
				Mul13[i] = Multiply(static_cast<uint8_t>(i), 13);
				Mul14[i] = Multiply(static_cast<uint8_t>(i), 14);
			}
		}
	};

	const FTables& Tables()
	{
		static const FTables Instance;
		return Instance;
	}

	constexpr int Rounds = 14;
}

Aes::FCipher::FCipher(const uint8_t* Key)
{
	auto& T = Tables();

	memcpy(RoundKeys.data(), Key, 32);

	uint8_t Rcon = 1;

	for (int i = 8; i < 60; i++)
	{
		uint8_t Word[4];
		memcpy(Word, &RoundKeys[(i - 1) * 4], 4);

		if (i % 8 == 0)
		{
			uint8_t First = Word[0];
			Word[0] = static_cast<uint8_t>(T.SBox[Word[1]] ^ Rcon);
			Word[1] = T.SBox[Word[2]];
			Word[2] = T.SBox[Word[3]];
			Word[3] = T.SBox[First];
			Rcon = T.Mul2[Rcon];
		}
		else if (i % 8 == 4)
		{
			for (auto& Byte : Word)
				Byte = T.SBox[Byte];
		}

		for (int j = 0; j < 4; j++)
			RoundKeys[i * 4 + j] = RoundKeys[(i - 8) * 4 + j] ^ Word[j];
	}
}

Aes::FBlock Aes::FCipher::Encrypt(const FBlock& In) const
{
	auto& T = Tables();
	FBlock State = In;

	for (int i = 0; i < 16; i++)
		State[i] ^= RoundKeys[i];

	for (int Round = 1; Round <= Rounds; Round++)
	{
		FBlock Shifted;

		for (int Column = 0; Column < 4; Column++)
		{
			for (int Row = 0; Row < 4; Row++)
				Shifted[Column * 4 + Row] = T.SBox[State[((Column + Row) % 4) * 4 + Row]];
		}

		if (Round != Rounds)
		{
			for (int Column = 0; Column < 4; Column++)
			{
				auto C = &Shifted[Column * 4];
				uint8_t A0 = C[0], A1 = C[1], A2 = C[2], A3 = C[3];

				C[0] = static_cast<uint8_t>(T.Mul2[A0] ^ T.Mul3[A1] ^ A2 ^ A3);
				C[1] = static_cast<uint8_t>(A0 ^ T.Mul2[A1] ^ T.Mul3[A2] ^ A3);
				C[2] = static_cast<uint8_t>(A0 ^ A1 ^ T.Mul2[A2] ^ T.Mul3[A3]);
				C[3] = static_cast<uint8_t>(T.Mul3[A0] ^ A1 ^ A2 ^ T.Mul2[A3]);
			}
		}

		for (int i = 0; i < 16; i++)
			State[i] = Shifted[i] ^ RoundKeys[Round * 16 + i];
	}

	return State;
}

Aes::FBlock Aes::FCipher::Decrypt(const FBlock& In) const
{
	auto& T = Tables();
	FBlock State = In;

	for (int i = 0; i < 16; i++)
		State[i] ^= RoundKeys[Rounds * 16 + i];

	for (int Round = Rounds - 1; Round >= 0; Round--)
	{
		FBlock Shifted;

		for (int Column = 0; Column < 4; Column++)
		{
			for (int Row = 0; Row < 4; Row++)
				Shifted[((Column + Row) % 4) * 4 + Row] = T.InvSBox[State[Column * 4 + Row]];
		}

		for (int i = 0; i < 16; i++)
			Shifted[i] ^= RoundKeys[Round * 16 + i];

		if (Round != 0)
		{
			for (int Column = 0; Column < 4; Column++)
			{
				auto C = &Shifted[Column * 4];
				uint8_t A0 = C[0], A1 = C[1], A2 = C[2], A3 = C[3];

				C[0] = static_cast<uint8_t>(T.Mul14[A0] ^ T.Mul11[A1] ^ T.Mul13[A2] ^ T.Mul9[A3]);
				C[1] = static_cast<uint8_t>(T.Mul9[A0] ^ T.Mul14[A1] ^ T.Mul11[A2] ^ T.Mul13[A3]);
				C[2] = static_cast<uint8_t>(T.Mul13[A0] ^ T.Mul9[A1] ^ T.Mul14[A2] ^ T.Mul11[A3]);
				C[3] = static_cast<uint8_t>(T.Mul11[A0] ^ T.Mul13[A1] ^ T.Mul9[A2] ^ T.Mul14[A3]);
			}
		}

		State = Shifted;
	}

	return State;
}

void Aes::FCipher::DecryptInPlace(uint8_t* Data, size_t Size) const
{
	for (size_t Offset = 0; Offset + 16 <= Size; Offset += 16)
	{
		FBlock Block;
		memcpy(Block.data(), Data + Offset, 16);
		Block = Decrypt(Block);
		memcpy(Data + Offset, Block.data(), 16);
	}
}

void Aes::FCipher::EncryptInPlace(uint8_t* Data, size_t Size) const
{
	for (size_t Offset = 0; Offset + 16 <= Size; Offset += 16)
	{
		FBlock Block;
		memcpy(Block.data(), Data + Offset, 16);
		Block = Encrypt(Block);
		memcpy(Data + Offset, Block.data(), 16);
	}
}

std::string Aes::ToHex(const FKey& Key)
{
	std::string Ret = "0x";

	for (auto Byte : Key)
		Ret += std::format("{:02X}", Byte);

	return Ret;
}

std::optional<Aes::FKey> Aes::FromHex(std::string_view Hex)
{
	if (Hex.starts_with("0x") || Hex.starts_with("0X"))
		Hex.remove_prefix(2);

	if (Hex.size() != 64)
		return std::nullopt;

	auto Nibble = [](char C) -> int
		{
			if (C >= '0' && C <= '9')
				return C - '0';

			if (C >= 'a' && C <= 'f')
				return C - 'a' + 10;

			if (C >= 'A' && C <= 'F')
				return C - 'A' + 10;

			return -1;
		};

	FKey Key;

	for (size_t i = 0; i < 32; i++)
	{
		auto High = Nibble(Hex[i * 2]);
		auto Low = Nibble(Hex[i * 2 + 1]);

		if (High < 0 || Low < 0)
			return std::nullopt;

		Key[i] = static_cast<uint8_t>(High << 4 | Low);
	}

	return Key;
}
