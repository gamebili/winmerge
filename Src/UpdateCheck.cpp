/**
 * @file  UpdateCheck.cpp
 *
 * @brief Implementation of the GitHub release update checker.
 *
 * Queries the latest published release of the configured GitHub repository
 * (see UpdateApiURL in Constants.h), compares its tag with the running file
 * version, and notifies the main window if a newer version exists. The network
 * request runs on a detached background thread so startup is never blocked.
 */
#include "pch.h"
#include "UpdateCheck.h"
#include "Constants.h"
#include "UnicodeString.h"
#include "VersionInfo.h"
#include <vector>
#include <string>
#include <thread>
#include <wininet.h>

#pragma comment(lib, "wininet.lib")

namespace
{
	/** @brief Issue an HTTPS GET and return the response body on HTTP 200. */
	bool HttpGetBody(const wchar_t* url, std::string& body)
	{
		// The agent string doubles as the User-Agent header, which the GitHub
		// API requires; requests without one are rejected.
		HINTERNET hInet = InternetOpenW(L"WinMerge-UpdateCheck",
			INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
		if (!hInet)
			return false;
		const DWORD flags = INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD |
			INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI;
		HINTERNET hUrl = InternetOpenUrlW(hInet, url,
			L"Accept: application/vnd.github+json\r\n",
			static_cast<DWORD>(-1), flags, 0);
		if (!hUrl)
		{
			InternetCloseHandle(hInet);
			return false;
		}
		DWORD status = 0, len = sizeof(status);
		HttpQueryInfoW(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
			&status, &len, nullptr);
		const bool ok = (status == 200);
		if (ok)
		{
			char buf[4096];
			DWORD read = 0;
			// Cap the body so a malformed endpoint cannot grow it without bound.
			while (body.size() < 1024 * 1024 &&
				InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0)
				body.append(buf, read);
		}
		InternetCloseHandle(hUrl);
		InternetCloseHandle(hInet);
		return ok && !body.empty();
	}

	/** @brief Pull the "tag_name" string value out of the release JSON. */
	std::string ExtractTagName(const std::string& json)
	{
		const std::string key = "\"tag_name\"";
		size_t p = json.find(key);
		if (p == std::string::npos) return "";
		p = json.find(':', p + key.size());
		if (p == std::string::npos) return "";
		p = json.find('"', p);
		if (p == std::string::npos) return "";
		const size_t q = json.find('"', p + 1);
		if (q == std::string::npos) return "";
		return json.substr(p + 1, q - p - 1);
	}

	/** @brief Parse "v2.16.56.6" / "2.16.56.6" into its numeric components. */
	std::vector<int> ParseVersion(const std::string& s)
	{
		std::vector<int> v;
		size_t i = 0;
		if (i < s.size() && (s[i] == 'v' || s[i] == 'V'))
			++i;
		int cur = 0;
		bool has = false;
		for (; i < s.size(); ++i)
		{
			const char c = s[i];
			if (c >= '0' && c <= '9') { cur = cur * 10 + (c - '0'); has = true; }
			else if (c == '.') { v.push_back(cur); cur = 0; has = false; }
			else break;
		}
		if (has || !v.empty())
			v.push_back(cur);
		return v;
	}

	/** @brief Component-wise version compare; missing trailing parts are 0. */
	int CompareVersions(const std::vector<int>& a, const std::vector<int>& b)
	{
		const size_t n = (a.size() > b.size()) ? a.size() : b.size();
		for (size_t i = 0; i < n; ++i)
		{
			const int ai = (i < a.size()) ? a[i] : 0;
			const int bi = (i < b.size()) ? b[i] : 0;
			if (ai != bi)
				return ai < bi ? -1 : 1;
		}
		return 0;
	}

	/** @brief Convert an ASCII version string to String (no codepage logic). */
	String AsciiToString(const std::string& s)
	{
		String r;
		r.reserve(s.size());
		for (char c : s)
			r += static_cast<tchar_t>(static_cast<unsigned char>(c));
		return r;
	}
}

namespace UpdateCheck
{
	void StartAsyncCheck(HWND hwndNotify, bool manual)
	{
		// Read the running file version on the caller (UI) thread.
		std::vector<int> current;
		{
			CVersionInfo ver(true);
			unsigned ms = 0, ls = 0;
			if (ver.GetFixedFileVersion(ms, ls))
				current = {
					static_cast<int>(ms >> 16), static_cast<int>(ms & 0xffff),
					static_cast<int>(ls >> 16), static_cast<int>(ls & 0xffff) };
		}

		std::thread([hwndNotify, manual, current]()
		{
			String* result = nullptr; // nullptr => the check failed
			std::string body;
			if (HttpGetBody(UpdateApiURL, body))
			{
				std::string tag = ExtractTagName(body);
				const std::vector<int> latest = ParseVersion(tag);
				if (!latest.empty())
				{
					if (CompareVersions(current, latest) < 0)
					{
						if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V'))
							tag.erase(0, 1);
						result = new String(AsciiToString(tag));
					}
					else
					{
						result = new String(); // up to date
					}
				}
			}
			::PostMessage(hwndNotify, WM_APP_UPDATECHECK_RESULT,
				manual ? 1 : 0, reinterpret_cast<LPARAM>(result));
		}).detach();
	}
}
