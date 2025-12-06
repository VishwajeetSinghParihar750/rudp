
#include "client/connection_setup.hpp"
#include "client/types.hpp"

int main()
{
    auto connection = create_connection("127.0.0.0", "4004");
    connection->connect();

    // connection->
    client_id clid;
    channel_id chid;
    char buf[1000];

    return 0;
}