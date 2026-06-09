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
using json = nlohmann::json;
using namespace std;

// Link the source state variable from your GUI options panel
extern int g_NatSource;

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

	std::map <CString, NATWaypoint> wp_map;

	try {
		// 1. Get DLL directory
		TCHAR dllpath[MAX_PATH];
		HMODULE hMod = GetModuleHandle(_T("euroNAT.dll"));
		GetModuleFileName(hMod, dllpath, MAX_PATH);

		CString wpfilename(dllpath);
		wpfilename = wpfilename.Left(wpfilename.ReverseFind(_T('\\')) + 1);
		wpfilename += _T("waypoints.txt");

		// 2. Create file if it doesn't exist
		if (!PathFileExists(wpfilename)) {
			std::ofstream outfile((LPCTSTR)wpfilename);
			if (outfile.is_open()) {
				outfile << "; Name\tLatitude\tLongitude" << std::endl;
				outfile.close();
			}
		}

		// 3. Proceed to read the file
		std::ifstream file((LPCTSTR)wpfilename);
		// ... rest of your waypoints file processing ...
	}
	catch (...) {
		euroNatPlugin->DisplayUserMessage("euroNAT", "Info", "waypoints.txt not found and/or unable to create", true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
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
		euroNatPlugin->DisplayUserMessage("euroNAT", "Error", errorMessage, true, true, true, true, true);
		CString message;
		message.Format("Couldn't open %s", targetURL);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Error", message, true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
	}

	// Check for 404
	if (grab.GetRawHeaders().Find("404") >= 0) {
		CString message;
		message.Format("Received '404: Not Found' at %s.", targetURL);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
	}
	grab.Close();


	if (g_NatSource == 1) {
		// =========================================================================
		// ENGINE A: VATSIM NATTRAK DIRECT NATIVE JSON PARSING (CORRECTED SCHEMA)
		// =========================================================================
		std::string jsonRawSource((LPCTSTR)response);
		try {
			auto parsedJson = json::parse(jsonRawSource);

			if (!parsedJson.is_array()) {
				NATShow::Loading = false;
				return -1;
			}

			TCHAR dllpath[2048];
			GetModuleFileName(GetModuleHandle("euroNAT.dll"), dllpath, 2048);
			CString pluginDir(dllpath);
			pluginDir = pluginDir.Left(pluginDir.ReverseFind('\\') + 1);
			CString isecPath = pluginDir + "ISEC.txt";
			bool isecExists = (GetFileAttributes(isecPath) != INVALID_FILE_ATTRIBUTES);

			for (const auto& trackItem : parsedJson) {
				if (NATcnt >= MAXNATS) break;

				// 1. Map track letter identifier
				std::string idStr = trackItem.value("identifier", "");
				if (idStr.empty()) continue;

				// 2. Extract true TMI (Day of Year) from the "valid_from" date string (e.g., "2026-06-09T...")
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
							// Account for leap years
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

				// 3. Evaluate direction string safely matching case-insensitivity
				std::string direction = trackItem.value("direction", "west");
				dta->m_pNats[NATcnt].Dir = (direction == "east" || direction == "EAST") ? EAST : WEST;

				// 4. Initialize and convert flight levels (e.g., 34000 -> 340)
				for (int i = 0; i <= 20; i++) dta->m_pNats[NATcnt].FlightLevels[i] = 0;

				if (trackItem.contains("flight_levels") && trackItem["flight_levels"].is_array()) {
					int flCounter = 0;
					for (const auto& lvl : trackItem["flight_levels"]) {
						if (flCounter >= FLCOUNT) break;
						dta->m_pNats[NATcnt].FlightLevels[flCounter++] = lvl.get<int>() / 100;
					}
				}

				// 5. Parse the space-separated track routing string token by token
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
						// This is a dynamic coordinate fix token like "61/20"
						CString latStr = wp.Left(slashIdx);
						CString lonStr = wp.Mid(slashIdx + 1);

						double latitude = _ttof(latStr);
						double longitude = _ttof(lonStr);
						longitude = -longitude; // Convert West longitudes to negative coordinates

						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Latitude = latitude;
						dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Longitude = longitude;

						// Apply tracking names for EuroScope representation
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
					}
				}
				dta->m_pNats[NATcnt].WPCount = waypoint_index;
				NATcnt++;
			}
		}
		catch (const std::exception& e) {
			CString errMessage;
			errMessage.Format("VATSIM Parsing Exception: %s", e.what());
			euroNatPlugin->DisplayUserMessage("euroNAT", "Error", errMessage, true, true, true, true, true);
			NATShow::Loading = false;
			return -1;
		}

	}
	else {
		// =========================================================================
		// ENGINE B: YOUR ORIGINAL UNALTERED FAA TEXT REGEX PARSER
		// =========================================================================
		std::string jsonRawSource((LPCTSTR)response);
		std::string stitchedLegacyText = "";

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
			euroNatPlugin->DisplayUserMessage("euroNAT", "Error", errMessage, true, true, true, true, true);
			NATShow::Loading = false;
			return -1;
		}

		response = stitchedLegacyText.c_str();
		response.Replace("\r", "");

		if (response.Find("NO DATA IS ACTIVE") >= 0) {
			CString message;
			message.Format("No NAT Data is active, %s", natURL);
			euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, true, true, true, true);
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
			CString message;
			message.Format("NAT Data not in expected format, %s", natURL);
			euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, true, true, true, true);
			NATShow::Loading = false;
			return -1;
		}

		TCHAR dllpath[2048];
		GetModuleFileName(GetModuleHandle("euroNAT.dll"), dllpath, 2048);
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
				if (CopyFile(navDataPath, isecPath, FALSE)) {
					euroNatPlugin->DisplayUserMessage("euroNAT", "Info", "ISEC.txt updated from NavData.", true, false, false, false, false);
				}
			}
		}
		else {
			euroNatPlugin->DisplayUserMessage("euroNAT", "Error", "ISEC.txt was not found. Check for correct instalation of the sectorpack", true, true, true, true, true);
		}

		bool isecExists = (GetFileAttributes(isecPath) != INVALID_FILE_ATTRIBUTES);
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
						int i = 0;

						while (nat[cursor] != '\n') {
							int flight_level = atoi(nat.Mid(cursor, 3));
							flight_levels[i] = flight_level;
							cursor += 3;
							if (nat[cursor] == ' ') cursor++;
							i++;
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
							errorMsg.Format("Cannot find %s in ISEC.txt (or out of bounds).", wp);
							euroNatPlugin->DisplayUserMessage("euroNAT", "Error", errorMsg, true, true, true, true, true);
						}
					}
					else {
						CString missingMsg;
						missingMsg.Format("ISEC.txt missing - Cannot search for %s.", wp);
						euroNatPlugin->DisplayUserMessage("euroNAT", "Error", missingMsg, true, true, true, true, true);
					}
					continue;
				}

				if (isdigit(nat[cursor])) {
					string lat;
					char lat_decimal = 'X';
					lat = nat.Mid(cursor, 2);
					cursor += 2;
					lat.operator+=('.');

					while (isdigit(nat[cursor])) {
						char current_digit = nat[cursor];
						if (current_digit == '3') {
							lat_decimal = current_digit;
							current_digit = '5';
						}
						lat = lat.operator+=(current_digit);
						cursor++;
					}

					cursor++; // Slash
					string lon;
					lon = nat.Mid(cursor, 2);
					cursor += 2;
					lon.operator+=('.');
					while (isdigit(nat[cursor])) {
						lon = lon.operator+=(nat[cursor]);
						cursor++;
					}

					double latitude = stod(lat);
					double longitude = stod(lon);
					longitude = longitude / -1;

					dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Latitude = latitude;
					dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Longitude = longitude;

					CString wp_name = lon.c_str();
					wp_name.Append("W");
					wp_name.Replace(".", "");
					dta->m_pNats[NATcnt].Waypoints[waypoint_index].ShortName = wp_name;

					wp_name = lat.c_str();
					wp_name.Replace(".", "");
					if (lat_decimal == '3') {
						int pos_of_5 = wp_name.Find('5', 2);
						if (pos_of_5 != -1) {
							wp_name.SetAt(pos_of_5, '3');
						}
					}
					wp_name.Append("N");
					wp_name.Append(lon.c_str());
					wp_name.Replace(".", "");
					wp_name.Append("W");

					dta->m_pNats[NATcnt].Waypoints[waypoint_index].Name = wp_name;
					waypoint_index++;
					continue;
				}
			}
			dta->m_pNats[NATcnt].WPCount = waypoint_index;
			NATcnt++;
		}
	}

	// =========================================================================
	// FINAL WRAP-UP (Runs for both engines)
	// =========================================================================
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
				if ((latVal >= 30.0 && latVal <= 90.0) && (lonVal >= -70.0 && lonVal <= -1.0)) {
					natwp->Name = name.c_str();
					natwp->ShortName = name.c_str();
					natwp->Position.m_Latitude = latVal;
					natwp->Position.m_Longitude = lonVal;

					// Append to waypoints.txt
					fstream wpfile(isecfilename.Left(isecfilename.ReverseFind('\\') + 1) + "waypoints.txt", fstream::app);
					wpfile << name << "\t" << lat << "\t" << lon << endl;
					wpfile.close();

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
	*dta->m_pNatCount += 5;

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



}







