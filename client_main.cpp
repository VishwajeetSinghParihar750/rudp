#include "client/client_setup.hpp"

void reader(auto client)
{
    char buf[80000];
    int len = 80000;

    while (true)
    {
        channel_id ch_id;
        int nread = client->read_from_channel_blocking(ch_id, buf, len);
        if (nread > 0)
        {
            std::cout << "got from server : " << nread << " chars ";
            std::cout.write(buf, nread);
            std::cout << std::endl;
        }
    }
}

int main()
{

    auto client = create_client("127.0.0.1", "3003");
    client->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);

    std::jthread readder([&](std::stop_token stoken)
                         { reader(client); });

    while (true)
    {

        std::string tosend;
        std::cout << "waiting for user message : ";
        std::cin >> tosend;

        int nsent = client->write_to_channel(1, tosend.c_str(), tosend.size());
        std::cout << nsent << "chars sent " << std::endl;
    }
}