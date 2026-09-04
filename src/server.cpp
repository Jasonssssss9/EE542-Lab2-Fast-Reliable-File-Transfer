#include "common.hpp"
#include "file_io.hpp"
#include "protocol.hpp"
#include "reliability.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

struct Options {
    std::string output;
    std::uint16_t port = 0;
    std::uint32_t mtu = 1500;
};

void print_usage(const char* program) {
    /*
     * 参数：
     * - program：argv[0] 提供的服务器程序名称或启动路径。
     *
     * 功能：
     * - 把 server 支持的命令行格式打印到标准错误流。
     * - 应说明必填的 --port、--output 和可选的 --mtu。
     *
     * 返回：
     * - 无返回值。
     */
     std::cerr << "Usage: " << program
              << " --port <port> --output <path> [--mtu <bytes>]\n";
}

std::string next_value(int& index, int argc, char* argv[]) {
    /*
     * 参数：
     * - index：当前命令行选项下标的引用；函数要把它推进到对应 value。
     * - argc：argv 中元素数量。
     * - argv：命令行字符串数组。
     *
     * 功能：
     * - 将 index 增加一并检查下一个字符串是否存在。
     * - 返回当前选项的 value，同时让外层解析循环跳过该 value。
     *
     * 返回：
     * - value 存在时返回对应 std::string。
     * - 选项后缺少 value 时抛出 std::invalid_argument。
     */
     if (++index >= argc) {
        throw std::invalid_argument(std::string("missing value after ") + argv[index - 1]);
    }
    return argv[index];
}

Options parse_options(int argc, char* argv[]) {
    /*
     * 参数：
     * - argc：server 命令行参数数量。
     * - argv：server 命令行参数数组。
     *
     * 功能：
     * - 解析 --port、--output、--mtu 和 --help。
     * - 验证端口在 1～65535 内，并确认 output 和 port 均已提供。
     * - 调用 chunk_size_for_mtu 验证 MTU 能用于当前 FRFT Header。
     * - 未指定 MTU 时保留 Options 中的 1500 默认值。
     *
     * 返回：
     * - 成功时返回完成验证的 Options。
     * - 参数未知、缺失或非法时抛出 std::invalid_argument 或数值转换异常。
     * - --help 应打印帮助并以成功状态结束程序。
     */
     Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "--port") {
            const auto value = std::stoul(next_value(i, argc, argv));
            if (value == 0 || value > 65535) {
                throw std::invalid_argument("port must be between 1 and 65535");
            }
            options.port = static_cast<std::uint16_t>(value);
        } else if (argument == "--output") {
            options.output = next_value(i, argc, argv);
        } else if (argument == "--mtu") {
            options.mtu = static_cast<std::uint32_t>(std::stoul(next_value(i, argc, argv)));
        } else if (argument == "--help") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }

    if (options.output.empty() || options.port == 0) {
        throw std::invalid_argument("--port and --output are required");
    }
    frft::chunk_size_for_mtu(options.mtu);
    return options;
}

bool same_endpoint(const sockaddr_in& left,
                   const sockaddr_in& right) {
    /*
     * 参数：
     * - left：第一个 IPv4 UDP endpoint。
     * - right：第二个 IPv4 UDP endpoint。
     *
     * 功能：
     * - 比较地址族、UDP 端口和 IPv4 地址。
     * - 用于确保活动会话只处理来自已接受 Client 的 datagram。
     *
     * 返回：
     * - endpoint 完全相同时返回 true，否则返回 false。
     */
     return left.sin_family == right.sin_family && left.sin_port == right.sin_port &&
           left.sin_addr.s_addr == right.sin_addr.s_addr;
}

frft::PacketHeader make_header(frft::PacketType type,
                               std::uint32_t session_id,
                               std::uint32_t number) {
    /*
     * 参数：
     * - type：要构造的 FRFT 报文类型。
     * - session_id：当前或待回复会话的 ID。
     * - number：由 type 定义含义的数值，例如 ACK 编号或 total_chunks。
     *
     * 功能：
     * - 创建具有头文件默认 magic、version、header length、flags 和 crc32c
     *   的 PacketHeader。
     * - 设置 type、session_id 和 number。
     *
     * 返回：
     * - 返回填充基础字段的 PacketHeader。
     */
     frft::PacketHeader header;
     header.type = type;
     header.session_id = session_id;
     header.number = number;
     return header;
}

void send_packet(int socket_fd,
                 const sockaddr_in& destination,
                 const frft::PacketHeader& header,
                 const std::uint8_t* payload,
                 std::size_t payload_size) {
    /*
     * 参数：
     * - socket_fd：服务器 UDP socket 文件描述符。
     * - destination：目标 Client 的 IPv4 地址和 UDP 端口。
     * - header：待发送 FRFT 报文的逻辑 Header。
     * - payload：类型专用 payload；长度为 0 时可为 nullptr。
     * - payload_size：payload 的实际字节数。
     *
     * 功能：
     * - 调用 serialize_packet 构造连续 datagram。
     * - 使用 sendto 发送到 destination。
     * - EINTR 时重试，并验证实际发送长度等于 datagram 长度。
     *
     * 返回：
     * - 无返回值；正常返回表示数据报已交给本机网络栈。
     * - 序列化或系统调用失败时抛出异常。
     */
     const auto datagram = frft::serialize_packet(header, payload, payload_size);
     ssize_t sent;
     do {
          sent = sendto(socket_fd,
                         datagram.data(),
                         datagram.size(),
                         0,
                         reinterpret_cast<const sockaddr*>(&destination),
                         sizeof(destination));
     } while (sent < 0 && errno == EINTR);

     if (sent < 0 || static_cast<std::size_t>(sent) != datagram.size()) {
          throw std::runtime_error(std::string("sendto failed: ") + std::strerror(errno));
     }
}

bool receive_packet(int socket_fd,
                    frft::Packet& packet,
                    sockaddr_in& source) {
    /*
     * 参数：
     * - socket_fd：已经 bind 的服务器 UDP socket。
     * - packet：成功时接收解析后 FRFT 报文的输出对象。
     * - source：成功时接收发送方 IPv4 endpoint 的输出对象。
     *
     * 功能：
     * - 阻塞调用 recvfrom 等待 UDP datagram。
     * - EINTR 时继续等待，其他系统错误应抛出异常。
     * - 调用 deserialize_packet 验证并解析报文。
     * - 无法解析的随机或非法 datagram 被静默丢弃，然后继续等待下一包。
     *
     * 返回：
     * - 获得一个合法 FRFT 报文时，把结果写入 packet 和 source，并返回 true。
     * - 当前设计持续等待合法包，因此正常情况下不返回 false。
     */
     std::vector<std::uint8_t> buffer(65535);
     while (true) {
          socklen_t source_length = sizeof(source);
          const ssize_t received = recvfrom(socket_fd,
                                             buffer.data(),
                                             buffer.size(),
                                             0,
                                             reinterpret_cast<sockaddr*>(&source),
                                             &source_length);
          if (received < 0) {
               if (errno == EINTR) {
                    continue;
               }
               throw std::runtime_error(std::string("recvfrom failed: ") + std::strerror(errno));
          }

          std::string error;
          if (frft::deserialize_packet(buffer.data(), received, packet, error)) {
               return true;
          }
     }
}

void send_start_ack(int socket_fd,
                    const sockaddr_in& client,
                    std::uint32_t session_id,
                    const frft::StartAckPayload& response) {
    /*
     * 参数：
     * - socket_fd：服务器 UDP socket。
     * - client：接收 START_ACK 的 Client endpoint。
     * - session_id：需要回复的 START 会话 ID。
     * - response：包含 status 和最终接受参数的 START_ACK payload。
     *
     * 功能：
     * - 序列化 response。
     * - 构造 type 为 START_ACK、number 为 0 的通用 Header。
     * - 调用 send_packet 把完整响应发送给 client。
     *
     * 返回：
     * - 无返回值；序列化或发送失败时向上抛出异常。
     */
     const auto payload = frft::serialize_start_ack(response);
     const auto header = make_header(frft::PacketType::START_ACK, session_id, 0);
     send_packet(socket_fd, client, header, payload.data(), payload.size());
}

void send_ack(int socket_fd,
              const sockaddr_in& client,
              std::uint32_t session_id,
              std::uint32_t ack_number,
              const frft::ReceiverTracker& tracker) {
    /*
     * 参数：
     * - socket_fd：服务器 UDP socket。
     * - client：当前活动会话的 Client endpoint。
     * - session_id：当前会话 ID。
     * - ack_number：本次 ACK snapshot 的递增编号。
     * - tracker：保存累计 ACK、最大接收位置和接收位图的 ReceiverTracker。
     *
     * 功能：
     * - 从 tracker 取得 cumulative_ack 和 largest_received_plus_one。
     * - 当前 Stage 1 构造 bitmap_bits 为 0 的累计 ACK，bitmap_base 使用
     *   当前 cumulative_ack。
     * - 序列化 ACK payload，构造 Header，并发送给 Client。
     * - 后续阶段可在保持函数职责不变的情况下追加 8192-bit SACK bitmap。
     *
     * 返回：
     * - 无返回值；序列化或发送失败时抛出异常。
     */
     const frft::AckPayload ack {
        tracker.cumulative_ack(),
        tracker.largest_received_plus_one(),
        tracker.cumulative_ack(),
        0,
     };
     const auto payload = frft::serialize_ack(ack);
     const auto header = make_header(frft::PacketType::ACK, session_id, ack_number);
     send_packet(socket_fd, client, header, payload.data(), payload.size());
}

void time_wait(int socket_fd,
               const sockaddr_in& client,
               std::uint32_t session_id,
               std::uint32_t total_chunks,
               const std::vector<std::uint8_t>& complete_ack_payload) {
    /*
     * 参数：
     * - socket_fd：完成传输后仍保持打开的服务器 UDP socket。
     * - client：已完成会话的 Client endpoint。
     * - session_id：已完成会话 ID。
     * - total_chunks：该文件的总 chunk 数，用于验证重复 COMPLETE。
     * - complete_ack_payload：此前成功发送的 COMPLETE_ACK payload；
     *   重复响应必须使用相同内容。
     *
     * 功能：
     * - 在协议规定的 TIME_WAIT 时间内使用 poll 等待可能的重复 COMPLETE。
     * - 过滤来源、解析结果、session、type、number 和空 payload 条件。
     * - 收到匹配的重复 COMPLETE 时重新发送同一 COMPLETE_ACK。
     * - 不重新创建文件、不修改统计，也不重新执行数据接收流程。
     * - EINTR 时继续等待；其他 poll 或 recvfrom 错误应抛出异常。
     *
     * 返回：
     * - 无返回值；TIME_WAIT 到期后正常返回。
     */
     const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
     std::vector<std::uint8_t> buffer(65535);

     while (std::chrono::steady_clock::now() < deadline) {
          const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
               deadline - std::chrono::steady_clock::now());
          pollfd descriptor {socket_fd, POLLIN, 0};
          const int ready = poll(&descriptor, 1, std::max(1, static_cast<int>(remaining.count())));
          if (ready == 0) {
               return;
          }
          if (ready < 0) {
               if (errno == EINTR) {
                    continue;
               }
               throw std::runtime_error(std::string("poll failed: ") + std::strerror(errno));
          }

          sockaddr_in source {};
          socklen_t source_length = sizeof(source);
          const ssize_t received = recvfrom(socket_fd,
                                             buffer.data(),
                                             buffer.size(),
                                             0,
                                             reinterpret_cast<sockaddr*>(&source),
                                             &source_length);
          if (received < 0) {
               if (errno == EINTR) {
                    continue;
               }
               throw std::runtime_error(std::string("recvfrom failed: ") + std::strerror(errno));
          }

          frft::Packet packet;
          std::string error;
          if (same_endpoint(source, client) &&
               frft::deserialize_packet(buffer.data(), received, packet, error) &&
               packet.header.session_id == session_id &&
               packet.header.type == frft::PacketType::COMPLETE &&
               packet.header.number == total_chunks && packet.payload.empty()) {
               const auto header =
                    make_header(frft::PacketType::COMPLETE_ACK, session_id, total_chunks);
               send_packet(socket_fd,
                         client,
                         header,
                         complete_ack_payload.data(),
                         complete_ack_payload.size());
          }
     }
}

int run_server(const Options& options) {
    /*
     * 参数：
     * - options：已经解析并验证的服务器配置，包括监听端口、输出路径和 MTU。
     *
     * 功能：
     * - 创建 IPv4 UDP socket，配置地址复用并 bind 到指定端口。
     * - 等待合法 START，验证 session、chunk size、file size、total chunks、
     *   发送窗口、ACK bitmap 覆盖范围和协议参数。
     * - 对非法请求发送拒绝 START_ACK；对活动会话之外的新 START 返回 BUSY。
     * - 根据 START 创建 MappedOutputFile；I/O 失败时返回 IO_ERROR。
     * - 返回成功 START_ACK，并创建 ReceiverTracker。
     * - 持续接收当前 Client 的 DATA，验证 chunk ID、offset 和 payload 长度。
     * - 对首次到达的 DATA 写入 mmap 文件、更新 tracker，并记录首包和末包时间；
     *   对重复 DATA 只增加重复统计。
     * - 当前 Stage 1 在处理每个 DATA 后发送累计 ACK。
     * - 处理重复 START 时重发已接受的 START_ACK。
     * - 收到 COMPLETE 时验证 number、空 payload 和文件完整状态；不完整则发送
     *   当前 ACK，完整则同步输出文件并发送 COMPLETE_ACK。
     * - 打印会话、文件、chunk、重复包、时间和吞吐量统计。
     * - 进入 TIME_WAIT 回应重复 COMPLETE，然后关闭 socket。
     * - 无论正常返回还是异常退出，都必须关闭 socket。
     *
     * 返回：
     * - 一次文件传输和结束握手成功完成时返回 0。
     * - 运行失败时抛出异常，由 main 转换为非零退出码。
     */
     const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
     if (socket_fd < 0) {
          throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
     }

     try {
          const int reuse = 1;
          setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

          sockaddr_in local {};
          local.sin_family = AF_INET;
          local.sin_addr.s_addr = htonl(INADDR_ANY);
          local.sin_port = htons(options.port);
          if (bind(socket_fd, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
               throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
          }

          std::cout << "Waiting for one client on UDP port " << options.port << "\n";

          frft::Packet start_packet;
          sockaddr_in client {};
          frft::StartPayload start;
          while (true) {
               receive_packet(socket_fd, start_packet, client);
               if (start_packet.header.type != frft::PacketType::START ||
                    start_packet.header.session_id == 0 || start_packet.header.number != 0 ||
                    !frft::deserialize_start(start_packet.payload, start)) {
                    continue;
               }

               bool valid = start.chunk_size == frft::chunk_size_for_mtu(options.mtu) &&
                              start.total_chunks == frft::chunk_count(start.file_size, start.chunk_size) &&
                              start.window_chunks > 0 && start.ack_bitmap_bits > 0 &&
                              start.window_chunks <= start.ack_bitmap_bits;
               if (!valid) {
                    const frft::StartAckPayload rejected {frft::StatusCode::INVALID_REQUEST, 0, 0, 0};
                    send_start_ack(socket_fd, client, start_packet.header.session_id, rejected);
                    continue;
               }
               break;
          }

          const std::uint32_t session_id = start_packet.header.session_id;
          std::unique_ptr<frft::MappedOutputFile> output;
          try {
               output = std::make_unique<frft::MappedOutputFile>(options.output, start.file_size);
          } catch (...) {
               const frft::StartAckPayload failed {frft::StatusCode::IO_ERROR, 0, 0, 0};
               send_start_ack(socket_fd, client, session_id, failed);
               throw;
          }

          const frft::StartAckPayload accepted {
               frft::StatusCode::OK,
               start.window_chunks,
               start.ack_bitmap_bits,
               start.ack_interval_ms,
          };
          send_start_ack(socket_fd, client, session_id, accepted);

          frft::ReceiverTracker tracker(start.total_chunks);
          std::uint32_t ack_number = 0;
          std::uint64_t duplicate_packets = 0;
          bool received_first_data = false;
          std::chrono::steady_clock::time_point first_data_time;
          std::chrono::steady_clock::time_point last_data_time;

          while (true) {
               frft::Packet packet;
               sockaddr_in source {};
               receive_packet(socket_fd, packet, source);

               if (packet.header.type == frft::PacketType::START) {
                    if (same_endpoint(source, client) && packet.header.session_id == session_id) {
                         send_start_ack(socket_fd, client, session_id, accepted);
                    } else {
                         const frft::StartAckPayload busy {frft::StatusCode::BUSY, 0, 0, 0};
                         send_start_ack(socket_fd, source, packet.header.session_id, busy);
                    }
                    continue;
               }
               if (!same_endpoint(source, client) || packet.header.session_id != session_id) {
                    continue;
               }

               if (packet.header.type == frft::PacketType::DATA) {
                    const std::uint32_t sequence = packet.header.number;
                    if (sequence >= start.total_chunks) {
                         continue;
                    }
                    const std::uint64_t offset = static_cast<std::uint64_t>(sequence) * start.chunk_size;
                    const std::size_t expected_size = static_cast<std::size_t>(
                         std::min<std::uint64_t>(start.chunk_size, start.file_size - offset));
                    if (packet.payload.size() != expected_size) {
                         continue;
                    }

                    if (tracker.has_received(sequence)) {
                         ++duplicate_packets;
                    } else {
                         if (expected_size != 0) {
                         std::memcpy(output->data() + offset, packet.payload.data(), expected_size);
                         }
                         tracker.mark_received(sequence);
                         const auto now = std::chrono::steady_clock::now();
                         if (!received_first_data) {
                         first_data_time = now;
                         received_first_data = true;
                         }
                         last_data_time = now;
                    }
                    send_ack(socket_fd, client, session_id, ack_number++, tracker);
                    continue;
               }

               if (packet.header.type != frft::PacketType::COMPLETE ||
                    packet.header.number != start.total_chunks || !packet.payload.empty()) {
                    continue;
               }
               if (!tracker.complete() || tracker.cumulative_ack() != start.total_chunks) {
                    send_ack(socket_fd, client, session_id, ack_number++, tracker);
                    continue;
               }

               output->sync();
               const std::uint64_t receiver_time_us = received_first_data
                                                            ? std::chrono::duration_cast<
                                                                 std::chrono::microseconds>(
                                                                 last_data_time - first_data_time)
                                                                 .count()
                                                            : 0;
               const frft::CompleteAckPayload completed {
                    frft::StatusCode::OK,
                    tracker.received_count(),
                    start.file_size,
                    receiver_time_us,
               };
               const auto complete_payload = frft::serialize_complete_ack(completed);
               const auto complete_header =
                    make_header(frft::PacketType::COMPLETE_ACK, session_id, start.total_chunks);
               send_packet(socket_fd,
                         client,
                         complete_header,
                         complete_payload.data(),
                         complete_payload.size());

               const double seconds = receiver_time_us / 1'000'000.0;
               const double throughput_mbps =
                    seconds > 0.0 ? start.file_size * 8.0 / seconds / 1'000'000.0 : 0.0;
               std::cout << "Transfer complete\n"
                         << "  session: " << session_id << '\n'
                         << "  file bytes: " << start.file_size << '\n'
                         << "  unique chunks: " << tracker.received_count() << '\n'
                         << "  duplicate DATA packets: " << duplicate_packets << '\n'
                         << "  receiver data time us: " << receiver_time_us << '\n'
                         << "  receiver throughput Mbps: " << throughput_mbps << '\n';

               time_wait(socket_fd,
                         client,
                         session_id,
                         start.total_chunks,
                         complete_payload);
               close(socket_fd);
               return 0;
          }
     } catch (...) {
          close(socket_fd);
          throw;
     }
}

}  // namespace

int main(int argc, char* argv[]) {
    /*
     * 参数：
     * - argc：server 命令行参数数量。
     * - argv：server 命令行参数数组。
     *
     * 功能：
     * - 调用 parse_options 解析参数，再调用 run_server 等待并完成一次传输。
     * - 捕获 std::exception，打印带 server 前缀的错误和命令行帮助。
     * - 防止异常越过程序入口。
     *
     * 返回：
     * - 服务器成功完成一次会话时返回 run_server 的 0。
     * - 参数错误或运行异常时返回 1。
     */
     try {
          return run_server(parse_options(argc, argv));
     } catch (const std::exception& error) {
          std::cerr << "server: " << error.what() << '\n';
          print_usage(argv[0]);
          return 1;
     }
}
