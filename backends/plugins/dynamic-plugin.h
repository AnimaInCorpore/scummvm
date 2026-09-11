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

#ifndef BACKENDS_PLUGINS_DYNAMICPLUGIN_H
#define BACKENDS_PLUGINS_DYNAMICPLUGIN_H

#ifdef ATARI_STE_GAME_ONLY
#define FORBIDDEN_SYMBOL_EXCEPTION_printf
#endif

#include "base/plugins.h"
#include "common/textconsole.h"

#ifdef ATARI_STE_GAME_ONLY
#undef printf
#include <stdio.h>
#endif


class DynamicPlugin : public Plugin {
protected:
	typedef int32 (*IntFunc)();
	typedef void (*VoidFunc)();
	typedef PluginObject *(*GetObjectFunc)();

	virtual VoidFunc findSymbol(const char *symbol) = 0;

	const Common::Path _filename;

public:
	DynamicPlugin(const Common::Path &filename) :
		_filename(filename) {}

	bool loadPlugin() override {
		// Validate the plugin API version
		IntFunc verFunc = (IntFunc)findSymbol("PLUGIN_getVersion");
		#ifdef ATARI_STE_GAME_ONLY
		printf("Atari plugin getVersion symbol %p\n", (void *)verFunc);
		#endif
		if (!verFunc) {
			unloadPlugin();
			return false;
		}
		if (verFunc() != PLUGIN_VERSION) {
			warning("Plugin uses a different API version (you have: '%d', needed is: '%d')", verFunc(), PLUGIN_VERSION);
			unloadPlugin();
			return false;
		}

		// Get the type of the plugin
		IntFunc typeFunc = (IntFunc)findSymbol("PLUGIN_getType");
		#ifdef ATARI_STE_GAME_ONLY
		printf("Atari plugin version accepted\n");
		#endif
		if (!typeFunc) {
			unloadPlugin();
			return false;
		}
		_type = (PluginType)typeFunc();
		if (_type >= PLUGIN_TYPE_MAX) {
			warning("Plugin type unknown: %d", _type);
			unloadPlugin();
			return false;
		}

		// Validate the plugin type API version
		IntFunc typeVerFunc = (IntFunc)findSymbol("PLUGIN_getTypeVersion");
		#ifdef ATARI_STE_GAME_ONLY
		printf("Atari plugin type %d\n", _type);
		#endif
		if (!typeVerFunc) {
			unloadPlugin();
			return false;
		}
		if (typeVerFunc() != pluginTypeVersions[_type]) {
			warning("Plugin uses a different type API version (you have: '%d', needed is: '%d')", typeVerFunc(), pluginTypeVersions[_type]);
			unloadPlugin();
			return false;
		}

		// Get the plugin's instantiator object
		GetObjectFunc getObject = (GetObjectFunc)findSymbol("PLUGIN_getObject");
		#ifdef ATARI_STE_GAME_ONLY
		printf("Atari plugin type version accepted, object symbol %p\n", (void *)getObject);
		#endif
		if (!getObject) {
			unloadPlugin();
			return false;
		}

		// Get the plugin object
		_pluginObject = getObject();
		#ifdef ATARI_STE_GAME_ONLY
		printf("Atari plugin object created %p\n", (void *)_pluginObject);
		#endif
		if (!_pluginObject) {
			warning("Couldn't get the plugin object");
			unloadPlugin();
			return false;
		}

		return true;
	}

	void unloadPlugin() override {
		delete _pluginObject;
		_pluginObject = nullptr;
	}

	Common::Path getFileName() const override {
		return _filename;
	}
};

#endif
