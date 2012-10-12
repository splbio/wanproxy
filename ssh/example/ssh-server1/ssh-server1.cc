#include <crypto/crypto_encryption.h>

#include <event/event_callback.h>
#include <event/event_main.h>
#include <event/event_system.h>

#include <io/net/tcp_server.h>

#include <io/pipe/pipe.h>
#include <io/pipe/pipe_producer.h>
#include <io/pipe/splice.h>

#include <io/socket/simple_server.h>

#include <ssh/ssh_algorithm_negotiation.h>
#include <ssh/ssh_protocol.h>
#include <ssh/ssh_server_host_key.h>
#include <ssh/ssh_session.h>
#include <ssh/ssh_transport_pipe.h>

class SSHConnection {
	LogHandle log_;
	Socket *peer_;
	SSH::Session session_;
	SSH::TransportPipe *pipe_;
	Action *receive_action_;
	Splice *splice_;
	Action *splice_action_;
	Action *close_action_;
public:
	SSHConnection(Socket *peer)
	: log_("/ssh/connection"),
	  peer_(peer),
	  session_(SSH::ServerRole),
	  pipe_(NULL),
	  splice_(NULL),
	  splice_action_(NULL),
	  close_action_(NULL)
	{
		session_.algorithm_negotiation_ = new SSH::AlgorithmNegotiation(&session_);
		if (session_.role_ == SSH::ServerRole) {
			SSH::ServerHostKey *server_host_key = SSH::ServerHostKey::server(&session_, "ssh-server1.pem");
			session_.algorithm_negotiation_->add_algorithm(server_host_key);
		}
		session_.algorithm_negotiation_->add_algorithms();

		pipe_ = new SSH::TransportPipe(&session_);
		EventCallback *rcb = callback(this, &SSHConnection::receive_complete);
		receive_action_ = pipe_->receive(rcb);

		splice_ = new Splice(log_ + "/splice", peer_, pipe_, peer_);
		EventCallback *cb = callback(this, &SSHConnection::splice_complete);
		splice_action_ = splice_->start(cb);
	}

	~SSHConnection()
	{
		ASSERT(log_, close_action_ == NULL);
		ASSERT(log_, splice_action_ == NULL);
		ASSERT(log_, splice_ == NULL);
		ASSERT(log_, receive_action_ == NULL);
		ASSERT(log_, pipe_ == NULL);
		ASSERT(log_, peer_ == NULL);
	}

private:
	void receive_complete(Event e)
	{
		receive_action_->cancel();
		receive_action_ = NULL;

		switch (e.type_) {
		case Event::Done:
			break;
		default:
			ERROR(log_) << "Unexpected event while waiting for a packet: " << e;
			return;
		}

		ASSERT(log_, !e.buffer_.empty());

		/*
		 * SSH Echo!
		 */
		pipe_->send(&e.buffer_);

		EventCallback *rcb = callback(this, &SSHConnection::receive_complete);
		receive_action_ = pipe_->receive(rcb);
	}

	void close_complete(void)
	{
		close_action_->cancel();
		close_action_ = NULL;

		ASSERT(log_, peer_ != NULL);
		delete peer_;
		peer_ = NULL;

		delete this;
	}

	void splice_complete(Event e)
	{
		splice_action_->cancel();
		splice_action_ = NULL;

		switch (e.type_) {
		case Event::EOS:
			DEBUG(log_) << "Peer exiting normally.";
			break;
		case Event::Error:
			ERROR(log_) << "Peer exiting with error: " << e;
			break;
		default:
			ERROR(log_) << "Peer exiting with unknown event: " << e;
			break;
		}

	        ASSERT(log_, splice_ != NULL);
		delete splice_;
		splice_ = NULL;

		if (receive_action_ != NULL) {
			INFO(log_) << "Peer exiting while waiting for a packet.";

			receive_action_->cancel();
			receive_action_ = NULL;
		}

		ASSERT(log_, pipe_ != NULL);
		delete pipe_;
		pipe_ = NULL;

		ASSERT(log_, close_action_ == NULL);
		SimpleCallback *cb = callback(this, &SSHConnection::close_complete);
		close_action_ = peer_->close(cb);
	}
};

class SSHServer : public SimpleServer<TCPServer> {
public:
	SSHServer(SocketAddressFamily family, const std::string& interface)
	: SimpleServer<TCPServer>("/ssh/server", family, interface)
	{ }

	~SSHServer()
	{ }

	void client_connected(Socket *client)
	{
		new SSHConnection(client);
	}
};


int
main(void)
{
	new SSHServer(SocketAddressFamilyIP, "[::]:2299");
	event_main();
}
