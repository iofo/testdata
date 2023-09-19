#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/http.hpp>
#include <boost/process.hpp>
#include <boost/process/async.hpp>
#include <iomanip>
#include <iostream>
#include <zlib.h>
#include <string.h>
#include <fstream>
#include <iostream>

namespace net     = boost::asio;
namespace ssl     = net::ssl;
namespace beast   = boost::beast;
namespace http    = beast::http;
namespace process = boost::process;

using boost::system::error_code;
using boost::system::system_error;
using net::ip::tcp;
using stream = ssl::stream<tcp::socket>;

auto ssl_context() {
    ssl::context ctx{ssl::context::sslv23};
    ctx.set_default_verify_paths();
    ctx.set_verify_mode(ssl::verify_peer);
    return ctx;
}

void connect_https(stream& s, std::string const& host, tcp::resolver::iterator eps) {
    net::connect(s.lowest_layer(), eps);
    s.lowest_layer().set_option(tcp::no_delay(true));

    if (!SSL_set_tlsext_host_name(s.native_handle(), host.c_str())) {
        throw system_error{ { (int)::ERR_get_error(), net::error::get_ssl_category() } };
    }
    s.handshake(stream::handshake_type::client);
}

auto get_request(std::string const& host, std::string const& path) {
    using namespace http;
    request<string_body> req;
    req.version(11);
    req.method(verb::get);
    req.target("https://" + host + path);
    req.set(field::user_agent, "test");
    req.set(field::host, host);
    std::cout << req << std::endl;
    return req;
}

struct Stream
{
    z_stream zstr;
    http::response_parser<http::buffer_body> response_reader;

    beast::flat_buffer read_buffer;
    beast::flat_buffer out_buffer;
    ssl::stream<tcp::socket> socket;
    net::io_context & io_;
    Stream(net::io_context & io, ssl::context & ctx) 
        : socket(io, ctx), io_(io)
    {
        response_reader.body_limit((std::numeric_limits<std::uint64_t>::max)());
        memset(&zstr,0,sizeof(zstr));
        inflateInit2(&zstr, 15 + 16);
    }

    bool start(const std::string & host,const std::string & url)
    {
        connect_https(socket, host, tcp::resolver{io_}.resolve(host, "https"));
        http::write(socket, get_request(host, url));
        http::read_header(socket, read_buffer, response_reader);
        if (response_reader.get().result_int() != 200)
        {
            std::cout << "BAD RESPONSE" << response_reader.get().result_int() << std::endl;
            for(auto & h : response_reader.get().base())
            {
                std::cout << h.name_string() << " : " << h.value()  << std::endl;
            }
            return false;
        }
        else
        {
            std::cout << "GOOD RESPONSE " << read_buffer.size() << " " << std::string_view((char*)net::buffer_sequence_begin(read_buffer.data())->data(),read_buffer.size()) << std::endl;
            return true;
        }
    }
    
    size_t nl_pos_ = 0;
    
    bool getline(std::string_view & v)
    {
        if (nl_pos_)
        {
            v = std::string_view((char*)boost::beast::buffers_front(out_buffer.data()).data(),nl_pos_);
            return true;
        }
        return false;
    }
    
    bool consume_line()
    {
        std::cout << "consume line " <<  nl_pos_ << " " << out_buffer.size() << std::endl;
        out_buffer.consume(nl_pos_+1);	// +1 to consume the new line also
        nl_pos_ = 0;
        std::cout << "consume line outbuffer.size after consume=" << out_buffer.size() << std::endl;
        if (out_buffer.size() == 0)
        {
            std::cout << "BUF SIZE IS ZERO read more data" << std::endl;
            out_buffer.clear();
            start_read_data();
            return false;
        }
        uint8_t * buf_start = (uint8_t*)net::buffer_sequence_begin(out_buffer.data())->data();        
        nl_pos_ = find_new_line(0,buf_start,buf_start+out_buffer.size());
        if (nl_pos_ == 0)
        {
            std::cout << "Didnt find new line so read more data " << std::endl;
            start_read_data();
            return false;
        }
        return true;
    }
    
    size_t find_new_line(size_t cur_end, uint8_t * b,uint8_t * e)
    {
        auto nl = std::find(b,e,'\n');
        if (nl != e)
        {
            auto nle = std::distance(b,nl);
            std::cout << "FOUND NEW LINE LENGTH = " << cur_end + nle << " (cur_end=" << cur_end << " nl=" << nle << " ) " << std::endl;
            return cur_end + nle;
        }
        else	
            return 0;
    }

    std::array<uint8_t,1024>  buf_;
    
    void start_read_data()
    {
        std::cout << "Calling readsome " << read_buffer.size() << " " << read_buffer.capacity() << std::endl;

        response_reader.get().body().data = buf_.data();
        response_reader.get().body().size = buf_.size();

        http::async_read(socket, read_buffer, response_reader, [this](error_code ec, size_t bytes_transferred) 
                {
                    size_t bsz = response_reader.get().body().size;
                    size_t rsz = buf_.size() - bsz;

                    std::cout << "async_read returned is_done=" << response_reader.is_done() << "bytes_transferred=" << bytes_transferred << " " << ec.message() << " bsz=" << bsz << " rsz=" << rsz  << std::endl;
                    uint8_t * in_start = (uint8_t*)buf_.data();
                    zstr.next_in = in_start;
                    size_t in_size = rsz; //net::buffer_sequence_begin(read_buffer.data())->size();
                    zstr.avail_in = in_size;
                    size_t out_size = buf_.size() * 3;
                    uint8_t * out_start = (uint8_t*)out_buffer.prepare(out_size).data();
                    zstr.next_out = out_start;
                    zstr.avail_out = out_size;
                    int err = 0;
                    printf("IN in=%p ins=%d out=%p outs=%d\n",zstr.next_in,zstr.avail_in,zstr.next_out,zstr.avail_out);
                    err = inflate(&zstr,Z_SYNC_FLUSH);
                    printf("OUT in=%p ins=%d out=%p outs=%d\n",zstr.next_in,zstr.avail_in,zstr.next_out,zstr.avail_out);
                    std::cout << "inflate ret=" << err << " " << std::endl;
                    if ( err != 0)
                    {
                        std::cout << "INFLATE ERRROR" << std::endl;
                        return;
                    }

                    size_t out_bytes = out_size - zstr.avail_out;

                    if (out_bytes == 0)
                    {
                        std::cout << "DIDNT MANAGED TO UNCOMPRESSS ANY - KICK OFF READ MORE " << std::endl;
                        out_buffer.commit(out_bytes);
                        start_read_data();
                        return;
                    }

                    auto out_end = out_start+out_bytes;
                    nl_pos_ = find_new_line(out_buffer.size(),out_start,out_end);                    

                    out_buffer.commit(out_bytes); // after call to commit all buffers prepared can become invalid, search for NL in this chunk before commiting
                    if (nl_pos_ == 0)
                    {
                        // if we didnt find a new line then read more data
                        start_read_data();
                    }

                    std::cout << "NOT SPAWNING NEW READ YET " << std::endl;

                } );
    }
};

int main(int argc, char ** argv) 
{
  net::io_context io; // main thread does all io
  auto ctx = ssl_context();

  std::string host = "raw.githubusercontent.com";

//https://raw.githubusercontent.com/iofo/testdata/master/test1.gz  
  std::vector<std::string> paths = {"/iofo/testdata/master/test1.gz", "/iofo/testdata/master/test2.gz"};

  std::vector<std::shared_ptr<Stream> > streams;
  for(auto & p : paths)
  {
      auto s = std::make_shared<Stream>(io,ctx);
      streams.push_back(s);
      if (!s->start(host,p))
      {
          return 1;
      }	
      s->start_read_data();
  }

  for(;;)
  {
    std::cout << "CALLING IO RUN" << std::endl;
    io.run();
    std::cout << "IO RUN RETURN" << std::endl;
    std::string_view v;
    for(auto & s : streams)
    {
        if (s->getline(v))
        {
            std::cout << "LINE= " << v.size() << " " << std::string_view(v.data(),std::min(v.size(),size_t(50))) << std::endl;
            if (!s->consume_line())
            {
                break;
            }
        }
    }
  }
}
