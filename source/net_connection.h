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

#ifndef _RME_NET_CONNECTION_H_
#define _RME_NET_CONNECTION_H_

#include "position.h"

#include <string>
#include <vector>
#include <cstdint>
#include <thread>
#include <mutex>
#include <atomic>
#include <memory>
#include <asio.hpp>

struct NetworkMessage
{
	NetworkMessage();

	void clear();
	void expand(const size_t length);

	// Check if there are enough bytes to read
	bool canRead(size_t bytes) const {
		return position + bytes <= buffer.size();
	}

	//
	template<typename T> T read()
	{
		if (!canRead(sizeof(T))) {
			throw std::runtime_error("NetworkMessage: read past end of buffer");
		}
		T value;
		memcpy(&value, &buffer[position], sizeof(T));
		position += sizeof(T);
		return value;
	}

	template<typename T> void write(const T& value)
	{
		expand(sizeof(T));
		memcpy(&buffer[position], &value, sizeof(T));
		position += sizeof(T);
	}

	//
	std::vector<uint8_t> buffer;
	size_t position;
	size_t size;
};

template<> std::string NetworkMessage::read<std::string>();
template<> Position NetworkMessage::read<Position>();
template<> void NetworkMessage::write<std::string>(const std::string& value);
template<> void NetworkMessage::write<Position>(const Position& value);

class NetworkConnection
{
	private:
		NetworkConnection();
		NetworkConnection(const NetworkConnection& copy) = delete;

	public:
		~NetworkConnection();

		static NetworkConnection& getInstance();

		bool start();
		void stop();

		asio::io_service& get_context();

	private:
		std::unique_ptr<asio::io_service> context;
		std::unique_ptr<asio::executor_work_guard<asio::io_service::executor_type>> workGuard;
		std::thread thread;
		std::atomic<bool> running;
};

#endif
