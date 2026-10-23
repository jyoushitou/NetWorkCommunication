/// @file        NetServer.cpp
/// @brief       TCP 服务器与连接会话实现（Session / Server）
/// @author      jyoushitou
/// @date        2026-09-16
/// @copyright   Copyright (c) 2026
/// @license     MIT License（详见仓库根目录的 LICENSE 文件）

#include "NetServer.h"

#include "Utils.h"

#include <algorithm>
#include <chrono>
#include <thread>

namespace Net
{
    namespace Server
    {
        namespace
        {
            /// @brief 优雅关闭的宽限期（秒）：超过则强制断开
            /// @details 对端不读数据时 async_write 永远完不成，优雅关闭会一直挂着
            constexpr long long kCloseGraceSeconds = 30;
        } // namespace

        /// @brief      构造函数
        /// @details    session的构造：接管 socket，保存业务回调、超时时间与发送上限，并初始化活动时间
        /// @param[in] io 连接的io_context
        /// @param[in] sock 连接的socket（按值传入，内部 move 给基类）
        /// @param[in] HF 消息回调函数（可为空，为空时收到消息即关闭会话）
        /// @param[in] timeout 空闲超时时间（秒）
        /// @param[in] maxSendBufLen 发送队列允许积压的最大字节数
        /// @note       timeout 传 0 时回退为默认 60 秒
        Session::Session(boost::asio::io_context& io, boost::asio::ip::tcp::socket sock,
                         std::shared_ptr<HandleFunction> HF, long long timeout, int maxSendBufLen)
            : Connection(std::move(sock), io, maxSendBufLen)
        {
            // 初始化停止状态置为false
            stop = false;
            // 给回调函数赋值
            this->HF = HF;
            // 非正数（含负数）回退为默认 60 秒：负数会让 timeOut() 恒为真，会话一进巡检就被秒关
            if (timeout <= 0)
            {
                Utils::Out::outErr("timeout 非法（" + std::to_string(timeout) + "），已回退为默认 60 秒");
            }
            this->timeout = (timeout > 0) ? timeout : 60;

            // 初始化时间
            updateTime();
        }

        /// @brief      业务处理
        /// @details    收到消息后的具体业务实现，由基类回调触发：输出消息、调用回调函数并把响应回复给客户端；
        ///             消息内容等于 kCloseCommand 时直接关闭会话
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @warning    禁止在回调内部长时间阻塞，会阻塞asio事件循环
        /// @note       未设置回调函数（HF 为空）时也会关闭会话
        void Session::recvToWork(const unsigned long long msg_id, std::string msg)
        {
            // 输出收到的消息
            Utils::Out::outNetMsg(msg_id, "收到客户端消息:" + msg);

            // 获得自身指针
            auto self = getSession();

            if (msg == kCloseCommand)
            {
                // 收到退出信号
                // 关闭这次会话，最后析构交给Server的监控线程
                closeSession();
                return;
            }

            // 判断是否获得了回调函数指针
            if (HF != nullptr)
            {
                // HF必须要可以有回复消息
                (*HF)(self, msg_id, msg);
                return;
            }
            else
            {
                Utils::Out::outErr("未设置消息回调函数！");

                closeSession();
                return;
            }
        }

        /// @brief      获取自身的指针
        /// @details    把 shared_from_this() 向下转型为 Session 的 shared_ptr（static_pointer_cast）
        /// @return     Session 的 shared_ptr（static_pointer_cast 不做运行时校验，正常不会为空）
        /// @warning    仅在对象由 shared_ptr 持有时可用，否则 shared_from_this() 抛出 std::bad_weak_ptr
        std::shared_ptr<Session> Session::getSession()
        {
            return std::static_pointer_cast<Session>(shared_from_this());
        }

        /// @brief      回复消息
        /// @details    线程安全的发送入口（内部 post 到 IO 线程），向该客户端回复一条消息
        /// @param[in] msg_id 消息全局唯一ID
        /// @param[in] msg 消息序列化字符串
        /// @warning    禁止在回调或事件循环中长时间阻塞；msg 为空时只打印错误日志并丢弃
        /// @note
        bool Session::reply(unsigned long long msg_id, const std::string& msg)
        {
            if (msg.empty())
            {
                Utils::Out::outErr("消息为空或者消息id错误");
                return false;
            }

            if (msg.size() > static_cast<std::size_t>(MAX_LENGTH))
            {
                Utils::Out::outErr("消息过长（" + std::to_string(msg.size()) + " > " + std::to_string(MAX_LENGTH) +
                                   "），已丢弃");
                return false;
            }

            Utils::Out::outMsg("正在发送回复数据");

            // 发送数据
            toSend(msg_id, msg);

            return true;
        }

        /// @brief      关闭函数
        /// @details    置位停止标志 stop，并调用基类的优雅关闭（先把发送队列发完再断开）
        /// @warning    线程安全，实际关闭动作在 IO 线程内完成
        /// @note       对象析构由 Server 的监控线程在回收 sessions 时完成
        void Session::closeSession()
        {
            stop = true;
            // 记下请求关闭的时刻，供 needForceClose() 计算优雅关闭已经等了多久
            closeReqTime.store(Utils::Time::nowTime());
            // 使用基类的关闭函数
            stopToClose();
        }

        /// @brief 同步硬关闭（不清队列、不等待）
        /// @warning **只在 IO 线程已停止时使用**（析构阶段 / ioc.stopped()）
        /// @note 内部走基类 errToClose()，幂等
        void Session::closeHard()
        {
            stop.store(true);
            // 错误的关闭
            errToClose();
        }

        /// @brief 重写关闭最后的流程
        /// @details 用于在session最后关闭时销毁其他动态数据
        /// @warning 当前是空实现：Session 没有需要额外释放的动态资源
        /// @note 由基类 socketClose() 保证只回调一次
        void Session::derivedToClose()
        {
        }

        /// @brief      更新连接时间
        /// @details    刷新 lastTime，供超时巡检使用，避免空闲连接长期占线
        /// @note       重写基类虚函数；每次读写操作后由基类调用，也可手动调用
        void Session::updateTime()
        {
            // 更新成员变量 lastTime（不能写成局部变量，否则只是遮蔽，等于没更新）
            lastTime = Utils::Time::nowTime();
        }

        /// @brief      判断是否超时
        /// @details    以最后一次活动时间 lastTime 为基准，与 timeout 比较
        /// @return     true超时，false未超时
        /// @note       由监控线程在 IO 线程内调用
        bool Session::timeOut()
        {
            return timeout < Utils::Time::computeTime(lastTime);
        }

        /// @brief      是否已关闭
        /// @details    判断连接是否已进入关闭流程或 socket 已断开
        /// @return     已关闭返回 true，否则返回 false
        /// @warning    仅在 IO 线程内调用，避免跨线程读取非原子状态
        /// @note
        bool Session::isClosed()
        {
            // 仅在 IO 线程内调用（socket.is_open() 非线程安全）
            return stop.load() || closing.load() || !socket.is_open();
        }

        /// @brief 是否超过优雅关闭宽限期
        /// @details 已请求关闭、但 socket 还没关，且距请求关闭已超过 graceSec 秒
        /// @warning 仅在 IO 线程内调用
        /// @note
        bool Session::needForceClose(long long graceSec)
        {
            if (!stop.load() || !socket.is_open())
            {
                return false;
            }

            const time_t req = closeReqTime.load();
            return req != 0 && Utils::Time::computeTime(req) > graceSec;
        }

        /// @brief 强制关闭（不等发送队列）
        /// @warning 仅在 IO 线程内调用
        /// @note
        void Session::forceClose()
        {
            Utils::Out::outErr("会话优雅关闭超过宽限期，强制断开");
            errToClose();
        }

        /// @brief  是否在忙（正在处理请求，含等待业务异步回包）
        /// @return 忙返回 true
        /// @note   供空闲巡检判断，避免误杀正在等业务回包的会话
        bool Session::busing() const
        {
            return busy.load(std::memory_order_relaxed);
        }

        /// @brief 忙状态是否已超过上限（仅在 IO 线程调用）
        /// @param limitSec 当前时间
        /// @return
        bool Session::busyTimedOut(long long limitSec) const
        {
            // 判断是否在非忙时候
            if (!busy.load(std::memory_order_relaxed))
            {
                return false;
            }
            // 判断是否超时
            return busySince != 0 && Utils::Time::computeTime(busySince) > limitSec;
        }

        //===Server===

        /// @brief      构造函数
        /// @details    服务器的构造，完成 acceptor 的 open / set_option / bind / listen，并保存各项参数
        /// @param[in] io 服务器的io_context
        /// @param[in] ep 监听的本地端点（地址与端口）
        /// @param[in] HF 业务处理回调（回调内调用 session->reply 回复，库不会自动回包）
        /// @param[in] timeOut 会话空闲超时时间（秒）
        /// @param[in] checkTime 监控线程检查会话的周期（秒）
        /// @param[in] maxSendBufLen 每个会话的发送队列允许积压的最大字节数
        /// @warning    须保证 io 的生命周期长于本服务器；非正数入参会回退为默认值（60 / 60 / 10MB）并打印错误日志
        /// @note
        Server::Server(boost::asio::io_context& io, boost::asio::ip::tcp::endpoint ep,
                       std::shared_ptr<HandleFunction> HF, long long timeOut, long long checkTime, int maxSendBufLen,
                       long long sessionTimeOut)
            : ioc(io), ep(ep), acceptor(io)
        {
            // 给智能指针赋值
            this->HF = HF;

            this->timeOut = (timeOut > 0) ? timeOut : 60;
            this->checkTime = (checkTime > 0) ? checkTime : 60;

            // 判断timeOut、checkTime是否合法
            if (timeOut <= 0 || checkTime <= 0)
            {
                Utils::Out::outErr("timeOut / checkTime 非法，已回退为默认 60 秒");
            }

            // 初始化每个 session 的发送队列上限（非正数含负数都回退为 10MB）
            if (maxSendBufLen <= 0)
            {
                Utils::Out::outErr("maxSendBufLen 非法，已回退为默认1024*1024*10");
            }
            this->maxSendBufLen = (maxSendBufLen > 0) ? maxSendBufLen : 1024 * 1024 * 10;

            this->sessionTimeOut = sessionTimeOut;

            // 将运行状态初始化
            running = false;

            // 初始化连接状态
            acceptorConnectionCount = 0;

            Utils::Out::outMsg("服务器对象初始化完成");
        }

        bool Server::openAcceptor(boost::system::error_code& ec)
        {
            if (acceptor.is_open())
            {
                ec.clear();
                return true;
            }

            acceptor.open(ep.protocol(), ec);
            if (ec)
            {
                return false;
            }
            acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), ec);
            if (ec)
            {
                return false;
            }
            acceptor.bind(ep, ec);
            if (ec)
            {
                return false;
            }
            acceptor.listen(boost::asio::socket_base::max_listen_connections, ec);
            if (ec)
            {
                return false;
            }
            return true;
        }

        /// @brief      析构函数
        /// @details    先 Stop() 停服并关闭所有会话，再 join 监控线程
        /// @note       派生类析构会自动调用基类析构；join 后可避免 std::thread 析构时 std::terminate
        Server::~Server()
        {
            // 判断session现在连接数，不影响正常析构
            if (!sessions.empty())
            {
                Utils::Out::outErr("Sessions仍有" + std::to_string(sessions.size()) + "个会话未关闭");
            }

            // 先触发停止：置 running=false 并关闭 acceptor / 会话
            stop();

            // 回收清理线程（Stop 已置 running=false，线程会自行退出循环）
            if (clearSessionThread.joinable())
            {
                clearSessionThread.join();
            }
        }

        /// @brief 启动
        /// @details 置 running、绑定 selfWeak（监控线程靠它保活），随后启动监控线程并开始 accept
        /// @warning 会启动一个监控线程；调用者必须已经用 shared_ptr 持有本对象
        /// @note 幂等：已在运行时只打印提示；Stop() 后重启会先 join 掉上一轮的监控线程
        void Server::start()
        {
            // 判断是否已经启动（幂等：已在运行时只打印提示，绝不能去 join 自己那一轮的线程）
            if (running.load())
            {
                Utils::Out::outMsg("服务器已经在运行监听了");
                return;
            }

            // Stop() 后重启：先回收上一轮的监控线程。
            // 给 joinable 的 std::thread 赋值会直接 std::terminate()，在这里兜住。
            if (clearSessionThread.joinable())
            {
                clearSessionThread.join();
            }

            // 尝试连接
            boost::system::error_code ec;
            if (!openAcceptor(ec))
            {
                // ec 现在按引用传出，异常消息里能看到真实原因（如 address in use）
                Utils::Out::outErr("Server 监听失败：" + ec.message());
                throw boost::system::system_error(ec, "Server监听失败");
            }

            // 置运行运行状态
            running = true;

            // 捕获运行时的错误
            try
            {

                // 此处一定安全
                auto self = shared_from_this();
                // 让清理线程有弱引用可用
                selfWeak = self;
                // 创建监控线程
                clearSessionThread = std::thread(&Server::clearSession, this);
                // 启动监听
                startAccept();
            }
            catch (...)
            {
                // 置空
                running.store(false);
                // 唤醒监控线程关闭
                queueCV.notify_all();
                // 等待监控线程结束
                if (clearSessionThread.joinable())
                {
                    clearSessionThread.join();
                }

                // 抛出异常
                throw;
            }
        }

        /// @brief 关闭监听器与全部会话并清空会话表
        /// @warning 只能在 IO 线程内调用；**例外**：析构阶段 / ioc.stopped() 时
        ///          （那时 IO 线程不跑了，也不会有别的线程动 sessions）
        void Server::closeAll()
        {
            // 尝试关闭acceptor
            boost::system::error_code ec;
            acceptor.close(ec);

            // IO 线程不会再执行任何 handler
            const bool ioDead = ioc.stopped();
            // 单独通知每个session
            for (auto& session : sessions)
            {
                if (!session)
                {
                    continue;
                }
                if (ioDead)
                {
                    session->closeHard();
                }
                else
                {
                    session->closeSession();
                }
            }

            // 清空队列
            sessions.clear();
        }

        /// @brief      开始接受连接
        /// @details    在 io_context 线程中被调用，异步等待并接受客户端连接，
        ///             为每个新连接创建对应的 Session（关闭 NoDelay）后继续接受下一个连接
        /// @warning    须在 io_context 运行后调用；acceptor 被关闭导致的取消错误会静默忽略
        /// @note       accept 失败时等 100ms 重试，服务器已停止则不再重试
        void Server::startAccept()
        {
            // 保活 Server 自身
            auto self = shared_from_this();

            // 判断运行条件
            if (!running.load())
            {
                return;
            }

            // 创建一个 socket 用于 accept
            auto sock = std::make_shared<boost::asio::ip::tcp::socket>(ioc);

            // 异步等待并接受新连接
            acceptor.async_accept(
                *sock,
                [this, self, sock](boost::system::error_code ec)
                {
                    // 服务器已停止：acceptor 被主动关闭导致的取消错误（如 system:995）
                    // 是正常退出流程的一部分，不是真正的错误，静默返回即可
                    if (!running)
                        return;

                    if (!ec)
                    {
                        // Session 继承自 enable_shared_from_this，必须用 shared_ptr 管理
                        // 构造函数按值接收 socket，需 std::move(*sock)
                        auto session = std::make_shared<Session>(ioc, std::move(*sock), HF, timeOut, maxSendBufLen);
                        // 开启 TCP_NODELAY（禁用 Nagle，小包立即发送）
                        session->offNoDelay();
                        // 将session放入sessions里面
                        sessions.push_back(session);
                        // 启动读（继承自 Connection::start()）
                        session->start();

                        this->acceptorConnectionCount = 0;
                        // 继续接受下一个连接
                        startAccept();
                    }
                    else
                    {
                        // 由自身引起的
                        if (ec == boost::asio::error::operation_aborted)
                        {
                            return;
                        }

                        //  监听器已废：重试一万次也不会成功，直接停机
                        if (ec == boost::asio::error::bad_descriptor)
                        {
                            Utils::Out::outErr("监听器已失效，停止 accept：" + ec.message());
                            {
                                std::lock_guard<std::mutex> lock(queueMutex);
                                running.store(false);
                            }
                            // 唤醒监控线程清理所有
                            queueCV.notify_all();
                            return;
                        }
                        // 仅在服务器仍在运行时，才输出真正的 accept 错误
                        Utils::Out::outErr("accept 错误: " + ec.what());

                        if (!running)
                        {
                            return;
                        }

                        acceptorConnectionCount++;

                        const unsigned shift = (std::min)(acceptorConnectionCount, 6u);
                        const long long delayMs = (std::min)(5000LL, 100LL << shift);

                        // 按退避时长重试（100ms → 200ms → … 上限 5s）
                        auto timer = std::make_shared<boost::asio::steady_timer>(ioc, std::chrono::milliseconds(delayMs));
                        timer->async_wait(
                            [this, self, timer](const boost::system::error_code& waitEC)
                            {
                                if (!waitEC || running)
                                {
                                    startAccept();
                                }
                                else
                                {
                                    return;
                                }
                            });
                    }
                });
        }

        /// @brief      监控线程主体
        /// @details    按 checkTime 周期睡眠（可被 Stop() 立即唤醒），醒来后 post 到 io_context：
        ///             对超时会话发送提示并关闭，随后移除已关闭的会话
        /// @warning    在独立线程中运行，禁止直接访问只在 IO 线程内读写的 sessions
        /// @note       每轮都通过 selfWeak 取保活指针，取不到说明 Server 已析构，直接退出
        void Server::clearSession()
        {
            // 服务器运行期间循环巡检（running=false 后退出循环）
            while (running.load())
            {
                {
                    // 创建锁变量
                    std::unique_lock<std::mutex> lock(queueMutex);

                    // 拿到锁后再确认一次运行状态
                    if (!running.load())
                    {
                        break;
                    }

                    // 先在锁内等一轮；被 Stop() 唤醒时立即退出，不必等满周期
                    queueCV.wait_for(lock, std::chrono::seconds(checkTime), [this]() { return !running.load(); });
                    if (!running.load())
                    {
                        break;
                    }
                }

                // 获取其保活指针
                auto self = selfWeak.lock();
                if (!self)
                {
                    return;
                }
                // post 到 io_context，在 IO 线程内清理失效会话（避免跨线程直接改 sessions）
                boost::asio::post(ioc,
                                  [this, self]()
                                  {
                                      // 逐个会话检查
                                      for (auto& it : sessions)
                                      { // 判断是否超时
                                          if (it->timeOut() && !it->isClosed())
                                          {
                                              if (it->busing())
                                              {
                                                  if (it->busyTimedOut(sessionTimeOut))
                                                  {
                                                      Utils::Out::outErr("回包时间过长");
                                                      // 发不发得出去都要关：原来只在发送成功时才关，失败会让这条连接永久占着
                                                      it->reply(1, "bussing timeout");
                                                      it->closeSession();
                                                  }
                                                  continue;
                                              }

                                              try
                                              {
                                                  // 回复提醒消息
                                                  it->reply(0LL, "长时间未发送，关闭连接");
                                              }
                                              catch (const std::exception& e)
                                              {
                                                  Utils::Out::outErr("发送关闭消息出现了错误" + std::string(e.what()));
                                              }
                                              catch (...)
                                              {
                                                  Utils::Out::outErr("未知错误");
                                              }
                                              // 关闭
                                              it->closeSession();
                                          }
                                      }

                                      // 第三轮：优雅关闭宽限期兜底（对端不读时发送队列永远发不完）
                                      for (auto& it : sessions)
                                      {
                                          if (it && it->needForceClose(kCloseGraceSeconds))
                                          {
                                              try
                                              {
                                                  it->forceClose();
                                              }
                                              catch (...)
                                              {
                                                  Utils::Out::outErr("强制关闭会话时出现未知错误");
                                              }
                                          }
                                      }

                                      // 检查是否关闭（socket 真正关闭才回收）
                                      for (auto it = sessions.begin(); it != sessions.end();)
                                      {
                                          if ((*it)->needReclaim())
                                          {
                                              // 避免悬空的迭代器
                                              it = sessions.erase(it);
                                              // 避免后面自增
                                              continue;
                                          }
                                          ++it;
                                      }
                                  });
            }
        }

        /// @brief      停止服务器
        /// @details    置 running=false 并 notify_all 唤醒监控线程，再 post 到 IO 线程关闭 acceptor、逐个关闭会话
        /// @warning    调用后服务器不再接受新连接，须重新构造使用
        /// @note       幂等，可重复调用；Server 尚未 start()（selfWeak 为空）时只置标志
        void Server::stop()
        {
            {
                // 加锁
                std::lock_guard<std::mutex> lock(queueMutex);

                // 无条件置停止标志（幂等；这里绝不做任何"闸门"早退，否则关闭动作会被跳过）
                running = false;
            }

            // 唤醒正在周期等待的监控线程，让它立即退出等待
            queueCV.notify_all();

            // 保活，不保存引用
            auto self = selfWeak.lock();

            // 从未 start() / 正在析构 / IO 线程已停：投递出去也没人执行 → 就地同步关闭
            if (self == nullptr || ioc.stopped())
            {
                closeAll();
                return;
            }

            // post 到 IO 线程执行关闭动作（复用 closeAll，避免两份关闭逻辑漂移）
            boost::asio::post(ioc, [this, self]() { closeAll(); });
        }
    } // namespace Server
} // namespace Net