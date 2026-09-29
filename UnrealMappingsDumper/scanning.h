#pragma once

namespace Scanner
{
	std::vector<uintptr_t> FindPattern(std::string_view Signature, size_t MaxResults = SIZE_MAX);

	uintptr_t FindPatternInRange(std::string_view Signature, uintptr_t Start, size_t Range);

	std::vector<uintptr_t> FindStringRefs(std::wstring_view String);
	std::vector<uintptr_t> FindStringRefs(std::string_view String);

	uintptr_t ResolveRelative(uintptr_t Instruction, int DisplacementOffset, int InstructionLength);
}

struct IScanObject
{
	virtual ~IScanObject() = default;

	virtual std::vector<uintptr_t> FindAll() = 0;

	virtual std::string Describe() const = 0;
};

struct PatternScanObject : public IScanObject
{
	PatternScanObject(
		std::string sig,
		int relativeAddressOffset = 0,
		bool relative = true,
		int resultOffset = 0)
		:
		Sig(sig),
		bRelative(relative),
		RelativeAddressOffset(relativeAddressOffset),
		ResultOffset(resultOffset)
	{
	}

	std::string Sig;
	bool bRelative;
	int RelativeAddressOffset;
	int ResultOffset;

	std::vector<uintptr_t> FindAll() override;
	std::string Describe() const override;
};

template <typename StrType = std::wstring>
struct StringRefScanObject : public IScanObject
{
	StringRefScanObject(
		StrType stringRef,
		bool scanBackwards = false,
		int offset = 0,
		bool relative = true,
		uint8_t opcodeToFind = 0,
		int opcodeFindsToSkip = 0)
		:
		StringRef(stringRef),
		OpcodeToFind(opcodeToFind),
		OpcodeFindsToSkip(opcodeFindsToSkip),
		bRelative(relative),
		Offset(offset),
		bScanBackwards(scanBackwards)
	{
	}

	StrType StringRef;
	uint8_t OpcodeToFind;
	int OpcodeFindsToSkip;
	bool bRelative;
	int Offset;
	bool bScanBackwards;

	std::vector<uintptr_t> FindAll() override;
	std::string Describe() const override;
};

struct StringRefCallScanObject : public IScanObject
{
	StringRefCallScanObject(std::wstring stringRef, std::vector<std::string> callSigs, size_t range = 0x50)
		: StringRef(std::move(stringRef)), CallSigs(std::move(callSigs)), Range(range)
	{
	}

	std::wstring StringRef;
	std::vector<std::string> CallSigs;
	size_t Range;

	std::vector<uintptr_t> FindAll() override;
	std::string Describe() const override;
};
