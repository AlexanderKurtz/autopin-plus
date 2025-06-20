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

#include <AutopinPlus/ObservedProcess.h>

#include <AutopinPlus/OSServices.h>
#include <deque>
#include <QFileInfo>
#include <QString>
#include <stdio.h>
#include <sys/wait.h>

namespace AutopinPlus {

ObservedProcess::ObservedProcess(Configuration *config, OSServices *service, const AutopinContext &context)
	: config(config), service(service), context(context), pid(-1), cmd(""), trace(false), running(false), phase(0),
	  comm_addr(""), comm_timeout(60) {
	integer = QRegExp("\\d+");
}

ObservedProcess::~ObservedProcess() { deinit(); }

void ObservedProcess::init() {
	context.enableIndentation();

	context.info("> Initializing the observed process");

	trace = config->getConfigOptionBool("Trace");

	if (config->configOptionExists("CommChan") == 1) {
		if (config->configOptionBool("CommChan")) {
			if (config->getConfigOptionBool("CommChan")) CHECK_ERRORV(comm_addr = service->getCommDefaultAddr());
		} else
			comm_addr = config->getConfigOption("CommChan");
	} else if (config->configOptionExists("CommChan") > 1) {
		REPORTV(Error::BAD_CONFIG, "inconsistent", "CommChan accepts only one value");
	}

	if (comm_addr != "" && config->getConfigOptionInt("CommChanTimeout") > 0)
		comm_timeout = config->getConfigOptionInt("CommChanTimeout");

	bool config_found = false;

	if (config->configOptionExists("Attach") > 0) {
		config_found = true;
		exec = false;

		QString attach_expr = config->getConfigOption("Attach");

		if (integer.exactMatch(attach_expr))
			pid = attach_expr.toInt();
		else {
			ProcessTree::autopin_tid_list procs;
			CHECK_ERRORV(procs = service->getPid(attach_expr));

			if (procs.size() == 0)
				REPORTV(Error::PROCESS, "not_found", "No process found with name " + attach_expr);
			else if (procs.size() > 1)
				REPORTV(Error::PROCESS, "found_many", "There exist more processes with name " + attach_expr);
			else {
				pid = *(procs.begin());
				cmd = service->getCmd(pid);
			}
		}
	}

	if (config_found == false && config->configOptionExists("Exec") > 0) {
		config_found = true;
		exec = true;
		cmd = config->getConfigOptionList("Exec").join(" ");
	}

	if (!config_found) REPORTV(Error::BAD_CONFIG, "option_missing", "No process configured");

	context.disableIndentation();
}

void ObservedProcess::deinit() {
	service->detachFromProcess();
	service->deinitCommChannel();
}

void ObservedProcess::start() {
	context.enableIndentation();

	if (running) REPORTV(Error::PROCESS, "already_connected", "The observed process is already running");

	// Initialize the communication channel
	if (comm_addr != "") {
		context.info("> Initializing the communication channel");
		CHECK_ERRORV(service->initCommChannel(this));
	}

	// Start the process
	if (pid == -1) {
		context.info("> Starting new process " + cmd);
		CHECK_ERRORV(pid = service->createProcess(cmd, trace));
	}

	running = true;

	// Setup process tracing if desired by the user
	if (trace) {
		context.info("> Attaching to process " + QString::number(pid));
		CHECK_ERRORV(service->attachToProcess(this));
	}

	if (comm_addr != "") {
		context.info("> Waiting for process " + QString::number(pid) + " to connect (Timeout: " +
					 QString::number(comm_timeout) + " sec)");
		CHECK_ERRORV(service->connectCommChannel(60));
		context.info("> The connection with the observed process has been established");
	}

	// We have either just created a new process or attached to an existing one. Either way, we should really
	// tell all interested parties to update their internal list of monitored threads now.
	emit sig_TaskCreated(pid);

	context.disableIndentation();
}

int ObservedProcess::getPid() { return pid; }

bool ObservedProcess::getExec() { return exec; }

int ObservedProcess::getExecutionPhase() { return phase; }

QString ObservedProcess::getCmd() { return cmd; }

QString ObservedProcess::getCommChanAddr() { return comm_addr; }

void ObservedProcess::setPhaseNotificationInterval(int interval) {
	if (comm_addr == "" || interval < 0) return;

	CHECK_ERRORV(service->sendMsg(APP_INTERVAL, interval, 0));
}

int ObservedProcess::getCommTimeout() { return comm_timeout; }

bool ObservedProcess::getTrace() { return trace; }

bool ObservedProcess::isRunning() { return running; }

ProcessTree ObservedProcess::getProcessTree() {
	if (!isRunning()) return ProcessTree::empty;

	ProcessTree::autopin_tid_list tasks;

	ProcessTree::autopin_tid_list child_procs;

	std::deque<std::pair<int, int>> worklist;

	ProcessTree result(pid);

	CHECK_ERROR(tasks = service->getProcessThreads(pid), ProcessTree::empty);
	result.addProcessTaskList(pid, tasks);

	CHECK_ERROR(child_procs = service->getChildProcesses(pid), ProcessTree::empty);

	for (const auto &child_proc : child_procs) worklist.push_back(std::pair<int, int>(pid, child_proc));

	while (!worklist.empty()) {
		std::pair<int, int> work_item = worklist.front();
		worklist.pop_front();

		result.addChildProcess(work_item.first, work_item.second);
		CHECK_ERROR(tasks = service->getProcessThreads(work_item.second), ProcessTree::empty);
		result.addProcessTaskList(work_item.second, tasks);

		CHECK_ERROR(child_procs = service->getChildProcesses(work_item.second), ProcessTree::empty);

		for (const auto &child_proc : child_procs)
			worklist.push_back(std::pair<int, int>(work_item.second, child_proc));
	}

	return result;
}

void ObservedProcess::slot_TaskTerminated(int tid) {
	if (tid == pid) {
		pid = -1;
		REPORTV(Error::PROCESS, "terminated", "ObservedProcess has terminated");
	} else {
		context.info(":: Task terminated: " + QString::number(tid));
		emit sig_TaskTerminated(tid);
	}
}

void ObservedProcess::slot_TaskCreated(int tid) {
	context.info(":: Task created: " + QString::number(tid));
	emit sig_TaskCreated(tid);
}

void ObservedProcess::slot_CommChannel(autopin_msg msg) {
	switch (msg.event_id) {
	case APP_NEW_PHASE:
		context.info(":: New execution phase: " + QString::number(msg.arg));
		phase = msg.arg;
		emit sig_PhaseChanged(msg.arg);
		break;
	case APP_USER:
		context.info(":: Received user-defined message");
		emit sig_UserMessage(msg.arg, msg.val);
		break;
	default:
		context.info(":: Received unkown message type");
		break;
	}
}

} // namespace AutopinPlus
