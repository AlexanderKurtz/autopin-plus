/*
 * This file is part of Autopin+.
 * Copyright (C) 2015 Technische Universität München - LRR
 *
 * This file is licensed under the GNU General Public License Version 3
 */

#include <AutopinPlus/Logger/External/Main.h>

#include <AutopinPlus/AutopinContext.h>			 // for AutopinContext
#include <AutopinPlus/Configuration.h>			 // for Configuration, etc
#include <AutopinPlus/DataLogger.h>				 // for DataLogger
#include <AutopinPlus/Error.h>					 // for Error, Error::::BAD_CONFIG
#include <AutopinPlus/Exception.h>				 // for Exception
#include <AutopinPlus/Logger/External/Process.h> // for Process
#include <AutopinPlus/PerformanceMonitor.h>		 // for PerformanceMonitor, etc
#include <AutopinPlus/Tools.h>					 // for Tools
#include <qelapsedtimer.h>						 // for QElapsedTimer
#include <qmutex.h>								 // for QMutex
#include <qstring.h>							 // for operator+, QString
#include <qstringlist.h>						 // for QStringList
#include <qtextstream.h>						 // for QTextStream, operator<<, etc
#include <qtimer.h>								 // for QTimer

namespace AutopinPlus {
namespace Logger {
namespace External {

Main::Main(const Configuration &config, PerformanceMonitor::monitor_list const &monitors, AutopinContext &context)
	: DataLogger(config, monitors, context) {
	name = "external";
}

void Main::init() {
	context.info("Initializing " + name);

	// Read and parse the "command" option.
	if (config.configOptionExists(name + ".command") > 0) {
		command = config.getConfigOptionList(name + ".command");

		context.info("  - " + name + ".command = " + command.join(" "));
	}

	// Read and parse the "interval" option.
	if (config.configOptionExists(name + ".interval") > 0) {
		try {
			interval = Tools::readInt(config.getConfigOption(name + ".interval"));
			context.info("  - " + name + ".interval = " + QString::number(interval));
		} catch (const Exception &e) {
			context.report(Error::BAD_CONFIG, "option_format",
						   name + ".init() failed: Could not parse the 'interval' option.");
			return;
		}
	}

	// Read and parse the "systemwide" option.
	if (config.configOptionExists(name + ".systemwide") > 0) {
		systemwide = config.getConfigOptionBool(name + ".systemwide");
		context.info("  - " + name + ".systemwide = " + (systemwide ? "true" : "false"));
	}

	// Connect stdout and stderr of the the external data logger to our callbacks...
	connect(&process, SIGNAL(readyReadStandardOutput()), this, SLOT(slot_readyReadStandardOutput()));
	connect(&process, SIGNAL(readyReadStandardError()), this, SLOT(slot_readyReadStandardError()));

	// ... and start it.
	process.start(command.first(), command.mid(1));

	// Wait for the data logger to be started before continuing. If startup fails, error out.
	if (!process.waitForStarted(-1)) {
		context.report(Error::BAD_CONFIG, "option_format",
					   name + ".init() failed: Could not start the specified command.");
		return;
	}

	// Start the timer which counts the time since the program was started.
	running.start();

	// Setup the timer which will periodically query the performance monitors and forward the data...
	connect(&timer, SIGNAL(timeout()), this, SLOT(slot_logDataPoint()));
	timer.setInterval(interval);

	// ... and start it.
	timer.start();
}

Configuration::configopts Main::getConfigOpts() const {
	Configuration::configopts result;

	result.push_back(Configuration::configopt("command", command));
	result.push_back(Configuration::configopt("interval", QStringList(QString::number(interval))));
	result.push_back(Configuration::configopt("interval", QStringList(systemwide ? "true" : "false")));

	return result;
}

void Main::slot_logDataPoint() {
	// If another thread is already here, just abort and don't do anything. This is also important in a single-threaded
	// program, as some performance monitors might hand control back to the event loop within their value() function
	// which leads to a race condition between our own timer and whatever it is that the performance monitor is waiting
	// for.
	if (!mutex.tryLock()) {
		return;
	}

	// Emit data points for all monitors and threads.
	for (auto i = monitors.begin(); i != monitors.end(); i++) {
		for (auto task : (*i)->getMonitoredTasks()) {
			QTextStream(&process) << (*i)->getName() << "	" << task << "	" << fixed << running.elapsed() / 1000.0
								  << "	" << fixed << (*i)->value(task) << "	"
								  << ((*i)->getUnit().isEmpty() ? "none" : (*i)->getUnit()) << endl;

			// If the user told us, that the performance monitors are system-wide, stop after the first thread.
			if (systemwide) {
				break;
			}
		}
	}

	// Release the lock, we are done here.
	mutex.unlock();
}

void Main::slot_readyReadStandardError() {
	QTextStream stream(process.readAllStandardError());

	// Log all lines written by the process to stderr.
	while (!stream.atEnd()) {
		context.info("[stderr] " + stream.readLine());
	}
}

void Main::slot_readyReadStandardOutput() {
	QTextStream stream(process.readAllStandardOutput());

	// Log all lines written by the process to stdout.
	while (!stream.atEnd()) {
		context.info("[stdout] " + stream.readLine());
	}
}

} // namespace External
} // namespace Logger
} // namespace AutopinPlus
