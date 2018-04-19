//
// Copyright Copyright 2012, System Insights, Inc.
//
//    Licensed under the Apache License, Version 2.0 (the "License");
//    you may not use this file except in compliance with the License.
//    You may obtain a copy of the License at
//
//       http://www.apache.org/licenses/LICENSE-2.0
//
//    Unless required by applicable law or agreed to in writing, software
//    distributed under the License is distributed on an "AS IS" BASIS,
//    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//    See the License for the specific language governing permissions and
//    limitations under the License.
//
#include <internal.hpp>
#include "fanuc_adapter.hpp"
#include <excpt.h>
#include <regex>
#include <iostream>
#include <thread>
#include <chrono>
#include <minIni.h>

constexpr int DEF_SERVICE_PORT = 7878;
constexpr int DEF_FOCAS2_PORT = 8193;

// C++11 helper function for obtaining a statically defined array's item count
template <typename T, std::size_t N>
constexpr std::size_t countof(T const (&)[N]) noexcept
{
	return N;
}


FanucAdapter::FanucAdapter(int aPort) :
	Adapter(aPort),
	mMaxPath(0),
	mMessage("message"),
	mAvail("avail"),
	mPartCount("part_count"),
	mMacroSample{0},
	mMacroPath{0},
	mMacroMin(99999),
	mMacroMax(0),
	mMacroSampleCount(0),
	mMacroPathCount(0),
	mPMCVariable{0},
	mPMCAddress{0},
	mPMCCount(0),
	mFlibhndl(0),
	mConnected(false),
	mDevicePort(DEF_FOCAS2_PORT),
	mDeviceIP{"localhost"}
{
	// Alarms
	addDatum(mMessage);

	// Controller
	addDatum(mAvail);
	addDatum(mPartCount);

	mConfigured = mConnected = false;
	mAvail.unavailable();
}


FanucAdapter::~FanucAdapter()
{
	for (auto path : mPaths)
		delete path;
	mPaths.clear();

	disconnect();
}


void FanucAdapter::initialize(int argc, const char *argv[])
{
	MTConnectService::initialize(argc, argv);

	auto iniFile = "adapter.ini";
	std::cout << "Arguments: " << argc << std::endl;
	if (argc > 1)
	{
		strncpy(mDeviceIP, argv[0], countof(mDeviceIP) - 1);
		mDevicePort = atoi(argv[1]);
		mPort = DEF_SERVICE_PORT;
		if (argc > 2)
			mPort = atoi(argv[2]);
	}
	else
	{
		mDevicePort = DEF_FOCAS2_PORT;
		strncpy(mDeviceIP, "localhost", countof(mDeviceIP) - 1);
		mPort = DEF_SERVICE_PORT;
		if (argc > 0)
			iniFile = argv[0];
		std::cout << "Ini File: " << iniFile << std::endl;
	}

	auto fp = fopen(iniFile, "r");
	std::cout << "FP = " << errno << ", " << fp << std::endl;
	if(fp)
	{
		fclose(fp);
		fp = nullptr;
	}

	configMacrosAndPMC(iniFile);
}


void FanucAdapter::start()
{
	startServer();
}


void FanucAdapter::stop()
{
	stopServer();
}


void FanucAdapter::innerGatherDeviceData()
{
	try
	{
		if (!mConnected)
			connect();
		else
		{
			getPathData();
			getMessages();
			getMacros();
			getPMC();
			getCounts();
		}
	}
	catch (...)
	{
		gLogger->error("Unhandled exception occurred during gathering device data, disconnecting.");
		disconnect();
	}
}


void FanucAdapter::gatherDeviceData()
{
	__try
	{
		innerGatherDeviceData();
	}

	__except(EXCEPTION_EXECUTE_HANDLER)
	{
		gLogger->error("Unhandled structured exception occurred during gathering device data, disconnecting.");
		disconnect();
	}
}


void FanucAdapter::disconnect()
{
	if (mConnected)
	{
		std::cout << "Machine has disconnected. Releasing Resources" << std::endl;
		cnc_freelibhndl(mFlibhndl);
		mFlibhndl = 0;
		mConnected = false;
		unavailable();
	}
}


void FanucAdapter::configMacrosAndPMC(const char *iniFile)
{
	// Read adapter configuration
	mPort = ini_getl("adapter", "port", mPort, iniFile);
	ini_gets("adapter",
		"service",
		"MTConnect Fanuc Adapter",
		mName,
		SERVICE_NAME_LEN,
		iniFile);
	mScanDelay = std::chrono::milliseconds{ ini_getl("adapter", "scanDelay", 100, iniFile) };

	ini_gets("focus", "host", mDeviceIP, mDeviceIP, countof(mDeviceIP), iniFile);
	mDevicePort = ini_getl("focus", "port", mDevicePort, iniFile);

	// Read adapter.ini to get additional macro variables and
	// PMC registers
	char name[100] = {0};
	const static char *sDigits = "0123456789";

	mMacroMin = 99999;
	mMacroMax = 0;

	// First look for macro variables
	for (int idx = 0;
		ini_getkey("macros", idx, name, countof(name), iniFile) > 0 && idx < MAX_MACROS;
		idx++)
	{
		char numbers[256] = {0};
		ini_gets("macros", name, "", numbers, countof(numbers), iniFile);
		if (numbers[0] == '[')
		{
			// We have a path macro.
			int x(0), y(0), z(0);
			char *cp = numbers + 1, *n;
			x = strtol(cp, &n, 10);
			if (cp == n)
				continue;
			cp = n;
			y = strtol(cp, &n, 10);
			if (cp == n)
				continue;
			cp = n;
			z = strtol(cp, &n, 10);
			if (cp == n)
				continue;

			auto i = mMacroPathCount++;
			mMacroPath[i] = new MacroPathPosition(name, x, y, z);
			addDatum(*mMacroPath[i]);

			std::cout << "Adding path macro '" << name << "' at location"
				<< x << " " << y << " " << z << std::endl;

			if (x > mMacroMax)
				mMacroMax = x;
			if (x < mMacroMin)
				mMacroMin = x;
			if (y > mMacroMax)
				mMacroMax = y;
			if (y < mMacroMin)
				mMacroMin = y;
			if (z > mMacroMax)
				mMacroMax = z;
			if (z < mMacroMin)
				mMacroMin = z;
		}
		else
		{
			char *cp = numbers, *n(nullptr);
			auto v = strtol(cp, &n, 10);
			if (cp == n)
				continue;
			auto i = mMacroSampleCount++;
			mMacroSample[i] = new MacroSample(name, v);
			addDatum(*mMacroSample[i]);

			std::cout << "Adding sample macro '" << name << "' at location " << v << std::endl;

			if (v > mMacroMax)
				mMacroMax = v;
			if (v < mMacroMin)
				mMacroMin = v;
		}
	}

	auto pmcIdx = 0;
	for (pmcIdx = 0;
		ini_getkey("pmc", pmcIdx, name, countof(name), iniFile) > 0 && pmcIdx < MAX_PMC;
		pmcIdx++)
	{
		auto v = ini_getl("pmc", name, 0, iniFile);
		mPMCVariable[pmcIdx] = new IntEvent(name);
		mPMCAddress[pmcIdx] = v;

		addDatum(*mPMCVariable[pmcIdx]);

		std::cout << "Adding pmc '" << name << "' at location " << v << std::endl;
	}

	mPMCCount = pmcIdx;
}


void FanucAdapter::configure()
{
	if (mConfigured || !mConnected)
		return;

	gLogger->info("Configuring...\n");

	short path(0);
	auto ret = cnc_getpath(mFlibhndl, &path, &mMaxPath);
	if (ret != EW_OK)
	{
		gLogger->error("Cannot find number of paths: %d", ret);
		mMaxPath = 1;
		path = 1;
	}

	for (auto pathNum = 1; pathNum <= mMaxPath; pathNum++)
	{
		auto fanucPath = new FanucPath(this, pathNum);
		if (!fanucPath->configure(mFlibhndl))
		{
			gLogger->error("Could not configure path: %d", pathNum);
			exit(1);
		}

		mPaths.push_back(fanucPath);
	}

	gLogger->info("Current path: %d, maximum paths: %d", path, mMaxPath);

	mConfigured = true;
}


void FanucAdapter::connect()
{
	if (mConnected)
		return;

	std::cout << "Connecting to Machine at " << mDeviceIP << " and port " << mDevicePort << std::endl;

	// If the device IP has been passed in the form HSSB_{1-9} then we should
	// connect via HSSB
	std::regex hssbRegEx("HSSB_([0-9])");
	std::cmatch matches;
	short ret = 0;
	if(std::regex_search(mDeviceIP, matches, hssbRegEx))
	{
		auto res = matches[1].str();
		int hssbAddress = std::strtol(res.c_str(), nullptr, 0);
		ret = ::cnc_allclibhndl2(hssbAddress, &mFlibhndl);
	}
	else
	{
		ret = ::cnc_allclibhndl3(mDeviceIP, mDevicePort, 10, &mFlibhndl);
	}

	std::cout << "Result: " << ret <<std::endl;
	if (ret == EW_OK)
	{
		mAvail.available();
		mConnected = true;
		if (!mConfigured)
			configure();

		// Resets conditions to normal.
		initializeDeviceDatum();
	}
	else
	{
		mConnected = false;
		unavailable();
		std::this_thread::sleep_for(std::chrono::seconds{5});
	}
}


void FanucAdapter::reconnect()
{
	if (mConnected)
	{
		cnc_freelibhndl(mFlibhndl);
		mConnected = false;
		connect();
	}
}


void FanucAdapter::getMacros()
{
	if (!mConnected)
		return;

	if (mMacroSampleCount == 0 && mMacroPathCount == 0)
		return;

	// For now we assume they are close in range. If this proves to not
	// be true, we will have to get more creative.
	auto macros = new IODBMR[mMacroMax - mMacroMin];
	short ret = cnc_rdmacror(mFlibhndl,
		mMacroMin,
		mMacroMax,
		sizeof(IODBMR) * (mMacroMax - mMacroMin + 1),
		macros);

	if (ret == EW_OK)
	{
		for (auto i = 0; i < mMacroSampleCount; i++)
		{
			auto off = mMacroSample[i]->getNumber() - mMacroMin;
			if (macros->data[off].mcr_val != 0 || macros->data[off].dec_val != -1)
			{
				mMacroSample[i]->setValue(((double) macros->data[off].mcr_val) /
								pow(10.0, macros->data[off].dec_val));
			}
			else
			{
				mMacroSample[i]->unavailable();
			}
		}
		for (auto i = 0; i < mMacroPathCount; i++)
		{
			auto x = mMacroPath[i]->getX() - mMacroMin;
			auto y = mMacroPath[i]->getY() - mMacroMin;
			auto z = mMacroPath[i]->getZ() - mMacroMin;

			if ((macros->data[x].mcr_val != 0 || macros->data[x].dec_val != -1) &&
				(macros->data[y].mcr_val != 0 || macros->data[y].dec_val != -1) &&
				(macros->data[z].mcr_val != 0 || macros->data[z].dec_val != -1))
			{
				mMacroPath[i]->setValue(((double) macros->data[x].mcr_val) /
											pow(10.0, macros->data[x].dec_val),
										((double) macros->data[y].mcr_val) /
											pow(10.0, macros->data[y].dec_val),
										((double) macros->data[z].mcr_val) /
											pow(10.0, macros->data[z].dec_val));
			}
			else
			{
				mMacroSample[i]->unavailable();
			}
		}
	}
	else
		std::cout << "Could not read macro variables: " << ret << std::endl;

	delete[] macros;
}


void FanucAdapter::getPMC()
{
	if (!mConnected)
		return;

	for (int i = 0; i < mPMCCount; i++)
	{
		IODBPMC buf;
		auto ret = pmc_rdpmcrng(
			mFlibhndl,
			0, // G
			0, // byte
			mPMCAddress[i],
			mPMCAddress[i],
			8 + 1,
			&buf);
		if (ret == EW_OK)
		{
			if (buf.u.cdata[0] < 0)
				mPMCVariable[i]->setValue(-buf.u.cdata[0] - 1);
			else
				mPMCVariable[i]->setValue(buf.u.cdata[0]);
		}
		else
		{
			std::cout << "Could not retrieve PMC data at " << mPMCAddress[i]
				<< " for " << mPMCVariable[i]->getName()
				<< ": " << ret
				<< std::endl;
		}
	}
}


void FanucAdapter::getMessages()
{
	if (!mConnected)
		return;

	OPMSG messages[6];
	auto ret = cnc_rdopmsg(mFlibhndl, 0, 6 + 256, messages);
	if (ret == EW_OK && messages->datano != -1)
	{
		char buf[32] = {0};
		sprintf(buf, "%04d", messages->datano);
		mMessage.setValue(messages->data, buf);
	}
}


void FanucAdapter::getCounts()
{
	if (!mConnected)
		return;

	// Should just be a parameter read
	IODBPSD buf;
	auto ret = cnc_rdparam(mFlibhndl, 6711, 0, 8, &buf);
	if (ret == EW_OK)
		mPartCount.setValue(buf.u.ldata);
}


void FanucAdapter::getPathData()
{
	if (!mConnected)
		return;

	int i;
	for (i = mMaxPath - 1; i >= 0; i--)
	{
		if (!mPaths[i]->gatherData(mFlibhndl))
		{
			disconnect();
			break;
		}
	}
	if (mConnected && i > 0)
		cnc_setpath(mFlibhndl, 0);
}
