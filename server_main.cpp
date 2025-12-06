
#include "server/server_setup.hpp"
#include "server/types.hpp"

int main()
{
    auto server = create_server();
    server->start_server();

    client_id clid;
    channel_id chid;
    char buf[1000];

    server->read_from_channel_blocking(chid, clid, buf, 1000);

    return 0;
}