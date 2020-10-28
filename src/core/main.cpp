#include <boost/asio.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/log/trivial.hpp>

int main(int argc, char* argv[])
{
    boost::asio::io_service io;
    boost::asio::signal_set signals(io, SIGINT, SIGTERM);

    signals.async_wait(
        [](boost::system::error_code ec, int signal)
        {
            BOOST_LOG_TRIVIAL(info) << "recv signal " << signal;

            if (ec)
            {
                BOOST_LOG_TRIVIAL(error) << "ec";
            }
        });

    io.run();

    return 0;
}
