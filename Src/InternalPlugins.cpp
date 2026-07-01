#include "pch.h"
#include "Plugins.h"
#define POCO_NO_UNWINDOWS 1
#include <Poco/FileStream.h>
#include <Poco/XML/XMLWriter.h>
#include <Poco/SAX/SAXParser.h>
#include <Poco/SAX/SAXException.h>
#include <Poco/SAX/ContentHandler.h>
#include <Poco/SAX/Attributes.h>
#include <Poco/SAX/AttributesImpl.h>
#include <Poco/Exception.h>
#include <vector>
#include <list>
#include <windows.h>
#include <Shlwapi.h>
#include "InternalPlugins.h"
#include "MergeApp.h"
#include "paths.h"
#include "Environment.h"
#include "OptionsMgr.h"
#include "OptionsDef.h"
#include "codepage_detect.h"
#include "UniFile.h"
#include "WinMergePluginBase.h"
#include "TempFile.h"
#include "Logger.h"
#include "libUE5Core/UE5Core.h"
#include <chrono>

using Poco::FileStream;
using Poco::Exception;
using Poco::XML::XMLWriter;
using Poco::XML::SAXParser;
using Poco::XML::ContentHandler;
using Poco::XML::Locator;
using Poco::XML::XMLChar;
using Poco::XML::XMLString;
using Poco::XML::Attributes;
using Poco::XML::AttributesImpl;
using namespace std::literals::string_literals;

namespace
{
	HRESULT ReadFile(const String& path, String& text)
	{
		UniMemFile file;
		if (!file.OpenReadOnly(path))
			return HRESULT_FROM_WIN32(GetLastError());
		file.ReadBom();
		if (!file.HasBom())
		{
			int iGuessEncodingType = GetOptionsMgr()->GetInt(OPT_CP_DETECT);
			int64_t fileSize = file.GetFileSize();
			FileTextEncoding encoding = codepage_detect::Guess(
				paths::FindExtension(path), file.GetBase(), static_cast<size_t>(
					fileSize < static_cast<int64_t>(codepage_detect::BufSize) ?
					fileSize : static_cast<int64_t>(codepage_detect::BufSize)),
				iGuessEncodingType);
			file.SetCodepage(encoding.m_codepage);
		}
		file.ReadStringAll(text);
		file.Close();
		return S_OK;
	}

	HRESULT WriteFile(const String& path, const String& text, bool bom = true)
	{
		UniStdioFile fileOut;
		if (!fileOut.Open(path, _T("wb")))
			return HRESULT_FROM_WIN32(GetLastError());
		fileOut.SetUnicoding(ucr::UNICODESET::UTF8);
		fileOut.WriteString(text);
		fileOut.Close();
		return S_OK;
	}

	bool IsExcelPluginLogEnabled()
	{
		DWORD length = GetEnvironmentVariable(_T("WINMERGE_EXCEL_LOG"), nullptr, 0);
		if (length == 0)
			return false;

		std::vector<tchar_t> buffer(length);
		if (GetEnvironmentVariable(_T("WINMERGE_EXCEL_LOG"), buffer.data(), static_cast<DWORD>(buffer.size())) == 0)
			return false;

		String value = strutils::makelower(strutils::trim_ws(buffer.data()));
		return !(value.empty() || value == _T("0") || value == _T("false") ||
			value == _T("off") || value == _T("no"));
	}

	int64_t ElapsedMilliseconds(std::chrono::steady_clock::time_point start)
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - start).count();
	}

	String FormatHResult(HRESULT hr)
	{
		return strutils::format(_T("0x%08X"), static_cast<unsigned>(hr));
	}

	String TrimLogOutput(const String& output)
	{
		String text = strutils::trim_ws(output);
		constexpr size_t MaxLogOutputChars = 12000;
		if (text.length() > MaxLogOutputChars)
			text = text.substr(0, MaxLogOutputChars) + _T("\n...<truncated>");
		return text;
	}

	HRESULT SetPluginErrorInfo(const String& source, const String& description, HRESULT fallback = DISP_E_EXCEPTION)
	{
		ICreateErrorInfo* pCreateErrorInfo = nullptr;
		if (SUCCEEDED(CreateErrorInfo(&pCreateErrorInfo)))
		{
			pCreateErrorInfo->SetSource(const_cast<OLECHAR*>(source.c_str()));
			pCreateErrorInfo->SetDescription(const_cast<OLECHAR*>(ucr::toUTF16(description).c_str()));
			IErrorInfo* pErrorInfo = nullptr;
			pCreateErrorInfo->QueryInterface(&pErrorInfo);
			SetErrorInfo(0, pErrorInfo);
			pErrorInfo->Release();
			pCreateErrorInfo->Release();
			return DISP_E_EXCEPTION;
		}
		return fallback;
	}

	String FormatWin32Error(DWORD error)
	{
		return strutils::format(_T("Win32 error %lu"), error);
	}

	using Excel2TsvUnpackFolderUtf16 = int (WINAPI *)(const wchar_t*, const wchar_t*, wchar_t*, size_t);

	HRESULT GetExcel2TsvUnpackFolder(Excel2TsvUnpackFolderUtf16& fn)
	{
#ifdef _UNICODE
		static HMODULE module = nullptr;
		static Excel2TsvUnpackFolderUtf16 cachedFn = nullptr;
		if (cachedFn)
		{
			fn = cachedFn;
			return S_OK;
		}

		const String dllPath = paths::ConcatPath(env::GetProgPath(), _T("Commands\\excel2tsv\\excel2tsv.dll"));
		if (!module)
		{
			module = LoadLibrary(dllPath.c_str());
			if (!module)
			{
				const DWORD error = GetLastError();
				return SetPluginErrorInfo(dllPath,
					strutils::format(_T("LoadLibrary failed for %s: %s"), dllPath.c_str(), FormatWin32Error(error).c_str()),
					HRESULT_FROM_WIN32(error));
			}
		}

		cachedFn = reinterpret_cast<Excel2TsvUnpackFolderUtf16>(
			GetProcAddress(module, "excel2tsv_unpack_folder_utf16"));
		if (!cachedFn)
		{
			const DWORD error = GetLastError();
			return SetPluginErrorInfo(dllPath,
				strutils::format(_T("GetProcAddress(excel2tsv_unpack_folder_utf16) failed: %s"), FormatWin32Error(error).c_str()),
				HRESULT_FROM_WIN32(error));
		}

		fn = cachedFn;
		return S_OK;
#else
		fn = nullptr;
		return E_NOTIMPL;
#endif
	}

	HRESULT UnpackExcelFolderInProcess(const wchar_t* source, const wchar_t* destination, bool logTiming)
	{
		Excel2TsvUnpackFolderUtf16 unpackFolder = nullptr;
		HRESULT hr = GetExcel2TsvUnpackFolder(unpackFolder);
		if (FAILED(hr))
			return hr;

		constexpr size_t DiagnosticChars = 65536;
		std::vector<wchar_t> diagnostics(DiagnosticChars);
		const int result = unpackFolder(source, destination, diagnostics.data(), diagnostics.size());
		const String diagnosticText = TrimLogOutput(diagnostics.data());
		if (logTiming && !diagnosticText.empty())
			RootLogger::Info(_T("[ExcelPlugin] dll output:\n") + diagnosticText);

		if (result != 0)
		{
			const String description = diagnosticText.empty() ?
				strutils::format(_T("excel2tsv.dll failed with code %d"), result) : diagnosticText;
			return SetPluginErrorInfo(_T("excel2tsv.dll"), description);
		}

		return S_OK;
	}

	HRESULT UnpackUE5CoreFolderInProcess(const wchar_t* source, const wchar_t* destination)
	{
		std::wstring error;
		if (!UE5Core::UnpackFolder(source, destination, &error))
		{
			String description = error.empty() ? _T("Failed to unpack Unreal Engine file") : String(error.c_str());
			return SetPluginErrorInfo(_T("libUE5Core"), description);
		}
		return S_OK;
	}

	// Locate the Lua 5.4 interpreter used to execute config files. Order:
	// explicit WINMERGE_LUA54 override, a copy bundled next to WinMerge, the
	// known project location, then bare "lua54.exe" (resolved via PATH).
	String ResolveLua54Path()
	{
		DWORD length = GetEnvironmentVariable(_T("WINMERGE_LUA54"), nullptr, 0);
		if (length > 0)
		{
			std::vector<tchar_t> buffer(length);
			if (GetEnvironmentVariable(_T("WINMERGE_LUA54"), buffer.data(), length) > 0)
			{
				String overridePath = strutils::trim_ws(buffer.data());
				if (!overridePath.empty() && paths::DoesPathExist(overridePath) == paths::IS_EXISTING_FILE)
					return overridePath;
			}
		}
		const String home = env::GetProgPath();
		static const tchar_t* candidates[] = {
			_T("lua54.exe"), _T("lua5.4.3\\lua54.exe"), _T("Commands\\lua\\lua54.exe")
		};
		for (const tchar_t* rel : candidates)
		{
			String candidate = paths::ConcatPath(home, rel);
			if (paths::DoesPathExist(candidate) == paths::IS_EXISTING_FILE)
				return candidate;
		}
		const String projectDefault = _T("D:\\p4_gl2\\PG2\\Trunk\\Common\\Util\\lua5.4.3\\lua54.exe");
		if (paths::DoesPathExist(projectDefault) == paths::IS_EXISTING_FILE)
			return projectDefault;
		return _T("lua54.exe");
	}

	// Converter executed by lua54: dofile() the config and emit a deterministic
	// CSV; on any failure pass the raw source through (minus the appended
	// "-----Json" marker) so two revisions stay symmetric and comparable.
	const String& Lua2CsvConverter()
	{
		static const String script = LR"LUA2CSV(
local src, dst = arg[1], arg[2]

local function passthrough()
  local inf = io.open(src, "rb")
  local outf = io.open(dst, "wb")
  if inf and outf then
    for line in inf:lines() do
      if line:sub(1, 9) ~= "-----Json" and not line:find("--generated code", 1, true) then
        outf:write(line, "\n")
      end
    end
  end
  if inf then inf:close() end
  if outf then outf:close() end
  os.exit(0)
end

if not src or not dst then os.exit(0) end

local chunk = loadfile(src)
if not chunk then passthrough() end

-- Bound runaway configs (e.g. an accidental infinite loop) so a bad file
-- cannot hang WinMerge indefinitely; abort turns into the passthrough below.
local hookCount = 0
debug.sethook(function()
  hookCount = hookCount + 1
  if hookCount > 2000 then error("lua2csv: instruction limit exceeded") end
end, "", 1000000)
local ok, ret, retMd5, retSheetMd5 = pcall(chunk)
debug.sethook()
if not ok or type(ret) ~= "table" then passthrough() end

-- deterministic total order: numbers numerically, ties broken by type then raw
-- string form (so a number key and its string form never compare "equal"),
-- then strings lexically.
local function key_lt(a, b)
  local na, nb = tonumber(a), tonumber(b)
  if na and nb then
    if na ~= nb then return na < nb end
    local ta, tb = type(a), type(b)
    if ta ~= tb then return ta < tb end
    return tostring(a) < tostring(b)
  end
  if na and not nb then return true end
  if nb and not na then return false end
  return tostring(a) < tostring(b)
end

local function sorted_keys(t)
  local ks = {}
  for k in pairs(t) do ks[#ks + 1] = k end
  table.sort(ks, key_lt)
  return ks
end

----------------------------------------------------------------------
-- Functions are compared by their source text. Recover it from the file
-- via debug.getinfo line info, extracting the exact function...end block.
-- The __FPOOL/shared-f forms define each function on its own line; true
-- inline functions may share a record line, so disambiguate by behavior.
----------------------------------------------------------------------
local srcLines
local function getSrcLines()
  if srcLines then return srcLines end
  srcLines = {}
  local fh = io.open(src, "r")
  if fh then
    for line in fh:lines() do srcLines[#srcLines + 1] = line end
    fh:close()
  end
  return srcLines
end

-- Return an array of every top-level "function...end" substring found in text,
-- in source order, skipping strings and comments and matching nested blocks.
local function extractBlocks(text)
  local blocks = {}
  local n = #text
  local i = 1
  while i <= n do
    local a, b = text:find("function", i, true)
    if not a then break end
    local before = a > 1 and text:sub(a - 1, a - 1) or ""
    local after = (b < n) and text:sub(b + 1, b + 1) or ""
    if before:match("[%w_]") or after:match("[%w_]") then
      i = b + 1
    else
      local depth, j, endpos = 0, a, nil
      while j <= n do
        local c = text:sub(j, j)
        if c == "-" and text:sub(j + 1, j + 1) == "-" then
          local eq = text:match("^%[(=*)%[", j + 2)
          if eq then
            local e = text:find("]" .. eq .. "]", j + 2, true)
            j = e and (e + #eq + 2) or (n + 1)
          else
            local e = text:find("\n", j + 2, true)
            j = e and (e + 1) or (n + 1)
          end
        elseif c == '"' or c == "'" then
          local q = c; j = j + 1
          while j <= n do
            local cc = text:sub(j, j)
            if cc == "\\" then j = j + 2
            elseif cc == q then j = j + 1; break
            else j = j + 1 end
          end
        elseif c == "[" and text:match("^%[=*%[", j) then
          local eq = text:match("^%[(=*)%[", j)
          local e = text:find("]" .. eq .. "]", j + 2, true)
          j = e and (e + #eq + 2) or (n + 1)
        elseif c:match("[%a_]") then
          local _, we, word = text:find("^([%a_][%w_]*)", j)
          if word == "function" or word == "if" or word == "do" then
            depth = depth + 1
          elseif word == "end" then
            depth = depth - 1
            if depth == 0 then endpos = we; break end
          end
          j = we + 1
        else
          j = j + 1
        end
      end
      if endpos then
        blocks[#blocks + 1] = text:sub(a, endpos)
        i = endpos + 1
      else
        blocks[#blocks + 1] = text:sub(a)
        break
      end
    end
  end
  return blocks
end
)LUA2CSV" LR"LUA2CSV(
local function hashstr(s)
  local h = 5381
  for k = 1, #s do h = (h * 33 + s:byte(k)) % 4294967296 end
  return string.format("%08x", h)
end

-- A behavioral fingerprint: outputs over fixed sample inputs. Used to map a
-- runtime function to its source block when several share one line. Pure config
-- formulas (the only multi-per-line case) are deterministic, so this is stable.
local BEHAVIOR_SAMPLES = { 0, 1, 2, 3, 7, 100, -1 }
local function behaviorSig(fn)
  local sig = {}
  for _, x in ipairs(BEHAVIOR_SAMPLES) do
    local ok, r = pcall(fn, x, x * 2 + 1, x - 1)
    sig[#sig + 1] = ok and (type(r) .. ":" .. tostring(r)) or "!"
  end
  return table.concat(sig, "|")
end

local funcCache = {}
local function funcToString(fn)
  local cached = funcCache[fn]
  if cached ~= nil then return cached end
  local result
  local info = debug.getinfo(fn, "S")
  if info and info.linedefined and info.linedefined > 0
      and info.source and info.source:sub(1, 1) == "@" then
    local lines = getSrcLines()
    local from, to = info.linedefined, info.lastlinedefined
    if not to or to < from then to = from end
    local seg = {}
    for li = from, to do seg[#seg + 1] = lines[li] or "" end
    local blocks = extractBlocks(table.concat(seg, "\n"))
    if #blocks == 1 then
      result = blocks[1]
    elseif #blocks > 1 then
      -- Multiple functions share this line. string.dump is context-dependent
      -- (an array-nested function dumps differently from a standalone one), so
      -- match by behavior: compare outputs on a fixed set of sample inputs.
      local targetSig = behaviorSig(fn)
      for _, bt in ipairs(blocks) do
        local cf = load("return " .. bt)
        if cf then
          local okc, cfn = pcall(cf)
          if okc and type(cfn) == "function" and behaviorSig(cfn) == targetSig then
            result = bt; break
          end
        end
      end
      if not result then result = blocks[1] end
    end
  end
  if not result then
    local okd, d = pcall(string.dump, fn, true)
    result = okd and ("<fn:" .. hashstr(d) .. ">") or "<function>"
  end
  funcCache[fn] = result
  return result
end

-- compact, stable serialization for nested values placed inside a CSV cell.
-- seen guards against reference cycles along the current path (cleared on the
-- way out so shared references in a DAG still serialize correctly).
local serialize
serialize = function(v, seen, depth)
  local tv = type(v)
  if tv == "table" then
    seen = seen or {}
    depth = (depth or 0) + 1
    if seen[v] or depth > 64 then return "<cycle>" end
    seen[v] = true
    local parts = {}
    local n = #v
    for i = 1, n do parts[#parts + 1] = serialize(v[i], seen, depth) end
    local extra = {}
    for k in pairs(v) do
      if not (type(k) == "number" and k >= 1 and k <= n and math.floor(k) == k) then
        extra[#extra + 1] = k
      end
    end
    table.sort(extra, key_lt)
    for _, k in ipairs(extra) do
      parts[#parts + 1] = tostring(k) .. "=" .. serialize(v[k], seen, depth)
    end
    seen[v] = nil
    return "{" .. table.concat(parts, ",") .. "}"
  elseif tv == "string" then
    return v
  elseif tv == "number" or tv == "boolean" then
    return tostring(v)
  elseif tv == "function" then
    return funcToString(v)
  elseif tv == "nil" then
    return ""
  else
    return "<" .. tv .. ">"
  end
end
)LUA2CSV" LR"LUA2CSV(
local function csv_escape(s)
  if s:find('[",\r\n]') then
    return '"' .. s:gsub('"', '""') .. '"'
  end
  return s
end

local function cellValue(rec, c)
  if c == "_value" then
    if type(rec) ~= "table" then return rec end
    return nil
  end
  if type(rec) ~= "table" then return nil end
  local v = rec[c]
  if v == nil then
    local nc = tonumber(c)
    if nc ~= nil then v = rec[nc] end
  end
  return v
end

-- Build and write the CSV; any unexpected error degrades to passthrough.
local emitOk = pcall(function()
  local rowKeys = sorted_keys(ret)
  local colSet, cols = {}, {}
  local function addCol(name)
    if not colSet[name] then colSet[name] = true; cols[#cols + 1] = name end
  end

  local hasScalar = false
  for _, k in ipairs(rowKeys) do
    local rec = ret[k]
    if type(rec) == "table" then
      for fkey in pairs(rec) do
        if tostring(fkey) ~= "ID" then addCol(tostring(fkey)) end
      end
    else
      hasScalar = true
    end
  end
  if hasScalar then addCol("_value") end
  table.sort(cols, key_lt)

  local outf = assert(io.open(dst, "wb"))
  local header = { "ID" }
  for _, c in ipairs(cols) do header[#header + 1] = csv_escape(c) end
  outf:write(table.concat(header, ",") .. "\n")

  for _, k in ipairs(rowKeys) do
    if not (type(k) == "string" and k:sub(1, 2) == "__") then
      local rec = ret[k]
      local idval = tostring(k)
      if type(rec) == "table" and rec.ID ~= nil then idval = tostring(rec.ID) end
      local cells = { csv_escape(idval) }
      for _, c in ipairs(cols) do
        cells[#cells + 1] = csv_escape(serialize(cellValue(rec, c)))
      end
      outf:write(table.concat(cells, ",") .. "\n")
    end
  end

  -- Append the config's file-level checksums (the 2nd/3rd return values) as
  -- trailing rows so a source-workbook change is visible even when the row
  -- data is otherwise identical.
  if retMd5 ~= nil then
    outf:write("md5," .. csv_escape(serialize(retMd5)) .. "\n")
  end
  if retSheetMd5 ~= nil then
    outf:write("sheet_md5," .. csv_escape(serialize(retSheetMd5)) .. "\n")
  end
  outf:close()
end)

if not emitOk then passthrough() end
)LUA2CSV";
		return script;
	}
}

namespace internal_plugin
{

inline static const std::string Empty = "";
inline static const std::string PluginsElement = "plugins";
inline static const std::string PluginElement = "plugin";
inline static const std::string EventElement = "event";
inline static const std::string DescriptionElement = "description";
inline static const std::string FileFiltersElement = "file-filters";
inline static const std::string IsAutomaticElement = "is-automatic";
inline static const std::string UnpackedFileExtensionElement = "unpacked-file-extension";
inline static const std::string ExtendedPropertiesElement = "extended-properties";
inline static const std::string ArgumentsElement = "arguments";
inline static const std::string PipelineElement = "pipeline";
inline static const std::string PrediffFileElement = "prediff-file";
inline static const std::string UnpackFileElement = "unpack-file";
inline static const std::string PackFileElement = "pack-file";
inline static const std::string IsFolderElement = "is-folder";
inline static const std::string UnpackFolderElement = "unpack-folder";
inline static const std::string PackFolderElement = "pack-folder";
inline static const std::string CommandElement = "command";
inline static const std::string ScriptElement = "script";
inline static const std::string NameAttribute = "name";
inline static const std::string ValueAttribute = "value";
inline static const std::string FileExtensionAttribute = "fileExtension";

class XMLHandler : public Poco::XML::ContentHandler
{
public:
	explicit XMLHandler(std::list<Info>* pPlugins) : m_pPlugins(pPlugins) {}

	void setDocumentLocator(const Locator* loc) {}
	void startDocument() {}
	void endDocument() {}
	void startElement(const XMLString& uri, const XMLString& localName, const XMLString& qname, const Attributes& attributes)
	{
		if (!m_stack.empty())
		{
			if (m_stack.top() == PluginsElement)
			{
				if (localName == PluginElement)
				{
					String name;
					int index = attributes.getIndex(Empty, NameAttribute);
					if (index >= 0)
						name = ucr::toTString(attributes.getValue(index));
					m_pPlugins->emplace_back(name);
					m_pMethod = nullptr;
				}
			}
			else if (m_stack.top() == PluginElement)
			{
				Info& plugin = m_pPlugins->back();
				String value;
				int index = attributes.getIndex(Empty, ValueAttribute);
				if (index >= 0)
				{
					value = ucr::toTString(attributes.getValue(index));
					if (localName == EventElement)
						plugin.m_event = value;
					else if (localName == DescriptionElement)
						plugin.m_description = value;
					else if (localName == FileFiltersElement)
						plugin.m_fileFilters = value;
					else if (localName == IsAutomaticElement)
					{
						tchar_t ch = value.c_str()[0];
						plugin.m_isAutomatic = (ch == 't' || ch == 'T');
					}
					else if (localName == UnpackedFileExtensionElement)
						plugin.m_unpackedFileExtension = value;
					else if (localName == ExtendedPropertiesElement)
						plugin.m_extendedProperties = value;
					else if (localName == ArgumentsElement)
						plugin.m_arguments = std::move(value);
					else if (localName == PipelineElement)
						plugin.m_pipeline = std::move(value);
				}
				else if (localName == PrediffFileElement)
				{
					plugin.m_prediffFile.reset(new Method());
					m_pMethod = plugin.m_prediffFile.get();
				}
				else if (localName == UnpackFileElement)
				{
					plugin.m_unpackFile.reset(new Method());
					m_pMethod = plugin.m_unpackFile.get();
				}
				else if (localName == PackFileElement)
				{
					plugin.m_packFile.reset(new Method());
					m_pMethod = plugin.m_packFile.get();
				}
				else if (localName == IsFolderElement)
				{
					plugin.m_isFolder.reset(new Method());
					m_pMethod = plugin.m_isFolder.get();
				}
				else if (localName == UnpackFolderElement)
				{
					plugin.m_unpackFolder.reset(new Method());
					m_pMethod = plugin.m_unpackFolder.get();
				}
				else if (localName == PackFolderElement)
				{
					plugin.m_packFolder.reset(new Method());
					m_pMethod = plugin.m_packFolder.get();
				}
			}
			else if (m_pMethod)
			{
				if (localName == ScriptElement)
				{
					m_pMethod->m_script.reset(new Script);
					int index = attributes.getIndex(Empty, FileExtensionAttribute);
					if (index >= 0)
						m_pMethod->m_script->m_fileExtension = ucr::toTString(attributes.getValue(index));
				}
			}
		}
		m_stack.push(localName);
	}
	void endElement(const XMLString& uri, const XMLString& localName, const XMLString& qname)
	{
		m_stack.pop();
	}
	void characters(const XMLChar ch[], int start, int length)
	{
		if (m_stack.empty())
			return;
		if (m_stack.top() == CommandElement && m_pMethod)
		{
			m_pMethod->m_command += xmlch2tstr(ch, length);
		}
		else if (m_stack.top() == ScriptElement && m_pMethod && m_pMethod->m_script)
		{
			m_pMethod->m_script->m_body += xmlch2tstr(ch, length);
		}
	}
	void ignorableWhitespace(const XMLChar ch[], int start, int length) {}
	void processingInstruction(const XMLString& target, const XMLString& data) {}
	void startPrefixMapping(const XMLString& prefix, const XMLString& uri) {}
	void endPrefixMapping(const XMLString& prefix) {}
	void skippedEntity(const XMLString& name) {}

private:
	static String xmlch2tstr(const XMLChar* ch, int length)
	{
		return ucr::toTString(std::string(ch, length));
	}

	std::list<Info>* m_pPlugins = nullptr;
	std::stack<std::string> m_stack;
	Method* m_pMethod = nullptr;
};

class UnpackerGeneratedFromEditorScript : public WinMergePluginBase
{
public:
	UnpackerGeneratedFromEditorScript(const PluginInfo& plugin, const std::wstring& funcname, int id)
		: WinMergePluginBase(
			L"FILE_PACK_UNPACK",
			strutils::format_string1(_T("Unpacker to execute %1 script (automatically generated)"), funcname),
			L"\\.nomatch$", L"")
		, m_pDispatch(plugin.m_lpDispatch)
		, m_funcid(id)
		, m_hasArgumentsProperty(plugin.m_hasArgumentsProperty)
		, m_hasVariablesProperty(plugin.m_hasVariablesProperty)
	{
		auto desc = plugin.GetExtendedPropertyValue(funcname + _T(".Description"));
		if (desc.has_value())
			m_sDescription = *desc;
		m_pDispatch->AddRef();
		auto menuCaption = plugin.GetExtendedPropertyValue(funcname + _T(".MenuCaption"));
		String caption = menuCaption.has_value() ? strutils::to_str(*menuCaption) : funcname;
		m_sExtendedProperties = ucr::toUTF16(strutils::format(_T("ProcessType=Editor script;MenuCaption=%s"), caption))
			+ (plugin.GetExtendedPropertyValue(funcname + _T(".ArgumentsRequired")).has_value() ? L";ArgumentsRequired" : L"");
		StringView args = plugin.GetExtendedPropertyValue(funcname + _T(".Arguments")).value_or(_T(""));
		m_sArguments = strutils::to_str(args);
	}

	virtual ~UnpackerGeneratedFromEditorScript()
	{
		m_pDispatch->Release();
	}

	HRESULT STDMETHODCALLTYPE UnpackFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL* pbChanged, INT* pSubcode, VARIANT_BOOL* pbSuccess) override
	{
		String text;
		HRESULT hr = ReadFile(fileSrc, text);
		if (FAILED(hr))
			return hr;
		if (m_hasVariablesProperty && !plugin::InvokePutPluginVariables(ucr::toTString(fileSrc), m_pDispatch))
			return E_FAIL;
		if (m_hasArgumentsProperty && !plugin::InvokePutPluginArguments(m_sArguments, m_pDispatch))
			return E_FAIL;
		int changed = 0;
		if (!plugin::InvokeTransformText(text, changed, m_pDispatch, m_funcid))
			return E_FAIL;
		hr = WriteFile(fileDst, text);
		if (FAILED(hr))
			return hr;
		*pSubcode = 0;
		*pbChanged = VARIANT_TRUE;
		*pbSuccess = VARIANT_TRUE;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE PackFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL* pbChanged, INT subcode, VARIANT_BOOL* pbSuccess) override
	{
		*pbChanged = VARIANT_FALSE;
		*pbSuccess = VARIANT_FALSE;
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE ShowSettingsDialog(VARIANT_BOOL* pbHandled) override
	{
		*pbHandled = plugin::InvokeShowSettingsDialog(m_pDispatch) ? VARIANT_TRUE : VARIANT_FALSE;
		return S_OK;
	}

private:
	IDispatch* m_pDispatch;
	int m_funcid;
	bool m_hasArgumentsProperty;
	bool m_hasVariablesProperty;
};

class InternalPlugin : public WinMergePluginBase
{
public:
	InternalPlugin(Info&& info)
		: WinMergePluginBase(info.m_event, info.m_description, info.m_fileFilters, info.m_unpackedFileExtension, info.m_extendedProperties, info.m_arguments, info.m_pipeline, info.m_isAutomatic)
		, m_info(std::move(info))
	{
	}

	virtual ~InternalPlugin()
	{
	}

	HRESULT STDMETHODCALLTYPE PrediffFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL* pbChanged, VARIANT_BOOL* pbSuccess) override
	{
		if (!m_info.m_prediffFile)
		{
			*pbChanged = VARIANT_FALSE;
			*pbSuccess = VARIANT_FALSE;
			return S_OK;
		}
		TempFile scriptFile;
		String command = replaceMacros(m_info.m_prediffFile->m_command, fileSrc, fileDst);
		if (m_info.m_prediffFile->m_script)
		{
			createScript(*m_info.m_prediffFile->m_script, scriptFile);
			strutils::replace(command, _T("${SCRIPT_FILE}"), scriptFile.GetPath());
		}
		DWORD dwExitCode;
		HRESULT hr = launchProgram(command, SW_HIDE, dwExitCode);

		*pbChanged = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		*pbSuccess = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		return hr;
	}

	HRESULT STDMETHODCALLTYPE UnpackFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL* pbChanged, INT* pSubcode, VARIANT_BOOL* pbSuccess) override
	{
		if (!m_info.m_unpackFile)
		{
			*pSubcode = 0;
			*pbChanged = VARIANT_FALSE;
			*pbSuccess = VARIANT_FALSE;
			return S_OK;
		}
		TempFile scriptFile;
		String command = replaceMacros(m_info.m_unpackFile->m_command, fileSrc, fileDst);
		if (m_info.m_unpackFile->m_script)
		{
			createScript(*m_info.m_unpackFile->m_script, scriptFile);
			strutils::replace(command, _T("${SCRIPT_FILE}"), scriptFile.GetPath());
		}
		const String internalCommand = strutils::trim_ws(command);
		if (strutils::compare_nocase(internalCommand, _T("internal:luatable:unpack-file")) == 0)
		{
			HRESULT hr = UnpackLuaTableInProcess(fileSrc, fileDst);
			*pSubcode = 0;
			*pbChanged = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
			*pbSuccess = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
			return hr;
		}
		DWORD dwExitCode;
		HRESULT hr = launchProgram(command, SW_HIDE, dwExitCode);

		*pSubcode = 0;
		*pbChanged = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		*pbSuccess = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		return hr;
	}

	HRESULT STDMETHODCALLTYPE PackFile(BSTR fileSrc, BSTR fileDst, VARIANT_BOOL* pbChanged, INT subcode, VARIANT_BOOL* pbSuccess) override
	{
		if (!m_info.m_packFile)
		{
			*pbChanged = VARIANT_FALSE;
			*pbSuccess = VARIANT_FALSE;
			return S_OK;
		}
		TempFile scriptFile;
		String command = replaceMacros(m_info.m_packFile->m_command, fileSrc, fileDst);
		if (m_info.m_packFile->m_script)
		{
			createScript(*m_info.m_packFile->m_script, scriptFile);
			strutils::replace(command, _T("${SCRIPT_FILE}"), scriptFile.GetPath());
		}
		DWORD dwExitCode;
		HRESULT hr = launchProgram(command, SW_HIDE, dwExitCode);

		*pbChanged = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		*pbSuccess = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		return hr;
	}

	HRESULT STDMETHODCALLTYPE IsFolder(BSTR file, VARIANT_BOOL* pbFolder) override
	{
		if (!m_info.m_isFolder)
		{
			*pbFolder = VARIANT_FALSE;
			return S_OK;
		}
		const bool logTiming = ShouldLogExcelTiming();
		const auto start = std::chrono::steady_clock::now();
		TempFile scriptFile;
		String command = replaceMacros(m_info.m_isFolder->m_command, file, file);
		if (m_info.m_isFolder->m_script)
		{
			createScript(*m_info.m_isFolder->m_script, scriptFile);
			strutils::replace(command, _T("${SCRIPT_FILE}"), scriptFile.GetPath());
		}
		const String constantResult = strutils::trim_ws(command);
		if (strutils::compare_nocase(constantResult, _T("internal:true")) == 0 ||
			strutils::compare_nocase(constantResult, _T("internal:false")) == 0)
		{
			// Constant IsFolder answers avoid spawning a helper process for plugins
			// whose file filter has already identified the input type.
			*pbFolder = strutils::compare_nocase(constantResult, _T("internal:true")) == 0 ? VARIANT_TRUE : VARIANT_FALSE;
			if (logTiming)
			{
				RootLogger::Info(strutils::format(
					_T("[ExcelPlugin] IsFolder constant plugin=\"%s\" result=%s elapsed=%lldms file=\"%s\""),
					m_info.m_name.c_str(), *pbFolder == VARIANT_TRUE ? _T("true") : _T("false"),
					ElapsedMilliseconds(start), file));
			}
			return S_OK;
		}
		DWORD dwExitCode = static_cast<DWORD>(-1);
		HRESULT hr = launchProgram(command, SW_HIDE, dwExitCode, logTiming);
		*pbFolder = SUCCEEDED(hr) && dwExitCode == 0 ? VARIANT_TRUE : VARIANT_FALSE;
		if (logTiming)
		{
			RootLogger::Info(strutils::format(
				_T("[ExcelPlugin] IsFolder process plugin=\"%s\" result=%s elapsed=%lldms hr=%s exit=%lu file=\"%s\""),
				m_info.m_name.c_str(), *pbFolder == VARIANT_TRUE ? _T("true") : _T("false"),
				ElapsedMilliseconds(start), FormatHResult(hr).c_str(), dwExitCode, file));
		}
		return hr;
	}

	HRESULT STDMETHODCALLTYPE UnpackFolder(BSTR fileSrc, BSTR folderDst, VARIANT_BOOL* pbChanged, INT* pSubcode, VARIANT_BOOL* pbSuccess) override
	{
		if (!m_info.m_unpackFolder)
		{
			*pSubcode = 0;
			*pbChanged = VARIANT_FALSE;
			*pbSuccess = VARIANT_FALSE;
			return S_OK;
		}
		const bool logTiming = ShouldLogExcelTiming();
		const auto start = std::chrono::steady_clock::now();
		if (logTiming)
		{
			RootLogger::Info(strutils::format(
				_T("[ExcelPlugin] UnpackFolder start plugin=\"%s\" source=\"%s\" destination=\"%s\""),
				m_info.m_name.c_str(), fileSrc, folderDst));
		}
		TempFile scriptFile;
		String command = replaceMacros(m_info.m_unpackFolder->m_command, fileSrc, folderDst);
		if (m_info.m_unpackFolder->m_script)
		{
			createScript(*m_info.m_unpackFolder->m_script, scriptFile);
			strutils::replace(command, _T("${SCRIPT_FILE}"), scriptFile.GetPath());
		}
		const String internalCommand = strutils::trim_ws(command);
		if (strutils::compare_nocase(internalCommand, _T("internal:excel2tsv:unpack-folder")) == 0)
		{
			HRESULT hr = UnpackExcelFolderInProcess(fileSrc, folderDst, logTiming);
			*pSubcode = 0;
			*pbChanged = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
			*pbSuccess = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
			if (logTiming)
			{
				RootLogger::Info(strutils::format(
					_T("[ExcelPlugin] UnpackFolder done plugin=\"%s\" mode=in-process changed=%s success=%s elapsed=%lldms hr=%s source=\"%s\" destination=\"%s\""),
					m_info.m_name.c_str(), *pbChanged == VARIANT_TRUE ? _T("true") : _T("false"),
					*pbSuccess == VARIANT_TRUE ? _T("true") : _T("false"), ElapsedMilliseconds(start),
					FormatHResult(hr).c_str(), fileSrc, folderDst));
			}
			return hr;
		}
		if (strutils::compare_nocase(internalCommand, _T("internal:ue5core:unpack-folder")) == 0)
		{
			HRESULT hr = UnpackUE5CoreFolderInProcess(fileSrc, folderDst);
			*pSubcode = 0;
			*pbChanged = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
			*pbSuccess = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
			return hr;
		}
		DWORD dwExitCode = static_cast<DWORD>(-1);
		HRESULT hr = launchProgram(command, SW_HIDE, dwExitCode, logTiming);

		*pSubcode = 0;
		*pbChanged = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		*pbSuccess = SUCCEEDED(hr) ? VARIANT_TRUE : VARIANT_FALSE;
		if (logTiming)
		{
			RootLogger::Info(strutils::format(
				_T("[ExcelPlugin] UnpackFolder done plugin=\"%s\" changed=%s success=%s elapsed=%lldms hr=%s exit=%lu source=\"%s\" destination=\"%s\""),
				m_info.m_name.c_str(), *pbChanged == VARIANT_TRUE ? _T("true") : _T("false"),
				*pbSuccess == VARIANT_TRUE ? _T("true") : _T("false"), ElapsedMilliseconds(start),
				FormatHResult(hr).c_str(), dwExitCode, fileSrc, folderDst));
		}
		return hr;
	}

	Info* GetInfo()
	{
		return &m_info;
	}

protected:

	std::vector<String> getMacroNames(const String& cmd)
	{
		std::vector<String> result;
		size_t start = 0;
		while ((start = cmd.find(_T("${"), start)) != String::npos) {
			start += 2;
			size_t end = cmd.find(_T("}"), start);
			if (end == std::string::npos)
				break;
			result.push_back(cmd.substr(start, end - start));
			start = end + 1;
		}
		return result;
	}

	std::vector<String> parseArguments(const String& arguments)
	{
		std::vector<String> result;
		String current;
		bool inQuotes = false;

		const tchar_t* p = arguments.c_str();
		while (*p)
		{
			if (!inQuotes)
			{
				if (*p == _T(' '))
				{
					if (!current.empty())
					{
						result.push_back(current);
						current.clear();
					}
					++p;
				}
				else if (*p == _T('"'))
				{
					inQuotes = true;
					++p;
				}
				else
				{
					current += *p++;
				}
			}
			else
			{
				if (*p == _T('"'))
				{
					if (*(p + 1) == _T('"'))
					{
						// Escaped quote
						current += _T('"');
						p += 2;
					}
					else
					{
						// End quote
						inQuotes = false;
						++p;
					}
				}
				else
				{
					current += *p++;
				}
			}
		}

		// Push last argument
		if (inQuotes)
		{
			// Unterminated quote → accept as-is
			result.push_back(current);
		}
		else if (!current.empty() || (!arguments.empty() && arguments.back() == _T('"')))
		{
			result.push_back(current);
		}

		return result;
	}

	String replaceMacros(const String& cmd, const String & fileSrc, const String& fileDst)
	{
		String command = cmd;
		if (paths::IsURL(fileSrc))
		{
			PARSEDURL parsedURL{sizeof(PARSEDURL)};
			ParseURL(fileSrc.c_str(), &parsedURL);
			strutils::replace(command, _T("${SRC_URL}"), fileSrc);
			strutils::replace(command, _T("${SRC_URL_PROTOCOL}"), String{ parsedURL.pszProtocol, parsedURL.cchProtocol });
			strutils::replace(command, _T("${SRC_URL_SUFFIX}"), 
				parsedURL.pszSuffix ? parsedURL.pszSuffix : _T(""));
		}
		if (paths::IsURL(fileDst))
		{
			PARSEDURL parsedURL{sizeof(PARSEDURL)};
			ParseURL(fileDst.c_str(), &parsedURL);
			strutils::replace(command, _T("${DST_URL}"), fileDst);
			strutils::replace(command, _T("${DST_URL_PROTOCOL}"), String{ parsedURL.pszProtocol, parsedURL.cchProtocol });
			strutils::replace(command, _T("${DST_URL_SUFFIX}"), 
				parsedURL.pszSuffix ? parsedURL.pszSuffix : _T(""));
		}

		std::vector<String> args;
		std::vector<String> macroNames = getMacroNames(cmd);
		for (const auto& name : macroNames)
		{
			if (name == _T("SRC_FILE"))
				strutils::replace(command, _T("${SRC_FILE}"), fileSrc);
			else if (name == _T("DST_FILE"))
				strutils::replace(command, _T("${DST_FILE}"), fileDst);
			else if (name == _T("SRC_FOLDER"))
				strutils::replace(command, _T("${SRC_FOLDER}"), fileSrc);
			else if (name == _T("DST_FOLDER"))
				strutils::replace(command, _T("${DST_FOLDER}"), fileDst);
			else if (name == _T("WINMERGE_HOME"))
				strutils::replace(command, _T("${WINMERGE_HOME}"), env::GetProgPath());
			else if (name.length() == 1 && tc::istdigit(name.front()))
			{
				if (args.empty())
					args = parseArguments(m_sArguments);
				int num = tc::ttoi(name.c_str());
				if (num < 1 || static_cast<size_t>(num) > args.size())
					continue;
				strutils::replace(command, strutils::format(_T("${%d}"), num), args[num - 1]);
			}
			else if (name == _T("*"))
				strutils::replace(command, _T("${*}"), m_sArguments);
			else if (name.find(_T("CFG:")) == 0)
			{
				std::vector<StringView> ary = strutils::split(name, ':');
				String optionName = String(ary[1].data(), ary[1].size());
				String defaultVal = ary.size() > 2 ? String(ary[2].data(), ary[2].size()) : _T("");
				auto val = GetOptionsMgr()->Get(optionName);
				switch (val.GetType())
				{
				case varprop::VT_STRING:
					strutils::replace(command, _T("${") + name + _T("}"), val.GetString());
					break;
				case varprop::VT_INT:
					strutils::replace(command, _T("${") + name + _T("}"), strutils::to_str(val.GetInt()));
					break;
				case varprop::VT_BOOL:
					strutils::replace(command, _T("${") + name + _T("}"), strutils::to_str(val.GetBool()));
					break;
				case varprop::VT_NULL:
					GetOptionsMgr()->InitOption(optionName, defaultVal);
					val = GetOptionsMgr()->Get(optionName);
					strutils::replace(command, _T("${") + name + _T("}"), val.GetString());
					break;
				}
			}
		}
		return command;
	}

	static HRESULT createScript(const Script& script, TempFile& tempFile)
	{
		String path = tempFile.Create(_T(""), script.m_fileExtension);
		return WriteFile(path, script.m_body, false);
	}

	static HRESULT launchProgram(const String& sCmd, WORD wShowWindow, DWORD &dwExitCode, bool logOutput = false)
	{
		TempFile stderrFile;
		String sOutputFile = stderrFile.Create();
		size_t size = 0;
		_wgetenv_s(&size, nullptr, 0, L"WINMERGE_HOME");
		if (size == 0)
			_wputenv_s(L"WINMERGE_HOME", env::GetProgPath().c_str());
		String command = sCmd;
		STARTUPINFO stInfo = { sizeof(STARTUPINFO) };
		stInfo.dwFlags = STARTF_USESHOWWINDOW;
		stInfo.wShowWindow = wShowWindow;
		SECURITY_ATTRIBUTES sa{ sizeof(sa) };
		sa.bInheritHandle = true;
		stInfo.hStdError = CreateFile(sOutputFile.c_str(), GENERIC_READ | GENERIC_WRITE,
			FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		stInfo.hStdOutput = stInfo.hStdError;
		stInfo.dwFlags |= STARTF_USESTDHANDLES;
		PROCESS_INFORMATION processInfo;
		bool retVal = !!CreateProcess(nullptr, (tchar_t*)command.c_str(),
			nullptr, nullptr, TRUE, CREATE_DEFAULT_ERROR_MODE, nullptr, nullptr,
			&stInfo, &processInfo);
		if (!retVal)
			return HRESULT_FROM_WIN32(GetLastError());
		WaitForSingleObject(processInfo.hProcess, INFINITE);
		GetExitCodeProcess(processInfo.hProcess, &dwExitCode);
		CloseHandle(processInfo.hThread);
		CloseHandle(processInfo.hProcess);
		DWORD dwStdErrorSize = 0;
		if (stInfo.hStdError != nullptr && stInfo.hStdError != INVALID_HANDLE_VALUE)
		{
			DWORD dwStdErrorSizeHigh = 0;
			dwStdErrorSize = GetFileSize(stInfo.hStdError, &dwStdErrorSizeHigh);
			CloseHandle(stInfo.hStdError);
		}
		String output;
		if (dwStdErrorSize > 0)
		{
			ReadFile(sOutputFile, output);
			if (logOutput)
			{
				String logOutputText = TrimLogOutput(output);
				if (!logOutputText.empty())
					RootLogger::Info(_T("[ExcelPlugin] helper output:\n") + logOutputText);
			}
		}
		if (dwExitCode != 0 && dwStdErrorSize > 0)
		{
			ICreateErrorInfo* pCreateErrorInfo = nullptr;
			if (SUCCEEDED(CreateErrorInfo(&pCreateErrorInfo)))
			{
				pCreateErrorInfo->SetSource(const_cast<OLECHAR*>(command.c_str()));
				pCreateErrorInfo->SetDescription(const_cast<OLECHAR*>(ucr::toUTF16(output).c_str()));
				IErrorInfo* pErrorInfo = nullptr;
				pCreateErrorInfo->QueryInterface(&pErrorInfo);
				SetErrorInfo(0, pErrorInfo);
				pErrorInfo->Release();
				pCreateErrorInfo->Release();
				return DISP_E_EXCEPTION;
			}
		}
		return S_OK;
	}

	// Run the embedded lua2csv converter through lua54 to turn a Lua config
	// file into CSV. If lua54 cannot be launched, fall back to a raw copy so the
	// comparison still works (both panes degrade identically for same-file revisions).
	static HRESULT UnpackLuaTableInProcess(const String& fileSrc, const String& fileDst)
	{
		TempFile converterFile;
		const String converterPath = converterFile.Create(_T(""), _T(".lua"));
		HRESULT hr = WriteFile(converterPath, Lua2CsvConverter(), false);
		if (FAILED(hr))
			return hr;

		const String luaExe = ResolveLua54Path();
		const String command = _T("\"") + luaExe + _T("\" \"") + converterPath +
			_T("\" \"") + fileSrc + _T("\" \"") + fileDst + _T("\"");
		DWORD dwExitCode = static_cast<DWORD>(-1);
		hr = launchProgram(command, SW_HIDE, dwExitCode);
		if (SUCCEEDED(hr) && dwExitCode == 0)
			return S_OK;

		// launchProgram may have set thread-local error info on the failure path;
		// clear it so a successful raw-copy fallback does not leave a stale error
		// that an unrelated later COM call could surface.
		SetErrorInfo(0, nullptr);
		if (CopyFile(fileSrc.c_str(), fileDst.c_str(), FALSE))
			return S_OK;
		return FAILED(hr) ? hr : E_FAIL;
	}

	bool ShouldLogExcelTiming() const
	{
		if (!IsExcelPluginLogEnabled())
			return false;
		String extendedProperties = strutils::makelower(m_info.m_extendedProperties);
		return m_info.m_name == _T("CompareExcelSheetsFast") ||
			extendedProperties.find(_T("filetype=ms-excel")) != String::npos;
	}

	Info m_info;
};

class EditorScriptGeneratedFromUnpacker: public WinMergePluginBase
{
public:
	EditorScriptGeneratedFromUnpacker(const PluginInfo& plugin, const String& funcname, bool hasArgumentProperty)
		: WinMergePluginBase(
			L"EDITOR_SCRIPT",
			plugin.m_description,
			plugin.m_filtersTextDefault, L"", plugin.m_extendedProperties,
			plugin.m_argumentsDefault)
		, m_pDispatch(plugin.m_lpDispatch)
		, m_hasArgumentsProperty(hasArgumentProperty)
	{
		auto menuCaption = plugin.GetExtendedPropertyValue(_T("MenuCaption"));
		if (menuCaption.has_value())
		{
			String menuCaptionStr = strutils::to_str(*menuCaption);
			m_sExtendedProperties = strutils::format(_T("%s;%s.MenuCaption=%s"),
					plugin.m_extendedProperties, funcname, menuCaptionStr);
		}
		m_pDispatch->AddRef();
		AddFunction(ucr::toUTF16(funcname), CallUnpackFile);
	}

	virtual ~EditorScriptGeneratedFromUnpacker()
	{
		m_pDispatch->Release();
	}

	static HRESULT STDMETHODCALLTYPE CallUnpackFile(IDispatch *pDispatch, BSTR text, BSTR* pbstrResult)
	{
		TempFile src, dst;
		String fileSrc = src.Create();
		String fileDst = dst.Create();
		HRESULT hr = WriteFile(fileSrc, text);
		if (FAILED(hr))
			return hr;
		int changed = 0;
		int subcode = 0;
		auto* thisObj = static_cast<EditorScriptGeneratedFromUnpacker*>(pDispatch);
		auto* pInternalPlugin = dynamic_cast<InternalPlugin*>(thisObj->m_pDispatch);
		if (pInternalPlugin)
		{
			BSTR bstrArguments = SysAllocString(thisObj->m_sArguments.c_str());
			pInternalPlugin->put_PluginArguments(bstrArguments);
			BSTR bstrFileSrc = SysAllocString(ucr::toUTF16(fileSrc).c_str());
			BSTR bstrFileDst= SysAllocString(ucr::toUTF16(fileDst).c_str());
			VARIANT_BOOL bChanged;
			VARIANT_BOOL bSuccess;
			hr = pInternalPlugin->UnpackFile(bstrFileSrc, bstrFileDst, &bChanged, &subcode, &bSuccess);
			SysFreeString(bstrFileSrc);
			SysFreeString(bstrFileDst);
			SysFreeString(bstrArguments);
			if (FAILED(hr))
				return hr;
		}
		else
		{
			if (thisObj->m_hasArgumentsProperty)
			{
				if (!plugin::InvokePutPluginArguments(thisObj->m_sArguments, thisObj->m_pDispatch))
					return E_FAIL;
			}
			if (!plugin::InvokeUnpackFile(fileSrc, fileDst, changed, thisObj->m_pDispatch, subcode))
				return E_FAIL;
		}
		String unpackedText;
		hr = ReadFile(fileDst, unpackedText);
		if (FAILED(hr))
			return hr;
		*pbstrResult = SysAllocStringLen(ucr::toUTF16(unpackedText).c_str(), 
			static_cast<unsigned>(unpackedText.length()));
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE ShowSettingsDialog(VARIANT_BOOL* pbHandled) override
	{
		*pbHandled = plugin::InvokeShowSettingsDialog(m_pDispatch) ? VARIANT_TRUE : VARIANT_FALSE;
		return S_OK;
	}

private:
	bool m_hasArgumentsProperty;
	IDispatch* m_pDispatch;
};

String GetPluginXMLPath(LocationType locationType)
{
	switch (locationType)
	{
	case LocationType::AppDataPath:
		return paths::ConcatPath(env::GetAppDataPath(), _T("WinMerge\\MergePlugins\\Plugins.xml"));
	case LocationType::DocumentsPath:
		return paths::ConcatPath(env::GetMyDocuments(), _T("WinMerge\\MergePlugins\\Plugins.xml"));
	default:
		return paths::ConcatPath(env::GetProgPath(), _T("MergePlugins\\Plugins.xml"));
	}
}

bool LoadFromXML(LocationType locationType, std::list<Info>& internalPlugins, String& errmsg)
{
	XMLHandler handler(&internalPlugins);
	SAXParser parser;
	parser.setFeature(SAXParser::FEATURE_EXTERNAL_GENERAL_ENTITIES, false);
	parser.setFeature(SAXParser::FEATURE_EXTERNAL_PARAMETER_ENTITIES, false);
	parser.setContentHandler(&handler);
	try
	{
		size_t size = internalPlugins.size();
		const String pluginsXMLPath = GetPluginXMLPath(locationType);
		try { parser.parse(ucr::toUTF8(pluginsXMLPath)); }
		catch (Poco::FileNotFoundException&) { }
		size_t i = 0;
		for (auto& info : internalPlugins)
		{
			if (i >= size)
				info.m_locationType = locationType;
			++i;
		}
	}
	catch (Poco::XML::SAXParseException& e)
	{
		errmsg = ucr::toTString(e.message());
		return false;
	}
	return true;
}

Info* GetInternalPluginInfo(const PluginInfo* plugin)
{
	auto* internalPlugin = (plugin->m_filepath.find(_T("Plugins.xml")) != String::npos) ? dynamic_cast<InternalPlugin*>(plugin->m_lpDispatch) : nullptr;
	if (!internalPlugin)
		return nullptr;
	return internalPlugin->GetInfo();
}

bool FindPluginNameConflict(const Info& info)
{
	for (auto& eventNames : { plugin::ProtocolHanlderEventNames, plugin::UnpackerEventNames, plugin::PredifferEventNames, plugin::EditorScriptEventNames })
	{
		if (std::find(eventNames.begin(), eventNames.end(), info.m_event) != eventNames.end())
		{
			for (auto& event : eventNames)
			{
				PluginInfo* plugin = CAllThreadsScripts::GetActiveSet()->GetPluginByName(event.c_str(), info.m_name);
				if (plugin)
					return true;
			}
		}
	}
	return false;
}

Info CreateUnpackerPluginExample()
{
	internal_plugin::Info info(_T("NewPluginName"));
	info.m_event = _T("FILE_PACK_UNPACK");
	info.m_description = _T("New plugin description");
	info.m_fileFilters = _T("\\.*$");
	info.m_extendedProperties = _T("ProcessType=&Others;MenuCaption=NewPlugin");
	info.m_unpackFile = std::make_unique <internal_plugin::Method>();
	info.m_unpackFile->m_command = _T("cmd /c echo Hello World! \"${SRC_FILE}\" > \"${DST_FILE}\"");
	info.m_locationType = GetOptionsMgr()->GetInt(OPT_USERDATA_LOCATION) == 0 ? LocationType::AppDataPath : LocationType::DocumentsPath;
	return info;
}

Info CreatePredifferPluginExample()
{
	internal_plugin::Info info(_T("NewPluginName"));
	info.m_event = _T("FILE_PREDIFF");
	info.m_description = _T("New plugin description");
	info.m_fileFilters = _T("\\.*$");
	info.m_extendedProperties = _T("ProcessType=&Others;MenuCaption=NewPlugin");
	info.m_unpackFile = std::make_unique <internal_plugin::Method>();
	info.m_unpackFile->m_command = _T("cmd /c type \"${SRC_FILE}\" | \"%ProgramFiles%\\Git\\usr\\bin\\sed.exe\" \"s/abc/xxx/g\" > \"${DST_FILE}\"");
	info.m_locationType = GetOptionsMgr()->GetInt(OPT_USERDATA_LOCATION) == 0 ? LocationType::AppDataPath : LocationType::DocumentsPath;
	return info;
}

Info CreateAliasPluginExample(PluginInfo* plugin, const String& event, const String& pipeline)
{
	internal_plugin::Info info(_(""));
	info.m_locationType = GetOptionsMgr()->GetInt(OPT_USERDATA_LOCATION) == 0 ? LocationType::AppDataPath : LocationType::DocumentsPath;
	info.m_event = event;
	for (tchar_t c : pipeline)
	{
		if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_')
			info.m_name += c;
	}
	info.m_name += _T("Alias");
	info.m_description = strutils::format_string1(_("Alias for plugin pipeline '%1'"), pipeline);
	String menuCaptionStr = pipeline;
	strutils::replace(menuCaptionStr, _T(";"), _T(""));
	info.m_extendedProperties += _T("MenuCaption=") + menuCaptionStr + _T(";");
	info.m_pipeline = pipeline;
	if (plugin)
	{
		info.m_fileFilters = plugin->m_filtersText;
		info.m_isAutomatic = plugin->m_bAutomatic;
		auto processType = PluginInfo::GetExtendedPropertyValue(plugin->m_extendedProperties, _T("ProcessType"));
		if (processType.has_value())
			info.m_extendedProperties += _T("ProcessType=") + String(processType.value()) + _T(";");
		if (!info.m_extendedProperties.empty())
			info.m_extendedProperties.pop_back();
	}
	return info;
}

bool AddPlugin(const Info& info, String& errmsg)
{
	if (FindPluginNameConflict(info))
	{
		errmsg = strutils::format_string1(_("Plugin name '%1' already exists."), info.m_name);
		return false;
	}
	std::list<internal_plugin::Info> list;
	if (!internal_plugin::LoadFromXML(info.m_locationType, list, errmsg))
		return false;
	list.push_back(info);
	if (!internal_plugin::SaveToXML(info.m_locationType, list, errmsg))
		return false;
	CAllThreadsScripts::GetActiveSet()->ReloadAllScripts();
	return true;
}

bool UpdatePlugin(const Info& info, String& errmsg)
{
	std::list<internal_plugin::Info> list;
	if (!internal_plugin::LoadFromXML(info.m_locationType, list, errmsg))
		return false;
	for (auto it = list.begin(); it != list.end(); ++it)
	{
		if (it->m_name == info.m_name)
		{
			list.insert(it, info);
			list.erase(it);
			break;
		}
	}
	if (!internal_plugin::SaveToXML(info.m_locationType, list, errmsg))
		return false;
	CAllThreadsScripts::GetActiveSet()->ReloadAllScripts();
	return true;
}

bool RemovePlugin(const Info& info, String& errmsg)
{
	std::list<internal_plugin::Info> list;
	if (!internal_plugin::LoadFromXML(info.m_locationType, list, errmsg))
		return false;
	for (auto it = list.begin(); it != list.end(); ++it)
	{
		if (it->m_name == info.m_name && it->m_event == info.m_event)
		{
			list.erase(it);
			break;
		}
	}
	if (!internal_plugin::SaveToXML(info.m_locationType, list, errmsg))
		return false;
	CAllThreadsScripts::GetActiveSet()->ReloadAllScripts();
	return true;
}

static void writeEmptyElement(XMLWriter& writer, const std::string& tagname, const std::string& attrname, const String& value)
{
	AttributesImpl attrs;
	attrs.addAttribute("", "", attrname, "", ucr::toUTF8(value));
	writer.emptyElement("", "", tagname, attrs);
}

static void writeMethodElement(XMLWriter& writer, const std::string& tagname, const Method& method)
{
	writer.startElement("", "", tagname);
	if (!method.m_command.empty())
	{
		writer.startElement("", "", CommandElement);
		writer.characters(ucr::toUTF8(method.m_command));
		writer.endElement("", "", CommandElement);
	}
	if (method.m_script)
	{
		AttributesImpl attrs;
		if (!method.m_script->m_fileExtension.empty())
			attrs.addAttribute("", "", FileExtensionAttribute, "", ucr::toUTF8(method.m_script->m_fileExtension));
		writer.startElement("", "", ScriptElement, attrs);
		writer.characters(ucr::toUTF8(method.m_script->m_body));
		writer.endElement("", "", ScriptElement);
	}
	writer.endElement("", "", tagname);
}

bool SaveToXML(LocationType locationType, const std::list<Info>& internalPlugins, String& errmsg)
{
	try
	{
		const String pluginsXMLPath = GetPluginXMLPath(locationType);
		paths::CreateIfNeeded(paths::GetPathOnly(pluginsXMLPath));
		FileStream out(ucr::toUTF8(pluginsXMLPath), FileStream::out | FileStream::trunc);
		XMLWriter writer(out, XMLWriter::WRITE_XML_DECLARATION | XMLWriter::PRETTY_PRINT);
		writer.startDocument();
		writer.startElement("", "", PluginsElement);
		{
			for (auto& item : internalPlugins)
			{
				AttributesImpl attrs;
				attrs.addAttribute("", "", NameAttribute, "", ucr::toUTF8(item.m_name));
				writer.startElement("", "", PluginElement, attrs);
				{
					if (!item.m_event.empty())
						writeEmptyElement(writer, EventElement, ValueAttribute, item.m_event);
					if (!item.m_description.empty())
						writeEmptyElement(writer, DescriptionElement, ValueAttribute, item.m_description);
					if (!item.m_fileFilters.empty())
						writeEmptyElement(writer, FileFiltersElement, ValueAttribute, item.m_fileFilters);
					writeEmptyElement(writer, IsAutomaticElement, ValueAttribute, item.m_isAutomatic ? _T("true") : _T("false"));
					if (!item.m_unpackedFileExtension.empty())
						writeEmptyElement(writer, UnpackedFileExtensionElement, ValueAttribute, item.m_unpackedFileExtension);
					if (!item.m_extendedProperties.empty())
						writeEmptyElement(writer, ExtendedPropertiesElement, ValueAttribute, item.m_extendedProperties);
					if (!item.m_arguments.empty())
						writeEmptyElement(writer, ArgumentsElement, ValueAttribute, item.m_arguments);
					if (!item.m_pipeline.empty())
						writeEmptyElement(writer, PipelineElement, ValueAttribute, item.m_pipeline);
					if (item.m_isFolder)
						writeMethodElement(writer, IsFolderElement, *item.m_isFolder.get());
					if (item.m_unpackFile)
						writeMethodElement(writer, UnpackFileElement, *item.m_unpackFile.get());
					if (item.m_packFile)
						writeMethodElement(writer, PackFileElement, *item.m_packFile.get());
					if (item.m_unpackFolder)
						writeMethodElement(writer, UnpackFolderElement, *item.m_unpackFolder.get());
					if (item.m_packFolder)
						writeMethodElement(writer, PackFolderElement, *item.m_packFolder.get());
					if (item.m_prediffFile)
						writeMethodElement(writer, PrediffFileElement, *item.m_prediffFile.get());
				}
				writer.endElement("", "", PluginElement);
			}
		}
		writer.endElement("", "", PluginsElement);
		writer.endDocument();
		return true;
	}
	catch (Exception& e)
	{
		errmsg = ucr::toTString(e.displayText());
		return false;
	}
}

struct Loader
{
	Loader()
	{
		CAllThreadsScripts::RegisterInternalPluginsLoader(&Load);
	}

	static bool Load(std::map<String, PluginArrayPtr>& plugins, String& errmsg)
	{
		if (plugins.find(L"EDITOR_SCRIPT") != plugins.end())
		{
			for (auto plugin : *plugins[L"EDITOR_SCRIPT"])
			{
				if (!plugin->m_disabled && plugin->GetExtendedPropertyValue(_T("GenerateUnpacker")).has_value())
				{
					std::vector<String> namesArray;
					std::vector<int> idArray;
					int validFuncs = plugin::GetMethodsFromScript(plugin->m_lpDispatch, namesArray, idArray);
					for (int i = 0; i < validFuncs; ++i)
					{
						if (namesArray[i] == L"PluginOnEvent" || namesArray[i] == L"ShowSettingsDialog")
							continue;
						if (plugins.find(L"FILE_PACK_UNPACK") == plugins.end())
							plugins[L"FILE_PACK_UNPACK"].reset(new PluginArray);
						PluginInfoPtr pluginNew(new PluginInfo());
						IDispatch* pDispatch = new UnpackerGeneratedFromEditorScript(*plugin, namesArray[i], idArray[i]);
						pDispatch->AddRef();
						pluginNew->MakeInfo(plugin->m_filepath, namesArray[i], pDispatch);
						plugins[L"FILE_PACK_UNPACK"]->push_back(pluginNew);
					}
				}
			}
		}

		std::list<Info> internalPlugins;
		for (auto locationType : {LocationType::InstallationPath, LocationType::AppDataPath, LocationType::DocumentsPath})
		{
			if (!LoadFromXML(locationType, internalPlugins, errmsg))
				return false;
		}

		for (auto& info : internalPlugins)
		{
			String event = info.m_event;
			String name = info.m_name;
			if (plugins.find(event) == plugins.end())
				plugins[event].reset(new PluginArray);
			PluginInfoPtr pluginNew(new PluginInfo());
			IDispatch* pDispatch = new InternalPlugin(std::move(info));
			pDispatch->AddRef();
			if (pluginNew->MakeInfo(GetPluginXMLPath(info.m_locationType), name, pDispatch) > 0)
				plugins[event]->push_back(pluginNew);
		}

		if (plugins.find(L"FILE_PACK_UNPACK") != plugins.end())
		{
			for (auto plugin : *plugins[L"FILE_PACK_UNPACK"])
			{
				if (!plugin->m_disabled && plugin->GetExtendedPropertyValue(_T("GenerateEditorScript")).has_value())
				{
					if (plugins.find(L"EDITOR_SCRIPT") == plugins.end())
						plugins[L"EDITOR_SCRIPT"].reset(new PluginArray);
					PluginInfoPtr pluginNew(new PluginInfo());
					IDispatch* pDispatch = new EditorScriptGeneratedFromUnpacker(*plugin, plugin->m_name, plugin->m_hasArgumentsProperty);
					pDispatch->AddRef();
					if (pluginNew->MakeInfo(plugin->m_filepath, plugin->m_name, pDispatch) > 0)
						plugins[L"EDITOR_SCRIPT"]->push_back(pluginNew);
				}
			}
		}

		return true;
	}
} g_loader;

}
