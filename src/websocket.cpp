/**
 * Copyright (c) 2020 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#if RTC_ENABLE_WEBSOCKET

#include "websocket.hpp"
#include "include.hpp"
#include "threadpool.hpp"

#include "tcptransport.hpp"
#include "tlstransport.hpp"
#include "verifiedtlstransport.hpp"
#include "wstransport.hpp"

#include <regex>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace rtc {

using std::shared_ptr;
using namespace std::placeholders;

WebSocket::WebSocket(std::optional<Configuration> config)
    : mConfig(config ? std::move(*config) : Configuration()),
      mRecvQueue(RECV_QUEUE_LIMIT, message_size_func) {
	PLOG_VERBOSE << "Creating WebSocket";
}

WebSocket::~WebSocket() {
	PLOG_VERBOSE << "Destroying WebSocket";
	remoteClose();
}

WebSocket::State WebSocket::readyState() const { return mState; }

void WebSocket::open(const string &url) {
	PLOG_VERBOSE << "Opening WebSocket to URL: " << url;

	if (mState != State::Closed)
		throw std::logic_error("WebSocket must be closed before opening");

	// Modified regex from RFC 3986, see https://tools.ietf.org/html/rfc3986#appendix-B
	static const char *rs =
	    R"(^(([^:.@/?#]+):)?(/{0,2}((([^:@]*)(:([^@]*))?)@)?(([^:/?#]*)(:([^/?#]*))?))?([^?#]*)(\?([^#]*))?(#(.*))?)";

	static const std::regex r(rs, std::regex::extended);

	std::smatch m;
	if (!std::regex_match(url, m, r) || m[10].length() == 0)
		throw std::invalid_argument("Invalid WebSocket URL: " + url);

	mScheme = m[2];
	if (mScheme.empty())
		mScheme = "ws";
	else if (mScheme != "ws" && mScheme != "wss")
		throw std::invalid_argument("Invalid WebSocket scheme: " + mScheme);

	mHostname = m[10];
	mService = m[12];
	if (mService.empty()) {
		mService = mScheme == "ws" ? "80" : "443";
		mHost = mHostname;
	} else {
		mHost = mHostname + ':' + mService;
	}

	while (!mHostname.empty() && mHostname.front() == '[')
		mHostname.erase(mHostname.begin());
	while (!mHostname.empty() && mHostname.back() == ']')
		mHostname.pop_back();

	mPath = m[13];
	if (mPath.empty())
		mPath += '/';
	if (string query = m[15]; !query.empty())
		mPath += "?" + query;

	changeState(State::Connecting);
	initTcpTransport();
}

void WebSocket::close() {
	auto state = mState.load();
	if (state == State::Connecting || state == State::Open) {
		PLOG_VERBOSE << "Closing WebSocket";
		changeState(State::Closing);
		if (auto transport = std::atomic_load(&mWsTransport))
			transport->close();
		else
			changeState(State::Closed);
	}
}

void WebSocket::remoteClose() {
	if (mState.load() != State::Closed) {
		close();
		closeTransports();
	}
}

bool WebSocket::send(message_variant data) { return outgoing(make_message(std::move(data))); }

bool WebSocket::send(const byte *data, size_t size) {
	return outgoing(make_message(data, data + size));
}

bool WebSocket::isOpen() const { return mState == State::Open; }

bool WebSocket::isClosed() const { return mState == State::Closed; }

size_t WebSocket::maxMessageSize() const { return DEFAULT_MAX_MESSAGE_SIZE; }

std::optional<message_variant> WebSocket::receive() {
	while (auto next = mRecvQueue.tryPop()) {
		message_ptr message = *next;
		if (message->type != Message::Control)
			return to_variant(std::move(*message));
	}
	return nullopt;
}

std::optional<message_variant> WebSocket::peek() {
	while (auto next = mRecvQueue.peek()) {
		message_ptr message = *next;
		if (message->type != Message::Control)
			return to_variant(std::move(*message));

		mRecvQueue.tryPop();
	}
	return nullopt;
}

size_t WebSocket::availableAmount() const { return mRecvQueue.amount(); }

bool WebSocket::changeState(State state) { return mState.exchange(state) != state; }

bool WebSocket::outgoing(message_ptr message) {
	if (mState != State::Open || !mWsTransport)
		throw std::runtime_error("WebSocket is not open");

	if (message->size() > maxMessageSize())
		throw std::runtime_error("Message size exceeds limit");

	return mWsTransport->send(message);
}

void WebSocket::incoming(message_ptr message) {
	if (!message) {
		remoteClose();
		return;
	}

	if (message->type == Message::String || message->type == Message::Binary) {
		mRecvQueue.push(message);
		triggerAvailable(mRecvQueue.size());
	}
}

shared_ptr<TcpTransport> WebSocket::initTcpTransport() {
	PLOG_VERBOSE << "Starting TCP transport";
	using State = TcpTransport::State;
	try {
		std::lock_guard lock(mInitMutex);
		if (auto transport = std::atomic_load(&mTcpTransport))
			return transport;

		auto transport = std::make_shared<TcpTransport>(
		    mHostname, mService, [this, weak_this = weak_from_this()](State state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (state) {
			    case State::Connected:
				    if (mScheme == "ws")
					    initWsTransport();
				    else
					    initTlsTransport();
				    break;
			    case State::Failed:
				    triggerError("TCP connection failed");
				    remoteClose();
				    break;
			    case State::Disconnected:
				    remoteClose();
				    break;
			    default:
				    // Ignore
				    break;
			    }
		    });
		std::atomic_store(&mTcpTransport, transport);
		if (mState == WebSocket::State::Closed) {
			mTcpTransport.reset();
			throw std::runtime_error("Connection is closed");
		}
		transport->start();
		return transport;

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("TCP transport initialization failed");
	}
}

shared_ptr<TlsTransport> WebSocket::initTlsTransport() {
	PLOG_VERBOSE << "Starting TLS transport";
	using State = TlsTransport::State;
	try {
		std::lock_guard lock(mInitMutex);
		if (auto transport = std::atomic_load(&mTlsTransport))
			return transport;

		auto lower = std::atomic_load(&mTcpTransport);
		auto stateChangeCallback = [this, weak_this = weak_from_this()](State state) {
			auto shared_this = weak_this.lock();
			if (!shared_this)
				return;
			switch (state) {
			case State::Connected:
				initWsTransport();
				break;
			case State::Failed:
				triggerError("TCP connection failed");
				remoteClose();
				break;
			case State::Disconnected:
				remoteClose();
				break;
			default:
				// Ignore
				break;
			}
		};

		shared_ptr<TlsTransport> transport;
#ifdef _WIN32
		if (!mConfig.disableTlsVerification) {
			PLOG_WARNING << "TLS certificate verification with root CA is not supported on Windows";
		}
		transport = std::make_shared<TlsTransport>(lower, mHostname, stateChangeCallback);
#else
		if (mConfig.disableTlsVerification)
			transport = std::make_shared<TlsTransport>(lower, mHostname, stateChangeCallback);
		else
			transport =
			    std::make_shared<VerifiedTlsTransport>(lower, mHostname, stateChangeCallback);
#endif

		std::atomic_store(&mTlsTransport, transport);
		if (mState == WebSocket::State::Closed) {
			mTlsTransport.reset();
			throw std::runtime_error("Connection is closed");
		}
		transport->start();
		return transport;

	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("TLS transport initialization failed");
	}
}

shared_ptr<WsTransport> WebSocket::initWsTransport() {
	PLOG_VERBOSE << "Starting WebSocket transport";
	using State = WsTransport::State;
	try {
		std::lock_guard lock(mInitMutex);
		if (auto transport = std::atomic_load(&mWsTransport))
			return transport;

		shared_ptr<Transport> lower = std::atomic_load(&mTlsTransport);
		if (!lower)
			lower = std::atomic_load(&mTcpTransport);

		WsTransport::Configuration wsConfig = {};
		wsConfig.host = mHost;
		wsConfig.path = mPath;
		wsConfig.protocols = mConfig.protocols;

		auto transport = std::make_shared<WsTransport>(
			lower, wsConfig, weak_bind(&WebSocket::incoming, this, _1),
		    [this, weak_this = weak_from_this()](State state) {
			    auto shared_this = weak_this.lock();
			    if (!shared_this)
				    return;
			    switch (state) {
			    case State::Connected:
				    if (mState == WebSocket::State::Connecting) {
					    PLOG_DEBUG << "WebSocket open";
					    changeState(WebSocket::State::Open);
					    triggerOpen();
				    }
				    break;
			    case State::Failed:
				    triggerError("WebSocket connection failed");
				    remoteClose();
				    break;
			    case State::Disconnected:
				    remoteClose();
				    break;
			    default:
				    // Ignore
				    break;
			    }
		    });
		std::atomic_store(&mWsTransport, transport);
		if (mState == WebSocket::State::Closed) {
			mWsTransport.reset();
			throw std::runtime_error("Connection is closed");
		}
		transport->start();
		return transport;
	} catch (const std::exception &e) {
		PLOG_ERROR << e.what();
		remoteClose();
		throw std::runtime_error("WebSocket transport initialization failed");
	}
}

void WebSocket::closeTransports() {
	PLOG_VERBOSE << "Closing transports";

	if (mState.load() != State::Closed) {
		changeState(State::Closed);
		triggerClosed();
	}

	// Reset callbacks now that state is changed
	resetCallbacks();

	// Pass the pointers to a thread, allowing to terminate a transport from its own thread
	auto ws = std::atomic_exchange(&mWsTransport, decltype(mWsTransport)(nullptr));
	auto tls = std::atomic_exchange(&mTlsTransport, decltype(mTlsTransport)(nullptr));
	auto tcp = std::atomic_exchange(&mTcpTransport, decltype(mTcpTransport)(nullptr));
	ThreadPool::Instance().enqueue([ws, tls, tcp]() mutable {
		if (ws)
			ws->stop();
		if (tls)
			tls->stop();
		if (tcp)
			tcp->stop();

		ws.reset();
		tls.reset();
		tcp.reset();
	});
}

} // namespace rtc

#endif
