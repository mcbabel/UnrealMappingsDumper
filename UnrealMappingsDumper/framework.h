#pragma once

#define _SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows Header Files
#include <windows.h>
#else
#define FORCEINLINE inline __attribute__((always_inline))
#endif
#include <stdio.h>
#include <cstring>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <functional>
#include <sstream>
#include <vector>
#include <filesystem>
#include <optional>
#include <fstream>
#include <format>
#include <mutex>
#include <array>
#include <unordered_map>
#include <algorithm>
#include <chrono>
#include <memory>
#include "../Dependencies/parallel_hashmap/phmap.h"