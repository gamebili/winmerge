/**
 * @file  UpdateCheck.h
 *
 * @brief Declaration of the GitHub release update checker.
 */
#pragma once

#include <windows.h>

namespace UpdateCheck
{
	/**
	 * @brief Message posted to the notify window when an async check finishes.
	 *
	 * wParam: non-zero if the check was triggered manually (then "up to date"
	 *         and error results are also reported); zero for the silent
	 *         automatic startup check.
	 * lParam: a heap-allocated String* the receiver takes ownership of and must
	 *         delete:
	 *           - non-empty -> a newer version is available (its version text)
	 *           - empty     -> already up to date
	 *           - nullptr   -> the check failed (network/parse error)
	 */
	constexpr UINT WM_APP_UPDATECHECK_RESULT = WM_APP + 0x100;

	/**
	 * @brief Start an asynchronous check against the fork's GitHub releases.
	 * @param [in] hwndNotify Window that receives WM_APP_UPDATECHECK_RESULT.
	 * @param [in] manual     true if the user requested the check explicitly.
	 */
	void StartAsyncCheck(HWND hwndNotify, bool manual);
}
