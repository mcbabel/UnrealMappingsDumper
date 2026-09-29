#include "pch.h"

#include "scanning.h"
#include "memory.h"

namespace
{
	struct ParsedPattern
	{
		std::vector<uint8_t> Bytes;
		std::vector<bool> Mask;

		size_t Size() const { return Bytes.size(); }
	};

	ParsedPattern ParsePattern(std::string_view Signature)
	{
		ParsedPattern Ret;

		size_t i = 0;
		while (i < Signature.size())
		{
			if (Signature[i] == ' ')
			{
				i++;
				continue;
			}

			if (Signature[i] == '?')
			{
				Ret.Bytes.push_back(0);
				Ret.Mask.push_back(false);

				while (i < Signature.size() && Signature[i] == '?')
					i++;

				continue;
			}

			auto Token = Signature.substr(i, 2);
			Ret.Bytes.push_back(static_cast<uint8_t>(std::stoul(std::string(Token), nullptr, 16)));
			Ret.Mask.push_back(true);
			i += Token.size();
		}

		return Ret;
	}

	bool MatchesAt(const uint8_t* Data, const ParsedPattern& Pattern)
	{
		for (size_t i = 0; i < Pattern.Size(); i++)
		{
			if (Pattern.Mask[i] && Data[i] != Pattern.Bytes[i])
				return false;
		}

		return true;
	}

	void ScanBuffer(uintptr_t Start, const uint8_t* Data, size_t Size, size_t Owned, const ParsedPattern& Pattern, std::vector<uintptr_t>& Out, size_t MaxResults)
	{
		if (Pattern.Size() == 0 || Size < Pattern.Size())
			return;

		auto Last = std::min(Size - Pattern.Size(), Owned ? Owned - 1 : 0);

		if (Pattern.Mask[0])
		{
			auto First = Pattern.Bytes[0];
			size_t i = 0;

			while (i <= Last)
			{
				auto Found = static_cast<const uint8_t*>(memchr(Data + i, First, Last - i + 1));

				if (!Found)
					break;

				i = Found - Data;

				if (MatchesAt(Found, Pattern))
				{
					Out.push_back(Start + i);

					if (Out.size() >= MaxResults)
						return;
				}

				i++;
			}

			return;
		}

		for (size_t i = 0; i <= Last; i++)
		{
			if (MatchesAt(Data + i, Pattern))
			{
				Out.push_back(Start + i);

				if (Out.size() >= MaxResults)
					return;
			}
		}
	}

	std::vector<uintptr_t> FindBytesInModule(const uint8_t* Bytes, size_t Size)
	{
		ParsedPattern Pattern;
		Pattern.Bytes.assign(Bytes, Bytes + Size);
		Pattern.Mask.assign(Size, true);

		std::vector<uintptr_t> Ret;

		for (auto& Section : Memory::GetSections())
		{
			Memory::ForEachSectionChunk(Section, Size, [&](uintptr_t Start, const uint8_t* Data, size_t ChunkSize, size_t Owned)
				{
					ScanBuffer(Start, Data, ChunkSize, Owned, Pattern, Ret, SIZE_MAX);
				});
		}

		return Ret;
	}

	std::vector<uintptr_t> FindLeaRefs(const std::vector<uintptr_t>& Targets)
	{
		std::vector<uintptr_t> Ret;

		if (Targets.empty())
			return Ret;

		for (auto& Section : Memory::GetSections())
		{
			if (!Section.IsExecutable())
				continue;

			Memory::ForEachSectionChunk(Section, 7, [&](uintptr_t Start, const uint8_t* Data, size_t Size, size_t Owned)
				{
					for (size_t i = 0; i < Owned && i + 7 <= Size; i++)
					{
						if ((Data[i] != 0x48 && Data[i] != 0x4C) || Data[i + 1] != 0x8D || (Data[i + 2] & 0xC7) != 0x05)
							continue;

						int32_t Disp = 0;
						memcpy(&Disp, Data + i + 3, sizeof(Disp));

						auto Target = Start + i + 7 + Disp;

						if (std::find(Targets.begin(), Targets.end(), Target) != Targets.end())
							Ret.push_back(Start + i);
					}
				});
		}

		return Ret;
	}
}

std::vector<uintptr_t> Scanner::FindPattern(std::string_view Signature, size_t MaxResults)
{
	auto Pattern = ParsePattern(Signature);
	std::vector<uintptr_t> Ret;

	for (auto& Section : Memory::GetSections())
	{
		if (!Section.IsExecutable() || Ret.size() >= MaxResults)
			continue;

		Memory::ForEachSectionChunk(Section, Pattern.Size(), [&](uintptr_t Start, const uint8_t* Data, size_t Size, size_t Owned)
			{
				if (Ret.size() < MaxResults)
					ScanBuffer(Start, Data, Size, Owned, Pattern, Ret, MaxResults);
			});
	}

	return Ret;
}

uintptr_t Scanner::FindPatternInRange(std::string_view Signature, uintptr_t Start, size_t Range)
{
	auto Data = Memory::ReadBytes(Start, Range);

	if (Data.empty())
		return 0;

	auto Pattern = ParsePattern(Signature);
	std::vector<uintptr_t> Ret;

	ScanBuffer(Start, Data.data(), Data.size(), Data.size(), Pattern, Ret, 1);

	return Ret.empty() ? 0 : Ret[0];
}

std::vector<uintptr_t> Scanner::FindStringRefs(std::wstring_view String)
{
	std::vector<char16_t> Terminated(String.begin(), String.end());
	Terminated.push_back(0);

	return FindLeaRefs(FindBytesInModule(reinterpret_cast<const uint8_t*>(Terminated.data()), Terminated.size() * sizeof(char16_t)));
}

std::vector<uintptr_t> Scanner::FindStringRefs(std::string_view String)
{
	std::string Terminated(String);
	auto Bytes = reinterpret_cast<const uint8_t*>(Terminated.c_str());

	return FindLeaRefs(FindBytesInModule(Bytes, Terminated.size() + 1));
}

uintptr_t Scanner::ResolveRelative(uintptr_t Instruction, int DisplacementOffset, int InstructionLength)
{
	int32_t Disp = 0;

	if (!Memory::Read(Instruction + DisplacementOffset, Disp))
		return 0;

	return Instruction + InstructionLength + Disp;
}

std::vector<uintptr_t> PatternScanObject::FindAll()
{
	std::vector<uintptr_t> Ret;

	for (auto Match : Scanner::FindPattern(Sig, 64))
	{
		auto Addy = Match;

		if (bRelative)
		{
			Addy = Scanner::ResolveRelative(Match, RelativeAddressOffset, RelativeAddressOffset + 4);

			if (!Addy)
				continue;
		}

		Addy += ResultOffset;

		if (std::find(Ret.begin(), Ret.end(), Addy) == Ret.end())
			Ret.push_back(Addy);
	}

	return Ret;
}

std::string PatternScanObject::Describe() const
{
	return std::format("pattern \"{}\"", Sig);
}

template <typename T>
static std::wstring ToWide(const T& Str)
{
	if constexpr (std::is_convertible_v<T, std::wstring_view>)
	{
		return std::wstring(std::wstring_view(Str));
	}
	else
	{
		std::string_view View(Str);
		return std::wstring(View.begin(), View.end());
	}
}

template <typename T>
std::vector<uintptr_t> StringRefScanObject<T>::FindAll()
{
	std::vector<uintptr_t> Refs;

	if constexpr (std::is_convertible_v<T, std::wstring_view>)
		Refs = Scanner::FindStringRefs(std::wstring_view(StringRef));
	else
		Refs = Scanner::FindStringRefs(std::string_view(StringRef));

	std::vector<uintptr_t> Ret;

	for (auto Ref : Refs)
	{
		auto Addy = Ref;

		if (OpcodeToFind)
		{
			int Skipped = 0;
			bool bFound = false;

			for (int i = 1; i < 2048; i++)
			{
				auto Current = bScanBackwards ? Ref - i : Ref + i;
				uint8_t Byte = 0;

				if (!Memory::Read(Current, Byte))
					break;

				if (Byte != OpcodeToFind)
					continue;

				if (Skipped++ < OpcodeFindsToSkip)
					continue;

				Addy = Current;
				bFound = true;
				break;
			}

			if (!bFound)
				continue;
		}

		if (bRelative)
		{
			Addy = Scanner::ResolveRelative(Addy, Offset, Offset + 4);

			if (!Addy)
				continue;
		}
		else
		{
			Addy += Offset;
		}

		if (std::find(Ret.begin(), Ret.end(), Addy) == Ret.end())
			Ret.push_back(Addy);
	}

	return Ret;
}

template <typename T>
std::string StringRefScanObject<T>::Describe() const
{
	auto Wide = ToWide(StringRef);
	std::string Narrow;

	for (size_t i = 0; i < std::min<size_t>(Wide.size(), 48); i++)
		Narrow += static_cast<char>(Wide[i]);

	return std::format("string ref \"{}\"", Narrow);
}

template struct StringRefScanObject<std::string>;
template struct StringRefScanObject<std::wstring>;
template struct StringRefScanObject<const char*>;
template struct StringRefScanObject<const wchar_t*>;

std::vector<uintptr_t> StringRefCallScanObject::FindAll()
{
	std::vector<uintptr_t> Ret;

	for (auto Ref : Scanner::FindStringRefs(std::wstring_view(StringRef)))
	{
		for (auto& Sig : CallSigs)
		{
			auto Match = Scanner::FindPatternInRange(Sig, Ref, Range);

			if (!Match)
				continue;

			auto SigLength = ParsePattern(Sig).Size();
			auto Call = Match + SigLength - 1;

			auto Target = Scanner::ResolveRelative(Call, 1, 5);

			if (Target && Memory::IsInModule(Target) && std::find(Ret.begin(), Ret.end(), Target) == Ret.end())
				Ret.push_back(Target);

			break;
		}
	}

	return Ret;
}

std::string StringRefCallScanObject::Describe() const
{
	std::string Narrow;

	for (auto C : StringRef)
		Narrow += static_cast<char>(C);

	return std::format("call after string ref \"{}\"", Narrow);
}
