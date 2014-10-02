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

#include <AutopinPlus/Exception.h>

#include <qbytearray.h> // for QByteArray
#include <qstring.h>    // for QString

namespace AutopinPlus {
	Exception::Exception(QString message){
		this->message = message.toUtf8();
	}

	const char * Exception::what() const throw() {
		return this->message.data();
	}
} // namespace AutopinPlus
