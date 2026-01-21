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
using namespace std;

NATData::NATWorkerCont NATData::NATWorkerData;
NATData * NATData::LastInstance = NULL;
CPlugIn* euroNatPlugin;

//Use only one of the below links (if prod, always the first)
//Remote FAA link to capture NAT Message
CString natURL = "https://notams.aim.faa.gov/nat.html";
//Local link to fake message for test purpose
//CString natURL = "";

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
			// Use std::ofstream to create the file and write a header/template
			std::ofstream outfile((LPCTSTR)wpfilename);
			if (outfile.is_open()) {
				outfile << "; Name\tLatitude\tLongitude" << std::endl;
				outfile.close();
			}
		}

		// 3. Proceed to read the file (your existing logic)
		std::ifstream file((LPCTSTR)wpfilename);
		// ... rest of your while(getline(file, line)) loop ...

	}
	catch (...) {
		euroNatPlugin->DisplayUserMessage("euroNAT", "Info", "waypoints.txt not found and/or unable to create", true, true, true, true, true);
		NATShow::Loading = false;
		return -1;

	}

	CWebGrab grab;
	CString response;
	int NATcnt = 0;

	if (!grab.GetFile(natURL, response)) {
		CString errorMessage =  grab.GetErrorMessage();
		euroNatPlugin->DisplayUserMessage("euroNAT", "Error", errorMessage, true, true, true, true, true);
		CString message;
		message.Format("Couldn't open %s", natURL);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Error", message, true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
	}

	// Check for 404
	if (grab.GetRawHeaders().Find("404") >= 0) {
		CString message;
		message.Format("Received '404: Not Found' at %s.", natURL);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
	}
	grab.Close();

	// Check for 'NO DATA IS ACTIVE'
	if (response.Find("NO DATA IS ACTIVE") >= 0) {
		CString message;
		message.Format("No NAT Data is active, %s", natURL);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
	}

	// TMI Could be 1 or 3 digits (day of the year)
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
	} else {
		CString message;
		message.Format("NAT Data not in expected format, %s", natURL);
		euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
	}

	// CString to string for regex searching
	string res((LPCTSTR) response);

	// TODO: Optimise?
	const regex track_regex("([a-zA-Z]\\s+)([a-zA-Z]{5}\\s+)*(\\d{2,}\\/\\d{2,}\\s+)*(\\d{2,}\\/\\d{2,})*([a-zA-Z]{5}\\s+)*([a-zA-Z]{5})*\\nEAST LVLS .+\\nWEST LVLS .+\\n");

	// Find all the matches
	auto words_begin = sregex_iterator(res.begin(), res.end(), track_regex);
	auto words_end = sregex_iterator();

	// Loop through every match (NAT data string)
	for (sregex_iterator iter = words_begin; iter != words_end; ++iter) {
		smatch match = *iter;
		CString nat = match.str().c_str();

		// Make a NAT
		dta->m_pNats[NATcnt].Concorde = false;
		dta->m_pNats[NATcnt].TMI = tmi;
		dta->m_pNats[NATcnt].Letter = nat[0];

		// Tracks the index for the next waypoint to add.
		int waypoint_index = 0;

		// Parse one NAT string, build and add one NAT.
		// Reset the cursor each loop
		int cursor = 2;
		while (cursor < nat.GetLength()) {
			// SPACE ----------------------------------------------------------
			if (nat[cursor] == ' ') { cursor++; continue; }

			// NEW LINE \n EAST OR WEST FL ------------------------------------
			if (nat[cursor] == '\n') {
				cursor++;
				// Check if there's more string
				if (cursor >= nat.GetLength()) continue;

				CString dir = nat.Mid(cursor, 4);
				// Skip 'EAST LVLS ' / 'WEST LVLS '
				cursor += 10;

				// 'NIL'
				if (nat[cursor] == 'N') {
					cursor += 3;
					continue;

				} else if (isdigit(nat[cursor])) {
					int flight_levels[FLCOUNT] = {0};
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

			// WAYPOINT -------------------------------------------------------
			if (isalpha(nat[cursor])) {
				CString wp = nat.Mid(cursor, 5);
				cursor += 5;

				if (wp_map.find(wp) == wp_map.end()) {
					// Not found in the waypoints.txt map
					CString message;
					message.Format("Didn't find %s in waypoints.txt, looking in ISEC.txt", wp);
					euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, false, false, false, false);

					// Look in ISEC.txt
					NATWaypoint natwp;
					if (checkISEC(wp, &natwp)) {
						// Found in ISEC.txt
						dta->m_pNats[NATcnt].Waypoints[waypoint_index] = natwp;
						waypoint_index++;

						wp_map.insert(pair<CString, NATWaypoint>(natwp.Name, natwp));
						
						CString message;
						message.Format("Found %s in ISEC.txt, added it to waypoints.txt", wp);
						euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, false, false, false, false);

					} else {
						CString message;
						message.Format("Cannot find %s in ISEC.txt. If it's a valid waypoint, consider updating ISEC.txt in the euroNAT.dll directory.", wp);
						euroNatPlugin->DisplayUserMessage("euroNAT", "Error", message, true, true, true, true, true);
					}
				} else {
				// Found
				dta->m_pNats[NATcnt].Waypoints[waypoint_index] = wp_map.at(wp);
				
				waypoint_index++;
				}
				
				continue;
			}

			// LAT/LON --------------------------------------------------------
			if (isdigit(nat[cursor])) {
				// Lat and Long each have at least 2 digits, I've seen up to 4 
				//(e.g 5730, which needs to be converted from base 60 to base 100, so it would be 57.50).
				string lat;
				char lat_decimal = 'X'; // Variable to handle the decimal portion of the lat
				lat = nat.Mid(cursor, 2);
				cursor += 2;

				lat.operator+=('.');
				// If lat has additional decimal numbers.
				while (isdigit(nat[cursor])) {
				    //dirty code to convert the half degree to decimal half
				    char current_digit = nat[cursor];
				    // Apply the substitution rule
				    if (current_digit == '3') {
						lat_decimal = current_digit; //store the 3 for using in the name
				        current_digit = '5'; // Now current_digit might be '5'
				    }
    			    // FIX: Append the corrected 'current_digit'
				    lat = lat.operator+=(current_digit); 
				    
				    cursor++;
				}

				// Eat a slash
				cursor++;

				string lon;
				lon = nat.Mid(cursor, 2);
				cursor += 2;
				lon.operator+=('.');
				// If lon has additional decimal numbers.
				while (isdigit(nat[cursor])) {
					lon = lon.operator+=(nat[cursor]);
					cursor++;
				}
				// String to Double
				double latitude = stod(lat);
				double longitude = stod(lon);
				// Longitudes are West, so negative sign is applied.
				longitude = longitude / -1;

				dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Latitude = latitude;
				dta->m_pNats[NATcnt].Waypoints[waypoint_index].Position.m_Longitude = longitude;

				// The Short Name
				CString wp_name = lon.c_str();
				wp_name.Append("W");
				// Remove the decimal.
				wp_name.Replace(".", "");
				
				dta->m_pNats[NATcnt].Waypoints[waypoint_index].ShortName = wp_name;

				// The Long Name
				wp_name = lat.c_str();
				wp_name.Replace(".", ""); // Remove the decimals.
				//replacing the decimal portion dirty put on 5 back to 3
				if  (lat_decimal == '3'){
					// This finds the position of the corrected '5' (index 2)
					int pos_of_5 = wp_name.Find('5', 2); 
    				    // Replace the '5' with the original '3'
					    if (pos_of_5 != -1) {
					        wp_name.SetAt(pos_of_5, '3');
					    }
					}
				wp_name.Append("N");
				wp_name.Append(lon.c_str());
				wp_name.Replace(".", ""); // Remove the decimals.
				wp_name.Append("W");
								
				
				dta->m_pNats[NATcnt].Waypoints[waypoint_index].Name = wp_name;

				// Increment for next waypoint to add
				waypoint_index++;

				continue;
			}

		}
		// Add total number of waypoints
		dta->m_pNats[NATcnt].WPCount = waypoint_index;

		NATcnt++;

		//End of each NAT loop
	}

	*dta->m_pNatCount = NATcnt;

	NATData::AddConcordTracks(dta);

	NATShow::Loading = false;
	return 0;
}

bool NATData::checkISEC(CString wp, NATWaypoint * natwp) {

	try {
		// Get dll directory
		TCHAR dllpath[2048];
		GetModuleFileName(GetModuleHandle("euroNAT.dll"), dllpath, 2048);

		// 1. Establish the Plugin Folder (FIR\Plugins\EuroNat\)
		CString pluginDir(dllpath);
		pluginDir = pluginDir.Left(pluginDir.ReverseFind('\\') + 1);
		CString isecfilename = pluginDir + "ISEC.txt";

		// 2. Check if ISEC.txt exists in the Plugin Folder
		if (GetFileAttributes(isecfilename) == INVALID_FILE_ATTRIBUTES) {

			// 3. Construct NavData path (Go up two levels from EuroNat -> Plugins -> FIR)
			CString navDataPath = pluginDir;
			for (int i = 0; i < 2; i++) {
				navDataPath = navDataPath.Left(navDataPath.GetLength() - 1); // remove trailing slash
				navDataPath = navDataPath.Left(navDataPath.ReverseFind('\\') + 1);
			}
			navDataPath += "NavData\\ISEC.txt";

			// 4. If found in NavData, copy it to the Plugin folder
			if (GetFileAttributes(navDataPath) != INVALID_FILE_ATTRIBUTES) {
				if (CopyFile(navDataPath, isecfilename, FALSE)) {
					CString message;
					message.Format("ISEC.txt copied from NavData to Plugin folder.", wp);
					euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, true, true, true, true);
				}
			}
			else {
				// Handle case where file is missing from both locations
				CString message;
				message.Format("ISEC.txt not found in EuroNat or NavData folders", wp);
				euroNatPlugin->DisplayUserMessage("euroNAT", "Error", message, true, true, true, true, true);
				return false;
			}
		}
		// 5. Now proceed to read the file as before
		// Read in waypoints
		ifstream file(isecfilename);
		string line, name, lat, lon;
		while (getline(file, line)) {
			if (line[0] == ';') continue;

			if (line.find(wp) != string::npos) {
				stringstream linestream(line);

				getline(linestream, name, '\t');
				getline(linestream, lat, '\t');
				getline(linestream, lon, '\t');

				double latVal = stod(lat);
				double lonVal = stod(lon);

				bool latInRange = (latVal >= 30.0 && latVal <= 90.0);
				bool lonInRange = (lonVal >= -70.0 && lonVal <= -1.0);

				if (latInRange && lonInRange) {
					natwp->Name = name.c_str();
					natwp->ShortName = name.c_str();
					natwp->Position.m_Latitude = latVal;
					natwp->Position.m_Longitude = lonVal;

					CString wpfilename(dllpath);
					wpfilename = wpfilename.Left(wpfilename.ReverseFind('\\') + 1);
					wpfilename += "waypoints.txt";

					fstream wpfile(wpfilename, fstream::app);
					wpfile << name + "\t" + lat + "\t" + lon << endl;
					wpfile.close();

					return true;
				}
				else {
					// Log why the waypoint was rejected
					CString message;
					message.Format("Found a %s in ISEC.txt but Coordinates are out of bounds - Skipping this one.", wp);
					euroNatPlugin->DisplayUserMessage("euroNAT", "Info", message, true, false, false, false, false);					
				}
			}
		}
		file.close();

	} catch (...) {
		euroNatPlugin->DisplayUserMessage("euroNAT", "Error", "Unable to open ISEC.txt in plugin directory", true, true, true, true, true);
		NATShow::Loading = false;
		return -1;
		//return false;
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





