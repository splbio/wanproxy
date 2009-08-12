#include <sys/socket.h>
#include <netinet/in.h>

#include <common/buffer.h>

#include <event/action.h>
#include <event/callback.h>
#include <event/event_system.h>

#include <io/file_descriptor.h>
#include <io/socket.h>

#include <net/tcp_client.h>

Action *
TCPClient::connect(Socket **socketp, const std::string& name,
		   EventCallback *cb)
{
	Socket *socket = Socket::create(PF_INET, SOCK_STREAM, "tcp");
	ASSERT(socket != NULL);
	*socketp = socket;
	return (socket->connect(name, cb));
}
