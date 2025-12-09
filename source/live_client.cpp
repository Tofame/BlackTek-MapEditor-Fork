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

#include "live_client.h"
#include "live_tab.h"
#include "live_action.h"
#include "live_packets.h"
#include "editor.h"

#include <wx/event.h>

LiveClient::LiveClient() : LiveSocket(),
	readMessage(), queryNodeList(), currentOperation(),
	resolver(nullptr), socket(nullptr), editor(nullptr), stopped(false)
{
	stopped.store(false);
	// Initialize buffer with minimum size to prevent "size 0" errors
	readMessage.buffer.resize(1024);
	readMessage.position = 0;
}

LiveClient::~LiveClient()
{
	close();
}

bool LiveClient::connect(const std::string& address, uint16_t port)
{
	NetworkConnection& connection = NetworkConnection::getInstance();
	if(!connection.start()) {
		setLastError("The previous connection has not been terminated yet.");
		return false;
	}

	auto& context = connection.get_context();
	if(!resolver) {
		resolver = std::make_shared<asio::ip::tcp::resolver>(context);
	}

	if(!socket) {
		socket = std::make_shared<asio::ip::tcp::socket>(context);
	}

	resolver->async_resolve(
		address,
		std::to_string(port),
		[this](const std::error_code& error, asio::ip::tcp::resolver::results_type results) -> void {
			if (!error) {
				tryConnect(results);
			}
		}
	);

	/*
	if(!client->WaitOnConnect(5, 0)) {
		if(log)
			log->Disconnect();
		last_err = "Connection timed out.";
		client->Destroy();
		client = nullptr;
		delete connection;
		return false;
	}

	if(!client->IsConnected()) {
		if(log)
			log->Disconnect();
		last_err = "Connection refused by peer.";
		client->Destroy();
		client = nullptr;
		delete connection;
		return false;
	}

	if(log)
		log->Message("Connection established!");
	*/
	return true;
}

void LiveClient::tryConnect(asio::ip::tcp::resolver::results_type endpoints)
{
	if(stopped.load()) {
		return;
	}

	// Pick the first endpoint to log (optional)
	if (!endpoints.empty()) {
		const auto& ep = *endpoints.begin();
		logMessage("Joining server " + ep.host_name() + ":" + ep.service_name() + "...");
	}

	asio::async_connect(
		*socket,
		endpoints,
		[this](std::error_code error, const asio::ip::tcp::endpoint& /*endpoint*/) -> void {
			if (stopped.load()) {
				return;
			}
			
			if (!socket->is_open() || error) {
				if (!handleError(error)) {
					wxTheApp->CallAfter([this]() {
						close();
						g_gui.CloseLiveEditors(this);
					});
				}
		} else {
			// Connected successfully
			logMessage("Connected to server!");
			
			std::error_code ec;
			socket->set_option(asio::ip::tcp::no_delay(true), ec);
			if (ec) {
				wxTheApp->CallAfter([this]() {
					close();
				});
				return;
			}

			sendHello();
			receiveHeader();
		}
		}
	);
}

void LiveClient::close()
{
	// Set stopped flag first to prevent new operations
	bool expected = false;
	if (!stopped.compare_exchange_strong(expected, true)) {
		return; // Already closing
	}

	if(resolver) {
		resolver->cancel();
	}

	if(socket) {
		std::error_code ec;
		socket->cancel(ec);
		socket->shutdown(asio::ip::tcp::socket::shutdown_both, ec);
		socket->close(ec);
	}

	if(log) {
		log->Message("Disconnected from server.");
		log->Disconnect();
		log = nullptr;
	}
}

bool LiveClient::handleError(const std::error_code& error)
{
	if(error == asio::error::eof || error == asio::error::connection_reset ||
	   error == asio::error::connection_aborted) {
		return true;
	}
	return false;
}

std::string LiveClient::getHostName() const
{
	if(!socket || !socket->is_open()) {
		return "not connected";
	}
	try {
		return socket->remote_endpoint().address().to_string();
	} catch (const std::system_error&) {
		return "not connected";
	}
}

void LiveClient::receiveHeader()
{
	if (stopped.load()) {
		return;
	}
	
	if (!socket || !socket->is_open()) {
		return;
	}
	
	// Ensure buffer is properly sized
	if (readMessage.buffer.size() < 4) {
		readMessage.buffer.resize(1024);
	}
	
	readMessage.position = 0;
	
	asio::async_read(*socket,
		asio::buffer(readMessage.buffer, 4),
		[this](const std::error_code& error, size_t bytesReceived) -> void {
			if (stopped.load()) {
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

void LiveClient::receive(uint32_t packetSize)
{
	if (stopped.load()) {
		return;
	}
	
	if(packetSize > MAX_NETWORK_PACKET_SIZE) {
		close();
		return;
	}
	
	readMessage.buffer.resize(readMessage.position + packetSize);
	asio::async_read(*socket,
		asio::buffer(&readMessage.buffer[readMessage.position], packetSize),
		[this, packetSize](const std::error_code& error, size_t bytesReceived) -> void {
			if (stopped.load()) {
				return;
			}
			
			if(error || bytesReceived < packetSize) {
				handleError(error);
				close();
			} else {
				// Copy the message data before parsing - we need readMessage for the next receive
				NetworkMessage messageToProcess = readMessage;
				
				wxTheApp->CallAfter([this, messageToProcess = std::move(messageToProcess)]() mutable {
					if (stopped.load()) {
						return;
					}
					parsePacket(std::move(messageToProcess));
					receiveHeader();
				});
			}
		}
	);
}

void LiveClient::send(NetworkMessage& message)
{
	if (stopped.load()) {
		return;
	}
	
	if (!socket || !socket->is_open()) {
		return;
	}
	
	// Validate message
	if (message.size == 0) {
		return;
	}
	
	// Write packet size to header
	memcpy(&message.buffer[0], &message.size, 4);
	
	// Copy the buffer for async send (message might go out of scope before async completes)
	auto buffer = std::make_shared<std::vector<uint8_t>>(message.buffer);
	size_t sendSize = message.size + 4;
	
	asio::async_write(*socket,
		asio::buffer(*buffer, sendSize),
		[this, buffer](const std::error_code& error, size_t bytesTransferred) -> void {
			// Silent - errors will be caught by receive
		}
	);
}

void LiveClient::updateCursor(const Position& position)
{
	LiveCursor cursor;
	cursor.id = 77; // Unimportant, server fixes it for us
	cursor.pos = position;
	cursor.color = wxColor(
		g_settings.getInteger(Config::CURSOR_RED),
		g_settings.getInteger(Config::CURSOR_GREEN),
		g_settings.getInteger(Config::CURSOR_BLUE),
		g_settings.getInteger(Config::CURSOR_ALPHA)
	);

	NetworkMessage message;
	message.write<uint8_t>(PACKET_CLIENT_UPDATE_CURSOR);
	writeCursor(message, cursor);

	send(message);
}

LiveLogTab* LiveClient::createLogWindow(wxWindow* parent)
{
	MapTabbook* mtb = dynamic_cast<MapTabbook*>(parent);
	ASSERT(mtb);

	log = newd LiveLogTab(mtb, this);
	log->Message("New Live mapping session started.");

	return log;
}

MapTab* LiveClient::createEditorWindow()
{
	MapTabbook* mtb = dynamic_cast<MapTabbook*>(g_gui.tabbook);
	ASSERT(mtb);

	MapTab* edit = newd MapTab(mtb, editor);
	edit->OnSwitchEditorMode(g_gui.IsSelectionMode() ? SELECTION_MODE : DRAWING_MODE);

	return edit;
}

void LiveClient::sendHello()
{
	NetworkMessage message;
	message.write<uint8_t>(PACKET_HELLO_FROM_CLIENT);
	message.write<uint32_t>(__RME_VERSION_ID__);
	message.write<uint32_t>(__LIVE_NET_VERSION__);
	message.write<uint32_t>(g_gui.GetCurrentVersionID());
	message.write<std::string>(nstr(name));
	message.write<std::string>(nstr(password));

	send(message);
}

void LiveClient::sendNodeRequests()
{
	if(queryNodeList.empty()) {
		return;
	}

	NetworkMessage message;
	message.write<uint8_t>(PACKET_REQUEST_NODES);

	message.write<uint32_t>(queryNodeList.size());
	for(uint32_t node : queryNodeList) {
		message.write<uint32_t>(node);
	}

	send(message);
	queryNodeList.clear();
}

void LiveClient::sendChanges(DirtyList& dirtyList)
{
	ChangeList& changeList = dirtyList.GetChanges();
	if(changeList.empty()) {
		return;
	}

	mapWriter.reset();
	for(Change* change : changeList) {
		switch (change->getType()) {
			case CHANGE_TILE: {
				const Position& position = static_cast<Tile*>(change->getData())->getPosition();
				sendTile(mapWriter, editor->map.getTile(position), &position);
				break;
			}
			default:
				break;
		}
	}
	mapWriter.endNode();

	NetworkMessage message;
	message.write<uint8_t>(PACKET_CHANGE_LIST);

	std::string data(reinterpret_cast<const char*>(mapWriter.getMemory()), mapWriter.getSize());
	message.write<std::string>(data);

	send(message);
}

void LiveClient::sendChat(const wxString& chatMessage)
{
	NetworkMessage message;
	message.write<uint8_t>(PACKET_CLIENT_TALK);
	message.write<std::string>(nstr(chatMessage));
	send(message);
}

void LiveClient::sendReady()
{
	NetworkMessage message;
	message.write<uint8_t>(PACKET_READY_CLIENT);
	send(message);
}

void LiveClient::queryNode(int32_t ndx, int32_t ndy, bool underground)
{
	uint32_t nd = 0;
	nd |= ((ndx >> 2) << 18);
	nd |= ((ndy >> 2) << 4);
	nd |= (underground ? 1 : 0);
	queryNodeList.insert(nd);
}

void LiveClient::parsePacket(NetworkMessage message)
{
	try {
		uint8_t packetType;
		while(message.position < message.buffer.size()) {
			packetType = message.read<uint8_t>();
			switch (packetType) {
				case PACKET_HELLO_FROM_SERVER:
					parseHello(message);
					break;
				case PACKET_KICK:
					parseKick(message);
					break;
				case PACKET_ACCEPTED_CLIENT:
					parseClientAccepted(message);
					break;
				case PACKET_CHANGE_CLIENT_VERSION:
					parseChangeClientVersion(message);
					break;
				case PACKET_SERVER_TALK:
					parseServerTalk(message);
					break;
				case PACKET_NODE:
					parseNode(message);
					break;
				case PACKET_CURSOR_UPDATE:
					parseCursorUpdate(message);
					break;
				case PACKET_START_OPERATION:
					parseStartOperation(message);
					break;
				case PACKET_UPDATE_OPERATION:
					parseUpdateOperation(message);
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

void LiveClient::parseHello(NetworkMessage& message)
{
	ASSERT(editor == nullptr);
	editor = newd Editor(g_gui.copybuffer, this);

	Map& map = editor->map;
	map.setName("Live Map - " + message.read<std::string>());
	map.setWidth(message.read<uint16_t>());
	map.setHeight(message.read<uint16_t>());

	createEditorWindow();
}

void LiveClient::parseKick(NetworkMessage& message)
{
	const std::string& kickMessage = message.read<std::string>();
	close();

	g_gui.PopupDialog("Disconnected", wxstr(kickMessage), wxOK);
}

void LiveClient::parseClientAccepted(NetworkMessage& message)
{
	// Note: Avoid using logMessage() here as it can cause issues with CallAfter during packet parsing
	// Just send the ready packet directly
	sendReady();
}

void LiveClient::parseChangeClientVersion(NetworkMessage& message)
{
	ClientVersionID clientVersion = static_cast<ClientVersionID>(message.read<uint32_t>());
	
	// Check if we need to change version at all
	if (static_cast<ClientVersionID>(g_gui.GetCurrentVersionID()) == clientVersion) {
		// Already on the correct version
		sendReady();
		return;
	}
	
	// Try to close all editors to switch version
	if(!g_gui.CloseAllEditors()) {
		// User cancelled or couldn't close editors
		if (log) {
			log->Message("Cannot join: server requires different client version. Close your maps first.");
		}
		g_gui.PopupDialog("Version Mismatch", 
			"The server is using a different client version.\nPlease close all open maps and try again.", wxOK);
		close();
		return;
	}

	wxString error;
	wxArrayString warnings;
	if (!g_gui.LoadVersion(clientVersion, error, warnings)) {
		if (log) {
			log->Message("Failed to load required client version: " + error);
		}
		g_gui.PopupDialog("Version Error", "Failed to load required client version:\n" + error, wxOK);
		close();
		return;
	}

	sendReady();
}

void LiveClient::parseServerTalk(NetworkMessage& message)
{
	const std::string& speaker = message.read<std::string>();
	const std::string& chatMessage = message.read<std::string>();
	if(log) {
		log->Chat(
			wxstr(speaker),
			wxstr(chatMessage)
		);
	}
}

void LiveClient::parseNode(NetworkMessage& message)
{
	uint32_t ind = message.read<uint32_t>();

	// Extract node position
	int32_t ndx = ind >> 18;
	int32_t ndy = (ind >> 4) & 0x3FFF;
	bool underground = ind & 1;

	Action* action = editor->actionQueue->createAction(ACTION_REMOTE);
	receiveNode(message, *editor, action, ndx, ndy, underground);
	editor->actionQueue->addAction(action);

	g_gui.RefreshView();
	g_gui.UpdateMinimap();
}

void LiveClient::parseCursorUpdate(NetworkMessage& message)
{
	LiveCursor cursor = readCursor(message);
	
	std::lock_guard<std::mutex> lock(cursorsLock);
	cursors[cursor.id] = cursor;

	g_gui.RefreshView();
}

void LiveClient::parseStartOperation(NetworkMessage& message)
{
	const std::string& operation = message.read<std::string>();

	currentOperation = wxstr(operation);
	g_gui.SetStatusText("Server Operation in Progress: " + currentOperation + "... (0%)");
}

void LiveClient::parseUpdateOperation(NetworkMessage& message)
{
	int32_t percent = message.read<uint32_t>();
	if(percent >= 100) {
		g_gui.SetStatusText("Server Operation Finished.");
	} else {
		g_gui.SetStatusText("Server Operation in Progress: " + currentOperation + "... (" + std::to_string(percent) + "%)");
	}
}
