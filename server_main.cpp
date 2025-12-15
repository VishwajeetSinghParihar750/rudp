#include "server/server_setup.hpp"

int main()
{
    auto server = create_server("3003");
    server->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);

    while (true)
    {
        char buf[100];
        int len = 100;

        channel_id ch_id;

        client_id cl_id;
        // std::cout << 1 << std::endl;
        std::cout << "waiting for message from client " << std::endl;
        int nread = server->read_from_channel_blocking(ch_id, cl_id, buf, len);
        if (nread > 0)
        {
            std::cout << "got message on channel : " << ch_id << ", client id  : " << cl_id << " = ";
            std::cout.write(buf, nread);
            std::cout << std::endl;
        }
    }
}
