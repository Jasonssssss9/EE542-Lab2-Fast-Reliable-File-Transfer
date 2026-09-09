#include "file_io.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <limits>
#include <stdexcept>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace frft {
namespace {

std::runtime_error system_error(const std::string& action) {
    /*
     * 参数：
     * - action：描述当前失败操作的文字，例如“打开输入文件”或“刷新输出文件”。
     *
     * 功能：
     * - 读取当前 errno 对应的系统错误信息。
     * - 把 action、分隔符和系统错误文本组合成便于诊断的异常对象。
     * - 本函数只负责构造异常，不负责打印、清理资源或抛出异常。
     *
     * 返回：
     * - 返回一个 std::runtime_error，其中包含操作描述和 errno 的可读说明。
     */
    return std::runtime_error(action + ": " + std::strerror(errno));
}

}  // namespace

MappedInputFile::MappedInputFile(const std::string& path) {
    /*
     * 参数：
     * - path：Sender 要读取的源文件路径。
     *
     * 功能：
     * - 以只读方式打开 path。
     * - 使用 fstat 获取并验证文件大小，把结果保存到 size_。
     * - 对非空文件建立只读的内存映射，把首地址保存到 data_。
     * - 空文件不能调用长度为 0 的 mmap，此时 data_ 保持 nullptr、size_ 为 0。
     * - 任一步骤失败时关闭已经打开的文件描述符，恢复安全成员状态并抛出异常。
     *
     * 返回：
     * - 构造函数没有返回值；成功后当前对象表示一个可随机读取的输入文件。
     * - 失败时抛出 std::runtime_error。
     */
    fd_ = open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        throw system_error("cannot open input file");
    }

    struct stat info {};
    if (fstat(fd_, &info) != 0) {
        const auto error = system_error("cannot inspect input file");
        close(fd_);
        fd_ = -1;
        throw error;
    }
    if (info.st_size < 0) {
        close(fd_);
        fd_ = -1;
        throw std::runtime_error("input file has an invalid size");
    }
    size_ = static_cast<std::uint64_t>(info.st_size);

    if (size_ != 0) {
        void* mapping = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (mapping == MAP_FAILED) {
            const auto error = system_error("cannot mmap input file");
            close(fd_);
            fd_ = -1;
            throw error;
        }
        data_ = static_cast<std::uint8_t*>(mapping);
    }
}

MappedInputFile::~MappedInputFile() {
    /*
     * 参数：
     * - 无显式参数；处理当前 MappedInputFile 对象持有的资源。
     *
     * 功能：
     * - 如果 data_ 指向有效映射，则解除整个输入文件映射。
     * - 如果 fd_ 是有效文件描述符，则关闭文件。
     * - 析构过程应安全处理空文件、构造未完全成功后的安全状态和重复的无效资源值。
     * - 析构函数不应向外抛出异常。
     *
     * 返回：
     * - 无返回值。
     */
    if (data_ != nullptr) {
        munmap(data_, size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

const std::uint8_t* MappedInputFile::data() const {
    /*
     * 参数：
     * - 无显式参数；读取当前对象保存的输入文件映射地址。
     *
     * 功能：
     * - 向 Sender 提供源文件映射的只读首地址。
     * - 调用者可通过该地址加 chunk offset 读取要发送的文件内容。
     *
     * 返回：
     * - 非空文件返回映射首地址。
     * - 空文件返回 nullptr。
     */
    return data_;
}

std::uint64_t MappedInputFile::size() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 查询当前输入文件的精确字节数。
     *
     * 返回：
     * - 返回构造时通过 fstat 获得并保存的 size_；空文件返回 0。
     */
    return size_;
}

MappedOutputFile::MappedOutputFile(const std::string& path, std::uint64_t size) 
: size_(size) 
{
    /*
     * 参数：
     * - path：Receiver 要创建或覆盖的目标文件路径。
     * - size：START 报文声明的最终文件字节数。
     *
     * 功能：
     * - 验证 size 能否由本机 off_t 表示。
     * - 以可读写、创建和截断方式打开目标文件。
     * - 使用 ftruncate 把文件精确调整到 size，并把 size 保存到 size_。
     * - 对非空文件建立可读写、共享的内存映射，把首地址保存到 data_。
     * - 空文件不执行长度为 0 的 mmap。
     * - 任一步骤失败时释放已经取得的资源并抛出异常。
     *
     * 返回：
     * - 构造函数没有返回值；成功后当前对象可按 chunk offset 写入目标文件。
     * - 失败时抛出 std::runtime_error。
     */
    if (size > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max())) {
        throw std::runtime_error("output file is too large for this system");
    }

    fd_ = open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        throw system_error("cannot create output file");
    }
    if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        const auto error = system_error("cannot resize output file");
        close(fd_);
        fd_ = -1;
        throw error;
    }

    if (size_ != 0) {
        void* mapping = mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (mapping == MAP_FAILED) {
            const auto error = system_error("cannot mmap output file");
            close(fd_);
            fd_ = -1;
            throw error;
        }
        data_ = static_cast<std::uint8_t*>(mapping);
    }
}

MappedOutputFile::~MappedOutputFile() {
    /*
     * 参数：
     * - 无显式参数；处理当前 MappedOutputFile 对象持有的资源。
     *
     * 功能：
     * - 如果存在有效映射，则解除映射。
     * - 如果存在有效文件描述符，则关闭文件。
     * - 应安全处理空文件和无效资源值，且不向外抛出异常。
     *
     * 返回：
     * - 无返回值。
     */
    if (data_ != nullptr) {
        munmap(data_, size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

std::uint8_t* MappedOutputFile::data() {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 向 Receiver 提供目标文件映射的可写首地址。
     * - 调用者可通过该地址加 chunk offset 写入乱序到达的数据。
     *
     * 返回：
     * - 非空文件返回可写映射首地址。
     * - 空文件返回 nullptr。
     */
     return data_;
}

std::uint64_t MappedOutputFile::size() const {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 查询 Receiver 目标文件的预期总字节数。
     *
     * 返回：
     * - 返回构造时保存的 size_；空文件返回 0。
     */
    return size_;
}

void MappedOutputFile::sync() {
    /*
     * 参数：
     * - 无显式参数。
     *
     * 功能：
     * - 对非空目标文件执行同步刷新，确保内存映射中的修改提交给文件系统。
     * - 空文件没有映射，因此应直接成功返回。
     * - 刷新失败时构造并抛出带系统错误信息的异常。
     *
     * 返回：
     * - 无返回值；正常返回表示刷新操作成功或文件为空。
     * - 失败时抛出 std::runtime_error。
     */
    if (data_ != nullptr && msync(data_, size_, MS_SYNC) != 0){
        throw system_error("cannot flush output file");
    }
}

}  // namespace frft
