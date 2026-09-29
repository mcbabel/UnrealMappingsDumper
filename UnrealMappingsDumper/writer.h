#pragma once

class StreamWriter
{
    std::stringstream m_Stream;

public:

    FORCEINLINE std::stringstream& GetBuffer()
    {
        return m_Stream;
    }

    FORCEINLINE void WriteString(std::string_view String)
    {
        m_Stream.write(String.data(), String.size());
    }

    FORCEINLINE void Write(const void* Input, size_t Size)
    {
        m_Stream.write(static_cast<const char*>(Input), Size);
    }

    uint32_t Size()
    {
        auto Pos = m_Stream.tellp();
        m_Stream.seekp(0, std::ios::end);
        auto Ret = m_Stream.tellp();
        m_Stream.seekp(Pos, std::ios::beg);

        return static_cast<uint32_t>(Ret);
    }

    template <typename T>
    FORCEINLINE void Write(T Input)
    {
        Write(&Input, sizeof(T));
    }
};

class FileWriter
{
    FILE* m_File = nullptr;
    bool m_bFailed = false;

public:

    explicit FileWriter(const std::filesystem::path& FileName)
    {
#ifdef _WIN32
        if (_wfopen_s(&m_File, FileName.c_str(), L"wb") != 0)
            m_File = nullptr;
#else
        m_File = std::fopen(FileName.c_str(), "wb");
#endif

        m_bFailed = m_File == nullptr;
    }

    ~FileWriter()
    {
        Close();
    }

    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;

    bool IsOpen() const
    {
        return m_File != nullptr;
    }

    bool HasFailed() const
    {
        return m_bFailed;
    }

    bool Close()
    {
        if (m_File)
        {
            if (std::fclose(m_File) != 0)
                m_bFailed = true;

            m_File = nullptr;
        }

        return !m_bFailed;
    }

    void Write(const void* Input, size_t Size)
    {
        if (!m_File)
        {
            m_bFailed = true;
            return;
        }

        if (Size && std::fwrite(Input, Size, 1, m_File) != 1)
            m_bFailed = true;
    }

    template <typename T>
    FORCEINLINE void Write(T Input)
    {
        Write(&Input, sizeof(T));
    }
};
