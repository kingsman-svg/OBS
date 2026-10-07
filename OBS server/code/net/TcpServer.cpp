#include "TcpServer.h"
#include "Acceptor.h"
#include "EventLoop.h"
#include <utility>

namespace obs::net {
// Server 是活跃连接的主要所有者。业务只需要配置回调，无需手动 accept/close。
TcpServer::TcpServer(EventLoop &loop, std::string address, std::uint16_t port)
    : m_loop(loop), m_address(std::move(address)), m_port(port) {
    loop.assertInLoopThread();
}
TcpServer::~TcpServer() {
    stop();
}
void TcpServer::setConnectionCallback(ConnectionCallback cb) {
    m_loop.assertInLoopThread();
    m_connectionCallback = std::move(cb);
}
void TcpServer::setMessageCallback(TcpConnection::MessageCallback cb) {
    m_loop.assertInLoopThread();
    m_messageCallback = std::move(cb);
}
void TcpServer::setDisconnectCallback(DisconnectCallback cb) {
    m_loop.assertInLoopThread();
    m_disconnectCallback = std::move(cb);
}
void TcpServer::setErrorCallback(ErrorCallback cb) {
    m_loop.assertInLoopThread();
    m_errorCallback = std::move(cb);
}
std::uint16_t TcpServer::port() const {
    m_loop.assertInLoopThread();
    return m_port;
}
std::size_t TcpServer::connectionCount() const {
    m_loop.assertInLoopThread();
    return m_connections.size();
}
void TcpServer::start() {
    // 先在局部对象上完成初始化，成功后才发布 m_acceptor，失败时 RAII 自动回滚。
    m_loop.assertInLoopThread();
    if (m_acceptor)
        return;
    auto acceptor = std::make_shared<Acceptor>(m_loop, m_address, m_port);
    acceptor->setAcceptCallback([this](UniqueFd socket) { onAccept(std::move(socket)); });
    acceptor->setErrorCallback([this](int error) {
        const auto callback = m_errorCallback;
        if (callback)
            callback(error);
    });
    acceptor->start();
    m_port = acceptor->port();
    m_acceptor = std::move(acceptor);
}
void TcpServer::onAccept(UniqueFd socket) {
    // 使用独立递增 id，而非 fd 作为 map 键，关闭回调只删除属于自己的连接记录。
    const auto id = m_nextId++;
    auto connection = std::make_shared<TcpConnection>(m_loop, std::move(socket));
    connection->setMessageCallback(m_messageCallback);
    connection->setCloseCallback(
        [this, id](const TcpConnection::Ptr &conn, const std::string &reason) {
            m_connections.erase(id);
            const auto callback = m_disconnectCallback;
            if (callback)
                callback(conn, reason);
        });
    connection->establish();
    // 插入失败时也必须先解除 epoll 注册，再释放连接。
    try {
        m_connections.emplace(id, connection);
    } catch (...) {
        connection->setCloseCallback({});
        connection->forceClose();
        throw;
    }
    const auto callback = m_connectionCallback;
    if (callback)
        callback(connection);
}
void TcpServer::stop() {
    // 整体移出连接集合后再逐一关闭，避免释放过程中修改正在遍历的 map。
    m_loop.assertInLoopThread();
    if (m_acceptor)
        m_acceptor->stop();
    m_acceptor.reset();
    auto connections = std::move(m_connections);
    m_connections.clear();
    for (auto &[id, connection] : connections) {
        (void)id;
        // 不再回调 Server，确保连接可由外部 shared_ptr 延长生命周期。
        connection->setCloseCallback({});
        connection->setMessageCallback({});
        connection->forceClose();
    }
}
} // namespace obs::net
