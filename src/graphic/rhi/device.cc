/*
 * Copyright (C) 2026 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "graphic/rhi/device.h"

#include "base/wexception.h"

namespace Rhi {

namespace {

Device* g_device = nullptr;

}  // namespace

bool has_device() {
	return g_device != nullptr;
}

Device& device() {
	if (g_device == nullptr) {
		throw wexception("Rhi::device(): no RHI device is registered (did you start with a backend "
		                 "that creates one, e.g. --renderer=glcore?)");
	}
	return *g_device;
}

CommandBuffer& command_buffer() {
	return device().current_command_buffer();
}

void set_device(Device* device) {
	g_device = device;
}

CommandBuffer& Device::current_command_buffer() {
	if (command_buffer_stack_.empty()) {
		throw wexception("Rhi::command_buffer(): no command buffer is being recorded (are you "
		                 "outside a frame or offscreen submit?)");
	}
	return *command_buffer_stack_.back();
}

void Device::push_command_buffer(CommandBuffer* command_buffer) {
	command_buffer_stack_.push_back(command_buffer);
}

void Device::pop_command_buffer() {
	command_buffer_stack_.pop_back();
}

}  // namespace Rhi
