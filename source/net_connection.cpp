//////////////////////////////////////////////////////////////////////
// This file is part of Remere's Map Editor
//////////////////////////////////////////////////////////////////////
// Remere's Map Editor is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Remere's Map Editor is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <http://www.gnu.org/licenses/>.
//////////////////////////////////////////////////////////////////////

#include "main.h"
#include "net_connection.h"

NetworkMessage::NetworkMessage()
{
	clear();
}

void NetworkMessage::clear()
{
	buffer.resize(4);
	position = 4;
	size = 0;
}

void NetworkMessage::expand(const size_t length)
{
	if(position + length >= buffer.size()) {
		buffer.resize(position + length + 1);
	}
	size += length;
}

template<> std::string NetworkMessage::read<std::string>()
{
	const uint16_t length = read<uint16_t>();
	if (!canRead(length)) {
		throw std::runtime_error("NetworkMessage: string read past end of buffer");
	}
	char* strBuffer = reinterpret_cast<char*>(&buffer[position]);
	position += length;
	return std::string(strBuffer, length);
}

template<> Position NetworkMessage::read<Position>()
{
	Position position;
	position.x = read<uint16_t>();
	position.y = read<uint16_t>();
	position.z = read<uint8_t>();
	return position;
}

template<> void NetworkMessage::write<std::string>(const std::string& value)
{
	const size_t length = value.length();
	write<uint16_t>(length);

	expand(length);
	memcpy(&buffer[position], &value[0], length);
	position += length;
}

template<> void NetworkMessage::write<Position>(const Position& value)
{
	write<uint16_t>(value.x);
	write<uint16_t>(value.y);
	write<uint8_t>(value.z);
}

// NetworkConnection
NetworkConnection::NetworkConnection() :
	context(nullptr), workGuard(nullptr), thread(), running(false) {
	//
}

NetworkConnection::~NetworkConnection()
{
	stop();
}

NetworkConnection& NetworkConnection::getInstance()
{
	static NetworkConnection connection;
	return connection;
}

bool NetworkConnection::start()
{
	// If already running, return true
	if (running.load()) {
		return true;
	}

	// If thread is joinable but not running, it means we need to clean up first
	if (thread.joinable()) {
		thread.join();
	}

	// Create fresh io_context
	context = std::make_unique<asio::io_context>();
	
	// Create work guard to keep io_context running even without pending work
	workGuard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(
		asio::make_work_guard(*context)
	);

	running.store(true);

	thread = std::thread([this]() -> void {
		try {
			context->run();
		} catch (std::exception& e) {
			std::cout << "NetworkConnection error: " << e.what() << std::endl;
		}
		running.store(false);
	});

	return true;
}

void NetworkConnection::stop()
{
	if (!running.load() && !thread.joinable()) {
		return;
	}

	// Release work guard to allow io_context to stop when no more work
	if (workGuard) {
		workGuard.reset();
	}

	// Stop the context
	if (context) {
		context->stop();
	}

	// Wait for thread to finish
	if (thread.joinable()) {
		thread.join();
	}

	// Clean up
	context.reset();
	running.store(false);
}

asio::io_context& NetworkConnection::get_context()
{
	return *context;
}
