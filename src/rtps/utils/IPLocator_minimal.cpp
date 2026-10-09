// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// IPLocator 最小实现（方法体移植自 Fast DDS 的 src/cpp/utils/IPLocator.cpp）
//
// ===========================================================================
// 为什么不整体移植
// ===========================================================================
//
// 原文件 1331 行，其余部分是地址解析（依赖 std::regex 常量）与 asio 集成；
// 而 thirdparty/asio 在本环境是**未初始化的空 submodule**（离线，无法获取）。
//
// 链路实际只引用 6 个方法：端口高低 16 位读写 + IPv4/IPv6 字符串格式化。
// 它们是**纯位运算与格式化**，与协议编解码无关，故此按需复制方法体
// （而非重新实现），保证语义与 Fast DDS 完全一致。

#include <stratum/rtps/utils/IPLocator.hpp>
#include <stratum/rtps/common/Locator.hpp>
#include <stratum/rtps/common/Types.hpp>

#include <sstream>
#include <string>

namespace eprosima {
namespace fastdds {
namespace rtps {

uint16_t IPLocator::getLogicalPort(
        const Locator_t& locator)
{
    const uint16_t* loc_logical = reinterpret_cast<const uint16_t*>(&locator.port);
#if FASTDDS_IS_BIG_ENDIAN_TARGET
    return loc_logical[0];
#else
    return loc_logical[1];
#endif // if FASTDDS_IS_BIG_ENDIAN_TARGET
}

uint16_t IPLocator::getPhysicalPort(
        const Locator_t& locator)
{
    const uint16_t* loc_physical = reinterpret_cast<const uint16_t*>(&locator.port);
#if FASTDDS_IS_BIG_ENDIAN_TARGET
    return loc_physical[1];
#else
    return loc_physical[0];
#endif // if FASTDDS_IS_BIG_ENDIAN_TARGET
}

bool IPLocator::setLogicalPort(
        Locator_t& locator,
        uint16_t port)
{
    uint16_t* loc_logical = reinterpret_cast<uint16_t*>(&locator.port);
#if FASTDDS_IS_BIG_ENDIAN_TARGET
    loc_logical[0] = port; // Logical port is stored at 0th and 1st bytes of port
#else
    loc_logical[1] = port; // Logical port is stored at 2nd and 3rd bytes of port
#endif // if FASTDDS_IS_BIG_ENDIAN_TARGET
    return port != 0;
}

bool IPLocator::setPhysicalPort(
        Locator_t& locator,
        uint16_t port)
{
    uint16_t* loc_physical = reinterpret_cast<uint16_t*>(&locator.port);
#if FASTDDS_IS_BIG_ENDIAN_TARGET
    loc_physical[1] = port; // Physical port is stored at 0 and 1st bytes of port
#else
    loc_physical[0] = port; // Physical port is stored at 0 and 1st bytes of port
#endif // if FASTDDS_IS_BIG_ENDIAN_TARGET
    return port != 0;
}

std::string IPLocator::toIPv4string(
        const Locator_t& locator)
{
    std::stringstream ss;
    ss << (int)locator.address[12] << "."
       << (int)locator.address[13] << "."
       << (int)locator.address[14] << "."
       << (int)locator.address[15];
    return ss.str();
}

std::string IPLocator::toIPv6string(
        const Locator_t& locator)
{
    /* RFC 5952 Recommendation
     *  4.1     No leading zeros before each block
     *  4.2.1   Shorten as much as possible (do not leave 0 blocks at sides of ::)
     *  4.2.2   No collapse size 1 blocks
     *  4.2.3   Collapse the largest block (or the first one in case of tie)
     *  4.3     Lowercase
     */

    int max_block_index = 0;
    int max_block_size = 0;
    int actual_block_index = 0;
    int actual_block_size = 0;

    // 4.1, 4.3     By default using << std::hex
    std::stringstream ss;
    ss << std::hex;
    bool compress = false;

    // First calculate if any 0 block must be collapsed and which one
    for (int i = 0; i != 16; i += 2)
    {
        if (locator.address[i] == 0 && locator.address[i + 1] == 0)
        {
            // 4.2.1 Shorten if it has already shortened the previous block
            if (compress)
            {
                ++actual_block_size;
            }
            else
            {
                compress = true;
                actual_block_index = i;
                actual_block_size = 1;
            }
        }
        else
        {
            if (compress)
            {
                compress = false;
                // 4.2.3 Choose the largest block
                if (actual_block_size > max_block_size)
                {
                    max_block_index = actual_block_index;
                    max_block_size = actual_block_size;
                }
            }
        }
    }

    // In case the last block is the one to compress
    if (compress && actual_block_size > max_block_size)
    {
        max_block_index = actual_block_index;
        max_block_size = actual_block_size;
    }

    // Use compress variable to know if there will be compression
    // 4.2.2 compress only if block size > 1
    compress = max_block_size >= 2;

    // Case that first block is compressed
    if (compress && max_block_index == 0)
    {
        ss << ":";
    }

    for (int i = 0; i != 16; i += 2)
    {
        // If it is after compress index and not all blocks has been compressed, compress it
        if (compress && i >= max_block_index && max_block_size > 0)
        {
            // Reduce number of blocks to compress
            --max_block_size;

            // Last block prints second :
            if (max_block_size == 0)
            {
                ss << ":";
            }
            continue;
        }

        // Stream the following block
        auto field = (locator.address[i] << 8) + locator.address[i + 1];
        if (i != 14)
        {
            ss << field << ":";
        }
        else
        {
            ss << field;
        }
    }

    return ss.str();
}

} // namespace rtps
} // namespace fastdds
} // namespace eprosima
