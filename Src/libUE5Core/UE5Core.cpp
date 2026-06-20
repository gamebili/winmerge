#include "UE5Core.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

namespace UE5Core
{
namespace
{
	constexpr int32_t PackageFileTag = 0x9E2A83C1;
	constexpr int32_t PackageFileTagSwapped = 0xC1832A9E;
	constexpr int32_t LegacyPackageVersionCurrent = -9;
	constexpr int32_t UE5InitialVersion = 1000;
	constexpr int32_t UE5NamesReferencedFromExportData = 1001;
	constexpr int32_t UE5PayloadToc = 1002;
	constexpr int32_t UE5AddSoftObjectPathList = 1008;
	constexpr int32_t UE5DataResources = 1009;
	constexpr int32_t UE5MetadataSerializationOffset = 1014;
	constexpr int32_t UE5VerseCells = 1015;
	constexpr int32_t UE5PackageSavedHash = 1016;
	constexpr uint32_t PakFileMagic = 0x5A6F12E1;
	constexpr int32_t PakFileVersionInitial = 1;
	constexpr int32_t PakFileVersionCompressionEncryption = 3;
	constexpr int32_t PakFileVersionIndexEncryption = 4;
	constexpr int32_t PakFileVersionRelativeChunkOffsets = 5;
	constexpr int32_t PakFileVersionEncryptionKeyGuid = 7;
	constexpr int32_t PakFileVersionFNameBasedCompressionMethod = 8;
	constexpr int32_t PakFileVersionFrozenIndex = 9;
	constexpr int32_t PakFileVersionPathHashIndex = 10;
	constexpr int32_t PakFileVersionLatest = 11;
	constexpr int32_t PakCompressionMethodNameLen = 32;
	constexpr int32_t PakMaxNumCompressionMethods = 5;
	constexpr int32_t AesBlockSize = 16;
	constexpr size_t PakFullEntryTreeRecordLimit = 10000;

	std::wstring ToLower(std::wstring value)
	{
		std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
		return value;
	}

	std::wstring ExtensionOf(const std::wstring& path)
	{
		return ToLower(std::filesystem::path(path).extension().wstring());
	}

	bool IsPackageExtension(const std::wstring& extension)
	{
		return extension == L".uasset" || extension == L".umap";
	}

	bool IsIoStoreExtension(const std::wstring& extension)
	{
		return extension == L".utoc" || extension == L".ucas";
	}

	bool IsPayloadExtension(const std::wstring& extension)
	{
		return extension == L".uexp" || extension == L".exp";
	}

	std::string WideToUtf8(const std::wstring& value)
	{
		if (value.empty())
			return {};

		int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
		if (required <= 0)
			return {};

		std::string result(required, '\0');
		WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
		return result;
	}

	std::wstring Utf8ToWide(std::string_view value)
	{
		if (value.empty())
			return {};

		int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
		if (required <= 0)
			return {};

		std::wstring result(required, L'\0');
		MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
		return result;
	}

	std::string JsonEscape(const std::wstring& value)
	{
		std::string utf8 = WideToUtf8(value);
		std::ostringstream out;
		for (unsigned char ch : utf8)
		{
			switch (ch)
			{
			case '\\': out << "\\\\"; break;
			case '"': out << "\\\""; break;
			case '\b': out << "\\b"; break;
			case '\f': out << "\\f"; break;
			case '\n': out << "\\n"; break;
			case '\r': out << "\\r"; break;
			case '\t': out << "\\t"; break;
			default:
				if (ch < 0x20)
					out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
				else
					out << ch;
				break;
			}
		}
		return out.str();
	}

	std::wstring TsvEscape(std::wstring value)
	{
		std::replace(value.begin(), value.end(), L'\t', L' ');
		std::replace(value.begin(), value.end(), L'\r', L' ');
		std::replace(value.begin(), value.end(), L'\n', L' ');
		return value;
	}

	void WriteUtf8File(const std::filesystem::path& path, const std::string& text)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		out.write(text.data(), static_cast<std::streamsize>(text.size()));
	}

	void WriteUtf8File(const std::filesystem::path& path, const std::wstring& text)
	{
		WriteUtf8File(path, WideToUtf8(text));
	}

	void WriteBinaryFile(const std::filesystem::path& path, const std::vector<uint8_t>& bytes)
	{
		std::filesystem::create_directories(path.parent_path());
		std::ofstream out(path, std::ios::binary);
		if (!bytes.empty())
			out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
	}

	std::wstring HexBytes(const uint8_t* bytes, size_t count)
	{
		std::wostringstream out;
		out << std::hex << std::setfill(L'0');
		for (size_t i = 0; i < count; ++i)
			out << std::setw(2) << static_cast<unsigned>(bytes[i]);
		return out.str();
	}

	class BinaryReader
	{
	public:
		explicit BinaryReader(std::vector<uint8_t> data) : m_data(std::move(data)) {}

		size_t Tell() const { return m_pos; }
		size_t Size() const { return m_data.size(); }
		size_t Remaining() const { return m_pos <= m_data.size() ? m_data.size() - m_pos : 0; }
		bool IsError() const { return m_error; }

		bool Seek(size_t pos)
		{
			if (pos > m_data.size())
			{
				m_error = true;
				return false;
			}
			m_pos = pos;
			return true;
		}

		template<typename T>
		T Read()
		{
			T value{};
			if (m_pos + sizeof(T) > m_data.size())
			{
				m_error = true;
				return value;
			}
			memcpy(&value, m_data.data() + m_pos, sizeof(T));
			m_pos += sizeof(T);
			return value;
		}

		bool ReadBytes(size_t count, std::vector<uint8_t>& out)
		{
			if (m_pos + count > m_data.size())
			{
				m_error = true;
				return false;
			}
			out.assign(m_data.begin() + m_pos, m_data.begin() + m_pos + count);
			m_pos += count;
			return true;
		}

		bool Skip(size_t count)
		{
			return Seek(m_pos + count);
		}

		std::wstring ReadFString()
		{
			int32_t count = Read<int32_t>();
			if (m_error || count == 0)
				return {};

			bool wide = count < 0;
			if (wide)
				count = -count;
			if (count < 0 || count > 1024 * 1024)
			{
				m_error = true;
				return {};
			}

			if (wide)
			{
				if (m_pos + static_cast<size_t>(count) * sizeof(uint16_t) > m_data.size())
				{
					m_error = true;
					return {};
				}
				std::wstring value;
				value.reserve(count);
				for (int32_t i = 0; i < count; ++i)
				{
					uint16_t ch = Read<uint16_t>();
					if (ch != 0)
						value.push_back(static_cast<wchar_t>(ch));
				}
				return value;
			}

			if (m_pos + count > m_data.size())
			{
				m_error = true;
				return {};
			}
			std::string value(reinterpret_cast<const char*>(m_data.data() + m_pos), count);
			m_pos += count;
			if (!value.empty() && value.back() == '\0')
				value.pop_back();
			return Utf8ToWide(value);
		}

	private:
		std::vector<uint8_t> m_data;
		size_t m_pos = 0;
		bool m_error = false;
	};

	bool LoadBinaryFile(const std::wstring& path, std::vector<uint8_t>& out, std::wstring& error)
	{
		std::ifstream file(std::filesystem::path(path), std::ios::binary);
		if (!file)
		{
			error = L"Failed to open file";
			return false;
		}
		file.seekg(0, std::ios::end);
		std::streamoff size = file.tellg();
		file.seekg(0, std::ios::beg);
		if (size < 0)
		{
			error = L"Failed to determine file size";
			return false;
		}
		out.resize(static_cast<size_t>(size));
		if (!out.empty())
			file.read(reinterpret_cast<char*>(out.data()), size);
		return true;
	}

	bool ReadFileRange(const std::filesystem::path& path, uint64_t offset, size_t size, std::vector<uint8_t>& out, std::wstring& error)
	{
		std::ifstream file(path, std::ios::binary);
		if (!file)
		{
			error = L"Failed to open file";
			return false;
		}
		file.seekg(0, std::ios::end);
		std::streamoff fileSize = file.tellg();
		if (fileSize < 0 || offset > static_cast<uint64_t>(fileSize) || size > static_cast<size_t>(static_cast<uint64_t>(fileSize) - offset))
		{
			error = L"Requested file range is outside the file";
			return false;
		}
		file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
		out.resize(size);
		if (!out.empty())
			file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
		if (!file && !out.empty())
		{
			error = L"Failed to read file range";
			return false;
		}
		return true;
	}

	int Base64Value(char ch)
	{
		if (ch >= 'A' && ch <= 'Z')
			return ch - 'A';
		if (ch >= 'a' && ch <= 'z')
			return ch - 'a' + 26;
		if (ch >= '0' && ch <= '9')
			return ch - '0' + 52;
		if (ch == '+')
			return 62;
		if (ch == '/')
			return 63;
		return -1;
	}

	bool DecodeBase64(std::string_view text, std::vector<uint8_t>& out)
	{
		out.clear();
		int value = 0;
		int bits = -8;
		for (char ch : text)
		{
			if (ch == '=')
				break;
			if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ')
				continue;
			int decoded = Base64Value(ch);
			if (decoded < 0)
				return false;
			value = (value << 6) | decoded;
			bits += 6;
			if (bits >= 0)
			{
				out.push_back(static_cast<uint8_t>((value >> bits) & 0xFF));
				bits -= 8;
			}
		}
		return true;
	}

	bool ExtractJsonStringValue(std::string_view text, std::string_view key, std::string& value)
	{
		std::string needle = "\"" + std::string(key) + "\"";
		size_t pos = text.find(needle);
		while (pos != std::string_view::npos)
		{
			pos = text.find(':', pos + needle.size());
			if (pos == std::string_view::npos)
				return false;
			++pos;
			while (pos < text.size() && isspace(static_cast<unsigned char>(text[pos])))
				++pos;
			if (pos >= text.size() || text[pos] != '"')
			{
				pos = text.find(needle, pos);
				continue;
			}
			++pos;
			std::ostringstream out;
			while (pos < text.size())
			{
				char ch = text[pos++];
				if (ch == '"')
				{
					value = out.str();
					return true;
				}
				if (ch == '\\' && pos < text.size())
				{
					char escaped = text[pos++];
					switch (escaped)
					{
					case '"': out << '"'; break;
					case '\\': out << '\\'; break;
					case '/': out << '/'; break;
					case 'b': out << '\b'; break;
					case 'f': out << '\f'; break;
					case 'n': out << '\n'; break;
					case 'r': out << '\r'; break;
					case 't': out << '\t'; break;
					default: out << escaped; break;
					}
					continue;
				}
				out << ch;
			}
			return false;
		}
		return false;
	}

	bool FindCryptoJsonForPak(const std::filesystem::path& pakPath, std::filesystem::path& cryptoJsonPath)
	{
		std::filesystem::path dir = pakPath.parent_path();
		while (!dir.empty())
		{
			std::filesystem::path candidate = dir / L"Crypto.json";
			if (std::filesystem::exists(candidate))
			{
				cryptoJsonPath = candidate;
				return true;
			}
			std::filesystem::path parent = dir.parent_path();
			if (parent == dir)
				break;
			dir = parent;
		}
		return false;
	}

	bool LoadCryptoKey(const std::filesystem::path& cryptoJsonPath, std::array<uint8_t, 32>& key, std::wstring& error)
	{
		std::vector<uint8_t> bytes;
		if (!LoadBinaryFile(cryptoJsonPath.wstring(), bytes, error))
			return false;
		std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
		std::string encodedKey;
		if (!ExtractJsonStringValue(text, "Key", encodedKey))
		{
			error = L"Crypto.json does not contain an EncryptionKey.Key field";
			return false;
		}

		std::vector<uint8_t> decoded;
		if (!DecodeBase64(encodedKey, decoded) || decoded.size() != key.size())
		{
			error = L"Crypto.json encryption key is not a 32-byte Base64 AES key";
			return false;
		}
		std::copy(decoded.begin(), decoded.end(), key.begin());
		return true;
	}

	bool BCryptOk(NTSTATUS status)
	{
		return status >= 0;
	}

	bool DecryptAes256Ecb(std::vector<uint8_t>& data, const std::array<uint8_t, 32>& key, std::wstring& error)
	{
		if ((data.size() % AesBlockSize) != 0)
		{
			error = L"Encrypted pak index size is not AES block aligned";
			return false;
		}

		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_KEY_HANDLE keyHandle = nullptr;
		bool success = false;
		do
		{
			if (!BCryptOk(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_AES_ALGORITHM, nullptr, 0)))
				break;
			if (!BCryptOk(BCryptSetProperty(algorithm, BCRYPT_CHAINING_MODE,
				reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
				static_cast<ULONG>((wcslen(BCRYPT_CHAIN_MODE_ECB) + 1) * sizeof(wchar_t)), 0)))
				break;
			if (!BCryptOk(BCryptGenerateSymmetricKey(algorithm, &keyHandle, nullptr, 0,
				const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0)))
				break;

			std::vector<uint8_t> decrypted(data.size());
			ULONG written = 0;
			if (!BCryptOk(BCryptDecrypt(keyHandle, data.data(), static_cast<ULONG>(data.size()), nullptr, nullptr, 0,
				decrypted.data(), static_cast<ULONG>(decrypted.size()), &written, 0)))
				break;
			if (written != decrypted.size())
				break;
			data.swap(decrypted);
			success = true;
		} while (false);

		if (keyHandle)
			BCryptDestroyKey(keyHandle);
		if (algorithm)
			BCryptCloseAlgorithmProvider(algorithm, 0);
		if (!success)
			error = L"Failed to decrypt pak index with Crypto.json AES key";
		return success;
	}

	bool Sha1Hash(const std::vector<uint8_t>& data, std::array<uint8_t, 20>& hash)
	{
		BCRYPT_ALG_HANDLE algorithm = nullptr;
		BCRYPT_HASH_HANDLE hashHandle = nullptr;
		bool success = false;
		do
		{
			if (!BCryptOk(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0)))
				break;
			if (!BCryptOk(BCryptCreateHash(algorithm, &hashHandle, nullptr, 0, nullptr, 0, 0)))
				break;
			if (!data.empty() && !BCryptOk(BCryptHashData(hashHandle, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0)))
				break;
			if (!BCryptOk(BCryptFinishHash(hashHandle, hash.data(), static_cast<ULONG>(hash.size()), 0)))
				break;
			success = true;
		} while (false);

		if (hashHandle)
			BCryptDestroyHash(hashHandle);
		if (algorithm)
			BCryptCloseAlgorithmProvider(algorithm, 0);
		return success;
	}

	struct CustomVersion
	{
		std::wstring Key;
		int32_t Version = 0;
	};

	struct PackageSummary
	{
		int32_t Tag = 0;
		int32_t LegacyFileVersion = 0;
		int32_t FileVersionUE4 = 0;
		int32_t FileVersionUE5 = 0;
		int32_t FileVersionLicenseeUE = 0;
		int32_t TotalHeaderSize = 0;
		std::wstring SavedHash;
		std::wstring PackageName;
		uint32_t PackageFlags = 0;
		int32_t NameCount = 0;
		int32_t NameOffset = 0;
		int32_t SoftObjectPathsCount = 0;
		int32_t SoftObjectPathsOffset = 0;
		std::wstring LocalizationId;
		int32_t GatherableTextDataCount = 0;
		int32_t GatherableTextDataOffset = 0;
		int32_t ExportCount = 0;
		int32_t ExportOffset = 0;
		int32_t ImportCount = 0;
		int32_t ImportOffset = 0;
		int32_t CellExportCount = 0;
		int32_t CellExportOffset = 0;
		int32_t CellImportCount = 0;
		int32_t CellImportOffset = 0;
		int32_t MetaDataOffset = 0;
		int32_t DependsOffset = 0;
		int32_t SoftPackageReferencesCount = 0;
		int32_t SoftPackageReferencesOffset = 0;
		int32_t SearchableNamesOffset = 0;
		int32_t ThumbnailTableOffset = 0;
		size_t ParsedBytes = 0;
		std::vector<CustomVersion> CustomVersions;
	};

	bool IsUE5Package(const PackageSummary& summary)
	{
		return summary.FileVersionUE5 >= UE5InitialVersion;
	}

	bool ReadCustomVersions(BinaryReader& reader, int32_t legacyFileVersion, std::vector<CustomVersion>& versions)
	{
		if (legacyFileVersion > -2)
			return true;

		int32_t count = reader.Read<int32_t>();
		if (reader.IsError() || count < 0 || count > 65536)
			return false;

		versions.reserve(static_cast<size_t>(count));
		for (int32_t i = 0; i < count; ++i)
		{
			CustomVersion version;
			if (legacyFileVersion == -2)
			{
				int32_t enumTag = reader.Read<int32_t>();
				version.Key = L"Enum:" + std::to_wstring(enumTag);
			}
			else
			{
				std::array<uint8_t, 16> guid{};
				for (uint8_t& byte : guid)
					byte = reader.Read<uint8_t>();
				version.Key = HexBytes(guid.data(), guid.size());
			}
			version.Version = reader.Read<int32_t>();
			if (reader.IsError())
				return false;
			versions.push_back(std::move(version));
		}
		return true;
	}

	bool TryReadPackageSummary(BinaryReader& reader, PackageSummary& summary, std::wstring& error)
	{
		summary.Tag = reader.Read<int32_t>();
		if (summary.Tag != PackageFileTag && summary.Tag != PackageFileTagSwapped)
		{
			error = L"Not an Unreal package tag";
			return false;
		}
		if (summary.Tag == PackageFileTagSwapped)
		{
			error = L"Byte-swapped Unreal packages are not supported yet";
			return false;
		}

		summary.LegacyFileVersion = reader.Read<int32_t>();
		if (summary.LegacyFileVersion < LegacyPackageVersionCurrent)
		{
			error = L"Unsupported newer legacy package summary version";
			return false;
		}
		if (summary.LegacyFileVersion >= 0)
		{
			error = L"UE3 package summaries are not supported";
			return false;
		}

		if (summary.LegacyFileVersion != -4)
			(void)reader.Read<int32_t>();
		summary.FileVersionUE4 = reader.Read<int32_t>();
		if (summary.LegacyFileVersion <= -8)
			summary.FileVersionUE5 = reader.Read<int32_t>();
		summary.FileVersionLicenseeUE = reader.Read<int32_t>();

		if (summary.FileVersionUE5 >= UE5PackageSavedHash)
		{
			std::vector<uint8_t> hash;
			reader.ReadBytes(20, hash);
			if (!hash.empty())
				summary.SavedHash = HexBytes(hash.data(), hash.size());
			summary.TotalHeaderSize = reader.Read<int32_t>();
		}

		if (!ReadCustomVersions(reader, summary.LegacyFileVersion, summary.CustomVersions))
		{
			error = L"Failed to read custom version table";
			return false;
		}

		if (summary.FileVersionUE5 < UE5PackageSavedHash)
		{
			int32_t jbInfo = reader.Read<int32_t>();
			if (jbInfo > 0)
				summary.TotalHeaderSize = jbInfo;
			else
				summary.TotalHeaderSize = reader.Read<int32_t>();
		}

		summary.PackageName = reader.ReadFString();
		summary.PackageFlags = reader.Read<uint32_t>();
		summary.NameCount = reader.Read<int32_t>();
		summary.NameOffset = reader.Read<int32_t>();

		if (summary.FileVersionUE5 >= UE5AddSoftObjectPathList)
		{
			summary.SoftObjectPathsCount = reader.Read<int32_t>();
			summary.SoftObjectPathsOffset = reader.Read<int32_t>();
		}

		if (IsUE5Package(summary))
			summary.LocalizationId = reader.ReadFString();

		if (IsUE5Package(summary))
		{
			summary.GatherableTextDataCount = reader.Read<int32_t>();
			summary.GatherableTextDataOffset = reader.Read<int32_t>();
		}

		summary.ExportCount = reader.Read<int32_t>();
		summary.ExportOffset = reader.Read<int32_t>();
		summary.ImportCount = reader.Read<int32_t>();
		summary.ImportOffset = reader.Read<int32_t>();

		if (summary.FileVersionUE5 >= UE5VerseCells)
		{
			summary.CellExportCount = reader.Read<int32_t>();
			summary.CellExportOffset = reader.Read<int32_t>();
			summary.CellImportCount = reader.Read<int32_t>();
			summary.CellImportOffset = reader.Read<int32_t>();
		}

		if (summary.FileVersionUE5 >= UE5MetadataSerializationOffset)
			summary.MetaDataOffset = reader.Read<int32_t>();

		summary.DependsOffset = reader.Read<int32_t>();

		if (IsUE5Package(summary))
		{
			summary.SoftPackageReferencesCount = reader.Read<int32_t>();
			summary.SoftPackageReferencesOffset = reader.Read<int32_t>();
			summary.SearchableNamesOffset = reader.Read<int32_t>();
		}

		summary.ThumbnailTableOffset = reader.Read<int32_t>();
		summary.ParsedBytes = reader.Tell();

		if (reader.IsError())
		{
			error = L"Package summary ended unexpectedly";
			return false;
		}
		return true;
	}

	std::vector<std::wstring> TryReadNameMap(BinaryReader& reader, const PackageSummary& summary)
	{
		std::vector<std::wstring> names;
		if (summary.NameCount <= 0 || summary.NameCount > 1000000 || summary.NameOffset <= 0)
			return names;
		if (!reader.Seek(static_cast<size_t>(summary.NameOffset)))
			return names;

		names.reserve(static_cast<size_t>(summary.NameCount));
		for (int32_t i = 0; i < summary.NameCount && !reader.IsError(); ++i)
		{
			std::wstring name = reader.ReadFString();
			if (IsUE5Package(summary))
				reader.Skip(sizeof(uint16_t) * 2);
			names.push_back(std::move(name));
		}
		if (reader.IsError())
			names.clear();
		return names;
	}

	std::string PackageSummaryJson(const PackageSummary& summary)
	{
		std::ostringstream out;
		out << "{\n";
		out << "  \"tag\": \"0x" << std::hex << std::uppercase << summary.Tag << std::dec << "\",\n";
		out << "  \"legacy_file_version\": " << summary.LegacyFileVersion << ",\n";
		out << "  \"file_version_ue4\": " << summary.FileVersionUE4 << ",\n";
		out << "  \"file_version_ue5\": " << summary.FileVersionUE5 << ",\n";
		out << "  \"file_version_licensee_ue\": " << summary.FileVersionLicenseeUE << ",\n";
		out << "  \"total_header_size\": " << summary.TotalHeaderSize << ",\n";
		out << "  \"saved_hash\": \"" << WideToUtf8(summary.SavedHash) << "\",\n";
		out << "  \"package_name\": \"" << JsonEscape(summary.PackageName) << "\",\n";
		out << "  \"package_flags\": " << summary.PackageFlags << ",\n";
		out << "  \"name_count\": " << summary.NameCount << ",\n";
		out << "  \"name_offset\": " << summary.NameOffset << ",\n";
		out << "  \"soft_object_paths_count\": " << summary.SoftObjectPathsCount << ",\n";
		out << "  \"soft_object_paths_offset\": " << summary.SoftObjectPathsOffset << ",\n";
		out << "  \"localization_id\": \"" << JsonEscape(summary.LocalizationId) << "\",\n";
		out << "  \"gatherable_text_data_count\": " << summary.GatherableTextDataCount << ",\n";
		out << "  \"gatherable_text_data_offset\": " << summary.GatherableTextDataOffset << ",\n";
		out << "  \"export_count\": " << summary.ExportCount << ",\n";
		out << "  \"export_offset\": " << summary.ExportOffset << ",\n";
		out << "  \"import_count\": " << summary.ImportCount << ",\n";
		out << "  \"import_offset\": " << summary.ImportOffset << ",\n";
		out << "  \"cell_export_count\": " << summary.CellExportCount << ",\n";
		out << "  \"cell_export_offset\": " << summary.CellExportOffset << ",\n";
		out << "  \"cell_import_count\": " << summary.CellImportCount << ",\n";
		out << "  \"cell_import_offset\": " << summary.CellImportOffset << ",\n";
		out << "  \"metadata_offset\": " << summary.MetaDataOffset << ",\n";
		out << "  \"depends_offset\": " << summary.DependsOffset << ",\n";
		out << "  \"soft_package_references_count\": " << summary.SoftPackageReferencesCount << ",\n";
		out << "  \"soft_package_references_offset\": " << summary.SoftPackageReferencesOffset << ",\n";
		out << "  \"searchable_names_offset\": " << summary.SearchableNamesOffset << ",\n";
		out << "  \"thumbnail_table_offset\": " << summary.ThumbnailTableOffset << ",\n";
		out << "  \"parsed_summary_bytes\": " << summary.ParsedBytes << "\n";
		out << "}\n";
		return out.str();
	}

	void WriteCustomVersions(const std::filesystem::path& destination, const PackageSummary& summary)
	{
		std::wostringstream out;
		out << L"key\tversion\n";
		for (const CustomVersion& version : summary.CustomVersions)
			out << TsvEscape(version.Key) << L'\t' << version.Version << L'\n';
		WriteUtf8File(destination / L"UE5Core" / L"02_custom_versions.tsv", out.str());
	}

	void WriteNameMap(const std::filesystem::path& destination, const std::vector<std::wstring>& names)
	{
		std::wostringstream out;
		out << L"index\tname\n";
		for (size_t i = 0; i < names.size(); ++i)
			out << i << L'\t' << TsvEscape(names[i]) << L'\n';
		WriteUtf8File(destination / L"UE5Core" / L"01_names.tsv", out.str());
	}

	bool LooksLikeTextureAsset(const std::vector<std::wstring>& names)
	{
		for (const std::wstring& name : names)
		{
			if (name == L"Texture2D" || name == L"TextureCube" || name == L"TextureRenderTarget2D" ||
				name == L"VolumeTexture" || name == L"Texture")
			{
				return true;
			}
		}
		return false;
	}

	std::vector<std::wstring> ExtractAsciiStrings(const std::vector<uint8_t>& bytes, size_t limit)
	{
		std::vector<std::wstring> result;
		std::string current;
		for (uint8_t byte : bytes)
		{
			if (byte >= 32 && byte <= 126)
			{
				current.push_back(static_cast<char>(byte));
				continue;
			}
			if (current.size() >= 5)
			{
				result.push_back(Utf8ToWide(current));
				if (result.size() >= limit)
					break;
			}
			current.clear();
		}
		if (result.size() < limit && current.size() >= 5)
			result.push_back(Utf8ToWide(current));
		return result;
	}

	std::vector<uint8_t> MakeStringSample(const std::vector<uint8_t>& bytes)
	{
		constexpr size_t SampleBytes = 8 * 1024 * 1024;
		if (bytes.size() <= SampleBytes * 2)
			return bytes;

		std::vector<uint8_t> sample;
		sample.reserve(SampleBytes * 2);
		sample.insert(sample.end(), bytes.begin(), bytes.begin() + SampleBytes);
		sample.insert(sample.end(), bytes.end() - SampleBytes, bytes.end());
		return sample;
	}

	void WriteStringSummary(const std::filesystem::path& destination, const std::vector<uint8_t>& bytes)
	{
		std::vector<uint8_t> sample = MakeStringSample(bytes);
		std::vector<std::wstring> strings = ExtractAsciiStrings(sample, 4000);
		std::wostringstream out;
		for (const std::wstring& text : strings)
			out << TsvEscape(text) << L'\n';
		WriteUtf8File(destination / L"Binary" / L"01_ascii_strings.txt", out.str());

		std::wostringstream paths;
		paths << L"path_like_string\n";
		for (const std::wstring& text : strings)
		{
			std::wstring lower = ToLower(text);
			if (lower.find(L".uasset") != std::wstring::npos || lower.find(L".umap") != std::wstring::npos ||
				lower.find(L".uexp") != std::wstring::npos || lower.find(L".ubulk") != std::wstring::npos ||
				lower.find(L"/game/") != std::wstring::npos || lower.find(L"content/") != std::wstring::npos)
			{
				paths << TsvEscape(text) << L'\n';
			}
		}
		WriteUtf8File(destination / L"Files" / L"00_likely_paths.tsv", paths.str());
	}

	void WriteHexPreview(const std::filesystem::path& destination, const std::vector<uint8_t>& bytes)
	{
		std::wostringstream out;
		size_t limit = std::min<size_t>(bytes.size(), 512);
		for (size_t offset = 0; offset < limit; offset += 16)
		{
			out << std::hex << std::setw(8) << std::setfill(L'0') << offset << L"  ";
			size_t count = std::min<size_t>(16, limit - offset);
			for (size_t i = 0; i < count; ++i)
				out << std::setw(2) << static_cast<unsigned>(bytes[offset + i]) << L' ';
			out << L'\n';
		}
		WriteUtf8File(destination / L"Binary" / L"00_header_hex.txt", out.str());
	}

	void WriteSidecars(const std::filesystem::path& source, const std::filesystem::path& destination)
	{
		static const wchar_t* Extensions[] = { L".uasset", L".umap", L".uexp", L".exp", L".ubulk", L".uptnl", L".ucas", L".utoc", L".pak", L".sig" };
		std::filesystem::path dir = source.parent_path();
		std::wstring stem = source.stem().wstring();
		std::wostringstream out;
		out << L"extension\tpath\tsize\n";
		for (const wchar_t* extension : Extensions)
		{
			std::filesystem::path candidate = dir / (stem + extension);
			if (std::filesystem::exists(candidate))
			{
				uintmax_t size = std::filesystem::file_size(candidate);
				out << extension << L'\t' << candidate.wstring() << L'\t' << size << L'\n';
			}
		}
		WriteUtf8File(destination / L"01_sidecars.tsv", out.str());
	}

	std::wstring SafeFileName(std::wstring value)
	{
		static const std::wstring InvalidChars = L"<>:\"/\\|?*";
		for (wchar_t& ch : value)
		{
			if (ch < 32 || InvalidChars.find(ch) != std::wstring::npos)
				ch = L'_';
		}
		while (!value.empty() && (value.back() == L'.' || value.back() == L' '))
			value.pop_back();
		if (value.empty())
			value = L"image";
		if (value.size() > 96)
			value.resize(96);
		return value;
	}

	bool LooksLikePng(const std::vector<uint8_t>& bytes)
	{
		static const uint8_t Signature[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		return bytes.size() >= sizeof(Signature) && memcmp(bytes.data(), Signature, sizeof(Signature)) == 0;
	}

	bool LooksLikeJpeg(const std::vector<uint8_t>& bytes)
	{
		return bytes.size() >= 4 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[bytes.size() - 2] == 0xFF && bytes[bytes.size() - 1] == 0xD9;
	}

	uint32_t ReadBigEndianUInt32(const std::vector<uint8_t>& bytes, size_t offset)
	{
		return (static_cast<uint32_t>(bytes[offset]) << 24) |
			(static_cast<uint32_t>(bytes[offset + 1]) << 16) |
			(static_cast<uint32_t>(bytes[offset + 2]) << 8) |
			static_cast<uint32_t>(bytes[offset + 3]);
	}

	bool TryFindPngEnd(const std::vector<uint8_t>& bytes, size_t start, size_t& end)
	{
		constexpr size_t MaxImageBytes = 64 * 1024 * 1024;
		static const uint8_t Signature[] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
		if (start + sizeof(Signature) > bytes.size() || memcmp(bytes.data() + start, Signature, sizeof(Signature)) != 0)
			return false;

		size_t pos = start + sizeof(Signature);
		while (pos + 12 <= bytes.size() && pos - start <= MaxImageBytes)
		{
			uint32_t length = ReadBigEndianUInt32(bytes, pos);
			if (length > MaxImageBytes || pos + 12 + static_cast<size_t>(length) > bytes.size())
				return false;
			const uint8_t* type = bytes.data() + pos + 4;
			pos += 12 + static_cast<size_t>(length);
			if (type[0] == 'I' && type[1] == 'E' && type[2] == 'N' && type[3] == 'D')
			{
				end = pos;
				return true;
			}
		}
		return false;
	}

	bool TryFindJpegEnd(const std::vector<uint8_t>& bytes, size_t start, size_t& end)
	{
		constexpr size_t MaxImageBytes = 64 * 1024 * 1024;
		if (start + 4 > bytes.size() || bytes[start] != 0xFF || bytes[start + 1] != 0xD8)
			return false;
		size_t limit = (std::min)(bytes.size(), start + MaxImageBytes);
		for (size_t pos = start + 2; pos + 1 < limit; ++pos)
		{
			if (bytes[pos] == 0xFF && bytes[pos + 1] == 0xD9)
			{
				end = pos + 2;
				return true;
			}
		}
		return false;
	}

	void ExtractEmbeddedImagesFromBytes(const std::vector<uint8_t>& bytes, const std::wstring& sourceLabel, const std::filesystem::path& destination,
		std::wostringstream& manifest, int& imageIndex)
	{
		constexpr int MaxImages = 64;
		for (size_t offset = 0; offset + 8 < bytes.size() && imageIndex < MaxImages; ++offset)
		{
			size_t end = 0;
			std::wstring extension;
			if (TryFindPngEnd(bytes, offset, end))
				extension = L".png";
			else if (TryFindJpegEnd(bytes, offset, end))
				extension = L".jpg";
			else
				continue;

			std::vector<uint8_t> image(bytes.begin() + offset, bytes.begin() + end);
			std::wostringstream filename;
			filename << std::setw(2) << std::setfill(L'0') << imageIndex << L"_"
				<< SafeFileName(sourceLabel) << extension;
			std::filesystem::path output = destination / L"Images" / L"Embedded" / filename.str();
			WriteBinaryFile(output, image);

			manifest << imageIndex << L'\t' << TsvEscape(sourceLabel) << L'\t' << offset << L'\t'
				<< image.size() << L'\t' << extension.substr(1) << L'\t'
				<< TsvEscape((std::filesystem::path(L"Images") / L"Embedded" / filename.str()).wstring()) << L'\n';

			++imageIndex;
			offset = end - 1;
		}
	}

	void WriteEmbeddedImages(const std::filesystem::path& source, const std::filesystem::path& destination, const std::vector<uint8_t>& bytes)
	{
		constexpr uintmax_t MaxSidecarScanBytes = 512ull * 1024ull * 1024ull;
		static const wchar_t* SidecarExtensions[] = { L".uexp", L".exp", L".ubulk", L".uptnl" };

		std::wostringstream manifest;
		manifest << L"index\tsource\toffset\tsize\tformat\toutput\n";
		int imageIndex = 0;
		ExtractEmbeddedImagesFromBytes(bytes, source.filename().wstring(), destination, manifest, imageIndex);

		std::filesystem::path dir = source.parent_path();
		std::wstring stem = source.stem().wstring();
		for (const wchar_t* extension : SidecarExtensions)
		{
			std::filesystem::path candidate = dir / (stem + extension);
			if (!std::filesystem::exists(candidate))
				continue;
			if (ToLower(candidate.wstring()) == ToLower(source.wstring()))
				continue;
			uintmax_t size = std::filesystem::file_size(candidate);
			if (size > MaxSidecarScanBytes)
				continue;

			std::wstring error;
			std::vector<uint8_t> sidecarBytes;
			if (LoadBinaryFile(candidate.wstring(), sidecarBytes, error))
				ExtractEmbeddedImagesFromBytes(sidecarBytes, candidate.filename().wstring(), destination, manifest, imageIndex);
		}

		if (imageIndex > 0)
			WriteUtf8File(destination / L"Images" / L"00_embedded_images.tsv", manifest.str());
	}

	struct ThumbnailEntry
	{
		std::wstring ObjectClassName;
		std::wstring ObjectPathWithoutPackageName;
		int32_t FileOffset = 0;
		int32_t Width = 0;
		int32_t Height = 0;
		bool IsJpeg = false;
		std::vector<uint8_t> CompressedImageData;
	};

	bool ReadByteArray(BinaryReader& reader, int32_t maxBytes, std::vector<uint8_t>& bytes)
	{
		int32_t count = reader.Read<int32_t>();
		if (reader.IsError() || count < 0 || count > maxBytes || static_cast<size_t>(count) > reader.Remaining())
			return false;
		return reader.ReadBytes(static_cast<size_t>(count), bytes);
	}

	bool ReadThumbnailPayload(const std::vector<uint8_t>& bytes, ThumbnailEntry& entry)
	{
		if (entry.FileOffset <= 0 || static_cast<size_t>(entry.FileOffset) >= bytes.size())
			return false;

		BinaryReader reader(bytes);
		if (!reader.Seek(static_cast<size_t>(entry.FileOffset)))
			return false;

		entry.Width = reader.Read<int32_t>();
		int32_t signedHeight = reader.Read<int32_t>();
		if (reader.IsError() || signedHeight == (std::numeric_limits<int32_t>::min)())
			return false;

		entry.IsJpeg = signedHeight < 0;
		entry.Height = signedHeight < 0 ? -signedHeight : signedHeight;
		if (entry.Width <= 0 || entry.Height <= 0 || entry.Width > 16384 || entry.Height > 16384)
			return false;

		if (!ReadByteArray(reader, 128 * 1024 * 1024, entry.CompressedImageData))
			return false;
		if (entry.CompressedImageData.empty())
			return false;
		if (LooksLikeJpeg(entry.CompressedImageData))
			entry.IsJpeg = true;
		else if (LooksLikePng(entry.CompressedImageData))
			entry.IsJpeg = false;
		return true;
	}

	void WriteThumbnailImages(const std::filesystem::path& destination, const std::vector<uint8_t>& bytes, const PackageSummary& summary)
	{
		if (summary.ThumbnailTableOffset <= 0 || static_cast<size_t>(summary.ThumbnailTableOffset) >= bytes.size())
			return;

		BinaryReader reader(bytes);
		if (!reader.Seek(static_cast<size_t>(summary.ThumbnailTableOffset)))
			return;

		int32_t count = reader.Read<int32_t>();
		if (reader.IsError() || count <= 0 || count > 4096)
			return;

		std::vector<ThumbnailEntry> thumbnails;
		thumbnails.reserve(static_cast<size_t>(count));
		for (int32_t index = 0; index < count && !reader.IsError(); ++index)
		{
			ThumbnailEntry entry;
			entry.ObjectClassName = reader.ReadFString();
			entry.ObjectPathWithoutPackageName = reader.ReadFString();
			entry.FileOffset = reader.Read<int32_t>();
			if (!reader.IsError() && ReadThumbnailPayload(bytes, entry))
				thumbnails.push_back(std::move(entry));
		}
		if (thumbnails.empty())
			return;

		std::wostringstream manifest;
		manifest << L"index\tclass\tobject_path\twidth\theight\tformat\tcompressed_size\tfile_offset\toutput\n";
		for (size_t index = 0; index < thumbnails.size(); ++index)
		{
			const ThumbnailEntry& entry = thumbnails[index];
			std::wstring extension = entry.IsJpeg ? L".jpg" : L".png";
			std::wostringstream filename;
			filename << std::setw(2) << std::setfill(L'0') << index << L"_"
				<< SafeFileName(entry.ObjectClassName + L"_" + entry.ObjectPathWithoutPackageName) << extension;
			std::filesystem::path output = destination / L"Images" / L"Thumbnails" / filename.str();
			WriteBinaryFile(output, entry.CompressedImageData);

			manifest << index << L'\t' << TsvEscape(entry.ObjectClassName) << L'\t'
				<< TsvEscape(entry.ObjectPathWithoutPackageName) << L'\t'
				<< entry.Width << L'\t' << entry.Height << L'\t'
				<< (entry.IsJpeg ? L"jpg" : L"png") << L'\t'
				<< entry.CompressedImageData.size() << L'\t' << entry.FileOffset << L'\t'
				<< TsvEscape((std::filesystem::path(L"Images") / L"Thumbnails" / filename.str()).wstring()) << L'\n';
		}
		WriteUtf8File(destination / L"Images" / L"00_thumbnails.tsv", manifest.str());
	}

	struct PakInfo
	{
		uint32_t Magic = 0;
		int32_t Version = 0;
		int64_t IndexOffset = -1;
		int64_t IndexSize = 0;
		std::array<uint8_t, 20> IndexHash{};
		bool EncryptedIndex = false;
		std::array<uint8_t, 16> EncryptionKeyGuid{};
		std::vector<std::wstring> CompressionMethods;
		size_t TrailerSize = 0;
	};

	struct PakCompressedBlock
	{
		int64_t Start = 0;
		int64_t End = 0;
	};

	struct PakEntry
	{
		int64_t Offset = -1;
		int64_t Size = 0;
		int64_t UncompressedSize = 0;
		std::array<uint8_t, 20> Hash{};
		std::vector<PakCompressedBlock> CompressionBlocks;
		uint32_t CompressionBlockSize = 0;
		uint32_t CompressionMethodIndex = 0;
		uint8_t Flags = 0;
		bool HasPayloadHash = false;
	};

	struct PakFileRecord
	{
		std::wstring Path;
		PakEntry Entry;
	};

	size_t GetPakInfoSerializedSize(int32_t version)
	{
		size_t size = sizeof(uint32_t) + sizeof(int32_t) + sizeof(int64_t) + sizeof(int64_t) + 20 + sizeof(uint8_t);
		if (version >= PakFileVersionEncryptionKeyGuid)
			size += 16;
		if (version >= PakFileVersionFNameBasedCompressionMethod)
			size += PakCompressionMethodNameLen * PakMaxNumCompressionMethods;
		if (version >= PakFileVersionFrozenIndex && version < PakFileVersionPathHashIndex)
			size += sizeof(bool);
		return size;
	}

	std::wstring GuidToString(const std::array<uint8_t, 16>& guid)
	{
		std::wostringstream out;
		out << std::hex << std::setfill(L'0');
		for (size_t i = 0; i < guid.size(); ++i)
		{
			if (i == 4 || i == 6 || i == 8 || i == 10)
				out << L'-';
			out << std::setw(2) << static_cast<unsigned>(guid[i]);
		}
		return out.str();
	}

	bool IsZeroGuid(const std::array<uint8_t, 16>& guid)
	{
		return std::all_of(guid.begin(), guid.end(), [](uint8_t byte) { return byte == 0; });
	}

	std::wstring HashToString(const std::array<uint8_t, 20>& hash)
	{
		return HexBytes(hash.data(), hash.size());
	}

	bool ReadPakInfoFromFile(const std::filesystem::path& source, uint64_t fileSize, PakInfo& info, std::wstring& error)
	{
		for (int32_t compatibleVersion = PakFileVersionLatest; compatibleVersion >= PakFileVersionInitial; --compatibleVersion)
		{
			size_t trailerSize = GetPakInfoSerializedSize(compatibleVersion);
			if (fileSize < trailerSize)
				continue;

			std::vector<uint8_t> trailer;
			if (!ReadFileRange(source, fileSize - trailerSize, trailerSize, trailer, error))
				return false;

			BinaryReader reader(std::move(trailer));
			PakInfo candidate;
			candidate.TrailerSize = trailerSize;
			candidate.CompressionMethods.push_back(L"None");
			if (compatibleVersion >= PakFileVersionEncryptionKeyGuid)
			{
				for (uint8_t& byte : candidate.EncryptionKeyGuid)
					byte = reader.Read<uint8_t>();
			}
			candidate.EncryptedIndex = reader.Read<uint8_t>() != 0;
			candidate.Magic = reader.Read<uint32_t>();
			if (candidate.Magic != PakFileMagic)
				continue;

			candidate.Version = reader.Read<int32_t>();
			candidate.IndexOffset = reader.Read<int64_t>();
			candidate.IndexSize = reader.Read<int64_t>();
			for (uint8_t& byte : candidate.IndexHash)
				byte = reader.Read<uint8_t>();
			if (candidate.Version >= PakFileVersionFrozenIndex && candidate.Version < PakFileVersionPathHashIndex)
				(void)reader.Read<uint8_t>();

			if (candidate.Version < PakFileVersionFNameBasedCompressionMethod)
			{
				candidate.CompressionMethods.push_back(L"Zlib");
				candidate.CompressionMethods.push_back(L"Gzip");
				candidate.CompressionMethods.push_back(L"Oodle");
			}
			else
			{
				for (int32_t methodIndex = 0; methodIndex < PakMaxNumCompressionMethods; ++methodIndex)
				{
					std::string method;
					for (int32_t charIndex = 0; charIndex < PakCompressionMethodNameLen; ++charIndex)
					{
						char ch = static_cast<char>(reader.Read<uint8_t>());
						if (ch != '\0')
							method.push_back(ch);
					}
					if (!method.empty())
						candidate.CompressionMethods.push_back(Utf8ToWide(method));
				}
			}

			if (reader.IsError() || candidate.Version < PakFileVersionInitial || candidate.Version > compatibleVersion ||
				candidate.IndexOffset < 0 || candidate.IndexSize < 0 ||
				static_cast<uint64_t>(candidate.IndexOffset) + static_cast<uint64_t>(candidate.IndexSize) > fileSize)
			{
				continue;
			}

			info = std::move(candidate);
			return true;
		}

		error = L"Not a supported Unreal pak file";
		return false;
	}

	bool ReadPakBool32(BinaryReader& reader)
	{
		return reader.Read<uint32_t>() != 0;
	}

	std::vector<uint8_t> ReadByteArray(BinaryReader& reader, int32_t maxBytes)
	{
		std::vector<uint8_t> bytes;
		int32_t count = reader.Read<int32_t>();
		if (reader.IsError() || count < 0 || count > maxBytes || static_cast<size_t>(count) > reader.Remaining())
		{
			reader.Seek(reader.Size() + 1);
			return bytes;
		}
		reader.ReadBytes(static_cast<size_t>(count), bytes);
		return bytes;
	}

	bool ReadPakEntry(BinaryReader& reader, int32_t version, PakEntry& entry)
	{
		entry = PakEntry{};
		entry.Offset = reader.Read<int64_t>();
		entry.Size = reader.Read<int64_t>();
		entry.UncompressedSize = reader.Read<int64_t>();
		if (version < PakFileVersionFNameBasedCompressionMethod)
		{
			int32_t legacyCompressionMethod = reader.Read<int32_t>();
			entry.CompressionMethodIndex = legacyCompressionMethod == 0 ? 0 : 1;
		}
		else
		{
			entry.CompressionMethodIndex = reader.Read<uint32_t>();
		}
		if (version <= PakFileVersionInitial)
			(void)reader.Read<int64_t>();
		for (uint8_t& byte : entry.Hash)
			byte = reader.Read<uint8_t>();
		entry.HasPayloadHash = true;
		if (version >= PakFileVersionCompressionEncryption)
		{
			if (entry.CompressionMethodIndex != 0)
			{
				int32_t blockCount = reader.Read<int32_t>();
				if (blockCount < 0 || blockCount > 1024 * 1024)
					return false;
				entry.CompressionBlocks.resize(static_cast<size_t>(blockCount));
				for (PakCompressedBlock& block : entry.CompressionBlocks)
				{
					block.Start = reader.Read<int64_t>();
					block.End = reader.Read<int64_t>();
				}
			}
			entry.Flags = reader.Read<uint8_t>();
			entry.CompressionBlockSize = reader.Read<uint32_t>();
		}
		return !reader.IsError();
	}

	size_t GetPakEntrySerializedSize(const PakEntry& entry, int32_t version)
	{
		size_t size = sizeof(int64_t) + sizeof(int64_t) + sizeof(int64_t) + sizeof(uint32_t) + 20;
		if (version < PakFileVersionFNameBasedCompressionMethod)
			size = sizeof(int64_t) + sizeof(int64_t) + sizeof(int64_t) + sizeof(int32_t) + 20;
		if (version <= PakFileVersionInitial)
			size += sizeof(int64_t);
		if (version >= PakFileVersionCompressionEncryption)
		{
			size += sizeof(uint8_t) + sizeof(uint32_t);
			if (entry.CompressionMethodIndex != 0)
				size += sizeof(int32_t) + entry.CompressionBlocks.size() * sizeof(int64_t) * 2;
		}
		return size;
	}

	uint64_t AlignUInt64(uint64_t value, uint64_t alignment)
	{
		return alignment == 0 ? value : ((value + alignment - 1) / alignment) * alignment;
	}

	bool DecodePakEntry(BinaryReader& reader, const PakInfo& info, PakEntry& entry)
	{
		entry = PakEntry{};
		uint32_t value = reader.Read<uint32_t>();
		uint32_t compressionBlockSize = 0;
		if ((value & 0x3f) == 0x3f)
			compressionBlockSize = reader.Read<uint32_t>();
		else
			compressionBlockSize = (value & 0x3f) << 11;

		entry.CompressionMethodIndex = (value >> 23) & 0x3f;
		if ((value & (1u << 31)) != 0)
			entry.Offset = reader.Read<uint32_t>();
		else
			entry.Offset = reader.Read<int64_t>();

		if ((value & (1u << 30)) != 0)
			entry.UncompressedSize = reader.Read<uint32_t>();
		else
			entry.UncompressedSize = reader.Read<int64_t>();

		if (entry.CompressionMethodIndex != 0)
		{
			if ((value & (1u << 29)) != 0)
				entry.Size = reader.Read<uint32_t>();
			else
				entry.Size = reader.Read<int64_t>();
		}
		else
		{
			entry.Size = entry.UncompressedSize;
		}

		bool encrypted = (value & (1u << 22)) != 0;
		if (encrypted)
			entry.Flags |= 0x01;
		uint32_t compressionBlockCount = (value >> 6) & 0xffff;
		entry.CompressionBlocks.resize(compressionBlockCount);
		entry.CompressionBlockSize = compressionBlockCount > 0 ? compressionBlockSize : 0;
		if (compressionBlockCount == 1)
			entry.CompressionBlockSize = static_cast<uint32_t>(entry.UncompressedSize);

		int64_t baseOffset = info.Version >= PakFileVersionRelativeChunkOffsets ? 0 : entry.Offset;
		if (compressionBlockCount == 1 && !encrypted)
		{
			entry.CompressionBlocks[0].Start = baseOffset + static_cast<int64_t>(GetPakEntrySerializedSize(entry, info.Version));
			entry.CompressionBlocks[0].End = entry.CompressionBlocks[0].Start + entry.Size;
		}
		else if (compressionBlockCount > 0)
		{
			uint64_t alignment = encrypted ? AesBlockSize : 1;
			int64_t blockOffset = baseOffset + static_cast<int64_t>(GetPakEntrySerializedSize(entry, info.Version));
			for (PakCompressedBlock& block : entry.CompressionBlocks)
			{
				uint32_t blockSize = reader.Read<uint32_t>();
				block.Start = blockOffset;
				block.End = blockOffset + blockSize;
				blockOffset += static_cast<int64_t>(AlignUInt64(blockSize, alignment));
			}
		}
		return !reader.IsError();
	}

	bool ResolvePakEntryLocation(int32_t location, const std::vector<uint8_t>& encodedEntries, const std::vector<PakEntry>& unencodedEntries,
		const PakInfo& info, PakEntry& entry)
	{
		if (location >= 0)
		{
			if (static_cast<size_t>(location) >= encodedEntries.size())
				return false;
			BinaryReader reader(encodedEntries);
			if (!reader.Seek(static_cast<size_t>(location)))
				return false;
			return DecodePakEntry(reader, info, entry);
		}

		int32_t index = -location - 1;
		if (index < 0 || static_cast<size_t>(index) >= unencodedEntries.size())
			return false;
		entry = unencodedEntries[static_cast<size_t>(index)];
		return true;
	}

	std::wstring CombinePakPath(const std::wstring& directory, const std::wstring& file)
	{
		if (directory.empty())
			return file;
		if (directory.back() == L'/' || directory.back() == L'\\')
			return directory + file;
		return directory + L"/" + file;
	}

	std::filesystem::path SanitizePakPathForOutput(const std::wstring& pakPath)
	{
		std::wstring normalized = pakPath;
		std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
		std::filesystem::path result;
		size_t start = 0;
		while (start <= normalized.size())
		{
			size_t slash = normalized.find(L'/', start);
			std::wstring part = normalized.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
			if (!part.empty() && part != L".")
			{
				if (part == L"..")
					part = L"_up";
				result /= SafeFileName(part);
			}
			if (slash == std::wstring::npos)
				break;
			start = slash + 1;
		}
		if (result.empty())
			result = L"_root";
		return result;
	}

	std::wstring PakCompressionMethodName(const PakInfo& info, uint32_t index)
	{
		if (index < info.CompressionMethods.size())
			return info.CompressionMethods[index];
		return L"Method" + std::to_wstring(index);
	}

	std::string PakEntryJson(const PakFileRecord& record, const PakInfo& info)
	{
		const PakEntry& entry = record.Entry;
		std::ostringstream out;
		out << "{\n";
		out << "  \"path\": \"" << JsonEscape(record.Path) << "\",\n";
		out << "  \"offset\": " << entry.Offset << ",\n";
		out << "  \"size\": " << entry.Size << ",\n";
		out << "  \"uncompressed_size\": " << entry.UncompressedSize << ",\n";
		out << "  \"compression_method_index\": " << entry.CompressionMethodIndex << ",\n";
		out << "  \"compression_method\": \"" << JsonEscape(PakCompressionMethodName(info, entry.CompressionMethodIndex)) << "\",\n";
		out << "  \"encrypted\": " << ((entry.Flags & 0x01) ? "true" : "false") << ",\n";
		out << "  \"deleted\": " << ((entry.Flags & 0x02) ? "true" : "false") << ",\n";
		out << "  \"compression_block_size\": " << entry.CompressionBlockSize << ",\n";
		out << "  \"compression_block_count\": " << entry.CompressionBlocks.size() << ",\n";
		out << "  \"sha1\": \"" << (entry.HasPayloadHash ? WideToUtf8(HashToString(entry.Hash)) : "") << "\"\n";
		out << "}\n";
		return out.str();
	}

	void WritePakFileTree(const std::filesystem::path& destination, const PakInfo& info, const std::vector<PakFileRecord>& records)
	{
		std::map<std::wstring, int> outputPathCounts;
		for (const PakFileRecord& record : records)
		{
			std::filesystem::path outputBase = destination / L"Files" / SanitizePakPathForOutput(record.Path);
			std::wstring key = ToLower(outputBase.wstring());
			int& duplicateIndex = outputPathCounts[key];
			std::filesystem::path output = outputBase;
			if (duplicateIndex > 0)
				output += L".duplicate" + std::to_wstring(duplicateIndex);
			output += L".ue5pakentry.json";
			++duplicateIndex;
			WriteUtf8File(output, PakEntryJson(record, info));
		}
	}

	std::wstring PakFilesTsv(const PakInfo& info, const std::vector<PakFileRecord>& records)
	{
		std::wostringstream out;
		out << L"path\tsize\tuncompressed_size\tcompression\tencrypted\tdeleted\toffset\tsha1\n";
		for (const PakFileRecord& record : records)
		{
			const PakEntry& entry = record.Entry;
			out << TsvEscape(record.Path) << L'\t'
				<< entry.Size << L'\t'
				<< entry.UncompressedSize << L'\t'
				<< TsvEscape(PakCompressionMethodName(info, entry.CompressionMethodIndex)) << L'\t'
				<< ((entry.Flags & 0x01) ? L"true" : L"false") << L'\t'
				<< ((entry.Flags & 0x02) ? L"true" : L"false") << L'\t'
				<< entry.Offset << L'\t'
				<< (entry.HasPayloadHash ? HashToString(entry.Hash) : L"") << L'\n';
		}
		return out.str();
	}

	bool EnvironmentVariableEnabled(const wchar_t* name)
	{
		DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
		if (length == 0)
			return false;
		std::wstring value(length, L'\0');
		if (GetEnvironmentVariableW(name, value.data(), length) == 0)
			return false;
		if (!value.empty() && value.back() == L'\0')
			value.pop_back();
		value = ToLower(value);
		return value == L"1" || value == L"true" || value == L"yes" || value == L"on";
	}

	bool ShouldWriteFullPakEntryTree(size_t recordCount)
	{
		if (EnvironmentVariableEnabled(L"WINMERGE_UE5_PAK_FULL_TREE"))
			return true;
		return recordCount <= PakFullEntryTreeRecordLimit;
	}

	void WritePakFolderView(const std::filesystem::path& destination, const PakInfo& info, const std::vector<PakFileRecord>& records,
		bool fullEntryTree)
	{
		std::wstring filesTsv = PakFilesTsv(info, records);
		WriteUtf8File(destination / L"Pak" / L"01_files.tsv", filesTsv);
		WriteUtf8File(destination / L"Files" / L"00_internal_files.tsv", filesTsv);

		if (fullEntryTree)
		{
			WritePakFileTree(destination, info, records);
		}
		else
		{
			std::wostringstream note;
			note << L"Large pak lightweight view.\n";
			note << L"Entry count: " << records.size() << L"\n";
			note << L"Full per-entry JSON tree is skipped by default above " << PakFullEntryTreeRecordLimit << L" entries to keep WinMerge responsive.\n";
			note << L"Set WINMERGE_UE5_PAK_FULL_TREE=1 to force the old per-entry tree output.\n";
			WriteUtf8File(destination / L"Files" / L"01_large_pak_note.txt", note.str());
		}
	}

	std::string PakSummaryJson(const std::filesystem::path& source, const PakInfo& info, const std::wstring& cryptoPath,
		const std::wstring& mountPoint, size_t recordCount, bool fullEntryTree)
	{
		std::ostringstream out;
		out << "{\n";
		out << "  \"kind\": \"Unreal pak\",\n";
		out << "  \"source_path\": \"" << JsonEscape(source.wstring()) << "\",\n";
		out << "  \"file_name\": \"" << JsonEscape(source.filename().wstring()) << "\",\n";
		out << "  \"size\": " << std::filesystem::file_size(source) << ",\n";
		out << "  \"version\": " << info.Version << ",\n";
		out << "  \"index_offset\": " << info.IndexOffset << ",\n";
		out << "  \"index_size\": " << info.IndexSize << ",\n";
		out << "  \"index_encrypted\": " << (info.EncryptedIndex ? "true" : "false") << ",\n";
		out << "  \"index_sha1\": \"" << WideToUtf8(HashToString(info.IndexHash)) << "\",\n";
		out << "  \"encryption_key_guid\": \"" << JsonEscape(IsZeroGuid(info.EncryptionKeyGuid) ? L"00000000-0000-0000-0000-000000000000" : GuidToString(info.EncryptionKeyGuid)) << "\",\n";
		out << "  \"crypto_json\": \"" << JsonEscape(cryptoPath) << "\",\n";
		out << "  \"mount_point\": \"" << JsonEscape(mountPoint) << "\",\n";
		out << "  \"file_count\": " << recordCount << ",\n";
		out << "  \"folder_view_mode\": \"" << (fullEntryTree ? "full-entry-tree" : "large-pak-lightweight") << "\",\n";
		out << "  \"full_entry_tree_limit\": " << PakFullEntryTreeRecordLimit << ",\n";
		out << "  \"generated_by\": \"libUE5Core\"\n";
		out << "}\n";
		return out.str();
	}

	bool DecryptAndValidatePakIndexData(std::vector<uint8_t>& data, const PakInfo& info, const std::array<uint8_t, 20>& expectedHash,
		const std::filesystem::path& pakPath, std::filesystem::path& cryptoJsonPath, std::wstring& error)
	{
		if (info.EncryptedIndex)
		{
			if (cryptoJsonPath.empty() && !FindCryptoJsonForPak(pakPath, cryptoJsonPath))
			{
				error = L"Missing Crypto.json for encrypted pak index. Searched upward from: " + pakPath.parent_path().wstring();
				return false;
			}

			std::array<uint8_t, 32> key{};
			if (!LoadCryptoKey(cryptoJsonPath, key, error))
				return false;
			if (!DecryptAes256Ecb(data, key, error))
				return false;
		}

		std::array<uint8_t, 20> actualHash{};
		if (!Sha1Hash(data, actualHash))
		{
			error = L"Failed to compute pak index SHA1";
			return false;
		}
		if (actualHash != expectedHash)
		{
			error = L"Pak index SHA1 mismatch after decrypt/read";
			return false;
		}
		return true;
	}

	bool ReadDirectoryIndex(BinaryReader& reader, const PakInfo& info, const std::vector<uint8_t>& encodedEntries,
		const std::vector<PakEntry>& unencodedEntries, std::vector<PakFileRecord>& records, std::wstring& error)
	{
		int32_t directoryCount = reader.Read<int32_t>();
		if (reader.IsError() || directoryCount < 0 || directoryCount > 1000000)
		{
			error = L"Invalid pak directory count";
			return false;
		}

		for (int32_t directoryIndex = 0; directoryIndex < directoryCount; ++directoryIndex)
		{
			std::wstring directoryName = reader.ReadFString();
			int32_t fileCount = reader.Read<int32_t>();
			if (reader.IsError() || fileCount < 0 || fileCount > 1000000)
			{
				error = L"Invalid pak file count in directory index";
				return false;
			}
			for (int32_t fileIndex = 0; fileIndex < fileCount; ++fileIndex)
			{
				std::wstring fileName = reader.ReadFString();
				int32_t location = reader.Read<int32_t>();
				PakEntry entry;
				if (!ResolvePakEntryLocation(location, encodedEntries, unencodedEntries, info, entry))
				{
					error = L"Invalid pak entry location";
					return false;
				}
				records.push_back({ CombinePakPath(directoryName, fileName), std::move(entry) });
			}
		}
		return !reader.IsError();
	}

	bool SkipPathHashIndex(BinaryReader& reader, std::wstring& error)
	{
		int32_t count = reader.Read<int32_t>();
		if (reader.IsError() || count < 0 || count > 10000000)
		{
			error = L"Invalid pak path hash index count";
			return false;
		}
		for (int32_t index = 0; index < count; ++index)
		{
			(void)reader.Read<uint64_t>();
			(void)reader.Read<int32_t>();
		}
		return !reader.IsError();
	}

	bool LoadPakIndex(const std::filesystem::path& source, const PakInfo& info, uint64_t fileSize, std::filesystem::path& cryptoJsonPath,
		std::wstring& mountPoint, std::vector<PakFileRecord>& records, std::wstring& error)
	{
		if (info.IndexSize > static_cast<int64_t>(512ull * 1024ull * 1024ull))
		{
			error = L"Pak index is too large for in-process comparison";
			return false;
		}

		std::vector<uint8_t> primaryIndexData;
		if (!ReadFileRange(source, static_cast<uint64_t>(info.IndexOffset), static_cast<size_t>(info.IndexSize), primaryIndexData, error))
			return false;
		if (!DecryptAndValidatePakIndexData(primaryIndexData, info, info.IndexHash, source, cryptoJsonPath, error))
			return false;

		BinaryReader primaryReader(std::move(primaryIndexData));
		mountPoint = primaryReader.ReadFString();
		if (mountPoint.size() > 65535)
		{
			error = L"Pak mount point is too long";
			return false;
		}

		int32_t entryCount = primaryReader.Read<int32_t>();
		if (primaryReader.IsError() || entryCount < 0 || entryCount > 10000000)
		{
			error = L"Invalid pak entry count";
			return false;
		}

		if (info.Version < PakFileVersionPathHashIndex)
		{
			records.reserve(static_cast<size_t>(entryCount));
			for (int32_t index = 0; index < entryCount; ++index)
			{
				std::wstring filename = primaryReader.ReadFString();
				PakEntry entry;
				if (!ReadPakEntry(primaryReader, info.Version, entry))
				{
					error = L"Failed to read legacy pak entry";
					return false;
				}
				records.push_back({ filename, std::move(entry) });
			}
			return !primaryReader.IsError();
		}

		(void)primaryReader.Read<uint64_t>(); // PathHashSeed
		bool hasPathHashIndex = ReadPakBool32(primaryReader);
		int64_t pathHashIndexOffset = -1;
		int64_t pathHashIndexSize = 0;
		std::array<uint8_t, 20> pathHashIndexHash{};
		if (hasPathHashIndex)
		{
			pathHashIndexOffset = primaryReader.Read<int64_t>();
			pathHashIndexSize = primaryReader.Read<int64_t>();
			for (uint8_t& byte : pathHashIndexHash)
				byte = primaryReader.Read<uint8_t>();
			hasPathHashIndex = pathHashIndexOffset != -1;
		}

		bool hasFullDirectoryIndex = ReadPakBool32(primaryReader);
		int64_t fullDirectoryIndexOffset = -1;
		int64_t fullDirectoryIndexSize = 0;
		std::array<uint8_t, 20> fullDirectoryIndexHash{};
		if (hasFullDirectoryIndex)
		{
			fullDirectoryIndexOffset = primaryReader.Read<int64_t>();
			fullDirectoryIndexSize = primaryReader.Read<int64_t>();
			for (uint8_t& byte : fullDirectoryIndexHash)
				byte = primaryReader.Read<uint8_t>();
			hasFullDirectoryIndex = fullDirectoryIndexOffset != -1;
		}

		std::vector<uint8_t> encodedEntries = ReadByteArray(primaryReader, 512 * 1024 * 1024);
		int32_t unencodedEntryCount = primaryReader.Read<int32_t>();
		if (primaryReader.IsError() || unencodedEntryCount < 0 || unencodedEntryCount > entryCount)
		{
			error = L"Invalid pak unencoded entry count";
			return false;
		}
		std::vector<PakEntry> unencodedEntries;
		unencodedEntries.reserve(static_cast<size_t>(unencodedEntryCount));
		for (int32_t index = 0; index < unencodedEntryCount; ++index)
		{
			PakEntry entry;
			if (!ReadPakEntry(primaryReader, info.Version, entry))
			{
				error = L"Failed to read pak unencoded entry";
				return false;
			}
			unencodedEntries.push_back(std::move(entry));
		}

		if (hasFullDirectoryIndex)
		{
			if (fullDirectoryIndexOffset < 0 || fullDirectoryIndexSize < 0 ||
				static_cast<uint64_t>(fullDirectoryIndexOffset) + static_cast<uint64_t>(fullDirectoryIndexSize) > fileSize ||
				fullDirectoryIndexSize > static_cast<int64_t>(512ull * 1024ull * 1024ull))
			{
				error = L"Invalid pak full directory index range";
				return false;
			}

			std::vector<uint8_t> directoryIndexData;
			if (!ReadFileRange(source, static_cast<uint64_t>(fullDirectoryIndexOffset), static_cast<size_t>(fullDirectoryIndexSize), directoryIndexData, error))
				return false;
			if (!DecryptAndValidatePakIndexData(directoryIndexData, info, fullDirectoryIndexHash, source, cryptoJsonPath, error))
				return false;

			BinaryReader directoryReader(std::move(directoryIndexData));
			return ReadDirectoryIndex(directoryReader, info, encodedEntries, unencodedEntries, records, error);
		}

		if (hasPathHashIndex)
		{
			if (pathHashIndexOffset < 0 || pathHashIndexSize < 0 ||
				static_cast<uint64_t>(pathHashIndexOffset) + static_cast<uint64_t>(pathHashIndexSize) > fileSize ||
				pathHashIndexSize > static_cast<int64_t>(512ull * 1024ull * 1024ull))
			{
				error = L"Invalid pak path hash index range";
				return false;
			}

			std::vector<uint8_t> pathIndexData;
			if (!ReadFileRange(source, static_cast<uint64_t>(pathHashIndexOffset), static_cast<size_t>(pathHashIndexSize), pathIndexData, error))
				return false;
			if (!DecryptAndValidatePakIndexData(pathIndexData, info, pathHashIndexHash, source, cryptoJsonPath, error))
				return false;

			BinaryReader pathReader(std::move(pathIndexData));
			if (!SkipPathHashIndex(pathReader, error))
				return false;
			return ReadDirectoryIndex(pathReader, info, encodedEntries, unencodedEntries, records, error);
		}

		error = L"Pak does not contain a readable directory index";
		return false;
	}

	std::string TopSummaryJson(const std::filesystem::path& source, const std::wstring& kind);

	bool UnpackPak(const std::filesystem::path& source, const std::filesystem::path& destination, std::wstring& error)
	{
		std::filesystem::create_directories(destination);
		WriteUtf8File(destination / L"00_summary.json", TopSummaryJson(source, L"Unreal pak"));
		WriteSidecars(source, destination);

		uint64_t fileSize = std::filesystem::file_size(source);
		PakInfo info;
		if (!ReadPakInfoFromFile(source, fileSize, info, error))
		{
			WriteUtf8File(destination / L"Pak" / L"00_error.txt", error + L"\n");
			return false;
		}

		std::filesystem::path cryptoJsonPath;
		std::wstring mountPoint;
		std::vector<PakFileRecord> records;
		if (!LoadPakIndex(source, info, fileSize, cryptoJsonPath, mountPoint, records, error))
		{
			WriteUtf8File(destination / L"Pak" / L"00_error.txt", error + L"\n");
			return false;
		}

		bool fullEntryTree = ShouldWriteFullPakEntryTree(records.size());
		WriteUtf8File(destination / L"Pak" / L"00_summary.json", PakSummaryJson(source, info, cryptoJsonPath.wstring(), mountPoint, records.size(), fullEntryTree));
		WritePakFolderView(destination, info, records, fullEntryTree);
		return true;
	}

	std::string TopSummaryJson(const std::filesystem::path& source, const std::wstring& kind)
	{
		uintmax_t size = std::filesystem::file_size(source);
		std::ostringstream out;
		out << "{\n";
		out << "  \"kind\": \"" << JsonEscape(kind) << "\",\n";
		out << "  \"source_path\": \"" << JsonEscape(source.wstring()) << "\",\n";
		out << "  \"file_name\": \"" << JsonEscape(source.filename().wstring()) << "\",\n";
		out << "  \"extension\": \"" << JsonEscape(ExtensionOf(source.wstring())) << "\",\n";
		out << "  \"size\": " << size << ",\n";
		out << "  \"generated_by\": \"libUE5Core\"\n";
		out << "}\n";
		return out.str();
	}

	bool UnpackPackage(const std::filesystem::path& source, const std::filesystem::path& destination, const std::vector<uint8_t>& bytes, std::wstring& error)
	{
		BinaryReader reader(bytes);
		PackageSummary summary;
		if (!TryReadPackageSummary(reader, summary, error))
		{
			WriteUtf8File(destination / L"UE5Core" / L"00_package_summary_error.txt", error + L"\n");
			return true;
		}

		WriteUtf8File(destination / L"UE5Core" / L"00_package_summary.json", PackageSummaryJson(summary));
		WriteCustomVersions(destination, summary);
		std::vector<std::wstring> names = TryReadNameMap(reader, summary);
		WriteNameMap(destination, names);
		WriteThumbnailImages(destination, bytes, summary);
		WriteEmbeddedImages(source, destination, bytes);
		if (LooksLikeTextureAsset(names) && !std::filesystem::exists(destination / L"Images"))
		{
			WriteUtf8File(destination / L"Images" / L"00_texture_asset_note.txt",
				L"Texture-like asset detected, but no package thumbnail or embedded PNG/JPEG image was found. Decoding cooked GPU texture mips is a later libUE5Core migration step.\n");
		}
		WriteUtf8File(destination / L"Objects" / L"00_exports_imports_note.txt",
			L"libUE5Core currently exports PackageFileSummary and NameMap locally. ImportMap, ExportMap, and reflected UObject property decoding are the next UE5Core migration step.\n");
		return true;
	}

	std::filesystem::path FindPayloadPackage(const std::filesystem::path& source)
	{
		static const wchar_t* PackageExtensions[] = { L".uasset", L".umap" };
		std::filesystem::path dir = source.parent_path();
		std::wstring stem = source.stem().wstring();
		for (const wchar_t* extension : PackageExtensions)
		{
			std::filesystem::path candidate = dir / (stem + extension);
			if (std::filesystem::exists(candidate) && std::filesystem::is_regular_file(candidate))
				return candidate;
		}
		return {};
	}

	std::string PayloadContextJson(const std::filesystem::path& source, const std::filesystem::path& packagePath, bool parsedPackage)
	{
		std::ostringstream out;
		out << "{\n";
		out << "  \"payload_path\": \"" << JsonEscape(source.wstring()) << "\",\n";
		out << "  \"payload_file_name\": \"" << JsonEscape(source.filename().wstring()) << "\",\n";
		if (packagePath.empty())
			out << "  \"context_package_path\": null,\n";
		else
			out << "  \"context_package_path\": \"" << JsonEscape(packagePath.wstring()) << "\",\n";
		out << "  \"context_package_parsed\": " << (parsedPackage ? "true" : "false") << "\n";
		out << "}\n";
		return out.str();
	}

	bool WritePayloadPackageContext(const std::filesystem::path& source, const std::filesystem::path& destination)
	{
		std::filesystem::path packagePath = FindPayloadPackage(source);
		if (packagePath.empty())
		{
			WriteUtf8File(destination / L"Payload" / L"00_context.json", PayloadContextJson(source, packagePath, false));
			WriteUtf8File(destination / L"UE5Core" / L"00_package_summary_error.txt",
				L"No same-stem .uasset or .umap package was found for this export payload.\n");
			return true;
		}

		std::wstring error;
		std::vector<uint8_t> packageBytes;
		if (!LoadBinaryFile(packagePath.wstring(), packageBytes, error))
		{
			WriteUtf8File(destination / L"Payload" / L"00_context.json", PayloadContextJson(source, packagePath, false));
			WriteUtf8File(destination / L"UE5Core" / L"00_package_summary_error.txt", error + L"\n");
			return true;
		}

		BinaryReader reader(packageBytes);
		PackageSummary summary;
		if (!TryReadPackageSummary(reader, summary, error))
		{
			WriteUtf8File(destination / L"Payload" / L"00_context.json", PayloadContextJson(source, packagePath, false));
			WriteUtf8File(destination / L"UE5Core" / L"00_package_summary_error.txt", error + L"\n");
			return true;
		}

		WriteUtf8File(destination / L"Payload" / L"00_context.json", PayloadContextJson(source, packagePath, true));
		WriteUtf8File(destination / L"UE5Core" / L"00_package_summary.json", PackageSummaryJson(summary));
		WriteCustomVersions(destination, summary);
		std::vector<std::wstring> names = TryReadNameMap(reader, summary);
		WriteNameMap(destination, names);
		WriteThumbnailImages(destination, packageBytes, summary);
		return true;
	}

	bool UnpackGeneric(const std::filesystem::path& source, const std::filesystem::path& destination, const std::wstring& kind, const std::vector<uint8_t>& bytes);

	bool UnpackPayload(const std::filesystem::path& source, const std::filesystem::path& destination, const std::wstring& kind, const std::vector<uint8_t>& bytes)
	{
		UnpackGeneric(source, destination, kind, bytes);
		WritePayloadPackageContext(source, destination);
		WriteEmbeddedImages(source, destination, bytes);
		WriteUtf8File(destination / L"Objects" / L"00_payload_note.txt",
			L"Export payload bytes are shown with binary/string summaries plus any same-stem package metadata. Reflected UObject property decoding is a later libUE5Core migration step.\n");
		return true;
	}

	bool UnpackGeneric(const std::filesystem::path& source, const std::filesystem::path& destination, const std::wstring& kind, const std::vector<uint8_t>& bytes)
	{
		std::filesystem::create_directories(destination);
		WriteUtf8File(destination / L"00_summary.json", TopSummaryJson(source, kind));
		WriteSidecars(source, destination);
		WriteHexPreview(destination, bytes);
		WriteStringSummary(destination, bytes);
		return true;
	}
}

bool IsSupportedFile(const std::wstring& path)
{
	std::wstring extension = ExtensionOf(path);
	return IsPackageExtension(extension) || IsIoStoreExtension(extension) || IsPayloadExtension(extension) || extension == L".pak";
}

bool UnpackFolder(const std::wstring& sourcePath, const std::wstring& destinationFolder, std::wstring* errorMessage)
{
	std::wstring error;
	try
	{
		std::filesystem::path source(sourcePath);
		std::filesystem::path destination(destinationFolder);
		std::wstring extension = ExtensionOf(sourcePath);
		if (extension == L".pak")
		{
			if (!UnpackPak(source, destination, error))
			{
				if (errorMessage)
					*errorMessage = error;
				return false;
			}
			return true;
		}

		std::vector<uint8_t> bytes;
		if (!LoadBinaryFile(sourcePath, bytes, error))
		{
			if (errorMessage)
				*errorMessage = error;
			return false;
		}

		std::wstring kind = L"Unreal file";
		if (IsPackageExtension(extension))
			kind = L"Unreal package";
		else if (extension == L".pak")
			kind = L"Unreal pak";
		else if (IsIoStoreExtension(extension))
			kind = L"Unreal IoStore container";
		else if (IsPayloadExtension(extension))
			kind = L"Unreal export payload";

		if (IsPackageExtension(extension))
		{
			UnpackGeneric(source, destination, kind, bytes);
			return UnpackPackage(source, destination, bytes, error);
		}
		if (IsPayloadExtension(extension))
			return UnpackPayload(source, destination, kind, bytes);
		UnpackGeneric(source, destination, kind, bytes);
		return true;
	}
	catch (const std::exception& ex)
	{
		error = Utf8ToWide(ex.what());
	}

	if (errorMessage)
		*errorMessage = error.empty() ? L"libUE5Core failed" : error;
	return false;
}
}
