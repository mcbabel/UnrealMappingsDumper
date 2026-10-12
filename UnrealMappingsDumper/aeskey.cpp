#include "pch.h"

#include "aeskey.h"
#include "log.h"
#include "memory.h"

#include <map>

namespace
{
	constexpr int RegRip = 16;
	constexpr int RegAbsolute = 17;
	constexpr int MaxInstructions = 96;
	constexpr int MaxIdleInstructions = 12;

	struct FCodeView
	{
		const uint8_t* Code = nullptr;
		size_t Size = 0;
		uintptr_t Address = 0;
		const AesKey::FReadFunction* Read = nullptr;
	};

	struct FOperand
	{
		int Mod = 0;
		int Reg = 0;
		int Rm = 0;
		int Base = -1;
		int64_t Disp = 0;
		size_t Length = 0;
	};

	struct FVector
	{
		size_t Size = 0;
		std::array<uint8_t, 32> Bytes;
	};

	struct FStore
	{
		int Base = 0;
		int64_t Disp = 0;
		size_t Size = 0;
		bool bKnown = false;
		std::array<uint8_t, 32> Bytes{};
	};

	struct FRun
	{
		std::array<uint64_t, 16> Gpr;
		uint32_t GprKnown = 0;
		std::array<FVector, 16> Vec;
		uint32_t VecKnown = 0;
		std::vector<FStore> Stores;
		bool bReturns = false;
		int Productive = 0;

		void Reset()
		{
			GprKnown = 0;
			VecKnown = 0;
			Productive = 0;
			Stores.clear();
			bReturns = false;
		}

		std::optional<uint64_t> GetGpr(int Reg) const
		{
			if (GprKnown & (1u << Reg))
				return Gpr[Reg];

			return std::nullopt;
		}

		void SetGpr(int Reg, std::optional<uint64_t> Value)
		{
			if (Value)
			{
				Gpr[Reg] = *Value;
				GprKnown |= 1u << Reg;
			}
			else
			{
				GprKnown &= ~(1u << Reg);
			}
		}

		bool IsKnown(int Reg) const
		{
			return VecKnown & (1u << Reg);
		}

		void SetKnown(int Reg, bool bKnown)
		{
			if (bKnown)
				VecKnown |= 1u << Reg;
			else
				VecKnown &= ~(1u << Reg);
		}

		void CopyVec(int To, int From)
		{
			Vec[To] = Vec[From];
			SetKnown(To, IsKnown(From));
		}
	};

	enum class ESimd
	{
		None,
		Load,
		Store,
		Zero
	};

	template <typename T>
	T Get(const uint8_t* Data)
	{
		T Value;
		memcpy(&Value, Data, sizeof(T));
		return Value;
	}

	bool DecodeOperand(const FCodeView& C, size_t Pos, uint8_t Rex, FOperand& Out)
	{
		if (Pos >= C.Size)
			return false;

		uint8_t ModRm = C.Code[Pos];
		Out.Mod = ModRm >> 6;
		Out.Reg = ((ModRm >> 3) & 7) | ((Rex & 4) << 1);
		Out.Rm = (ModRm & 7) | ((Rex & 1) << 3);

		size_t i = Pos + 1;

		if (Out.Mod == 3)
		{
			Out.Length = 1;
			return true;
		}

		if ((ModRm & 7) == 4)
		{
			if (i >= C.Size)
				return false;

			uint8_t Sib = C.Code[i++];
			int Index = ((Sib >> 3) & 7) | ((Rex & 2) << 2);

			if (Index != 4)
				return false;

			Out.Base = (Sib & 7) == 5 && Out.Mod == 0 ? RegAbsolute : (Sib & 7) | ((Rex & 1) << 3);
		}
		else if ((ModRm & 7) == 5 && Out.Mod == 0)
		{
			Out.Base = RegRip;
		}
		else
		{
			Out.Base = Out.Rm;
		}

		if (Out.Mod == 1)
		{
			if (i + 1 > C.Size)
				return false;

			Out.Disp = static_cast<int8_t>(C.Code[i]);
			i += 1;
		}
		else if (Out.Mod == 2 || Out.Base == RegRip || Out.Base == RegAbsolute)
		{
			if (i + 4 > C.Size)
				return false;

			Out.Disp = Get<int32_t>(C.Code + i);
			i += 4;
		}

		Out.Length = i - Pos;
		return true;
	}

	ESimd ClassifySimd(uint8_t Op, int Pp)
	{
		switch (Op)
		{
		case 0x10:
		case 0x28:
			return Pp <= 1 ? ESimd::Load : ESimd::None;
		case 0x11:
		case 0x29:
			return Pp <= 1 ? ESimd::Store : ESimd::None;
		case 0x6F:
			return Pp == 1 || Pp == 2 ? ESimd::Load : ESimd::None;
		case 0x7F:
			return Pp == 1 || Pp == 2 ? ESimd::Store : ESimd::None;
		case 0x57:
			return Pp <= 1 ? ESimd::Zero : ESimd::None;
		case 0xEF:
			return Pp == 1 ? ESimd::Zero : ESimd::None;
		default:
			return ESimd::None;
		}
	}

	size_t StepSimd(const FCodeView& C, size_t Pos, size_t i, FRun& Run, uint8_t Op, int Pp, uint8_t Rex, size_t Width, int Source)
	{
		auto Kind = ClassifySimd(Op, Pp);

		if (Kind == ESimd::None)
			return 0;

		FOperand M;

		if (!DecodeOperand(C, i, Rex, M))
			return 0;

		size_t End = i + M.Length;

		if (Kind == ESimd::Zero)
		{
			if (M.Mod != 3)
				return 0;

			auto First = Source >= 0 ? Source : M.Reg;
			Run.SetKnown(M.Reg, First == M.Rm);

			if (First == M.Rm)
			{
				Run.Vec[M.Reg].Size = 32;
				Run.Vec[M.Reg].Bytes.fill(0);
			}

			return End - Pos;
		}

		if (Kind == ESimd::Load)
		{
			if (M.Mod == 3)
			{
				Run.CopyVec(M.Reg, M.Rm);
				return End - Pos;
			}

			auto& Value = Run.Vec[M.Reg];
			Value.Size = Width;
			bool bLoaded = M.Base == RegRip && (*C.Read)(C.Address + End + M.Disp, Value.Bytes.data(), Width);
			Run.SetKnown(M.Reg, bLoaded);
			Run.Productive += bLoaded;
			return End - Pos;
		}

		if (M.Mod == 3)
		{
			Run.CopyVec(M.Rm, M.Reg);
			return End - Pos;
		}

		if (M.Base == RegRip || M.Base == RegAbsolute)
			return 0;

		auto& Value = Run.Vec[M.Reg];

		FStore Store;
		Store.Base = M.Base;
		Store.Disp = M.Disp;
		Store.Size = Width;
		Store.bKnown = Run.IsKnown(M.Reg) && Value.Size >= Width;
		memcpy(Store.Bytes.data(), Value.Bytes.data(), Width);

		Run.Stores.push_back(Store);
		Run.Productive += Store.bKnown;
		return End - Pos;
	}

	size_t Step(const FCodeView& C, size_t Pos, FRun& Run, bool& bReturn)
	{
		size_t i = Pos;
		bool b66 = false;
		bool bF3 = false;
		bool bF2 = false;
		bool bSegment = false;

		while (i < C.Size && i - Pos < 4)
		{
			auto Byte = C.Code[i];

			if (Byte == 0x66)
				b66 = true;
			else if (Byte == 0xF3)
				bF3 = true;
			else if (Byte == 0xF2)
				bF2 = true;
			else if (Byte == 0x64 || Byte == 0x65)
				bSegment = true;
			else
				break;

			i++;
		}

		if (i >= C.Size)
			return 0;

		uint8_t Rex = 0;

		if ((C.Code[i] & 0xF0) == 0x40)
		{
			Rex = C.Code[i++];

			if (i >= C.Size)
				return 0;
		}

		bool bWide = Rex & 8;
		bool bLegacy = b66 || bF3 || bF2;
		uint8_t Op = C.Code[i++];

		if (bSegment)
		{
			FOperand M;

			if (Op != 0x8B || bLegacy || !DecodeOperand(C, i, Rex, M) || M.Mod == 3)
				return 0;

			Run.SetGpr(M.Reg, std::nullopt);
			return i + M.Length - Pos;
		}

		if (Op == 0x90 && !bF3 && !bF2)
			return i - Pos;

		if (Op == 0x0F && bF3 && !b66 && !bF2 && i + 2 <= C.Size && C.Code[i] == 0x1E && (C.Code[i + 1] == 0xFA || C.Code[i + 1] == 0xFB))
			return i + 2 - Pos;

		if (Op == 0x0F && !bF3 && !bF2 && i < C.Size && C.Code[i] == 0x1F)
		{
			FOperand M;

			if (!DecodeOperand(C, i + 1, Rex, M))
				return 0;

			return i + 1 + M.Length - Pos;
		}

		if (!bLegacy && ((Op >= 0x50 && Op <= 0x57) || (Op >= 0x58 && Op <= 0x5F)))
		{
			if (Op >= 0x58)
				Run.SetGpr((Op - 0x58) | ((Rex & 1) << 3), std::nullopt);

			return i - Pos;
		}

		if ((Op == 0x83 || Op == 0x81 || Op == 0x8D) && !bLegacy)
		{
			FOperand M;

			if (!DecodeOperand(C, i, Rex, M))
				return 0;

			i += M.Length;

			if (Op == 0x8D)
			{
				if (M.Mod == 3)
					return 0;

				Run.SetGpr(M.Reg, std::nullopt);
				return i - Pos;
			}

			size_t ImmSize = Op == 0x83 ? 1 : 4;

			if (i + ImmSize > C.Size)
				return 0;

			i += ImmSize;

			if (M.Mod == 3)
			{
				if ((M.Reg & 7) != 7)
					Run.SetGpr(M.Rm, std::nullopt);

				return i - Pos;
			}

			if (M.Base == RegRip || M.Base == RegAbsolute)
				return 0;

			if ((M.Reg & 7) != 7)
			{
				FStore Store;
				Store.Base = M.Base;
				Store.Disp = M.Disp;
				Store.Size = bWide ? 8 : 4;
				Run.Stores.push_back(Store);
			}

			return i - Pos;
		}

		if (Op == 0xC3 && !Rex && !b66 && !bF2)
		{
			bReturn = true;
			return i - Pos;
		}

		if (Op >= 0xB8 && Op <= 0xBF && !bLegacy)
		{
			size_t ImmSize = bWide ? 8 : 4;

			if (i + ImmSize > C.Size)
				return 0;

			uint64_t Value = 0;
			memcpy(&Value, C.Code + i, ImmSize);
			Run.SetGpr((Op - 0xB8) | ((Rex & 1) << 3), Value);
			Run.Productive++;

			return i + ImmSize - Pos;
		}

		if ((Op == 0xC7 || Op == 0xC6) && !bF3 && !bF2 && !(b66 && (Op == 0xC6 || bWide)))
		{
			FOperand M;

			if (!DecodeOperand(C, i, Rex, M) || M.Mod == 3 || (M.Reg & 7) != 0 || M.Base == RegRip || M.Base == RegAbsolute)
				return 0;

			i += M.Length;

			size_t ImmSize = Op == 0xC6 ? 1 : b66 ? 2 : 4;

			if (i + ImmSize > C.Size)
				return 0;

			int64_t Value = 0;

			if (ImmSize == 1)
				Value = C.Code[i];
			else if (ImmSize == 2)
				Value = Get<uint16_t>(C.Code + i);
			else
				Value = Get<int32_t>(C.Code + i);

			i += ImmSize;

			FStore Store;
			Store.Base = M.Base;
			Store.Disp = M.Disp;
			Store.Size = ImmSize == 4 && bWide ? 8 : ImmSize;
			Store.bKnown = true;
			memcpy(Store.Bytes.data(), &Value, Store.Size);

			Run.Stores.push_back(Store);
			Run.Productive++;
			return i - Pos;
		}

		if ((Op == 0x89 || Op == 0x8B || Op == 0x31 || Op == 0x33) && !bLegacy)
		{
			FOperand M;

			if (!DecodeOperand(C, i, Rex, M))
				return 0;

			i += M.Length;

			auto Truncate = [bWide](std::optional<uint64_t> Value)
				{
					if (Value && !bWide)
						*Value &= 0xFFFFFFFF;

					return Value;
				};

			if (Op == 0x31 || Op == 0x33)
			{
				if (M.Mod != 3)
					return 0;

				auto Target = Op == 0x31 ? M.Rm : M.Reg;
				Run.SetGpr(Target, M.Reg == M.Rm ? std::optional<uint64_t>(0) : std::nullopt);
				return i - Pos;
			}

			if (M.Mod == 3)
			{
				if (Op == 0x89)
					Run.SetGpr(M.Rm, Truncate(Run.GetGpr(M.Reg)));
				else
					Run.SetGpr(M.Reg, Truncate(Run.GetGpr(M.Rm)));

				return i - Pos;
			}

			size_t Width = bWide ? 8 : 4;

			if (Op == 0x8B)
			{
				uint64_t Value = 0;

				if (M.Base == RegRip && (*C.Read)(C.Address + i + M.Disp, &Value, Width))
					Run.SetGpr(M.Reg, Value);
				else
					Run.SetGpr(M.Reg, std::nullopt);

				return i - Pos;
			}

			if (M.Base == RegRip || M.Base == RegAbsolute)
				return 0;

			auto Value = Run.GetGpr(M.Reg);

			FStore Store;
			Store.Base = M.Base;
			Store.Disp = M.Disp;
			Store.Size = Width;
			Store.bKnown = Value.has_value();

			if (Value)
				memcpy(Store.Bytes.data(), &*Value, Width);

			Run.Stores.push_back(Store);
			Run.Productive += Store.bKnown;
			return i - Pos;
		}

		if (Op == 0x0F && !bF2 && !(b66 && bF3))
		{
			if (i >= C.Size)
				return 0;

			uint8_t Op2 = C.Code[i++];
			return StepSimd(C, Pos, i, Run, Op2, b66 ? 1 : bF3 ? 2 : 0, Rex, 16, -1);
		}

		if ((Op == 0xC5 || Op == 0xC4) && !bLegacy && !Rex)
		{
			uint8_t VexRex = 0;
			int Source = 0;
			bool bLong = false;
			int Pp = 0;

			if (Op == 0xC5)
			{
				if (i >= C.Size)
					return 0;

				uint8_t B1 = C.Code[i++];

				if (!(B1 & 0x80))
					VexRex |= 4;

				Source = (~B1 >> 3) & 15;
				bLong = B1 & 4;
				Pp = B1 & 3;
			}
			else
			{
				if (i + 2 > C.Size)
					return 0;

				uint8_t B1 = C.Code[i++];
				uint8_t B2 = C.Code[i++];

				if ((B1 & 0x1F) != 1)
					return 0;

				if (!(B1 & 0x80))
					VexRex |= 4;

				if (!(B1 & 0x40))
					VexRex |= 2;

				if (!(B1 & 0x20))
					VexRex |= 1;

				Source = (~B2 >> 3) & 15;
				bLong = B2 & 4;
				Pp = B2 & 3;
			}

			if (i >= C.Size || Pp == 3)
				return 0;

			uint8_t Op2 = C.Code[i++];
			return StepSimd(C, Pos, i, Run, Op2, Pp, VexRex, bLong ? 32 : 16, Source);
		}

		return 0;
	}

	bool IsHashConstant(const Aes::FKey& Key)
	{
		static constexpr uint32_t Sha256[8] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A, 0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19 };
		static constexpr uint32_t Sha224[8] = { 0xC1059ED8, 0x367CD507, 0x3070DD17, 0xF70E5939, 0xFFC00B31, 0x68581511, 0x64F98FA7, 0xBEFA4FA4 };

		uint32_t Words[8];
		memcpy(Words, Key.data(), 32);

		return memcmp(Words, Sha256, 32) == 0 || memcmp(Words, Sha224, 32) == 0;
	}

	int CountDistinct(const uint8_t* Data, size_t Size = 32)
	{
		uint64_t Seen[4] = {};
		int Count = 0;

		for (size_t i = 0; i < Size; i++)
		{
			auto& Word = Seen[Data[i] >> 6];
			auto Bit = 1ull << (Data[i] & 63);

			if (!(Word & Bit))
			{
				Word |= Bit;
				Count++;
			}
		}

		return Count;
	}

	bool LooksRandom(const uint8_t* Data)
	{
		int Zeros = 0;
		int Printable = 0;

		for (int i = 0; i < 32; i++)
		{
			Zeros += Data[i] == 0;
			Printable += Data[i] >= 0x20 && Data[i] < 0x7F;
		}

		if (Zeros > 4 || Printable >= 28 || CountDistinct(Data) < 20)
			return false;

		int Steps[256] = {};

		for (int i = 1; i < 32; i++)
		{
			if (++Steps[static_cast<uint8_t>(Data[i] - Data[i - 1])] >= 8)
				return false;
		}

		for (int Lane = 0; Lane < 4; Lane++)
		{
			uint8_t Bytes[8];

			for (int i = 0; i < 8; i++)
				Bytes[i] = Data[i * 4 + Lane];

			if (CountDistinct(Bytes, 8) <= 3)
				return false;
		}

		return true;
	}

	void Evaluate(const FRun& Run, uintptr_t Address, std::vector<AesKey::FCandidate>& Out)
	{
		std::array<size_t, 18> KnownBytes{};
		uint64_t Seen[4] = {};

		auto IsZero = [](const FStore& Store)
			{
				return Store.bKnown && std::all_of(Store.Bytes.begin(), Store.Bytes.begin() + Store.Size, [](uint8_t Byte) { return Byte == 0; });
			};

		for (auto& Store : Run.Stores)
		{
			if (!Store.bKnown || IsZero(Store))
				continue;

			KnownBytes[Store.Base] += Store.Size;

			for (size_t k = 0; k < Store.Size; k++)
				Seen[Store.Bytes[k] >> 6] |= 1ull << (Store.Bytes[k] & 63);
		}

		int Distinct = 0;

		for (auto Word : Seen)
		{
			for (; Word; Word &= Word - 1)
				Distinct++;
		}

		if (Distinct < 8)
			return;

		std::vector<std::pair<int64_t, int>> Bytes;

		for (int Base = 0; Base < 16; Base++)
		{
			if (KnownBytes[Base] < 32)
				continue;

			Bytes.clear();

			for (auto& Store : Run.Stores)
			{
				if (Store.Base != Base || IsZero(Store))
					continue;

				for (size_t k = 0; k < Store.Size; k++)
					Bytes.emplace_back(Store.Disp + static_cast<int64_t>(k), Store.bKnown ? Store.Bytes[k] : -1);
			}

			std::stable_sort(Bytes.begin(), Bytes.end(), [](const auto& A, const auto& B) { return A.first < B.first; });

			std::vector<std::pair<int64_t, int>> Latest;

			for (size_t i = 0; i < Bytes.size(); i++)
			{
				if (i + 1 < Bytes.size() && Bytes[i + 1].first == Bytes[i].first)
					continue;

				Latest.push_back(Bytes[i]);
			}

			for (size_t Start = 0; Start < Latest.size();)
			{
				size_t End = Start + 1;
				bool bAllKnown = Latest[Start].second >= 0;

				while (End < Latest.size() && Latest[End].first == Latest[End - 1].first + 1)
				{
					bAllKnown &= Latest[End].second >= 0;
					End++;
				}

				if (End - Start == 32 && bAllKnown)
				{
					AesKey::FCandidate Candidate;
					Candidate.Address = Address;
					Candidate.bReturns = Run.bReturns;

					for (size_t k = 0; k < 32; k++)
						Candidate.Key[k] = static_cast<uint8_t>(Latest[Start + k].second);

					if (CountDistinct(Candidate.Key.data()) >= 8 && !IsHashConstant(Candidate.Key))
						Out.push_back(Candidate);
				}

				Start = End;
			}
		}
	}

	bool ReadFileAt(std::ifstream& File, uint64_t Offset, void* Out, size_t Size)
	{
		File.clear();
		File.seekg(static_cast<std::streamoff>(Offset));

		if (!File)
			return false;

		File.read(static_cast<char*>(Out), static_cast<std::streamsize>(Size));
		return static_cast<size_t>(File.gcount()) == Size;
	}

	std::optional<AesKey::FContainer> ReadPak(const std::filesystem::path& Path, std::ifstream& File, uint64_t FileSize)
	{
		auto TailSize = static_cast<size_t>(std::min<uint64_t>(FileSize, 1024));
		std::vector<uint8_t> Tail(TailSize);

		if (TailSize < 45 || !ReadFileAt(File, FileSize - TailSize, Tail.data(), TailSize))
			return std::nullopt;

		for (size_t Pos = TailSize - 24; Pos >= 1; Pos--)
		{
			if (Get<uint32_t>(&Tail[Pos]) != 0x5A6F12E1)
				continue;

			auto Version = Get<int32_t>(&Tail[Pos + 4]);
			auto IndexOffset = Get<int64_t>(&Tail[Pos + 8]);
			auto IndexSize = Get<int64_t>(&Tail[Pos + 16]);

			if (Version < 1 || Version > 64 || IndexOffset < 0 || IndexSize <= 0 ||
				static_cast<uint64_t>(IndexOffset) + static_cast<uint64_t>(IndexSize) > FileSize)
				continue;

			AesKey::FContainer Container;
			Container.Path = Path;
			Container.bEncrypted = Version >= 4 && Tail[Pos - 1] != 0;

			if (Version >= 7 && Pos >= 17)
				memcpy(Container.Guid.data(), &Tail[Pos - 17], 16);

			if (Container.bEncrypted && IndexSize >= 16)
				Container.bHasBlock = ReadFileAt(File, static_cast<uint64_t>(IndexOffset), Container.FirstBlock.data(), 16);

			return Container;
		}

		return std::nullopt;
	}

	std::optional<AesKey::FContainer> ReadToc(const std::filesystem::path& Path, std::ifstream& File, uint64_t FileSize)
	{
		uint8_t Header[144];

		if (FileSize < sizeof(Header) || !ReadFileAt(File, 0, Header, sizeof(Header)) || memcmp(Header, "-==--==--==--==-", 16) != 0)
			return std::nullopt;

		auto Version = Header[16];
		uint64_t HeaderSize = Get<uint32_t>(Header + 20);
		uint64_t EntryCount = Get<uint32_t>(Header + 24);
		uint64_t BlockCount = Get<uint32_t>(Header + 28);
		uint64_t BlockEntrySize = Get<uint32_t>(Header + 32);
		uint64_t MethodCount = Get<uint32_t>(Header + 36);
		uint64_t MethodLength = Get<uint32_t>(Header + 40);
		uint64_t DirectoryIndexSize = Get<uint32_t>(Header + 48);
		auto Flags = Header[80];
		uint64_t SeedCount = Get<uint32_t>(Header + 84);
		uint64_t OverflowCount = Get<uint32_t>(Header + 96);

		AesKey::FContainer Container;
		Container.Path = Path;
		Container.bIoStore = true;
		Container.bEncrypted = Flags & 2;
		memcpy(Container.Guid.data(), Header + 64, 16);

		bool bSigned = Flags & 4;
		bool bIndexed = Flags & 8;

		if (!Container.bEncrypted || !bIndexed || Version < 2 || DirectoryIndexSize < 16)
			return Container;

		uint64_t Offset = HeaderSize + EntryCount * 12 + EntryCount * 10;

		if (Version >= 4)
			Offset += SeedCount * 4;

		if (Version >= 5)
			Offset += OverflowCount * 4;

		Offset += BlockCount * BlockEntrySize + MethodCount * MethodLength;

		if (bSigned)
		{
			int32_t HashSize = 0;

			if (!ReadFileAt(File, Offset, &HashSize, 4) || HashSize < 0 || HashSize > 4096)
				return Container;

			Offset += 4 + static_cast<uint64_t>(HashSize) * 2 + BlockCount * 20;
		}

		if (Offset + DirectoryIndexSize <= FileSize)
			Container.bHasBlock = ReadFileAt(File, Offset, Container.FirstBlock.data(), 16);

		return Container;
	}

	std::string Lower(std::string Text)
	{
		for (auto& C : Text)
			C = static_cast<char>(tolower(static_cast<unsigned char>(C)));

		return Text;
	}

	std::string Describe(const AesKey::FContainer& Container)
	{
		return Memory::PathToUtf8(Container.Path.filename());
	}
}

std::vector<AesKey::FCandidate> AesKey::ScanCode(const uint8_t* Code, size_t Size, size_t Owned, uintptr_t Address, const FReadFunction& Read)
{
	std::vector<FCandidate> Ret;

	FCodeView View{ Code, Size, Address, &Read };
	std::vector<uint8_t> Covered(Size, 0);

	static const auto Starts = []
		{
			std::array<bool, 256> Ret{};

			for (int Byte : { 0x0F, 0x31, 0x33, 0x64, 0x65, 0x66, 0x81, 0x83, 0x89, 0x8B, 0x8D, 0x90, 0xC4, 0xC5, 0xC7, 0xF2, 0xF3 })
				Ret[Byte] = true;

			for (int Byte = 0x40; Byte <= 0x5F; Byte++)
				Ret[Byte] = true;

			for (int Byte = 0xB8; Byte <= 0xBF; Byte++)
				Ret[Byte] = true;

			return Ret;
		}();

	FRun Run;

	for (size_t Pos = 0; Pos < Owned && Pos < Size; Pos++)
	{
		if (Covered[Pos] || !Starts[Code[Pos]])
			continue;

		Run.Reset();
		size_t Current = Pos;
		int Count = 0;
		int LastProductive = 0;
		int Productive = 0;

		while (Count < MaxInstructions && Current < Size && Count - LastProductive <= MaxIdleInstructions)
		{
			bool bReturn = false;
			auto Length = Step(View, Current, Run, bReturn);

			if (!Length)
				break;

			if (Count)
				Covered[Current] = 1;

			Current += Length;
			Count++;

			if (Run.Productive != Productive)
			{
				Productive = Run.Productive;
				LastProductive = Count;
			}

			if (bReturn)
			{
				Run.bReturns = true;
				break;
			}
		}

		if (Count >= 2)
			Evaluate(Run, Address + Pos, Ret);
	}

	return Ret;
}

std::optional<AesKey::FContainer> AesKey::ReadContainer(const std::filesystem::path& Path)
{
	std::error_code Error;
	auto FileSize = std::filesystem::file_size(Path, Error);

	if (Error)
		return std::nullopt;

	std::ifstream File(Path, std::ios::binary);

	if (!File)
		return std::nullopt;

	auto Extension = Lower(Memory::PathToUtf8(Path.extension()));

	if (Extension == ".pak")
		return ReadPak(Path, File, FileSize);

	if (Extension == ".utoc")
		return ReadToc(Path, File, FileSize);

	return std::nullopt;
}

std::vector<AesKey::FContainer> AesKey::ReadContainers(const std::filesystem::path& Directory)
{
	std::vector<std::filesystem::path> Files;
	std::error_code Error;

	auto Options = std::filesystem::directory_options::skip_permission_denied;

	for (auto It = std::filesystem::recursive_directory_iterator(Directory, Options, Error);
		!Error && It != std::filesystem::recursive_directory_iterator(); It.increment(Error))
	{
		if (Files.size() >= 5000)
			break;

		auto Extension = Lower(Memory::PathToUtf8(It->path().extension()));

		if ((Extension == ".pak" || Extension == ".utoc") && It->is_regular_file(Error))
			Files.push_back(It->path());
	}

	std::sort(Files.begin(), Files.end());

	std::vector<FContainer> Ret;

	for (auto& File : Files)
	{
		if (auto Container = ReadContainer(File))
			Ret.push_back(std::move(*Container));
	}

	return Ret;
}

std::filesystem::path AesKey::GuessPakDirectory(const std::filesystem::path& Executable)
{
	std::error_code Error;

	auto HasPaks = [&](const std::filesystem::path& Project)
		{
			auto Paks = Project / "Content" / "Paks";
			return std::filesystem::is_directory(Paks, Error) ? Paks : std::filesystem::path();
		};

	auto Directory = Executable.parent_path();

	for (int Depth = 0; Depth < 4 && !Directory.empty() && Directory.has_relative_path(); Depth++)
	{
		if (Lower(Memory::PathToUtf8(Directory.filename())) == "binaries")
		{
			auto Project = Directory.parent_path();

			if (auto Paks = HasPaks(Project); !Paks.empty())
				return Paks;

			auto Root = Project.parent_path();

			for (auto& Entry : std::filesystem::directory_iterator(Root, std::filesystem::directory_options::skip_permission_denied, Error))
			{
				if (Lower(Memory::PathToUtf8(Entry.path().filename())) == "engine")
					continue;

				if (auto Paks = HasPaks(Entry.path()); !Paks.empty())
					return Paks;
			}

			break;
		}

		Directory = Directory.parent_path();
	}

	return {};
}

bool AesKey::IsMountPoint(const Aes::FBlock& Block)
{
	auto Length = Get<int32_t>(Block.data());

	if (Length > 1 && Length <= 1024)
	{
		auto Chars = std::min<size_t>(static_cast<size_t>(Length) - 1, 12);

		for (size_t k = 0; k < Chars; k++)
		{
			if (Block[4 + k] < 0x20 || Block[4 + k] > 0x7E)
				return false;
		}

		return Length > 12 || Block[4 + Length - 1] == 0;
	}

	if (Length < -1 && Length >= -1024)
	{
		auto Count = static_cast<size_t>(-Length);
		auto Chars = std::min<size_t>(Count - 1, 6);

		for (size_t k = 0; k < Chars; k++)
		{
			auto Char = Get<uint16_t>(Block.data() + 4 + k * 2);

			if (Char < 0x20 || Char > 0x7E)
				return false;
		}

		return Count > 6 || Get<uint16_t>(Block.data() + 4 + (Count - 1) * 2) == 0;
	}

	return false;
}

bool AesKey::Decrypts(const Aes::FKey& Key, const FContainer& Container)
{
	if (!Container.bHasBlock)
		return false;

	return IsMountPoint(Aes::FCipher(Key.data()).Decrypt(Container.FirstBlock));
}

std::string AesKey::GuidToString(const FGuid& Guid)
{
	uint32_t Parts[4];
	memcpy(Parts, Guid.data(), 16);

	return std::format("{:08X}{:08X}{:08X}{:08X}", Parts[0], Parts[1], Parts[2], Parts[3]);
}

bool AesKey::IsZeroGuid(const FGuid& Guid)
{
	return std::all_of(Guid.begin(), Guid.end(), [](uint8_t Byte) { return Byte == 0; });
}

AesKey::FResult AesKey::Find(const std::filesystem::path& PakDirectory)
{
	FResult Result;
	Result.PakDirectory = PakDirectory;

	if (!PakDirectory.empty())
		Result.Containers = ReadContainers(PakDirectory);

	std::vector<const FContainer*> MainTargets;
	std::map<FGuid, std::vector<const FContainer*>> GuidTargets;
	size_t Encrypted = 0;

	for (auto& Container : Result.Containers)
	{
		if (!Container.bEncrypted)
			continue;

		Encrypted++;

		if (!Container.bHasBlock)
			continue;

		if (IsZeroGuid(Container.Guid))
			MainTargets.push_back(&Container);
		else
			GuidTargets[Container.Guid].push_back(&Container);
	}

	if (!PakDirectory.empty())
	{
		UE_LOG("Found %zu pak and utoc files, %zu of them are encrypted.", Result.Containers.size(), Encrypted);

		for (auto& [Guid, Targets] : GuidTargets)
			UE_LOG("%s uses the dynamic key %s.", Describe(*Targets[0]).c_str(), GuidToString(Guid).c_str());
	}

	std::vector<FCandidate> Candidates;

	std::mutex ReadMutex;

	FReadFunction Read = [&](uintptr_t Address, void* Out, size_t Size)
		{
			std::lock_guard Lock(ReadMutex);
			return Memory::ReadRaw(Address, Out, Size);
		};

	struct FChunk
	{
		uintptr_t Start = 0;
		std::vector<uint8_t> Data;
		size_t Owned = 0;
		std::vector<FCandidate> Found;
	};

	auto Threads = std::clamp<size_t>(std::thread::hardware_concurrency(), 1, 16);
	std::vector<FChunk> Batch;

	auto ScanBatch = [&]
		{
			std::atomic<size_t> Next = 0;
			std::vector<std::thread> Workers;

			for (size_t t = 0; t < std::min(Threads, Batch.size()); t++)
			{
				Workers.emplace_back([&]
					{
						for (size_t i = Next++; i < Batch.size(); i = Next++)
							Batch[i].Found = ScanCode(Batch[i].Data.data(), Batch[i].Data.size(), Batch[i].Owned, Batch[i].Start, Read);
					});
			}

			for (auto& Worker : Workers)
				Worker.join();

			for (auto& Chunk : Batch)
				Candidates.insert(Candidates.end(), Chunk.Found.begin(), Chunk.Found.end());

			Batch.clear();
		};

	for (auto& Section : Memory::GetSections())
	{
		if (!Section.IsExecutable())
			continue;

		Memory::ForEachSectionChunk(Section, 2048, [&](uintptr_t Start, const uint8_t* Data, size_t Size, size_t Owned)
			{
				Batch.push_back({ Start, std::vector<uint8_t>(Data, Data + Size), Owned, {} });

				if (Batch.size() >= Threads * 2)
					ScanBatch();
			});
	}

	ScanBatch();

	std::stable_sort(Candidates.begin(), Candidates.end(), [](const FCandidate& A, const FCandidate& B) { return A.bReturns > B.bReturns; });

	{
		std::vector<FCandidate> Unique;

		for (auto& Candidate : Candidates)
		{
			if (std::none_of(Unique.begin(), Unique.end(), [&](const FCandidate& U) { return U.Key == Candidate.Key; }))
				Unique.push_back(Candidate);
		}

		Candidates = std::move(Unique);
	}

	UE_LOG("Found %zu key-like constants in the code of the game.", Candidates.size());

	auto FindKey = [&](const FGuid& Guid) -> FFoundKey*
		{
			for (auto& Key : Result.Keys)
			{
				if (Key.bVerified && Key.Guid == Guid)
					return &Key;
			}

			return nullptr;
		};

	auto TryKey = [&](const Aes::FKey& Key, const std::vector<const FContainer*>& Targets, const FGuid& Guid, const char* Source)
		{
			if (Targets.empty() || FindKey(Guid) || !Decrypts(Key, *Targets[0]))
				return false;

			FFoundKey Found;
			Found.Key = Key;
			Found.Guid = Guid;
			Found.bVerified = true;
			Found.Source = Source;

			for (auto Target : Targets)
				Found.Containers += Decrypts(Key, *Target);

			Result.Keys.push_back(Found);
			return true;
		};

	for (auto& Candidate : Candidates)
	{
		TryKey(Candidate.Key, MainTargets, {}, "code");

		for (auto& [Guid, Targets] : GuidTargets)
			TryKey(Candidate.Key, Targets, Guid, "code");
	}

	if (!MainTargets.empty() && !FindKey({}))
	{
		UE_LOG("None of them decrypts the pak files, searching the data of the game...");

		for (auto& Section : Memory::GetSections())
		{
			if (Section.IsExecutable() || FindKey({}))
				continue;

			Memory::ForEachSectionChunk(Section, 32, [&](uintptr_t, const uint8_t* Data, size_t Size, size_t Owned)
				{
					for (size_t Pos = 0; Pos + 32 <= Size && Pos < Owned; Pos += 4)
					{
						if (!LooksRandom(Data + Pos))
							continue;

						Aes::FKey Key;
						memcpy(Key.data(), Data + Pos, 32);

						if (TryKey(Key, MainTargets, {}, "data"))
							return;
					}
				});
		}
	}

	std::vector<FGuid> MissingGuids;

	for (auto& [Guid, Targets] : GuidTargets)
	{
		if (!FindKey(Guid))
			MissingGuids.push_back(Guid);
	}

	bool bNeedMain = !MainTargets.empty() && !FindKey({});

	if (bNeedMain || !MissingGuids.empty())
	{
		auto Regions = Memory::Get().GetReadableRegions();
		uint64_t Total = 0;

		for (auto& Region : Regions)
			Total += Region.End - Region.Start;

		if (Total >= 1024ull * 1024 * 1024)
			UE_LOG("Searching %.1f GB of memory of the game for loaded keys...", static_cast<double>(Total) / (1024.0 * 1024.0 * 1024.0));
		else
			UE_LOG("Searching %.0f MB of memory of the game for loaded keys...", static_cast<double>(Total) / (1024.0 * 1024.0));

		constexpr size_t ChunkSize = 4 * 1024 * 1024;
		std::vector<uint8_t> Buffer;

		for (auto& Region : Regions)
		{
			for (uintptr_t ChunkStart = Region.Start; ChunkStart < Region.End; ChunkStart += ChunkSize)
			{
				bNeedMain = bNeedMain && !FindKey({});

				std::erase_if(MissingGuids, [&](const FGuid& Guid) { return FindKey(Guid) != nullptr; });

				if (!bNeedMain && MissingGuids.empty())
					break;

				auto Base = std::max<uintptr_t>(Region.Start, ChunkStart >= 16 ? ChunkStart - 16 : 0);
				auto End = std::min<uintptr_t>(Region.End, ChunkStart + ChunkSize + 32);

				Buffer.resize(End - Base);

				if (!Memory::ReadRaw(Base, Buffer.data(), Buffer.size()))
					continue;

				auto Data = Buffer.data();

				for (auto Address = std::max<uintptr_t>(ChunkStart, Base + 16); Address + 32 <= End && Address < ChunkStart + ChunkSize; Address += 4)
				{
					auto Pos = Address - Base;
					auto Guid = Data + Pos - 16;

					if (bNeedMain && Get<uint64_t>(Guid) == 0 && Get<uint64_t>(Guid + 8) == 0 && Get<uint32_t>(Data + Pos) != 0 &&
						CountDistinct(Data + Pos) >= 12)
					{
						Aes::FKey Key;
						memcpy(Key.data(), Data + Pos, 32);

						if (TryKey(Key, MainTargets, {}, "memory"))
							bNeedMain = false;
					}

					for (auto& Missing : MissingGuids)
					{
						if (Get<uint32_t>(Guid) != Get<uint32_t>(Missing.data()) || memcmp(Guid, Missing.data(), 16) != 0)
							continue;

						Aes::FKey Key;
						memcpy(Key.data(), Data + Pos, 32);
						TryKey(Key, GuidTargets[Missing], Missing, "memory");
					}
				}
			}
		}
	}

	if (!FindKey({}))
	{
		size_t Added = 0;

		for (auto& Candidate : Candidates)
		{
			if (Added >= 10)
				break;

			if (!LooksRandom(Candidate.Key.data()))
				continue;

			if (std::any_of(Result.Keys.begin(), Result.Keys.end(), [&](const FFoundKey& K) { return K.Key == Candidate.Key; }))
				continue;

			FFoundKey Found;
			Found.Key = Candidate.Key;
			Found.Source = "code";
			Result.Keys.push_back(Found);
			Added++;
		}
	}

	return Result;
}
