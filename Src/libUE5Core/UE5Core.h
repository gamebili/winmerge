#pragma once

#include <string>

namespace UE5Core
{
	bool IsSupportedFile(const std::wstring& path);
	bool UnpackFolder(const std::wstring& sourcePath, const std::wstring& destinationFolder, std::wstring* errorMessage = nullptr);
}
