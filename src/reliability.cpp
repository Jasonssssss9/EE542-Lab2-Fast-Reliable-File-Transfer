#include "reliability.hpp"

#include <algorithm>
#include <stdexcept>

namespace frft {

SenderWindow::SenderWindow(std::uint32_t total_chunks, std::uint32_t window_chunks) {
    /*
     * 参数：
     * - total_chunks：本次文件传输需要发送的 chunk 总数。
     * - window_chunks：滑动窗口最多允许同时容纳的未累计确认 chunk 数。
     *
     * 功能：
     * - 保存文件总 chunk 数和窗口大小到对应成员。
     * - 将窗口左边界 base_ 和下一个首次发送编号 next_sequence_ 初始化为 0。
     * - 验证窗口至少能容纳一个 chunk；即使传输空文件，窗口配置本身也必须合法。
     *
     * 返回：
     * - 构造函数没有返回值；成功后窗口处于尚未发送任何 chunk 的初始状态。
     * - window_chunks 为 0 时抛出 std::invalid_argument。
     */
}

bool SenderWindow::can_send() const {
    /*
     * 参数：
     * - 无显式参数；读取当前窗口的 total_chunks_、window_chunks_、base_ 和
     *   next_sequence_。
     *
     * 功能：
     * - 判断是否仍有从未发送过的 chunk。
     * - 判断 next_sequence_ 是否位于 [base_, base_ + window_chunks_) 窗口范围内。
     * - 加法和比较应避免 32-bit 溢出。
     *
     * 返回：
     * - 如果可以立即取出一个新的 chunk ID 发送，返回 true。
     * - 如果文件已全部首次发送或当前窗口已满，返回 false。
     */
}

std::uint32_t SenderWindow::take_next_sequence() {
    /*
     * 参数：
     * - 无显式参数；操作当前 SenderWindow 的 next_sequence_。
     *
     * 功能：
     * - 先确认 can_send() 为 true。
     * - 取出当前 next_sequence_ 作为下一次原始 DATA 的 chunk ID。
     * - 将 next_sequence_ 加一，使同一个 chunk 不会在原始发送路径中被重复分配。
     * - 本函数只负责分配新的 sequence，不处理重传队列。
     *
     * 返回：
     * - 返回当前分配的 chunk ID。
     * - 窗口无可发送 chunk 时抛出 std::logic_error。
     */
}

bool SenderWindow::apply_cumulative_ack(std::uint32_t cumulative_ack) {
    /*
     * 参数：
     * - cumulative_ack：Receiver 报告的累计确认值；所有小于该值的 chunk
     *   均已连续收到。
     *
     * 功能：
     * - 拒绝超过 total_chunks_ 的非法 ACK。
     * - 拒绝确认 Sender 尚未发送 chunk 的 ACK。
     * - 只允许 base_ 单调向前移动，旧 ACK 或重复 ACK 不能使窗口倒退。
     * - 当 ACK 有效且更大时，把 base_ 更新为 cumulative_ack。
     *
     * 返回：
     * - 如果 base_ 实际向前推进，返回 true。
     * - 如果 ACK 非法、重复或没有提供新进度，返回 false。
     */
}

bool SenderWindow::all_acked() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 判断累计确认窗口左边界是否已经到达 total_chunks_。
     * - 该条件用于决定是否可以进入 COMPLETE 握手。
     *
     * 返回：
     * - 所有 chunk 均已累计确认时返回 true，否则返回 false。
     * - 对 total_chunks_ 为 0 的空文件，初始状态应返回 true。
     */
}

std::uint32_t SenderWindow::base() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 查询滑动窗口当前左边界，也就是当前累计 ACK。
     *
     * 返回：
     * - 返回 base_。
     */
}

std::uint32_t SenderWindow::next_sequence() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 查询下一个尚未通过原始发送路径分配的 chunk ID。
     *
     * 返回：
     * - 返回 next_sequence_；它可能等于 total_chunks_，表示所有 chunk
     *   都已经至少发送过一次。
     */
}

ReceiverTracker::ReceiverTracker(std::uint32_t total_chunks) {
    /*
     * 参数：
     * - total_chunks：START 报文声明并经验证的文件 chunk 总数。
     *
     * 功能：
     * - 创建长度为 total_chunks 的 received_ 位图或字节数组，并全部初始化为未收到。
     * - 将 received_count_、cumulative_ack_ 和 largest_received_plus_one_
     *   初始化为 0。
     *
     * 返回：
     * - 构造函数没有返回值；成功后 tracker 表示尚未收到任何 DATA。
     */
}

bool ReceiverTracker::has_received(std::uint32_t sequence) const {
    /*
     * 参数：
     * - sequence：要查询的 chunk ID。
     *
     * 功能：
     * - 首先检查 sequence 是否位于 received_ 的合法下标范围。
     * - 查询对应 chunk 是否已经被标记为收到。
     *
     * 返回：
     * - sequence 合法且此前已经收到时返回 true。
     * - sequence 越界或尚未收到时返回 false。
     */
}

bool ReceiverTracker::mark_received(std::uint32_t sequence) {
    /*
     * 参数：
     * - sequence：当前新收到并已通过报文长度检查的 DATA chunk ID。
     *
     * 功能：
     * - 检查 sequence 是否越界，以及该 chunk 是否已经被记录。
     * - 对首次收到的 chunk 设置 received_[sequence]。
     * - 增加唯一 chunk 计数 received_count_。
     * - 更新 largest_received_plus_one_。
     * - 从当前 cumulative_ack_ 开始连续扫描已收到的 chunk，使累计 ACK
     *   尽可能向前推进。
     *
     * 返回：
     * - 成功记录一个此前未收到的合法 chunk 时返回 true。
     * - chunk ID 越界或属于重复 DATA 时返回 false。
     */
}

bool ReceiverTracker::complete() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 比较唯一接收 chunk 数与 received_ 的总长度，判断文件数据是否收齐。
     *
     * 返回：
     * - 每一个 chunk 都至少收到一次时返回 true，否则返回 false。
     * - 对零 chunk 的空文件返回 true。
     */
}

std::uint32_t ReceiverTracker::received_count() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 查询 Receiver 已收到的唯一 chunk 数量；重复 DATA 不计入该数值。
     *
     * 返回：
     * - 返回 received_count_。
     */
}

std::uint32_t ReceiverTracker::cumulative_ack() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 查询当前累计确认值。
     * - 所有小于该值的 chunk 都已连续收到；该值本身是连续前缀之后的第一个位置。
     *
     * 返回：
     * - 返回 cumulative_ack_；文件完整时应等于 total_chunks。
     */
}

std::uint32_t ReceiverTracker::largest_received_plus_one() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 查询 Receiver 观察到的最大 chunk ID 加一，用于 ACK 信息和丢包判断。
     *
     * 返回：
     * - 尚未收到 DATA 时返回 0。
     * - 否则返回最大已接收 chunk ID 加一。
     */
}

}  // namespace frft
