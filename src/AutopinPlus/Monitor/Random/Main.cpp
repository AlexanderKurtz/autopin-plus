/*
 * Autopin+ - Automatic thread-to-core-pinning tool
 * Copyright (C) 2012 LRR
 *
 * Author:
 * Florian Walter
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Contact address:
 * LRR (I10)
 * Technische Universitaet Muenchen
 * Boltzmannstr. 3
 * D-85784 Garching b. Muenchen
 * http://autopin.in.tum.de
 */

#include <AutopinPlus/Monitor/Random/Main.h>

#include <QTime>
#include <stdlib.h>

namespace AutopinPlus {
namespace Monitor {
namespace Random {

Main::Main(QString name, Configuration *config, const AutopinContext &context)
	: PerformanceMonitor(name, config, context) {
	type = "random";
}

void Main::init() {
	context.enableIndentation();
	context.info("  :: Initializing \"" + name + "\" (random)");

	// Set random seed
	qsrand(QTime::currentTime().msec());

	// Set standard values
	rand_min = 0;
	rand_max = 1;
	valtype = MAX;
	QString valtype_str = "MAX";

	if (config->configOptionExists(name + ".rand_min") == 1)
		rand_min = config->getConfigOptionDouble(name + ".rand_min");
	if (config->configOptionExists(name + ".rand_max") == 1)
		rand_max = config->getConfigOptionDouble(name + ".rand_max");
	if (config->configOptionExists(name + ".valtype") == 1) {
		if (config->getConfigOption(name + ".valtype") == "max") {
			valtype = MAX;
			valtype_str = "MAX";
		} else if (config->getConfigOption(name + ".valtype") == "min") {
			valtype = MIN;
			valtype_str = "MIN";
		} else {
			valtype = UNKNOWN;
			valtype_str = "UNKNOWN";
		}
	}

	context.info("     - Minimum random value " + QString::number(rand_min));
	context.info("     - Maximum random value " + QString::number(rand_max));

	context.disableIndentation();
}

Configuration::configopts Main::getConfigOpts() {
	Configuration::configopts result;

	result.push_back(Configuration::configopt("rand_min", QStringList(QString::number(rand_min))));
	result.push_back(Configuration::configopt("rand_max", QStringList(QString::number(rand_max))));

	switch (valtype) {
	case MAX:
		result.push_back(Configuration::configopt("valtype", QStringList("MAX")));
		break;
	case MIN:
		result.push_back(Configuration::configopt("valtype", QStringList("MIN")));
		break;
	default:
		result.push_back(Configuration::configopt("valtype", QStringList("UNKNOWN")));
		break;
	}

	return result;
}

void Main::start(int tid) { rands[tid] = getRandomValue(); }

double Main::value(int tid) {
	if (rands.find(tid) != rands.end()) return rands[tid];

	REPORT(Error::MONITOR, "value", "Could not random result for " + QString::number(tid), 0);

	return 0;
}

double Main::stop(int tid) {
	double result;
	CHECK_ERROR(result = value(tid), 0);
	rands.erase(tid);
	return result;
}

void Main::clear(int tid) {
	if (rands.find(tid) != rands.end()) rands.erase(tid);
}

ProcessTree::autopin_tid_list Main::getMonitoredTasks() {
	ProcessTree::autopin_tid_list result;

	for (auto &elem : rands) result.insert(elem.first);

	return result;
}

double Main::getRandomValue() {
	double result = qrand();

	// Adapt result to the requested range
	result = ((rand_max - rand_min) / RAND_MAX) * result + rand_min;

	return result;
}

} // namespace Random
} // namespace Monitor
} // namespace AutopinPlus
