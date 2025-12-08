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

#include "live_server.h"
#include "live_peer.h"
#include "live_tab.h"
#include "live_action.h"

#include "editor.h"

LiveServer::LiveServer(Editor& editor) : LiveSocket(),
	clients(), acceptor(nullptr), socket(nullptr), editor(&editor),
	clientIds(0), port(0), stopped(false)
{
	stopped.store(false);
}

LiveServer::~LiveServer()
{
	//
}

bool LiveServer::bind()
{
	NetworkConnection& connection = NetworkConnection::getInstance();
	if(!connection.start()) {
		setLastError("The previous connection has not been terminated yet.");
		return false;
	}

	auto& context = connection.get_context();
	acceptor = std::make_shared<asio::ip::tcp::acceptor>(context);

	asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), port);
	
	std::error_code error;
	acceptor->open(endpoint.protocol(), error);
	if(error) {
		setLastError("Error opening socket: " + error.message());
		return false;
	}

	// Allow reuse of address to prevent "address already in use" errors
	acceptor->set_option(asio::socket_base::reuse_address(true), error);
	if(error) {
		setLastError("Error setting reuse_address: " + error.message());
		return false;
	}

	acceptor->set_option(asio::ip::tcp::no_delay(true), error);
	if(error) {
		setLastError("Error setting no_delay: " + error.message());
		return false;
	}

	acceptor->bind(endpoint, error);
	if(error) {
		setLastError("Error binding to port " + std::to_string(port) + ": " + error.message());
		return false;
	}
	
	acceptor->listen(asio::socket_base::max_listen_connections, error);
	if(error) {
		setLastError("Error listening: " + error.message());
		return false;
	}

	acceptClient();
	return true;
}

void LiveServer::close()
{
	stopped.store(true);
	
	// Close all client connections
	for(auto& clientEntry : clients) {
		if (clientEntry.second) {
			clientEntry.second->close();
		}
	}
	clients.clear();

	if(log) {
		log->Message("Server was shutdown.");
		log->Disconnect();
		log = nullptr;
	}

	if(acceptor) {
		std::error_code ec;
		acceptor->cancel(ec);
		acceptor->close(ec);
	}

	if(socket) {
		std::error_code ec;
		socket->close(ec);
	}
}

void LiveServer::acceptClient()
{
	static uint32_t nextPeerId = 1;
	
	if(stopped.load()) {
		return;
	}

	// Always create a fresh socket for accepting new connections
	socket = std::make_shared<asio::ip::tcp::socket>(
		NetworkConnection::getInstance().get_context()
	);

	acceptor->async_accept(*socket, [this](const std::error_code& error) -> void
	{
		if(stopped.load()) {
			return;
		}
		
		if(!error) {
			uint32_t peerId = nextPeerId++;
			auto peer = std::make_shared<LivePeer>(this, std::move(*socket));
			peer->id = peerId;
			peer->log = log;
			peer->receiveHeader();

			clients.insert(std::make_pair(peerId, peer));
		}
		
		socket.reset();
		acceptClient();
	});
}

void LiveServer::removeClient(uint32_t id)
{
	auto it = clients.find(id);
	if(it == clients.end()) {
		return;
	}

	const uint32_t clientId = it->second->getClientId();
	if(clientId != 0) {
		clientIds &= ~clientId;
		editor->map.clearVisible(clientIds);
	}

	clients.erase(it);
	updateClientList();
}

void LiveServer::updateCursor(const Position& position)
{
	LiveCursor cursor;
	cursor.id = 0;
	cursor.pos = position;
	cursor.color = wxColor(
		g_settings.getInteger(Config::CURSOR_RED),
		g_settings.getInteger(Config::CURSOR_GREEN),
		g_settings.getInteger(Config::CURSOR_BLUE),
		g_settings.getInteger(Config::CURSOR_ALPHA)
	);
	broadcastCursor(cursor);
}

void LiveServer::updateClientList() const
{
	log->UpdateClientList(clients);
}

uint16_t LiveServer::getPort() const
{
	return port;
}

bool LiveServer::setPort(int32_t newPort)
{
	if(newPort < 1 || newPort > 65535) {
		setLastError("Port must be a number in the range 1-65535.");
		return false;
	}
	port = newPort;
	return true;
}

uint32_t LiveServer::getFreeClientId()
{
	for(int32_t bit = 1; bit < (1 << 16); bit <<= 1) {
		if(!testFlags(clientIds, bit)) {
			clientIds |= bit;
			return bit;
		}
	}
	return 0;
}

std::string LiveServer::getHostName() const
{
	if(acceptor) {
		auto endpoint = acceptor->local_endpoint();
		return endpoint.address().to_string() + ":" + std::to_string(endpoint.port());
	}
	return "localhost";
}

void LiveServer::broadcastNodes(DirtyList& dirtyList)
{
	if(dirtyList.Empty()) {
		return;
	}

	for(const auto& ind : dirtyList.GetPosList()) {
		int32_t ndx = ind.pos >> 18;
		int32_t ndy = (ind.pos >> 4) & 0x3FFF;
		uint32_t floors = ind.floors;

		QTreeNode* node = editor->map.getLeaf(ndx * 4, ndy * 4);
		if(!node) {
			continue;
		}

		for(auto& clientEntry : clients) {
			auto& peer = clientEntry.second;
			if (!peer || peer->isClosing()) {
				continue;
			}

			const uint32_t clientId = peer->getClientId();
			if(dirtyList.owner != 0 && dirtyList.owner == clientId) {
				continue;
			}

			if(node->isVisible(clientId, true)) {
				peer->sendNode(clientId, node, ndx, ndy, floors & 0xFF00);
			}

			if(node->isVisible(clientId, false)) {
				peer->sendNode(clientId, node, ndx, ndy, floors & 0x00FF);
			}
		}
	}
}

void LiveServer::broadcastCursor(const LiveCursor& cursor)
{
	if(clients.empty()) {
		return;
	}

	if(cursor.id != 0) {
		cursors[cursor.id] = cursor;
	}

	NetworkMessage message;
	message.write<uint8_t>(PACKET_CURSOR_UPDATE);
	writeCursor(message, cursor);

	for(auto& clientEntry : clients) {
		auto& peer = clientEntry.second;
		if(peer && !peer->isClosing() && peer->getClientId() != cursor.id) {
			peer->send(message);
		}
	}
}

void LiveServer::broadcastChat(const wxString& speaker, const wxString& chatMessage, uint32_t excludeClientId)
{
	NetworkMessage message;
	message.write<uint8_t>(PACKET_SERVER_TALK);
	message.write<std::string>(nstr(speaker));
	message.write<std::string>(nstr(chatMessage));

	for(auto& clientEntry : clients) {
		auto& peer = clientEntry.second;
		// Skip the client who sent the message (they already see it locally)
		if(peer && !peer->isClosing() && peer->getClientId() != excludeClientId) {
			peer->send(message);
		}
	}

	if (log) {
		log->Chat(speaker, chatMessage);
	}
}

void LiveServer::startOperation(const wxString& operationMessage)
{
	if(clients.empty()) {
		return;
	}

	NetworkMessage message;
	message.write<uint8_t>(PACKET_START_OPERATION);
	message.write<std::string>(nstr(operationMessage));

	for(auto& clientEntry : clients) {
		auto& peer = clientEntry.second;
		if(peer && !peer->isClosing()) {
			peer->send(message);
		}
	}
}

void LiveServer::updateOperation(int32_t percent)
{
	if(clients.empty()) {
		return;
	}

	NetworkMessage message;
	message.write<uint8_t>(PACKET_UPDATE_OPERATION);
	message.write<uint32_t>(percent);

	for(auto& clientEntry : clients) {
		auto& peer = clientEntry.second;
		if(peer && !peer->isClosing()) {
			peer->send(message);
		}
	}
}

LiveLogTab* LiveServer::createLogWindow(wxWindow* parent)
{
	MapTabbook* mapTabBook = dynamic_cast<MapTabbook*>(parent);
	ASSERT(mapTabBook);

	log = newd LiveLogTab(mapTabBook, this);
	log->Message("New Live mapping session started.");
	log->Message("Hosted on server " + getHostName() + ".");

	updateClientList();
	return log;
}
