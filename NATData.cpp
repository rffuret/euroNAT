#include "StdAfx.h"
#include "NATData.h"
#include "NATShow.h"
#include <windows.h>
#include <regex>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <map>
#include "json.hpp"
#include <iomanip>
#include <vector>
using json = nlohmann::json;
using namespace std;

// Link the source state variable from your GUI options panel
extern int g_NatSource;

void LogToFile(const CString& level, const CString& message) {
	HMODULE hMod = NULL;
	if (GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		(LPCTSTR)&LogToFile, &hMod))
	{
		TCHAR dllpath[MAX_PATH];
		GetModuleFileName(hMod, dllpath, MAX_PATH);
		CString logPath(dllpath);
		logPath = logPath.Left(logPath.ReverseFind(_T('\\')) + 1) + _T("euroNAT.log");

		// This flag persists for the lifetime of the plugin session instance
		static bool bSessionInitialized = false;

		if (!bSessionInitialized) {
			bSessionInitialized = true;

			std::vector<std::string> lines;
			std::ifstream inFile;
			inFile.open((LPCTSTR)logPath); // FIXED: Separated declaration and open to block parsing errors
			std::string line;
			std::vector<size_t> sessionMarkers;

			// Read the existing file and track where previous sessions started
			if (inFile.is_open()) {
				while (std::getline(inFile, line)) {
					if (line.find("=== NEW SESSION ===") != std::string::npos) {
						sessionMarkers.push_back(lines.size());
					}
					lines.push_back(line);
				}
				inFile.close();
			}

			// If we already have 3 or more historical sessions, truncate the oldest one.
			if (sessionMarkers.size() >= 3) {
				size_t keepFromLine = sessionMarkers[sessionMarkers.size() - 2];
				std::ofstream outFile;
				outFile.open((LPCTSTR)logPath, std::ios::trunc); // FIXED
				if (outFile.is_open()) {
					for (size_t i = keepFromLine; i < lines.size(); ++i) {
						outFile << lines[i] << "\n";
					}
					outFile.close();
				}
			}

			// Append a clear session separation block
			std::ofstream logFile;
			logFile.open((LPCTSTR)logPath, std::ios::app); // FIXED
			if (logFile.is_open()) {
				logFile << "\n==================== === NEW SESSION === ====================\n" << std::endl;
				logFile.close();
			}
		}

		// Write the actual log message entry
		std::ofstream logFile;
		logFile.open((LPCTSTR)logPath, std::ios::app); // FIXED
		if (logFile.is_open()) {
			SYSTEMTIME st;
			GetLocalTime(&st);
			char timestamp[32];
			sprintf_s(timestamp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
				st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

			logFile << timestamp << "[" << (LPCTSTR)level << "] " << (LPCTSTR)message << std::endl;
			logFile.close();
		}
	}
}

void LogPayloadToFile(const CString& payload) {
	HMODULE hMod = GetModuleHandle(_T("euroNAT.dll"));
	if (!hMod) return;

	TCHAR dllpath[MAX_PATH];
	GetModuleFileName(hMod, dllpath, MAX_PATH);
	CString logPath(dllpath);
	logPath = logPath.Left(logPath.ReverseFind(_T('\\')) + 1) + _T("FetchedData.log");

	std::vector<std::string> lines;
	std::ifstream inFile;
	inFile.open((LPCTSTR)logPath);
	std::string line;
	std::vector<size_t> downloadMarkers;

	// 1. Read existing file and track the line numbers where downloads start
	if (inFile.is_open()) {
		while (std::getline(inFile, line)) {
			if (line.find("=== DOWNLOAD START") != std::string::npos) {
				downloadMarkers.push_back(lines.size());
			}
			lines.push_back(line);
		}
		inFile.close();
	}

	// 2. Truncate oldest downloads if we already have 8 or more entries.
	// Keeping the last 7 means the upcoming append will make exactly 8.
	if (downloadMarkers.size() >= 8) {
		size_t keepFromLine = downloadMarkers[downloadMarkers.size() - 7];
		std::ofstream outFile;
		outFile.open((LPCTSTR)logPath, std::ios::trunc);
		if (outFile.is_open()) {
			for (size_t i = keepFromLine; i < lines.size(); ++i) {
				outFile << lines[i] << "\n";
			}
			outFile.close();
		}
	}

	// 3. Append the new incoming payload block with timestamp and source header
	std::ofstream logFile;
	logFile.open((LPCTSTR)logPath, std::ios::app);
	if (logFile.is_open()) {
		SYSTEMTIME st;
		GetLocalTime(&st);
		char timestamp[64];
		sprintf_s(timestamp, "=== DOWNLOAD START [%04d-%02d-%02d %02d:%02d:%02d] ===",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

		logFile << timestamp << "\n";
		logFile << "Source: " << (g_NatSource == 1 ? "VATSIM natTrak API (v2)" : "FAA System JSON") << "\n";
		logFile << "------------------------------------------------------------\n";
		logFile << (LPCTSTR)payload << "\n";
		logFile << "=== DOWNLOAD END ===\n\n" << std::endl;
		logFile.close();
	}
}

NATData::NATWorkerCont NATData::NATWorkerData;
NATData * NATData::LastInstance = NULL;
CPlugIn* euroNatPlugin;

//Use only one of the below links (if prod, always the first)
//Remote FAA link to capture NAT Message
CString natURL = "https://nms.aim.faa.gov/datanat/nat.json";
//Local link to fake message for test purpose
//CString natURL = "";

static ULONGLONG GetFileWriteTime(LPCTSTR filePath)
{
	WIN32_FILE_ATTRIBUTE_DATA fileInfo;
	if (!GetFileAttributesEx(filePath, GetFileExInfoStandard, &fileInfo))
		return 0; // File not found or error

	ULARGE_INTEGER ull;
	ull.LowPart = fileInfo.ftLastWriteTime.dwLowDateTime;
	ull.HighPart = fileInfo.ftLastWriteTime.dwHighDateTime;
	return ull.QuadPart;
}

NATData::NATData(void) {
	this->m_nats = new NAT[MAXNATS];
	this->m_natcount = new int;
	*this->m_natcount = 0;

	NATWorkerData.m_pNats = this->m_nats;
	NATWorkerData.m_pNatCount = this->m_natcount;

	NATData::LastInstance = this;
}


NATData::~NATData(void) {
	delete[] this->m_nats;
	delete this->m_natcount;
}

void NATData::Refresh(void) {

	NATShow::Loading = true;

	this->workerThread = AfxBeginThread(NATData::FetchDataWorker, &NATWorkerData);
	
}

void NATData::SetPlugin(CPlugIn* plugin) {
	euroNatPlugin = plugin;
}

UINT NATData::FetchDataWorker(LPVOID pvar) {
	NATWorkerCont* dta = &NATData::NATWorkerData;

	// FORCE A LOG ENTRY HERE TO VERIFY LOG.TXT WRITING PERMISSIONS BEFORE PROCEEDING WITH ANY OTHER OPERATIONS
	//LogToFile("INFO", "euroNAT.dll loaded successfully.");

	std::map <CString, NATWaypoint> wp_map;
	bool missingFixesDetected = false; // Scope lifted here: Available to both engines and the wrap-up

	// 1. Get DLL directory
	TCHAR dllpath[MAX_PATH];
	HMODULE hMod = GetModuleHandle(_T("euroNAT.dll"));
	GetModuleFileName(hMod, dllpath, MAX_PATH);

	CString wpfilename(dllpath);
	wpfilename = wpfilename.Left(wpfilename.ReverseFind(_T('\\')) + 1);
	wpfilename += _T("waypoints.txt");

	// 2. Create file if it doesn't exist
	if (!PathFileExists(wpfilename)) {
		std::ofstream outfile; // Declare first
		outfile.open((LPCTSTR)wpfilename); // Open second (Safely avoids most vexing parse)

		if (!outfile.is_open()) {
			LogToFile("ERROR", "Unable to create waypoints.txt. Check folder permissions.");
			euroNatPlugin->DisplayUserMessage("euroNAT", "Error", "Unable to create waypoints.txt. Check euroNAT.log.", true, true, false, false, false);
			NATShow::Loading = false;
			return -1;
		}
		outfile << "; Name\tLatitude\tLongitude" << std::endl;
		outfile.close();
	}

	// 3. Proceed to read the file
	std::ifstream file;
	file.open((LPCTSTR)wpfilename);
	if (file.is_open()) {
		std::string wpLine;
		while (std::getline(file, wpLine)) {
			// Skip comments or empty lines
			if (wpLine.empty() || wpLine[0] == ';') continue;

			std::stringstream ss(wpLine);
			std::string wpName, wpLat, wpLon;

			// Parse the tab-separated format: Name \t Latitude \t Longitude
			if (std::getline(ss, wpName, '\t') &&
				std::getline(ss, wpLat, '\t') &&
				std::getline(ss, wpLon, '\t'))
			{
				try {
					NATWaypoint cachedWp;
					cachedWp.Name = wpName.c_str();
					cachedWp.ShortName = wpName.c_str();
					cachedWp.Position.m_Latitude = std::stod(wpLat);
					cachedWp.Position.m_Longitude = std::stod(wpLon);

					// Cache it into our map using its name as the lookup key
					wp_map[cachedWp.Name] = cachedWp;
				}
				catch (...) {
					// Malformed line safety check
					continue;
				}
			}
		}
		file.close();
	}

	// Determine destination target based on your GUI selection
	CString targetURL = natURL;
	if (g_NatSource == 1) {
		targetURL = "https://nattrak.vatsim.net/api/v2/tracks";
	}
	

	CWebGrab grab;
	CString response;
	int NATcnt = 0;

	if (!grab.GetFile(targetURL, response)) {
		CString errorMessage = grab.GetErrorMessage();
		LogToFile("ERROR", "Network fetch failed. URL: " + targetURL + " | Reason: " + errorMessage);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Fetch Error", "Failed to download NAT data. Check euroNAT.log for details.", true, true, false, false, false);
		NATShow::Loading = false;
		return -1;
	}

	// Check for 404
	if (grab.GetRawHeaders().Find("404") >= 0) {
		CString message;
		message.Format("Received '404: Not Found' at %s.", (LPCTSTR)targetURL);
		LogToFile("ERROR", message);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Info", "Received '404: Not Found' from data server. Details logged.", true, true, false, false, false);
		NATShow::Loading = false;
		return -1;
	}
	grab.Close();

	// ==========================================
	// ADDED: Log the raw payload from this fetch cycle
	// ==========================================
	LogPayloadToFile(response);

	// =========================================================================
	// Strip trailing network buffer corruption/garbage
	// =========================================================================
	int lastBracket = response.ReverseFind(']');
	int lastBrace = response.ReverseFind('}');
	int jsonEndIdx = (lastBracket > lastBrace) ? lastBracket : lastBrace;

	if (jsonEndIdx >= 0) {
		response = response.Left(jsonEndIdx + 1);
	}


	if (g_NatSource == 1) {
		LogToFile("INFO", "Loading natTrak Data");
		// =========================================================================
		// ENGINE A: VATSIM NATTRAK DIRECT NATIVE JSON PARSING
		// =========================================================================
		std::string jsonRawSource((LPCTSTR)response);
		try {
			auto parsedJson = json::parse(jsonRawSource);

			if (!parsedJson.is_array()) {
				NATShow::Loading = false;
				return -1;
			}

			CString pluginDir(dllpath);
			pluginDir = pluginDir.Left(pluginDir.ReverseFind('\\') + 1);
			CString isecPath = pluginDir + "ISEC.txt";
			bool isecExists = (GetFileAttributes(isecPath) != INVALID_FILE_ATTRIBUTES);
			if (!isecExists) {
				LogToFile("ERROR", "ISEC.txt is missing from the plugin directory! Named intersections cannot be resolved.");
			}

			for (const auto& trackItem : parsedJson) {
				if (NATcnt >= MAXNATS) break;

				std::string idStr = trackItem.value("identifier", "");
				if (idStr.empty()) continue;

				int tmi = 0;
				std::string validFrom = trackItem.value("valid_from", "");
				if (validFrom.length() >= 10 && validFrom[4] == '-' && validFrom[7] == '-') {
					try {
						int year = std::stoi(validFrom.substr(0, 4));
						int month = std::stoi(validFrom.substr(5, 2));
						int day = std::stoi(validFrom.substr(8, 2));
						int daysBeforeMonth[] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
						if (month >= 1 && month <= 12) {
							tmi = daysBeforeMonth[month - 1] + day;
							if (month > 2 && ((year % 4 == 0 && year % 100 != 0) || (year % 400 == 0))) {
								tmi += 1;
							}
						}
					}
					catch (...) { tmi = 0; }
				}

				int concordeVal = trackItem.value("concorde", 0);
				dta->m_pNats[NATcnt].Concorde = (concordeVal != 0);
				dta->m_pNats[NATcnt].TMI = tmi;
				dta->m_pNats[NATcnt].Letter = idStr[0];

				std::string direction = trackItem.value("direction", "west");
				dta->m_pNats[NATcnt].Dir = (direction == "east" || direction == "EAST") ? EAST : WEST;

				for (int i = 0; i <= 20; i++) dta->m_pNats[NATcnt].FlightLevels[i] = 0;

				if (trackItem.contains("flight_levels") && trackItem["flight_levels"].is_array()) {
					int flCounter = 0;
					for (const auto& lvl : trackItem["flight_levels"]) {
						if (flCounter >= FLCOUNT) break;
						dta->m_pNats[NATcnt].FlightLevels[flCounter++] = lvl.get<int>() / 100;
					}
				}

				int waypoint_index = 0;
				std::string routeStr = trackItem.value("last_routeing", "");
				std::stringstream ss(routeStr);
				std::string token;

				while (ss >> token) {
					if (waypoint_index >= 30) break;
					CString wp(token.c_str());
					wp.Trim();
					if (wp.IsEmpty()) continue;

					int slashIdx = wp.Find('/');
					if (slashIdx >= 0) {
						// This is a dynamic coordinate fix token like "61/20" or "5930/30"
						CString latStr = wp.Left(slashIdx);
						CString lonStr = wp.Mid(slashIdx + 1);

						// Parse Latitude (Handles whole degrees "61" or degrees+minutes "5930")
						double latitude = 0.0;
						if (latStr.GetLength() > 2) {
							double deg = _ttof(latStr.Left(2));
							double min = _ttof(latStr.Mid(2));
							latitude = deg + (min / 60.0); // Convert minutes to decimal (e.g., 30/60 = 0.5)
						}
						else {
							latitude = _ttof(latStr);
						}

						// Parse Longitude (Handles whole degrees "30" or degrees+minutes "3030" just in case)
						double longitude = 0.0;
						if (lonStr.GetLength() > 2) {
							double deg = _ttof(lonStr.Left(2));
							double min = _ttof(lonStr.Mid(2));
							longitude = deg + (min / 60.0);
						}
						else {
							longitude = _ttof(lonStr);
						}
						longitude = -longitude; // Convert West longitudes to negative coordinates

						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Latitude = latitude;
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Longitude = longitude;

						// Apply tracking names for EuroScope representation (e.g., Short: "30W", Full: "5930N30W")
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].ShortName = lonStr + _T("W");
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Name = latStr + _T("N") + lonStr + _T("W");
						waypoint_index++;
					}
					else {
						// This is a standard named entry/exit intersection fix like "BALIX"
						if (wp_map.find(wp) != wp_map.end()) {
							dta->m_pNats[NATcnt].Waypoints[waypoint_index] = wp_map.at(wp);
							waypoint_index++;
						}
						else if (isecExists && checkISEC(wp, &dta->m_pNats[NATcnt].Waypoints[waypoint_index])) {
							wp_map.insert(pair<CString, NATWaypoint>(dta->m_pNats[NATcnt].Waypoints[waypoint_index].Name, dta->m_pNats[NATcnt].Waypoints[waypoint_index]));
							waypoint_index++;
						}
						else {
							//Log missing named fixes safely here
							CString errorMsg;
							errorMsg.Format("VATSIM Track %c: Fix %s not found in ISEC.txt or out of bounds.", dta->m_pNats[NATcnt].Letter, (LPCTSTR)wp);
							LogToFile("WARNING", errorMsg);
							missingFixesDetected = true;
						}
					}
				}
				dta->m_pNats[NATcnt].WPCount = waypoint_index;
				NATcnt++;
			}
		}
		catch (const std::exception& e) {
			CString errMessage;
			errMessage.Format("VATSIM Parsing Exception: %s", e.what());
			LogToFile("EXCEPTION", errMessage);
			euroNatPlugin->DisplayUserMessage("euroNAT", "Data Error", "Error processing VATSIM data feed. Details logged.", true, true, false, false, false);
			NATShow::Loading = false;
			return -1;
		}

	}
	else {
		// =========================================================================
		// ENGINE B: ORIGINAL FAA TEXT REGEX PARSER
		// =========================================================================
		std::string jsonRawSource((LPCTSTR)response);
		std::string stitchedLegacyText = "";

		LogToFile("INFO", "Loading FAA Data");

		try {
			auto parsedJson = json::parse(jsonRawSource);
			if (parsedJson.is_array()) {
				for (const auto& part : parsedJson) {
					if (part.contains("condition_message") && part["condition_message"].is_string()) {
						stitchedLegacyText += part["condition_message"].get<std::string>();
						stitchedLegacyText += "\r\n";
					}
				}
			}
		}
		catch (const std::exception& e) {
			CString errMessage;
			errMessage.Format("JSON Parsing Exception: %s", e.what());
			LogToFile("EXCEPTION", errMessage);
			euroNatPlugin->DisplayUserMessage("euroNAT", "Error", "Error parsing legacy FAA payload wrap. Details logged.", true, true, false, false, false);
			NATShow::Loading = false;
			return -1;
		}

		response = stitchedLegacyText.c_str();
		response.Replace("\r", "");

		if (response.Find("NO DATA IS ACTIVE") >= 0) {
			LogToFile("INFO", "FAA source reported: NO DATA IS ACTIVE.");
			euroNatPlugin->DisplayUserMessage("euroNAT", "Info", "No FAA NAT Data is active currently.", true, true, false, false, false);
			NATShow::Loading = false;
			return -1;
		}

		int tmi = -1;
		int tmi_cursor = response.Find("TMI IS ");
		if (tmi_cursor >= 0) {
			CString tmi_string = response.Mid(tmi_cursor + 7, 3);
			string tmi_temp;
			int i = 0;
			while (isdigit(tmi_string[i])) {
				tmi_temp += tmi_string[i];
				i++;
			}
			tmi = stoi(tmi_temp);
		}
		else {
			LogToFile("ERROR", "FAA payload structurally malformed; 'TMI IS' token not found.");
			euroNatPlugin->DisplayUserMessage("euroNAT", "Info", "FAA data format unexpected. Details logged.", true, true, false, false, false);
			NATShow::Loading = false;
			return -1;
		}

		CString pluginDir(dllpath);
		pluginDir = pluginDir.Left(pluginDir.ReverseFind('\\') + 1);
		CString isecPath = pluginDir + "ISEC.txt";

		CString navDataPath = pluginDir;
		for (int i = 0; i < 2; i++) {
			navDataPath = navDataPath.Left(navDataPath.GetLength() - 1);
			navDataPath = navDataPath.Left(navDataPath.ReverseFind('\\') + 1);
		}
		navDataPath += "NavData\\ISEC.txt";

		bool sourceExists = (GetFileAttributes(navDataPath) != INVALID_FILE_ATTRIBUTES);
		bool targetExists = (GetFileAttributes(isecPath) != INVALID_FILE_ATTRIBUTES);

		if (sourceExists) {
			if (!targetExists || (GetFileWriteTime(navDataPath) > GetFileWriteTime(isecPath))) {
				if (!CopyFile(navDataPath, isecPath, FALSE)) {
					LogToFile("WARNING", "Found newer local NavData\\ISEC.txt but failed to copy it to the plugin directory.");
				}
				else {
					LogToFile("INFO", "ISEC.txt successfully updated from local NavData directory.");
				}
			}
		}

		bool isecExists = (GetFileAttributes(isecPath) != INVALID_FILE_ATTRIBUTES);
		if (!isecExists) {
			LogToFile("ERROR", "ISEC.txt is missing from the plugin directory! Named fix extraction might fail.");
		}
		string res((LPCTSTR)response);
		const regex track_regex("([a-zA-Z]\\s+)([a-zA-Z]{5}\\s+)*(\\d{2,}\\/\\d{2,}\\s+)*(\\d{2,}\\/\\d{2,})*([a-zA-Z]{5}\\s+)*([a-zA-Z]{5})*\\nEAST LVLS .+\\nWEST LVLS .+\\n");

		auto words_begin = sregex_iterator(res.begin(), res.end(), track_regex);
		auto words_end = sregex_iterator();

		for (sregex_iterator iter = words_begin; iter != words_end; ++iter) {
			smatch match = *iter;
			CString nat = match.str().c_str();

			dta->m_pNats[NATcnt].Concorde = false;
			dta->m_pNats[NATcnt].TMI = tmi;
			dta->m_pNats[NATcnt].Letter = nat[0];

			int waypoint_index = 0;
			int cursor = 2;
			while (cursor < nat.GetLength()) {
				if (nat[cursor] == ' ') { cursor++; continue; }

				if (nat[cursor] == '\n') {
					cursor++;
					if (cursor >= nat.GetLength()) continue;

					CString dir = nat.Mid(cursor, 4);
					cursor += 10;

					if (nat[cursor] == 'N') {
						cursor += 3;
						continue;
					}
					else if (isdigit(nat[cursor])) {
						int flight_levels[FLCOUNT] = { 0 };
						int flCount = 0; 

						while (nat[cursor] != '\n') {
							int flight_level = atoi(nat.Mid(cursor, 3));
							flight_levels[flCount] = flight_level;
							cursor += 3;
							if (nat[cursor] == ' ') cursor++;
							flCount++;
							continue;
						}

						Direction dir_enum;
						(dir == "EAST") ? dir_enum = EAST : dir_enum = WEST;
						dta->m_pNats[NATcnt].Dir = dir_enum;

						for (int i = 0; i <= 20; i++) {
							dta->m_pNats[NATcnt].FlightLevels[i] = flight_levels[i];
						}
					}
				}

				if (isalpha(nat[cursor])) {
					CString wp = nat.Mid(cursor, 5);
					cursor += 5;

					if (wp_map.find(wp) != wp_map.end()) {
						dta->m_pNats[NATcnt].Waypoints[waypoint_index] = wp_map.at(wp);
						waypoint_index++;
					}
					else if (isecExists) {
						NATWaypoint natwp;
						if (checkISEC(wp, &natwp)) {
							dta->m_pNats[NATcnt].Waypoints[waypoint_index] = natwp;
							waypoint_index++;
							wp_map.insert(pair<CString, NATWaypoint>(natwp.Name, natwp));
						}
						else {
							CString errorMsg;
							errorMsg.Format("FAA Track %c: Fix %s not found in ISEC.txt or out of bounds.", dta->m_pNats[NATcnt].Letter, (LPCTSTR)wp);
							LogToFile("WARNING", errorMsg);
							missingFixesDetected = true;
						}
					}
					continue;
				}

				if (isdigit(nat[cursor])) {
					int slashIdx = nat.Find('/', cursor);
					if (slashIdx >= 0) {
						// Extract raw latitude string up to the slash
						CString latStr = nat.Mid(cursor, slashIdx - cursor);

						// Extract raw longitude string (all digits following the slash)
						int lonStart = slashIdx + 1;
						int lonEnd = lonStart;
						while (lonEnd < nat.GetLength() && isdigit(nat[lonEnd])) {
							lonEnd++;
						}
						CString lonStr = nat.Mid(lonStart, lonEnd - lonStart);

						// Advance main loop cursor past this coordinate block cleanly
						cursor = lonEnd;

						// Parse Latitude mathematically (Handles "55" or "5930")
						double latitude = 0.0;
						if (latStr.GetLength() > 2) {
							double deg = _ttof(latStr.Left(2));
							double min = _ttof(latStr.Mid(2));
							latitude = deg + (min / 60.0); // True decimal conversion
						}
						else {
							latitude = _ttof(latStr);
						}

						// Parse Longitude mathematically (Handles "20" or "2030")
						double longitude = 0.0;
						if (lonStr.GetLength() > 2) {
							double deg = _ttof(lonStr.Left(2));
							double min = _ttof(lonStr.Mid(2));
							longitude = deg + (min / 60.0);
						}
						else {
							longitude = _ttof(lonStr);
						}
						longitude = -longitude; // Convert West longitudes to negative coordinates

						// Assign position coordinates
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Latitude = latitude;
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Longitude = longitude;

						// Construct EuroScope tracking identifiers directly from raw strings
						// e.g., ShortName = "30W", Name = "5930N30W"
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].ShortName = lonStr + _T("W");
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Name = latStr + _T("N") + lonStr + _T("W");

						waypoint_index++;
						continue;
					}
				}
			}
			dta->m_pNats[NATcnt].WPCount = waypoint_index;
			NATcnt++;
		}
	}

	// =========================================================================
	// FINAL WRAP-UP (Runs for both engines)
	// =========================================================================
	if (missingFixesDetected) {
		euroNatPlugin->DisplayUserMessage("euroNAT", "Warning", "Some track waypoints were missing from ISEC.txt and skipped. See euroNAT.log.", true, true, false, false, false);
	}

	*dta->m_pNatCount = NATcnt;
	NATData::AddConcordTracks(dta);
	NATShow::Loading = false;
	return 0;
}


bool NATData::checkISEC(CString wp, NATWaypoint* natwp) {
	TCHAR dllpath[2048];
	GetModuleFileName(GetModuleHandle("euroNAT.dll"), dllpath, 2048);
	CString isecfilename(dllpath);
	isecfilename = isecfilename.Left(isecfilename.ReverseFind('\\') + 1) + "ISEC.txt";

	ifstream file(isecfilename);
	string line, name, lat, lon;

	while (getline(file, line)) {
		if (line.empty() || line[0] == ';') continue;

		if (line.find(wp) != string::npos) {
			stringstream linestream(line);
			getline(linestream, name, '\t');
			getline(linestream, lat, '\t');
			getline(linestream, lon, '\t');

			try {
				double latVal = stod(lat);
				double lonVal = stod(lon);

				// North Atlantic Bounding Box
				if ((latVal >= 30.0 && latVal <= 90.0) && (lonVal >= -65.0 && lonVal <= 1.0)) {
					natwp->Name = name.c_str();
					natwp->ShortName = name.c_str();
					natwp->Position.m_Latitude = latVal;
					natwp->Position.m_Longitude = lonVal;

					// Append to waypoints.txt
					fstream wpfile(isecfilename.Left(isecfilename.ReverseFind('\\') + 1) + "waypoints.txt", fstream::app);
					wpfile << name << "\t" << lat << "\t" << lon << endl;
					wpfile.close();

					// ADDED: Log successfully resolved waypoints
					CString logMsg;
					logMsg.Format("Found fix '%s' via ISEC.txt [Lat: %s, Lon: %s] and added to waypoints.txt.",
						(LPCTSTR)wp, lat.c_str(), lon.c_str());
					LogToFile("INFO", logMsg);

					return true;
				}
			}
			catch (...) { continue; }
		}
	}
	return false;
}


void NATData::AddConcordTracks(NATWorkerCont* dta) {
	int i = *dta->m_pNatCount;

	// SM
	dta->m_pNats[i].Concorde = true;
	dta->m_pNats[i].Dir = Direction::NONE;
	dta->m_pNats[i].Letter = 'M';
	dta->m_pNats[i].Waypoints[0].Name = "15W";
	dta->m_pNats[i].Waypoints[0].Position.m_Latitude = 50.683333;
	dta->m_pNats[i].Waypoints[0].Position.m_Longitude = -15;
	dta->m_pNats[i].Waypoints[1].Name = "20W";
	dta->m_pNats[i].Waypoints[1].Position.m_Latitude = 50.833333;
	dta->m_pNats[i].Waypoints[1].Position.m_Longitude = -20;
	dta->m_pNats[i].Waypoints[2].Name = "30W";
	dta->m_pNats[i].Waypoints[2].Position.m_Latitude = 50.5;
	dta->m_pNats[i].Waypoints[2].Position.m_Longitude = -30;
	dta->m_pNats[i].Waypoints[3].Name = "40W";
	dta->m_pNats[i].Waypoints[3].Position.m_Latitude = 49.266667;
	dta->m_pNats[i].Waypoints[3].Position.m_Longitude = -40;
	dta->m_pNats[i].Waypoints[4].Name = "50W";
	dta->m_pNats[i].Waypoints[4].Position.m_Latitude = 47.05;
	dta->m_pNats[i].Waypoints[4].Position.m_Longitude = -50;
	dta->m_pNats[i].Waypoints[5].Name = "53W";
	dta->m_pNats[i].Waypoints[5].Position.m_Latitude = 46.166667;
	dta->m_pNats[i].Waypoints[5].Position.m_Longitude = -53;
	dta->m_pNats[i].Waypoints[6].Name = "60W";
	dta->m_pNats[i].Waypoints[6].Position.m_Latitude = 44.233333;
	dta->m_pNats[i].Waypoints[6].Position.m_Longitude = -60;
	dta->m_pNats[i].Waypoints[7].Name = "65W";
	dta->m_pNats[i].Waypoints[7].Position.m_Latitude = 42.766667;
	dta->m_pNats[i].Waypoints[7].Position.m_Longitude = -65;
	dta->m_pNats[i].Waypoints[8].Name = "67W";
	dta->m_pNats[i].Waypoints[8].Position.m_Latitude = 42;
	dta->m_pNats[i].Waypoints[8].Position.m_Longitude = -67;
	dta->m_pNats[i].WPCount = 9;
	i++;

	//SN
	dta->m_pNats[i].Concorde = true;
	dta->m_pNats[i].Dir = Direction::NONE;
	dta->m_pNats[i].Letter = 'N';
	dta->m_pNats[i].Waypoints[0].Name = "67W";
	dta->m_pNats[i].Waypoints[0].Position.m_Latitude = 40.416667;
	dta->m_pNats[i].Waypoints[0].Position.m_Longitude = -67;
	dta->m_pNats[i].Waypoints[1].Name = "65W";
	dta->m_pNats[i].Waypoints[1].Position.m_Latitude = 41.666667;
	dta->m_pNats[i].Waypoints[1].Position.m_Longitude = -65;
	dta->m_pNats[i].Waypoints[2].Name = "60W";
	dta->m_pNats[i].Waypoints[2].Position.m_Latitude = 43.116667;
	dta->m_pNats[i].Waypoints[2].Position.m_Longitude = -60;
	dta->m_pNats[i].Waypoints[3].Name = "5230W";
	dta->m_pNats[i].Waypoints[3].Position.m_Latitude = 45.166667;
	dta->m_pNats[i].Waypoints[3].Position.m_Longitude = -52.5;
	dta->m_pNats[i].Waypoints[4].Name = "50W";
	dta->m_pNats[i].Waypoints[4].Position.m_Latitude = 45.9;
	dta->m_pNats[i].Waypoints[4].Position.m_Longitude = -50;
	dta->m_pNats[i].Waypoints[5].Name = "40W";
	dta->m_pNats[i].Waypoints[5].Position.m_Latitude = 48.166667;
	dta->m_pNats[i].Waypoints[5].Position.m_Longitude = -40;
	dta->m_pNats[i].Waypoints[6].Name = "30W";
	dta->m_pNats[i].Waypoints[6].Position.m_Latitude = 49.433333;
	dta->m_pNats[i].Waypoints[6].Position.m_Longitude = -30;
	dta->m_pNats[i].Waypoints[7].Name = "20W";
	dta->m_pNats[i].Waypoints[7].Position.m_Latitude = 49.816667;
	dta->m_pNats[i].Waypoints[7].Position.m_Longitude = -20;
	dta->m_pNats[i].Waypoints[8].Name = "15W";
	dta->m_pNats[i].Waypoints[8].Position.m_Latitude = 49.683333;
	dta->m_pNats[i].Waypoints[8].Position.m_Longitude = -15;
	dta->m_pNats[i].WPCount = 9;
	i++;

	//SL
	/*
	dta->m_pNats[i].Concorde = true;
	dta->m_pNats[i].Dir = Direction::NONE;
	dta->m_pNats[i].Letter = 'L';
	dta->m_pNats[i].Waypoints[0].Name = NATShow::ShortWPNames ? "50W" : "57N50W";
	dta->m_pNats[i].Waypoints[0].Position.m_Latitude = 57;
	dta->m_pNats[i].Waypoints[0].Position.m_Longitude = -50;
	dta->m_pNats[i].Waypoints[1].Name = NATShow::ShortWPNames ? "40W" : "57N40W";
	dta->m_pNats[i].Waypoints[1].Position.m_Latitude = 57;
	dta->m_pNats[i].Waypoints[1].Position.m_Longitude = -40;
	dta->m_pNats[i].Waypoints[2].Name = NATShow::ShortWPNames ? "30W" : "56N30W";
	dta->m_pNats[i].Waypoints[2].Position.m_Latitude = 56;
	dta->m_pNats[i].Waypoints[2].Position.m_Longitude = -30;
	dta->m_pNats[i].Waypoints[3].Name = NATShow::ShortWPNames ? "20W" : "54N20W";
	dta->m_pNats[i].Waypoints[3].Position.m_Latitude = 54;
	dta->m_pNats[i].Waypoints[3].Position.m_Longitude = -20;
	dta->m_pNats[i].Waypoints[4].Name = NATShow::ShortWPNames ? "15W" : "52N15W";
	dta->m_pNats[i].Waypoints[4].Position.m_Latitude = 52;
	dta->m_pNats[i].Waypoints[4].Position.m_Longitude = -15;
	dta->m_pNats[i].WPCount = 5;
	i++;
	*/
	//SP
	dta->m_pNats[i].Concorde = true;
	dta->m_pNats[i].Dir = Direction::NONE;
	dta->m_pNats[i].Letter = 'P';
	dta->m_pNats[i].Waypoints[0].Name = "20W";
	dta->m_pNats[i].Waypoints[0].Position.m_Latitude = 46.816667;
	dta->m_pNats[i].Waypoints[0].Position.m_Longitude = -20;
	dta->m_pNats[i].Waypoints[1].Name = "45N";
	dta->m_pNats[i].Waypoints[1].Position.m_Latitude = 45;
	dta->m_pNats[i].Waypoints[1].Position.m_Longitude = -23.883333;
	dta->m_pNats[i].Waypoints[2].Name = "30W";
	dta->m_pNats[i].Waypoints[2].Position.m_Latitude = 41.6;
	dta->m_pNats[i].Waypoints[2].Position.m_Longitude = -30;
	dta->m_pNats[i].Waypoints[3].Name = "40W";
	dta->m_pNats[i].Waypoints[3].Position.m_Latitude = 34.366667;
	dta->m_pNats[i].Waypoints[3].Position.m_Longitude = -40;
	dta->m_pNats[i].Waypoints[4].Name = "27N";
	dta->m_pNats[i].Waypoints[4].Position.m_Latitude = 27;
	dta->m_pNats[i].Waypoints[4].Position.m_Longitude = -47.783333;
	dta->m_pNats[i].Waypoints[5].Name = "50W";
	dta->m_pNats[i].Waypoints[5].Position.m_Latitude = 24.633333;
	dta->m_pNats[i].Waypoints[5].Position.m_Longitude = -50;
	dta->m_pNats[i].Waypoints[6].Name = "18N";
	dta->m_pNats[i].Waypoints[6].Position.m_Latitude = 18;
	dta->m_pNats[i].Waypoints[6].Position.m_Longitude = -55.65;
	dta->m_pNats[i].WPCount = 7;
	i++;

	//SO
	dta->m_pNats[i].Concorde = true;
	dta->m_pNats[i].Dir = Direction::NONE;
	dta->m_pNats[i].Letter = 'O';
	dta->m_pNats[i].Waypoints[0].Name = "15W";
	dta->m_pNats[i].Waypoints[0].Position.m_Latitude = 48.666667;
	dta->m_pNats[i].Waypoints[0].Position.m_Longitude = -15;
	dta->m_pNats[i].Waypoints[1].Name = "20W";
	dta->m_pNats[i].Waypoints[1].Position.m_Latitude = 48.8;
	dta->m_pNats[i].Waypoints[1].Position.m_Longitude = -20;
	dta->m_pNats[i].Waypoints[2].Name = "30W";
	dta->m_pNats[i].Waypoints[2].Position.m_Latitude = 48.366667;
	dta->m_pNats[i].Waypoints[2].Position.m_Longitude = -30;
	dta->m_pNats[i].Waypoints[3].Name = "40W";
	dta->m_pNats[i].Waypoints[3].Position.m_Latitude = 47.066667;
	dta->m_pNats[i].Waypoints[3].Position.m_Longitude = -40;
	dta->m_pNats[i].Waypoints[4].Name = "50W";
	dta->m_pNats[i].Waypoints[4].Position.m_Latitude = 44.75;
	dta->m_pNats[i].Waypoints[4].Position.m_Longitude = -50;
	dta->m_pNats[i].Waypoints[5].Name = "52W";
	dta->m_pNats[i].Waypoints[5].Position.m_Latitude = 44.166667;
	dta->m_pNats[i].Waypoints[5].Position.m_Longitude = -52;
	dta->m_pNats[i].Waypoints[6].Name = "60W";
	dta->m_pNats[i].Waypoints[6].Position.m_Latitude = 42;
	dta->m_pNats[i].Waypoints[6].Position.m_Longitude = -60;
	dta->m_pNats[i].WPCount = 7;
	i++; // Increment final tracking state properly

	// Assign the absolute final count dynamically with no buffer padding gaps
	*dta->m_pNatCount = i;
}







