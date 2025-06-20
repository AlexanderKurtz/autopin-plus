/*
 * This file is part of Autopin+.
 *
 * Copyright (C) 2014 Alexander Kurtz <alexander@kurtz.be>

 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <AutopinPlus/AutopinContext.h>		  // for AutopinContext, etc
#include <AutopinPlus/Configuration.h>		  // for Configuration, etc
#include <AutopinPlus/Monitor/GPerf/Sensor.h> // for Sensor
#include <AutopinPlus/PerformanceMonitor.h>   // for PerformanceMonitor
#include <AutopinPlus/ProcessTree.h>		  // for ProcessTree, etc
#include <qatomic_x86_64.h>					  // for QBasicAtomicInt::deref
#include <qglobal.h>						  // for qFree
#include <qlist.h>							  // for QList
#include <qmap.h>							  // for QMap
#include <qstring.h>						  // for QString
#include <sys/types.h>						  // for pid_t

namespace AutopinPlus {
namespace Monitor {
namespace GPerf {

/*!
 * \brief A generic performance monitor based on the perf subsystem of the Linux kernel.
 */
class Main : public PerformanceMonitor {
  public:
	/*!
	 * \brief Constructor
	 *
	 * \param[in] name    Name of this monitor
	 * \param[in] config  Pointer to the configuration
	 * \param[in] context Pointer to the context
	 */
	Main(QString name, Configuration *config, const AutopinContext &context);

	// Overridden from the base class
	void init() override;

	// Overridden from the base class
	Configuration::configopts getConfigOpts() override;

	// Overridden from the base class
	void start(int tid) override;

	// Overridden from the base class
	double value(int tid) override;

	// Overridden from the base class
	double stop(int tid) override;

	// Overridden from the base class
	void clear(int tid) override;

	// Overridden from the base class
	ProcessTree::autopin_tid_list getMonitoredTasks() override;

	// Overridden from the base class
	QString getUnit() override;

  private:
	/*!
	 * \brief Parses a string to a sensor.
	 *
	 * This function parses the specified string to a sensor. If this fails, an exception is thrown.
	 *
	 * \param[in] string The string to be parsed.
	 *
	 * \exception Exception This exception will be thrown if the string could not be parsed.
	 *
	 * \return The parsed sensor.
	 */
	Sensor readSensor(const QString &input);

	/*!
	 * \brief Converts a sensor into a string.
	 *
	 * This function converts the supplied sensor into a string.
	 *
	 * \param[in] type The sensor to be converted.
	 *
	 * \return A string representing the supplied sensor.
	 */
	QString showSensor(const Sensor &input);

	/*!
	 * \brief A wrapper around the "perf_event_open()" syscall.
	 *
	 * \param[in] attr     The "perf_event_attr" describing the desired sensor.
	 * \param[in] pid      The process or thread to be monitored. Pass -1 here to monitor all threads.
	 * \param[in] cpu      The CPU to be monitored. Pass -1 to monitor all CPUs.
	 * \param[in] group_fd The file descriptor to which the returned file descriptor should be assigned as a slave. Pass
	 *                     -1 here to create a new group master.
	 * \param[in] flags    An optional list of flags. See the documentation of the "perf_event_open" syscall for
	 *                     details.
	 *
	 * \return The opened file descriptor or -1 if there was an error (in which case errno will be set appropriatly).
	 */
	int perf_event_open(struct perf_event_attr *attr, pid_t pid, int cpu, int group_fd, unsigned long flags);

	/*!
	 * The list of processors to monitor, as configured by the user.
	 */
	QList<int> processors;

	/*!
	 * The perf sensor to use.
	 */
	Sensor sensor;

	/*!
	 * A mapping from a specific thread to a list of associated file descriptors.
	 */
	QMap<int, QList<int>> threads;
}; // class Main

} // namespace GPerf
} // namespace Monitor
} // namespace AutopinPlus
