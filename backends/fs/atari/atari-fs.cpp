/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// Disable symbol overrides so that we can use system headers.
#define FORBIDDEN_SYMBOL_ALLOW_ALL

#include "backends/fs/atari/atari-fs.h"

#ifdef ATARI_STE_GAME_ONLY
#include <mint/osbind.h>
#endif

void AtariFilesystemNode::setFlags() {
#ifdef ATARI_STE_GAME_ONLY
	// On plain TOS, stat() converts the file's DOS time stamp with mktime(),
	// which costs most of a tenth of a second per file on an 8 MHz 68000.
	// GEMDOS Fattrib() answers whether the path exists and is a directory.
	Common::String path(_path);
	for (uint i = 0; i < path.size(); ++i) {
		if (path[i] == '/')
			path.setChar('\\', i);
	}
	while (path.size() > 3 && path.lastChar() == '\\')
		path.deleteLastChar();

	if (path.size() <= 3 && path.size() >= 2 && path[1] == ':') {
		// A drive root, which Fattrib() does not report on.
		_isValid = true;
		_isDirectory = true;
	} else {
		const long attributes = Fattrib(path.c_str(), 0, 0);
		if (attributes >= 0) {
			_isValid = true;
			_isDirectory = (attributes & FA_DIR) != 0;
		} else {
			DrivePOSIXFilesystemNode::setFlags();
		}
	}
#else
	DrivePOSIXFilesystemNode::setFlags();
#endif

	if (!_displayNameChecked) {
		if (_fileHashMap.contains(_displayName))
			_displayName = _fileHashMap[_displayName];

		_displayNameChecked = true;
	}
}
