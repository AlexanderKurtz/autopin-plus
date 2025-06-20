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

#include <AutopinPlus/OS/Linux/OSServicesLinux.h>

#include <AutopinPlus/ObservedProcess.h>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMutexLocker>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <sched.h>
#include <stdlib.h>
#include <sys/ptrace.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

/*!
 * \brief Maximum length for paths of UNIX domain sockets
 *
 * Taken from sys/un.h
 */
#define UNIX_PATH_MAX 108

namespace AutopinPlus {
namespace OS {
namespace Linux {

OSServicesLinux::OSServicesLinux(const AutopinContext &context)
	: OSServices(context), tracer(context), comm_notifier(nullptr), server_socket(-1), client_socket(-1) {
	integer = QRegExp("\\d+");

	connect(&tracer, SIGNAL(sig_TaskCreated(int)), this, SIGNAL(sig_TaskCreated(int)));
	connect(&tracer, SIGNAL(sig_TaskTerminated(int)), this, SIGNAL(sig_TaskTerminated(int)));
}

OSServicesLinux::~OSServicesLinux() {
	detachFromProcess();
	deinitCommChannel();

	if (tracer.isRunning()) tracer.terminate();

	current_service = nullptr;

	if (comm_notifier != nullptr) delete comm_notifier;
}

int OSServicesLinux::sigchldFd[2];

bool OSServicesLinux::autopin_attached = false;

void OSServicesLinux::init() {
	context.enableIndentation();

	int ret = 0;

	if (current_service != nullptr)
		REPORTV(Error::SYSTEM, "already_initialized", "Another OS service instance is already initialized!");

	context.info("> Initializing OS services");

	// Setting up signal handling for SIGCHLD
	// Create socket
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sigchldFd) != 0)
		REPORTV(Error::SYSTEM, "create_socket", "Cannot create socket");

	snChld = new QSocketNotifier(sigchldFd[1], QSocketNotifier::Read, this);
	connect(snChld, SIGNAL(activated(int)), this, SLOT(slot_handleSigChld()));

	// Setup signal handler
	struct sigaction chld;
	memset(&chld, 0, sizeof(struct sigaction));
	chld.sa_sigaction = chldSignalHandler;
	ret |= sigemptyset(&chld.sa_mask);
	chld.sa_flags = 0;
	chld.sa_flags |= SA_RESTART | SA_SIGINFO | SA_NOCLDSTOP;
	ret |= sigaction(SIGCHLD, &chld, &old_chld);
	if (ret != 0) REPORTV(Error::SYSTEM, "sigset", "Cannot setup signal handling");

	current_service = this;
	context.disableIndentation();
}

QString OSServicesLinux::getHostname() { return getHostname_static(); }

QString OSServicesLinux::getCommDefaultAddr() {
	QString result;
	const char *homedir = getenv("HOME");

	if (homedir == nullptr) REPORT(Error::COMM, "default_addr", "Cannot determine the user's home directory", result);

	result = homedir;
	result += "/.autopin_socket";

	return result;
}

QString OSServicesLinux::getHostname_static() {
	QString qhostname;

	char hostname[30];
	gethostname(hostname, 30);
	qhostname = hostname;

	return qhostname;
}

int OSServicesLinux::createProcess(QString cmd, bool wait) {
	pid_t pid;

	struct sigaction old_usr;

	// install handler for signal SIGUSR1
	if (wait) {
		int ret = 0;
		struct sigaction usr;
		memset(&usr, 0, sizeof(struct sigaction));
		usr.sa_handler = usrSignalHandler;
		usr.sa_flags = SA_RESTART;
		ret |= sigemptyset(&usr.sa_mask);
		ret |= sigaction(SIGUSR1, &usr, &old_usr);
		if (ret != 0) REPORT(Error::SYSTEM, "sigset", "Cannot setup signal handling", -1);
	}

	pid = fork();

	if (pid == 0) {
		QStringList args_str = getArguments(cmd);
		QString bin = args_str[0];
		char *args[args_str.size() + 1];

		// Get new pid
		pid = getpid();

		context.debug("Binary to start: " + bin);

		for (int i = 0; i < args_str.size(); i++) {
			int size = args_str[i].size() + 1;
			args[i] = (char *)malloc(size * sizeof(char));
			strcpy(args[i], args_str[i].toStdString().c_str());
		}

		args[args_str.size()] = nullptr;

		if (wait) {
			context.debug("Waiting for autopin to attach");

			while (!autopin_attached) {

				context.debug("Wait for signal from autopin+");
				usleep(1000);
			}

			context.debug("Autopin has attached!");
		}

		execvp(bin.toStdString().c_str(), args);

		context.report(Error::PROCESS, "create", "Could not create new process from binary " + bin);
		exit(-1);

	} else {
		// Restore original signal handler
		if (wait) {
			int ret;
			ret = sigaction(SIGUSR1, &old_usr, nullptr);
			if (ret != 0) REPORT(Error::SYSTEM, "sigset", "Cannot setup signal handling", -1);
		}

		return pid;
	}

	return -1;
}

void OSServicesLinux::attachToProcess(ObservedProcess *observed_process) {
	QMutexLocker locker(&attach);
	int ret = 0;

	if (tracer.isRunning()) {
		REPORTV(Error::PROC_TRACE, "in_use", "Process tracing is already running");
	}

	// Block the alarm signal so that it will always be delivered to the TraceThread
	sigset_t sigset;
	ret |= sigemptyset(&sigset);
	ret |= sigaddset(&sigset, SIGALRM);
	ret |= pthread_sigmask(SIG_BLOCK, &sigset, nullptr);
	if (ret != 0) REPORTV(Error::SYSTEM, "sigset", "Cannot setup signal handling");

	// Reset SIGCHLD handler
	ret = sigaction(SIGCHLD, &old_chld, nullptr);
	if (ret != 0) REPORTV(Error::SYSTEM, "sigset", "Cannot setup signal handling");

	context.debug("Starting TraceThread");
	QMutex *traceattach;
	CHECK_ERRORV(traceattach = tracer.init(observed_process));

	// Wait until the thread has attached to all tasks
	context.debug("OSServicesLinux is waiting until the thread has attached");
	traceattach->lock();
	traceattach->unlock();
}

void OSServicesLinux::detachFromProcess() {
	if (!tracer.isRunning()) return;
	context.enableIndentation();
	context.info("> Detaching from the observed process");
	tracer.deinit();
	context.disableIndentation();
}

void OSServicesLinux::initCommChannel(ObservedProcess *proc) {

	if (server_socket != -1)
		REPORTV(Error::COMM, "already_initialized", "The communication channel is already initialized");

	// Create a new server socket and bind it to a path
	server_socket = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (server_socket == -1) REPORTV(Error::COMM, "socket", "Could not create a new socket");

	// Get flags of the socket
	long socket_flags = fcntl(server_socket, F_GETFL);
	if (socket_flags == -1)
		REPORTVA(Error::COMM, "socket", "Cannot access socket properties", {
			close(server_socket);
			server_socket = -1;
		});
	socket_flags |= O_NONBLOCK;
	int result = fcntl(server_socket, F_SETFL, socket_flags);
	if (result == -1)
		REPORTVA(Error::COMM, "socket", "Cannot set socket properties", {
			close(server_socket);
			server_socket = -1;
		});

	// Create the socket path
	socket_path = proc->getCommChanAddr();
	if (socket_path.size() + 1 > UNIX_PATH_MAX)
		REPORTV(Error::COMM, "comm_target", "The path for the communication socket exceeds the maximum length");

	// Create the address structure for the socket
	struct sockaddr_un socket_addr;
	socket_addr.sun_family = AF_UNIX;
	strcpy(socket_addr.sun_path, socket_path.toStdString().c_str());

	// Bind the socket
	result = bind(server_socket, (struct sockaddr *)&socket_addr, sizeof(socket_addr));
	if (result == -1) {
		REPORTVA(Error::COMM, "socket", "Cannot bind the communication socket.", {
			close(server_socket);
			server_socket = -1;
		});
	}

	// Make the socket a listening socket
	result = listen(server_socket, 1);
	if (result == -1)
		REPORTVA(Error::COMM, "socket", "Cannot setup communication socket", {
			close(server_socket);
			remove(socket_path.toStdString().c_str());
			server_socket = -1;
		});
}

void OSServicesLinux::deinitCommChannel() {
	if (comm_notifier != nullptr) {
		delete comm_notifier;
		comm_notifier = nullptr;
	}
	if (client_socket != -1) close(client_socket);
	if (server_socket != -1) {
		close(server_socket);
		remove(socket_path.toStdString().c_str());
	}
}

void OSServicesLinux::connectCommChannel(int timeout) {
	int result;

	if (server_socket == -1) {
		REPORTV(Error::COMM, "not_initialized", "The communication channel is not initialized");
	} else if (client_socket != -1) {
		REPORTV(Error::COMM, "already_initialized", "The communication channel is already connected");
	}

	for (int i = 0; i < timeout; i++) {
		// Sleep for 1 second
		sleep(1);

		// Process new events (if available)
		CHECK_ERRORV(QCoreApplication::processEvents());

		client_socket = accept(server_socket, nullptr, nullptr);
		if (client_socket == -1 && errno != EAGAIN && errno != EWOULDBLOCK) {
			REPORTV(Error::COMM, "connect", "Error while connecting to the observed process");
		} else if (client_socket != -1) {
			// Set flags of the socket
			long socket_flags = fcntl(client_socket, F_GETFL);
			if (socket_flags == -1)
				REPORTVA(Error::COMM, "socket", "Cannot access socket properties", {
					close(client_socket);
					client_socket = -1;
				});
			socket_flags |= O_NONBLOCK;
			result = fcntl(client_socket, F_SETFL, socket_flags);
			if (result == -1)
				REPORTVA(Error::COMM, "socket", "Cannot set socket properties", {
					close(client_socket);
					client_socket = -1;
				});

			break;
		}
	}

	if (client_socket == -1) {
		REPORTV(Error::COMM, "connect", "Cannot connect to the observed process");
	} else {
		// Setup notifier
		comm_notifier = new QSocketNotifier(client_socket, QSocketNotifier::Read);

		// Connect the notifier to the handler
		connect(comm_notifier, SIGNAL(activated(int)), this, SLOT(slot_msgReceived(int)));

		// Enable notifier
		comm_notifier->setEnabled(true);

		// Send acknowlegement to the observed process
		struct autopin_msg ack;
		ack.event_id = APP_READY;

		result = send(client_socket, &ack, sizeof(ack), 0);
		if (result == -1) REPORTV(Error::COMM, "connect", "Cannot connect to the observed process");
	}
}

void OSServicesLinux::sendMsg(int event_id, int arg, double val) {

	if (client_socket == -1) REPORTV(Error::COMM, "not_initialized", "The communication channel is not active");

	int result;
	struct autopin_msg msg;
	msg.event_id = event_id;
	msg.arg = arg;
	msg.val = val;

	result = send(client_socket, &msg, sizeof(msg), 0);
	if (result == -1) REPORTV(Error::COMM, "send", "Cannot send data to the observed process");
}

void OSServicesLinux::slot_msgReceived(int socket) {
	comm_notifier->setEnabled(false);

	struct autopin_msg msg;

	int result = recv(client_socket, &msg, sizeof(msg), 0);

	while (result > 0) {
		emit sig_CommChannel(msg);
		result = recv(client_socket, &msg, sizeof(msg), 0);
	}

	comm_notifier->setEnabled(true);
}

ProcessTree::autopin_tid_list OSServicesLinux::getProcessThreads(int pid) {
	QMutexLocker locker(&mutex);

	ProcessTree::autopin_tid_list result;
	QString procpath;
	QStringList threads;
	QDir procdir;

	bool ret;

	// set path
	procpath = "/proc/" + QString::number(pid) + "/task";

	procdir.setSorting(QDir::Name);
	procdir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);

	// try to enter directory
	ret = procdir.cd(procpath);
	if (ret == false)
		REPORT(Error::SYSTEM, "get_threads", "Could not get threads of process " + QString::number(pid), result);

	// get a list of the threads in the directory
	threads = procdir.entryList();

	// convert the list of thread into a autopin_tid_list
	result = convertQStringList(threads);

	return result;
}

ProcessTree::autopin_tid_list OSServicesLinux::getChildProcesses(int pid) {
	QMutexLocker locker(&mutex);

	ProcessTree::autopin_tid_list result;
	QString procpath;
	QStringList procs;
	QDir procdir;
	bool ret = false;
	bool success = true;

	// set path
	procpath = "/proc";

	procdir.setSorting(QDir::Name);
	procdir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);

	// try to enter proc directory
	ret = procdir.cd(procpath);
	if (ret == false) REPORT(Error::SYSTEM, "access_proc", "Could not access the proc filesystem", result);

	procs = procdir.entryList();

	for (int i = 0; i < procs.size(); i++) {

		QString tmp_pid = procs.at(i);

		if (!integer.exactMatch(tmp_pid)) continue;

		QString ppid = getProcEntry(tmp_pid.toInt(), 3, false);
		if (ppid == "") {
			success = false;
			break;
		} else if (ppid.toInt() == pid)
			result.insert(tmp_pid.toInt());
	}

	if (!success)
		REPORT(Error::SYSTEM, "get_children", "Could not get all children of process " + QString::number(pid), result);

	return result;
}

int OSServicesLinux::getTaskSortId(int tid) {
	QMutexLocker locker(&mutex);

	int result = 0;
	QString str_result;

	CHECK_ERROR(str_result = getProcEntry(tid, 21), result);
	result = str_result.toInt();

	return result;
}

void OSServicesLinux::setAffinity(int tid, int cpu) {
	cpu_set_t cores;
	pid_t linux_tid = tid;
	int ret = 0;

	// Setup CPU mask
	CPU_ZERO(&cores);
	CPU_SET(cpu, &cores);

	// set affinity
	ret = sched_setaffinity(linux_tid, sizeof(cores), &cores);

	if (ret != 0)
		REPORTV(Error::SYSTEM, "set_affinity",
				"Could not pin thread " + QString::number(tid) + " to cpu " + QString::number(cpu));
}

ProcessTree::autopin_tid_list OSServicesLinux::getPid(QString proc) {
	QMutexLocker locker(&mutex);

	ProcessTree::autopin_tid_list result;
	QStringList procs;
	QDir procdir;
	bool ret;

	// set filters for proc directory
	procdir.setSorting(QDir::Name);
	procdir.setFilter(QDir::Dirs | QDir::NoDotAndDotDot);

	// try to enter proc directory
	ret = procdir.cd("/proc");
	if (ret == false) REPORT(Error::SYSTEM, "access_proc", "Could not access the proc filesystem", result);

	procs = procdir.entryList();

	for (int i = 0; i < procs.size(); i++) {

		QString tmp_pid = procs[i];

		if (!integer.exactMatch(tmp_pid)) continue;

		QString procname = getProcEntry(tmp_pid.toInt(), 1, true);

		if (procname == "")
			continue;
		else if (procname == proc)
			result.insert(tmp_pid.toInt());
	}

	return result;
}

QString OSServicesLinux::getCmd(int pid) {
	QMutexLocker locker(&mutex);
	QString result;

	QFile procfile("/proc/" + QString::number(pid) + "/cmdline");
	if (!procfile.open(QIODevice::ReadOnly)) return result;

	// The last byte of the file is 0 and is ignored
	char c;
	procfile.read(&c, 1);
	while (!procfile.atEnd()) {
		if (c != '\0')
			result += c;
		else
			result += ' ';

		procfile.read(&c, 1);
	}

	procfile.close();

	return result;
}

QString OSServicesLinux::getProcEntry(int tid, int index, bool error) {
	QString result = "";

	if (index < 0 || index > 43) {
		if (error) {
			REPORT(Error::SYSTEM, "access_proc", "Invalid index for stat file of: " + QString::number(index), result);
		} else
			return result;
	}

	QFile stat_file("/proc/" + QString::number(tid) + "/stat");

	if (!stat_file.open(QIODevice::ReadOnly)) {
		if (error) {
			REPORT(Error::SYSTEM, "access_proc", "Could not get status information for process " + QString::number(tid),
				   result);
		} else
			return result;
	}

	QTextStream stat_stream(&stat_file);
	QString proc_stat = stat_stream.readAll();

	stat_file.close();

	int start_pos = 0, end_pos = 0, current_index = 0;
	bool proc_name = false;

	// Determine the desired entry. Keep in mind that the process name may contain spaces

	while (end_pos < proc_stat.size()) {
		switch (proc_stat.toStdString().at(end_pos)) {
		case ' ':
			if (proc_name)
				end_pos++;
			else if (current_index == index) {
				result = proc_stat.mid(start_pos, end_pos - start_pos);
				return result;
			} else {
				end_pos++;
				start_pos = end_pos;
				current_index++;
			}
			break;

		case '(':
			proc_name = true;
			end_pos++;
			start_pos = end_pos;
			break;

		case ')':
			proc_name = false;
			if (current_index == index) {
				result = proc_stat.mid(start_pos, end_pos - start_pos);
				return result;
			}
			end_pos += 2;
			start_pos = end_pos;
			current_index++;
			break;

		default:
			end_pos++;
		}
	}

	if (index == current_index) {
		result = proc_stat.mid(start_pos, end_pos - start_pos - 1);
		return result;
	}

	if (error)
		REPORT(Error::SYSTEM, "access_proc", "Could not get status information for process " + QString::number(tid),
			   result);

	return result;
}

void OSServicesLinux::slot_handleSigChld() {
	snChld->setEnabled(false);

	siginfo_t info;
	read(sigchldFd[1], &info, sizeof(siginfo_t));
	waitpid(info.si_pid, nullptr, WNOHANG);

	emit sig_TaskTerminated(info.si_pid);

	snChld->setEnabled(true);
}

void OSServicesLinux::chldSignalHandler(int param, siginfo_t *info, void *paramv) {
	write(sigchldFd[0], info, sizeof(siginfo_t));
}

void OSServicesLinux::usrSignalHandler(int param) { autopin_attached = true; }

QStringList OSServicesLinux::getArguments(QString cmd) {
	QStringList result;
	QString token = "";
	int state = 0;
	bool escape = false;

	for (auto &elem : cmd) {
		switch (state) {
		case 0:
			if (elem == ' ')
				break;
			else {
				state = 1;
				token += elem;
			}
			break;
		case 1:
			if (elem == ' ') {
				if (!escape) {
					state = 0;
					result.push_back(token);
					token.clear();
				} else {
					token += elem;
					escape = false;
				}

				break;
			} else if (elem == '\\') {
				if (escape) {
					token += elem;
					escape = false;
				} else
					escape = true;
				break;
			} else {
				token += elem;
				escape = false;
				break;
			}
		default:
			break;
		}
	}

	if (state == 1) result.push_back(token);

	return result;
}

ProcessTree::autopin_tid_list OSServicesLinux::convertQStringList(QStringList &qlist) {
	ProcessTree::autopin_tid_list result;

	for (int i = 0; i < qlist.size(); i++) {
		int val = qlist.at(i).toInt();
		result.insert(val);
	}

	return result;
}

} // namespace Linux
} // namespace OS
} // namespace AutopinPlus
