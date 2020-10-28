#include "websocket_listener.hpp"

using pt::core::websocket_listener;

websocket_listener::websocket_listener(
    boost::asio::io_context& ioc,
    boost::asio::ip::tcp::endpoint endpoint)
    : m_io(ioc),
    m_acceptor(ioc)
{
    boost::beast::error_code ec;

    m_acceptor.open(endpoint.protocol(), ec);

    if(ec)
    {
        throw std::exception(ec.message().c_str());
    }

    m_acceptor.set_option(boost::asio::socket_base::reuse_address(true), ec);

    if(ec)
    {
        throw std::exception(ec.message().c_str());
    }

    m_acceptor.bind(endpoint, ec);

    if(ec)
    {
        throw std::exception(ec.message().c_str());
    }

    // Start listening for connections
    m_acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);

    if(ec)
    {
        throw std::exception(ec.message().c_str());
    }
}

void websocket_listener::run()
{
    do_accept();
}

void websocket_listener::do_accept()
{
    m_acceptor.async_accept(
        boost::asio::make_strand(m_io),
        boost::beast::bind_front_handler(
            &websocket_listener::on_accept,
            shared_from_this()));
}

void websocket_listener::on_accept(boost::beast::error_code ec, boost::asio::ip::tcp::socket socket)
{
    if(ec)
    {
        throw std::exception(ec.message().c_str());
    }
    else
    {
        // Create the session and run it
        // std::make_shared<session>(std::move(socket))->run();
    }

    // Accept another connection
    do_accept();
}
