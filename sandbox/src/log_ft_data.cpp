/*
 * log_ft_data.cpp
 *
 *  Created on: Apr 26, 2010
 *      Author: dc
 */

#include <iostream>
#include <string>
#include <cstdio>
#include <cstdlib>

#include <sys/mman.h>
#include <signal.h>
#include <unistd.h>

#include <boost/thread.hpp>
#include <boost/tuple/tuple.hpp>

#include <native/task.h>
#include <native/timer.h>
#include <libconfig.h++>

#include <barrett/bus/bus.h>
#include <barrett/bus/bus_can.h>
#include <barrett/log.h>
#include <barrett/detail/stacktrace.h>


void waitForEnter() {
	static std::string line;
	std::getline(std::cin, line);
}

void warnOnSwitchToSecondaryMode(int)
{
	syslog(LOG_ERR, "WARNING: Switched out of RealTime. Stack-trace:");
	syslog_stacktrace();
	std::cerr << "WARNING: Switched out of RealTime. Stack-trace in syslog.\n";
}

RTIME secondsToRTIME(double s) {
	return static_cast<RTIME>(s * 1e9);
}

void canThread();

using namespace barrett;

double T_s = 0.002;
const int ID = 8;
const RTIME FLIGHT_TIME = 75000;

bool going = true;
bool fileMode = false;
int windowSize, numSamples, numSets = 0;
math::Vector<6>::type sum;
struct bt_bus* bus;

typedef boost::tuple<double, math::Vector<6>::type> tuple_type;
log::RealTimeWriter<tuple_type>* lw;


int main(int argc, char** argv) {
	char* outFile = NULL;

	if (argc == 4) {
		T_s = std::atof(argv[3]);
	} else if (argc != 3) {
		printf("Usage: %s {-f <fileName> | -a <windowSize>} [<samplePeriodInSeconds>]\n", argv[0]);
		printf("    -f <fileName>    Log data to a file\n");
		printf("    -a <windowSize>  Print statistics on segments of data\n");
		return 0;
	}

	printf("Sample period: %fs\n", T_s);
	if (strcmp(argv[1], "-f") == 0) {
		fileMode = true;
		outFile = argv[2];
		printf("Output file: %s\n", outFile);
	} else if (strcmp(argv[1], "-a") == 0) {
		fileMode = false;
		windowSize = atoi(argv[2]);
		numSamples = windowSize;
		printf("Window size: %d\n", windowSize);
	}
	printf("\n");


	mlockall(MCL_CURRENT|MCL_FUTURE);
	signal(SIGXCPU, &warnOnSwitchToSecondaryMode);

	libconfig::Config config;
	config.readFile("/etc/wam/wam4-new.config");

	if (bt_bus_create(&bus, config.lookup("wam.low_level.bus").getCSetting(), bt_bus_UPDATE_POS_DIFF)) {
		printf("Couldn't create bus.\n");
		return 1;
	}

	if (fileMode) {
		printf(">>> Press [Enter] to start collecting data.\n");
		waitForEnter();

		char tmpFile[L_tmpnam];
		if (std::tmpnam(tmpFile) == NULL) {
			printf("Couldn't create temporary file!\n");
			return 1;
		}
		lw = new log::RealTimeWriter<tuple_type>(tmpFile, T_s);
		boost::thread t(canThread);


		printf(">>> Press [Enter] to stop collecting data.\n");
		waitForEnter();

		going = false;
		t.join();
		delete lw;

		log::Reader<tuple_type> lr(tmpFile);
		lr.exportCSV(outFile);

		std::remove(tmpFile);
	} else {
		boost::thread t(canThread);

		printf(">>> Press [Enter] to start a sample.\n");
		while (true) {
			waitForEnter();
			numSamples = 0;
			while (numSamples != windowSize) {
				usleep(100000);
			}

			sum /= windowSize;
			printf("%d,%f,%f,%f,%f,%f,%f", ++numSets, sum[0], sum[1], sum[2], sum[3], sum[4], sum[5]);
		}
	}

	return 0;
}


void canThread() {
	int id, property;
	long value;
	tuple_type t;

	rt_task_shadow(new RT_TASK, NULL, 10, 0);
	rt_task_set_mode(0, T_PRIMARY | T_WARNSW, NULL);
	rt_task_set_periodic(NULL, TM_NOW, secondsToRTIME(T_s));

	RTIME t_0 = rt_timer_read();
	RTIME t_1 = t_0;

	while (going) {
		bt_bus_can_async_get_property(bus->dev, ID, 34);
		rt_timer_spin(FLIGHT_TIME);
		bt_bus_can_async_get_property(bus->dev, ID, 35);
		rt_timer_spin(FLIGHT_TIME);
		bt_bus_can_async_get_property(bus->dev, ID, 36);
		rt_timer_spin(FLIGHT_TIME);
		bt_bus_can_async_get_property(bus->dev, ID, 37);
		rt_timer_spin(FLIGHT_TIME);
		bt_bus_can_async_get_property(bus->dev, ID, 38);
		rt_timer_spin(FLIGHT_TIME);
		bt_bus_can_async_get_property(bus->dev, ID, 39);

		rt_task_wait_period(NULL);

		t_0 = rt_timer_read();
		boost::get<0>(t) = ((double) t_0 - t_1) * 1e-9;
		t_1 = t_0;

		for (int i = 0; i < 6; ++i) {
			bt_bus_can_async_read(bus->dev, &id, &property, &value, NULL, 1, 1);
			boost::get<1>(t)[property-34] = value;
		}

		if (fileMode) {
			lw->putRecord(t);
		} else {
			if (numSamples == 0) {
				sum.setConstant(0.0);
			}
			if (numSamples < windowSize) {
				sum += boost::get<1>(t);
				++numSamples;
			}
		}
	}

	rt_task_set_mode(T_WARNSW, 0, NULL);
}
