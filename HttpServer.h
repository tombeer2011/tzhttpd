/*-
 * Copyright (c) 2018 TAO Zhijiang<taozhijiang@gmail.com>
 *
 * Licensed under the BSD-3-Clause license, see LICENSE for full information.
 *
 */

#ifndef __TZHTTPD_HTTP_SERVER_H__
#define __TZHTTPD_HTTP_SERVER_H__


#include <libconfig.h++>

#include <set>
#include <map>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <algorithm>

#include <boost/noncopyable.hpp>

#include "EQueue.h"
#include "ThreadPool.h"
#include "AliveTimer.h"

#include "HttpHandler.h"
#include "HttpParser.h"

namespace tzhttpd {

class TCPConnAsync;

typedef TCPConnAsync ConnType;
typedef std::shared_ptr<ConnType> ConnTypePtr;
typedef std::weak_ptr<ConnType>   ConnTypeWeakPtr;

class HttpServer;
class HttpConf {

    friend class HttpServer;

private:
    bool load_config(const libconfig::Config& cfg);

private:
    std::string bind_addr_;
    unsigned short listen_port_;
    std::set<std::string> safe_ip_;

    int backlog_size_;
    int io_thread_number_;

    // 加载、更新配置的时候保护竞争状态
    std::mutex        lock_;

    int conn_time_out_;
    int conn_time_out_linger_;

    int ops_cancel_time_out_;  // sec 会话超时自动取消ops

    bool              http_service_enabled_;  // 服务开关
    int64_t           http_service_speed_;
    volatile int64_t  http_service_token_;

    bool check_safe_ip(const std::string& ip) {
        std::lock_guard<std::mutex> lock(lock_);
        return ( safe_ip_.empty() || (safe_ip_.find(ip) != safe_ip_.cend()) );
    }

    bool get_http_service_token() {
        std::lock_guard<std::mutex> lock(lock_);

        // if (!http_service_enabled_) {
        //    tzhttpd_log_alert("http_service not enabled ...");
        //    return false;
        // }

        if (http_service_speed_ == 0) // 没有限流
            return true;

        if (http_service_token_ <= 0) {
            tzhttpd_log_alert("http_service not speed over ...");
            return false;
        }

        -- http_service_token_;
        return true;
    }

    void withdraw_http_service_token() {    // 支持将令牌还回去
        std::lock_guard<std::mutex> lock(lock_);
        ++ http_service_token_;
    }

    void feed_http_service_token(){
        std::lock_guard<std::mutex> lock(lock_);
        http_service_token_ = http_service_speed_;
    }

    std::shared_ptr<boost::asio::deadline_timer> timed_feed_token_;
    void timed_feed_token_handler(const boost::system::error_code& ec);

};  // end class HttpConf



class HttpServer : public boost::noncopyable,
                   public std::enable_shared_from_this<HttpServer> {

    friend class TCPConnAsync;  // can not work with typedef, ugly ...

public:

    /// Construct the server to listen on the specified TCP address and port
    explicit HttpServer(const std::string& cfgfile, const std::string& instance_name);
    bool init();

    int update_run_cfg(const libconfig::Config& cfg);
    void service();

private:
    const std::string instance_name_;
    io_service io_service_;

    // 侦听地址信息
    ip::tcp::endpoint ep_;
    std::unique_ptr<ip::tcp::acceptor> acceptor_;

    std::shared_ptr<boost::asio::deadline_timer> timed_checker_;
    void timed_checker_handler(const boost::system::error_code& ec);

    HttpConf conf_;
    HttpHandler handler_;

    void do_accept();
    void accept_handler(const boost::system::error_code& ec, SocketPtr ptr);

    AliveTimer<ConnType>    conns_alive_;

public:

    int ops_cancel_time_out() const {
        return conf_.ops_cancel_time_out_;
    }

    int conn_add(ConnTypePtr p_conn) {
        conns_alive_.INSERT(p_conn);
        return 0;
    }

    void conn_touch(ConnTypePtr p_conn) {
        conns_alive_.TOUCH(p_conn);
    }

    void conn_drop(ConnTypePtr p_conn) {
        conns_alive_.DROP(p_conn);
    }

    void conn_drop(ConnType* ptr) {
        conns_alive_.DROP(ptr);
    }

    int conn_destroy(ConnTypePtr p_conn);


    int register_http_get_handler(std::string uri_regex, const HttpGetHandler& handler) {
        return handler_.register_http_get_handler(uri_regex, handler);
    }
    int register_http_post_handler(std::string uri_regex, const HttpPostHandler& handler) {
        return handler_.register_http_post_handler(uri_regex, handler);
    }

    int find_http_get_handler(std::string uri, HttpGetHandler& handler) {
        if (!conf_.http_service_enabled_) {
            uri = handler_.pure_uri_path(uri);
            if (boost::iequals(uri, "/manage")) {
                handler = http_handler::manage_http_get_handler;
                return 0;
            }

            tzhttpd_log_err("http_service_enabled_ == false, reject request GET %s ... ", uri.c_str());
            return -1;
        }
        return handler_.find_http_get_handler(uri, handler);
    }

    int find_http_post_handler(std::string uri, HttpPostHandler& handler) {
        if (!conf_.http_service_enabled_) {
            tzhttpd_log_err("http_service_enabled_ == false, reject request POST %s ... ", uri.c_str());
            return -1;
        }

        return handler_.find_http_post_handler(uri, handler);
    }


public:
    ThreadPool io_service_threads_;
    void io_service_run(ThreadObjPtr ptr);  // main task loop
    int io_service_stop_graceful();
    int io_service_join();
};


} // end namespace tzhttpd

#endif //__TZHTTPD_HTTP_SERVER_H__
