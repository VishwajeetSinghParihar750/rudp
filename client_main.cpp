#include "client/client_setup.hpp"

int main()
{

    auto client = create_client("127.0.0.1", "3003");
    client->add_channel(1, channel_type::RELIABLE_ORDERED_CHANNEL);

    while (true)
    {

        std::string tosend;
        std::cout << "waiting for user message : ";
        std::cin >> tosend;

        int nsent = client->write_to_channel(1, tosend.c_str(), tosend.size());
        std::cout << nsent << "chars sent " << std::endl;
    }
}