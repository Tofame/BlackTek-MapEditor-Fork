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

#include "live_peer.h"
#include "live_server.h"
#include "live_tab.h"
#include "live_action.h"
#include "live_packets.h"

#include "editor.h"

LivePeer::LivePeer(LiveServer* server, asio::ip::tcp::socket socket) : LiveSocket(),
	readMessage(), server(server), socket(std::move(socket)), color(), id(0), clientId(0), connected(false), closing(false)
{
	ASSERT(server != nullptr);
	// Initialize buffer with minimum size to prevent "size 0" errors
	readMessage.buffer.resize(1024);
	readMessage.position = 0;
}

LivePeer::~LivePeer()
{
	if(socket.is_open()) {
		std::error_code ec;
		socket.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
		socket.close(ec);
	}
}

void LivePeer::close()
{
	// Set closing flag to prevent new operations
	bool expected = false;
	if (!closing.compare_exchange_strong(expected, true)) {
		return; // Already closing
	}
	
	// Cancel any pending socket operations
	if (socket.is_open()) {
		std::error_code ec;
		socket.cancel(ec);
	}
	
	server->removeClient(id);
}

bool LivePeer::handleError(const std::error_code& error)
{
	if(error == asio::error::eof || error == asio::error::connection_reset ||
	   error == asio::error::connection_aborted) {
		close();
		return true;
	}
	return false;
}

std::string LivePeer::getHostName() const
{
	if (!socket.is_open()) {
		return "disconnected";
	}
	try {
		return socket.remote_endpoint().address().to_string();
	} catch (const std::system_error&) {
		return "disconnected";
	}
}

void LivePeer::receiveHeader()
{
	if (closing.load()) {
		return;
	}
	
	// Ensure buffer is properly sized
	if (readMessage.buffer.size() < 4) {
		readMessage.buffer.resize(1024);
	}
	
	readMessage.position = 0;
	
	// Capture shared_ptr to keep peer alive during async operation
	auto self = shared_from_this();
	
	asio::async_read(socket,
		asio::buffer(readMessage.buffer, 4),
		[this, self](const std::error_code& error, size_t bytesReceived) -> void {
			if (closing.load()) {
				return;
			}
			
			if(error || bytesReceived < 4) {
				handleError(error);
				close();
			} else {
				uint32_t packetSize = readMessage.read<uint32_t>();
				if (packetSize == 0) {
					receiveHeader();
				} else {
					receive(packetSize);
				}
			}
		}
	);
}

void LivePeer::receive(uint32_t packetSize)
{
	if (closing.load()) {
		return;
	}
	
	if(packetSize > MAX_NETWORK_PACKET_SIZE) {
		close();
		return;
	}
	
	readMessage.buffer.resize(readMessage.position + packetSize);
	
	auto self = shared_from_this();
	
	asio::async_read(socket,
		asio::buffer(&readMessage.buffer[readMessage.position], packetSize),
		[this, self, packetSize](const std::error_code& error, size_t bytesReceived) -> void {
			if (closing.load()) {
				return;
			}
			
			if(error || bytesReceived < packetSize) {
				handleError(error);
				close();
			} else {
				// Copy the message data before parsing - we need readMessage for the next receive
				NetworkMessage messageToProcess = readMessage;
				
				wxTheApp->CallAfter([this, self, messageToProcess = std::move(messageToProcess)]() mutable {
					if (closing.load()) {
						return;
					}
					
					if(connected) {
						parseEditorPacket(std::move(messageToProcess));
					} else {
						parseLoginPacket(std::move(messageToProcess));
					}
					receiveHeader();
				});
			}
		}
	);
}

void LivePeer::send(NetworkMessage& message)
{
	if (closing.load() || !socket.is_open()) {
		return;
	}
	
	// Validate message
	if (message.size == 0) {
		return;
	}
	
	// Write packet size to header (first 4 bytes)
	memcpy(&message.buffer[0], &message.size, 4);
	
	// Copy the buffer for async send (message might go out of scope)
	auto buffer = std::make_shared<std::vector<uint8_t>>(message.buffer);
	auto self = shared_from_this();
	size_t totalSize = message.size + 4;
	
	asio::async_write(socket,
		asio::buffer(*buffer, totalSize),
		[this, self, buffer](const std::error_code& error, size_t bytesTransferred) -> void {
			// Silent - errors will be caught by receive
		}
	);
}

void LivePeer::parseLoginPacket(NetworkMessage message)
{
	try {
		uint8_t packetType;
		while(message.position < message.buffer.size()) {
			packetType = message.read<uint8_t>();
			switch (packetType) {
				case PACKET_HELLO_FROM_CLIENT:
					parseHello(message);
					break;
				case PACKET_READY_CLIENT:
					parseReady(message);
					break;
				default:
					close();
					return;
			}
		}
	} catch (const std::exception& e) {
		close();
	}
}

void LivePeer::parseEditorPacket(NetworkMessage message)
{
	try {
		uint8_t packetType;
		while(message.position < message.buffer.size()) {
			packetType = message.read<uint8_t>();
			switch (packetType) {
				case PACKET_REQUEST_NODES:
					parseNodeRequest(message);
					break;
				case PACKET_CHANGE_LIST:
					parseReceiveChanges(message);
					break;
				case PACKET_ADD_HOUSE:
					parseAddHouse(message);
					break;
				case PACKET_EDIT_HOUSE:
					parseEditHouse(message);
					break;
				case PACKET_REMOVE_HOUSE:
					parseRemoveHouse(message);
					break;
				case PACKET_CLIENT_UPDATE_CURSOR:
					parseCursorUpdate(message);
					break;
				case PACKET_CLIENT_TALK:
					parseChatMessage(message);
					break;
				default:
					close();
					return;
			}
		}
	} catch (const std::exception& e) {
		close();
	}
}

void LivePeer::parseHello(NetworkMessage& message)
{
	if(connected) {
		close();
		return;
	}

	uint32_t rmeVersion = message.read<uint32_t>();
	if(rmeVersion != __RME_VERSION_ID__) {
		NetworkMessage outMessage;
		outMessage.write<uint8_t>(PACKET_KICK);
		outMessage.write<std::string>("Wrong editor version.");

		send(outMessage);
		close();
		return;
	}

	uint32_t netVersion = message.read<uint32_t>();
	if(netVersion != __LIVE_NET_VERSION__) {
		NetworkMessage outMessage;
		outMessage.write<uint8_t>(PACKET_KICK);
		outMessage.write<std::string>("Wrong protocol version.");

		send(outMessage);
		close();
		return;
	}

	uint32_t clientVersion = message.read<uint32_t>();
	std::string nickname = message.read<std::string>();
	std::string password = message.read<std::string>();

	if(server->getPassword() != wxString(password.c_str(), wxConvUTF8)) {
		if(log) {
			log->Message("Client tried to connect, but used the wrong password, connection refused.");
		}
		close();
		return;
	}

	name = wxString(nickname.c_str(), wxConvUTF8);

	NetworkMessage outMessage;
	if(static_cast<ClientVersionID>(clientVersion) != g_gui.GetCurrentVersionID()) {
		outMessage.write<uint8_t>(PACKET_CHANGE_CLIENT_VERSION);
		outMessage.write<uint32_t>(g_gui.GetCurrentVersionID());
	} else {
		outMessage.write<uint8_t>(PACKET_ACCEPTED_CLIENT);
	}
	send(outMessage);
}

void LivePeer::parseReady(NetworkMessage& message)
{
	if(connected) {
		close();
		return;
	}

	connected = true;

	clientId = server->getFreeClientId();
	if(clientId == 0) {
		NetworkMessage outMessage;
		outMessage.write<uint8_t>(PACKET_KICK);
		outMessage.write<std::string>("Server is full.");

		send(outMessage);
		close();
		return;
	}

	// Assign a default color to the new client
	color = wxColor(
		128 + rand() % 127,
		128 + rand() % 127,
		128 + rand() % 127,
		255
	);

	server->updateClientList();
	if(log) {
		log->Message(name + " (" + getHostName() + ") joined the session.");
	}

	// Send HELLO_FROM_SERVER packet with map information
	NetworkMessage outMessage;
	outMessage.write<uint8_t>(PACKET_HELLO_FROM_SERVER);

	Map& map = server->getEditor()->map;
	outMessage.write<std::string>(map.getName());
	outMessage.write<uint16_t>(map.getWidth());
	outMessage.write<uint16_t>(map.getHeight());

	send(outMessage);
}

void LivePeer::parseNodeRequest(NetworkMessage& message)
{
	Map& map = server->getEditor()->map;
	for(uint32_t nodes = message.read<uint32_t>(); nodes != 0; --nodes) {
		uint32_t ind = message.read<uint32_t>();

		int32_t ndx = ind >> 18;
		int32_t ndy = (ind >> 4) & 0x3FFF;
		bool underground = ind & 1;

		QTreeNode* node = map.createLeaf(ndx * 4, ndy * 4);
		if(node) {
			sendNode(clientId, node, ndx, ndy, underground ? 0xFF00 : 0x00FF);
		}
	}
}

void LivePeer::parseReceiveChanges(NetworkMessage& message)
{
	Editor& editor = *server->getEditor();

	// Read the tile data. Handle the node format properly.
	const std::string& data = message.read<std::string>();
	if (data.empty()) {
		return;
	}
	
	// Create a buffer with a fake first byte for the NODE_START that getRootNode skips
	std::string nodeData;
	nodeData.reserve(data.size() + 1);
	nodeData.push_back(0); // Fake NODE_START byte
	nodeData.append(data);
	
	mapReader.assign(reinterpret_cast<const uint8_t*>(nodeData.c_str()), nodeData.size());

	BinaryNode* rootNode = mapReader.getRootNode();
	BinaryNode* tileNode = rootNode->getChild();

	NetworkedAction* action = static_cast<NetworkedAction*>(editor.actionQueue->createAction(ACTION_REMOTE));
	action->owner = clientId;

	if(tileNode) do {
		Tile* tile = readTile(tileNode, editor, nullptr);
		if(tile) {
			action->addChange(newd Change(tile));
		}
	} while(tileNode->advance());
	mapReader.close();

	editor.actionQueue->addAction(action);

	g_gui.RefreshView();
	g_gui.UpdateMinimap();
}

void LivePeer::parseAddHouse(NetworkMessage& message)
{
}

void LivePeer::parseEditHouse(NetworkMessage& message)
{
}

void LivePeer::parseRemoveHouse(NetworkMessage& message)
{
}

void LivePeer::parseCursorUpdate(NetworkMessage& message)
{
	LiveCursor cursor = readCursor(message);
	cursor.id = clientId;

	if(cursor.color != color) {
		setUsedColor(cursor.color);
		server->updateClientList();
	}

	server->broadcastCursor(cursor);
	g_gui.RefreshView();
}

void LivePeer::parseChatMessage(NetworkMessage& message)
{
	const std::string& chatMessage = message.read<std::string>();
	// Pass our clientId so we don't receive our own message back
	server->broadcastChat(name, wxstr(chatMessage), clientId);
}
