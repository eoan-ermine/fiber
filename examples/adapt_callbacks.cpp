//          Copyright Nat Goodspeed 2015.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#include <boost/fiber/all.hpp>
#include <thread>
#include <chrono>
#include <iostream>
#include <sstream>
#include <exception>
#include <tuple>                    // std::tie()
#include <cassert>

/*****************************************************************************
*   example async API
*****************************************************************************/
class AsyncAPI
{
public:
    // constructor acquires some resource that can be read and written
    AsyncAPI();

    // callbacks accept an int error code; 0 == success
    typedef int errorcode;

    // write callback only needs to indicate success or failure
    void init_write(const std::string& data,
                    const std::function<void(errorcode)>& callback);

    // read callback needs to accept both errorcode and data
    void init_read(const std::function<void(errorcode, const std::string&)>&);

    // ... other operations ...
    void inject_error(errorcode ec);

private:
    std::string data_;
    errorcode injected_;
};

/*****************************************************************************
*   fake AsyncAPI implementation... pay no attention to the little man behind
*   the curtain...
*****************************************************************************/
AsyncAPI::AsyncAPI():
    injected_(0)
{}

void AsyncAPI::inject_error(errorcode ec)
{
    injected_ = ec;
}

void AsyncAPI::init_write(const std::string& data,
                          const std::function<void(errorcode)>& callback)
{
    // make a local copy of injected_
    errorcode injected(injected_);
    // reset it synchronously with caller
    injected_ = 0;
    // update data_ (this might be an echo service)
    if (! injected)
        data_ = data;
    // Simulate an asynchronous I/O operation by launching a detached thread
    // that sleeps a bit before calling completion callback. Echo back to
    // caller any previously-injected errorcode.
    std::thread([injected, callback](){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        callback(injected);
    }).detach();
}

void AsyncAPI::init_read(const std::function<void(errorcode, const std::string&)>& callback)
{
    // make a local copy of injected_
    errorcode injected(injected_);
    // reset it synchronously with caller
    injected_ = 0;
    // local copy of data_ so we can capture in lambda
    std::string data(data_);
    // Simulate an asynchronous I/O operation by launching a detached thread
    // that sleeps a bit before calling completion callback. Echo back to
    // caller any previously-injected errorcode.
    std::thread([injected, callback, data](){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        callback(injected, data);
    }).detach();
}

/*****************************************************************************
*   adapters
*****************************************************************************/
// helper function used in a couple of the adapters
std::runtime_error make_exception(const std::string& desc, AsyncAPI::errorcode);

AsyncAPI::errorcode write_ec(AsyncAPI& api, const std::string& data)
{
    boost::fibers::promise<AsyncAPI::errorcode> promise;
    boost::fibers::future<AsyncAPI::errorcode> future(promise.get_future());
    // We can confidently bind a reference to local variable 'promise' into
    // the lambda callback because we know for a fact we're going to suspend
    // (preserving the lifespan of both 'promise' and 'future') until the
    // callback has fired.
    api.init_write(data,
        [&promise](AsyncAPI::errorcode ec){
            promise.set_value(ec);
        });
    return future.get();
}

void write(AsyncAPI& api, const std::string& data)
{
    AsyncAPI::errorcode ec = write_ec(api, data);
    if (ec)
        throw make_exception("write", ec);
}

std::pair<AsyncAPI::errorcode, std::string>
read_ec(AsyncAPI& api)
{
    typedef std::pair<AsyncAPI::errorcode, std::string> result_pair;
    boost::fibers::promise<result_pair> promise;
    boost::fibers::future<result_pair> future(promise.get_future());
    // We promise that both 'promise' and 'future' will survive until our
    // lambda has been called.
    api.init_read(
        [&promise](AsyncAPI::errorcode ec, const std::string& data){
            promise.set_value(result_pair(ec, data));
        });
    return future.get();
}

std::string read(AsyncAPI& api)
{
    boost::fibers::promise<std::string> promise;
    boost::fibers::future<std::string> future(promise.get_future());
    // Both 'promise' and 'future' will survive until our lambda has been
    // called.
    api.init_read(
        [&promise](AsyncAPI::errorcode ec, const std::string& data){
            if (! ec)
                promise.set_value(data);
            else
                promise.set_exception(std::make_exception_ptr(make_exception("read", ec)));
        });
    return future.get();
}

/*****************************************************************************
*   helpers
*****************************************************************************/
std::runtime_error make_exception(const std::string& desc, AsyncAPI::errorcode ec)
{
    std::ostringstream buffer;
    buffer << "Error in AsyncAPI::" << desc << "(): " << ec;
    return std::runtime_error(buffer.str());
}

/*****************************************************************************
*   driving logic
*****************************************************************************/
int main(int argc, char *argv[])
{
    AsyncAPI api;

    // successful write(): prime AsyncAPI with some data
    write(api, "abcd");
    // successful read(): retrieve it
    assert(read(api) == "abcd");

    // successful write_ec()
    assert(write_ec(api, "efgh") == 0);

    // write_ec() with error
    api.inject_error(1);
    assert(write_ec(api, "ijkl") == 1);

    // write() with error
    std::string thrown;
    api.inject_error(2);
    try
    {
        write(api, "mnop");
    }
    catch (const std::exception& e)
    {
        thrown = e.what();
    }
    assert(thrown == make_exception("write", 2).what());

    // successful read_ec()
    AsyncAPI::errorcode ec;
    std::string data;
    std::tie(ec, data) = read_ec(api);
    assert(! ec);
    assert(data == "efgh");         // last successful write_ec()

    // read_ec() with error
    api.inject_error(3);
    std::tie(ec, data) = read_ec(api);
    assert(ec == 3);
    // 'data' in unspecified state, don't test

    // read() with error
    thrown.clear();
    api.inject_error(4);
    try
    {
        data = read(api);
    }
    catch (const std::exception& e)
    {
        thrown = e.what();
    }
    assert(thrown == make_exception("read", 4).what());

    return EXIT_SUCCESS;
}
