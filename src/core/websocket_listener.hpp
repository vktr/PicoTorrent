#include <memory>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

namespace pt::core
{
    class websocket_listener : public std::enable_shared_from_this<websocket_listener>
    {
    public:
        websocket_listener(
            boost::asio::io_context& ioc,
            boost::asio::ip::tcp::endpoint endpoint);

        void run();

    private:
        void do_accept();
        void on_accept(
            boost::beast::error_code ec,
            boost::asio::ip::tcp::socket socket);

        boost::asio::io_context& m_io;
        boost::asio::ip::tcp::acceptor m_acceptor;
    };
}
