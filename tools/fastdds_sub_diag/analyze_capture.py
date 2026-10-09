#!/usr/bin/env python3
# ---------------------------------------------------------------------------
# 文件：tools/fastdds_sub_diag/analyze_capture.py
# 用途：分析 capture_udp 落盘的报文文件（格式：[uint32 LE 长度][原始报文]...）
#
# 它只做**边界遍历**，不做 RTPS 语义解析 —— 目的是回答"哪些报文真的到达了"，
# 因此不与被测方的解析器共享任何假设。
#
# 用法：
#   python3 analyze_capture.py <capture_file> [--dump-writer 000003c2 --out f.bin]
# ---------------------------------------------------------------------------

import argparse
import struct
import sys

ENTITY_NAMES = {
    0x000100C2: "SPDP_W",
    0x000100C7: "SPDP_R",
    0x000003C2: "SEDP_PUB_W",
    0x000003C7: "SEDP_PUB_R",
    0x000004C2: "SEDP_SUB_W",
    0x000004C7: "SEDP_SUB_R",
    0x000002C2: "SEDP_TOPIC_W",
    0x000002C7: "SEDP_TOPIC_R",
    0x00000103: "USER_W",
    0x00000104: "USER_R",
    0x00000000: "UNKNOWN_ENTITY",
}


def entity_name(raw: bytes) -> str:
    if len(raw) < 4:
        return "??"
    value = struct.unpack(">I", raw[:4])[0]
    return ENTITY_NAMES.get(value, "0x%08x" % value)


def read_records(path: str):
    """按 [uint32 LE 长度][字节] 读出所有报文。"""
    with open(path, "rb") as handle:
        blob = handle.read()
    offset = 0
    while offset + 4 <= len(blob):
        (length,) = struct.unpack("<I", blob[offset:offset + 4])
        offset += 4
        if length == 0 or offset + length > len(blob):
            break
        yield blob[offset:offset + length]
        offset += length


def data_ids_offset(packet: bytes, sid_pos: int) -> int:
    """返回 DATA 子消息里 readerId 的偏移（子消息起始 sid_pos 起算）。

    **不要按 RTPS 版本号判断前缀**：本项目实测存在"版本号 2.2 却含 extraFlags"
    的报文（见 AI_COLLAB 缺陷 8）。正确做法与 Stratum 的 DataTypePrefixSize
    一致 —— 用 octetsToInlineQos 的值做数据驱动判定，两个候选位置是：
      · 子消息起始 + 6 处的 u16 == 16 → 有 extraFlags，readerId 在 +8
      · 子消息起始 + 4 处的 u16 == 16 → 无 extraFlags，readerId 在 +6
    （等价于项目里 body = 子消息起始 + 4 时的 body[2..3] / body[0..1]。）
    """
    if sid_pos + 8 <= len(packet):
        with_extra = struct.unpack_from("<H", packet, sid_pos + 6)[0]
        if with_extra == 16:
            return sid_pos + 8
    if sid_pos + 6 <= len(packet):
        without_extra = struct.unpack_from("<H", packet, sid_pos + 4)[0]
        if without_extra == 16:
            return sid_pos + 6
    # 兜底：按版本号（仅当前两种判定都不成立时）
    return sid_pos + (8 if packet[5] >= 3 else 6)


def walk_submessages(packet: bytes):
    """返回 [(sid, olen, offset, reader_name, writer_name)]。"""
    if len(packet) < 20 or packet[:4] != b"RTPS":
        return []
    out = []
    pos = 20
    while pos + 4 <= len(packet):
        sid = packet[pos]
        olen = struct.unpack_from("<H", packet, pos + 2)[0]
        reader_name = writer_name = ""
        if sid == 0x15:
            ids = data_ids_offset(packet, pos)
            if ids + 8 <= len(packet):
                reader_name = entity_name(packet[ids:ids + 4])
                writer_name = entity_name(packet[ids + 4:ids + 8])
        out.append((sid, olen, pos, reader_name, writer_name))
        if olen == 0:
            break
        pos += 4 + olen
    return out


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("capture")
    parser.add_argument("--dump-writer", default="",
                        help="导出写入该 writer entity（hex，如 000003c2）的完整报文")
    parser.add_argument("--out", default="",
                        help="导出文件路径（配合 --dump-writer）")
    parser.add_argument("--dump-pids", action="store_true",
                        help="打印 SPDP 报文的参数表（locator 解成 ip:port）")
    args = parser.parse_args()

    packets = list(read_records(args.capture))
    print("报文总数 = %d" % len(packets))

    writer_hist = {}
    sid_hist = {}
    malformed = 0
    for packet in packets:
        if len(packet) < 20 or packet[:4] != b"RTPS":
            malformed += 1
            continue
        submsgs = walk_submessages(packet)
        sid_hist[tuple(s[0] for s in submsgs)] = sid_hist.get(
            tuple(s[0] for s in submsgs), 0) + 1
        for sid, _olen, _pos, _reader, writer in submsgs:
            if sid == 0x15:
                writer_hist[writer] = writer_hist.get(writer, 0) + 1

    print("非 RTPS 报文 = %d" % malformed)
    print("\n=== DATA 子消息按 writer 统计 ===")
    for name, count in sorted(writer_hist.items(), key=lambda kv: -kv[1]):
        print("  %-16s x%d" % (name, count))

    print("\n=== 子消息 id 序列统计（前 10）===")
    for seq, count in sorted(sid_hist.items(), key=lambda kv: -kv[1])[:10]:
        print("  %-30s x%d" % (",".join("0x%02x" % s for s in seq), count))

    if args.dump_writer:
        target = int(args.dump_writer, 16)
        out_path = args.out or "/tmp/sedp_pub_capture.bin"
        kept = 0
        with open(out_path, "wb") as handle:
            for packet in packets:
                match = False
                for sid, _olen, _pos, _reader, writer in walk_submessages(packet):
                    if sid != 0x15:
                        continue
                    ids = None
                    # 用与 walk_submessages 相同的数据驱动偏移
                    for s in walk_submessages(packet):
                        if s[0] == 0x15:
                            ids = data_ids_offset(packet, s[2])
                            break
                    if ids is not None and ids + 8 <= len(packet):
                        value = struct.unpack(">I", packet[ids + 4:ids + 8])[0]
                        if value == target:
                            match = True
                    break
                if match:
                    handle.write(struct.pack("<I", len(packet)))
                    handle.write(packet)
                    kept += 1
        print("\n已导出 writer=0x%08x 的报文 %d 条 → %s" % (target, kept, out_path))

    if args.dump_pids:
        dump_pids(packets)

    return 0


PID_NAMES = {
    0x0001: "SENTINEL",
    0x0002: "DOMAIN_ID",
    0x0005: "TOPIC_NAME",
    0x0007: "TYPE_NAME",
    0x0015: "PROTOCOL_VERSION",
    0x0016: "VENDORID",
    0x001A: "RELIABILITY",
    0x001D: "DURABILITY",
    0x002F: "UNICAST_LOCATOR",
    0x0031: "UNICAST_LOCATOR6?",
    0x0030: "MULTICAST_LOCATOR6?",
    0x0032: "MULTICAST_LOCATOR",
    0x0050: "PARTICIPANT_GUID",
    0x0058: "BUILTIN_ENDPOINTS",
    0x005A: "ENDPOINT_GUID",
    0x0060: "TYPE_MAX_SIZE_SERIALIZED",
    0x0070: "KEY_HASH",
    0x0073: "DATA_REPRESENTATION",
    0x8007: "PARTICIPANT_NAME?",
}


def pid_locators(value: bytes):
    """从 locator 参数值里解出 (kind, ip, port) 列表。

    Fast DDS 的 PID_UNICAST_LOCATOR 参数值是 Locator_t 序列：
      kind(int32) + port(uint32) + address(16B, IPv4 在低 4 字节)
    """
    out = []
    offset = 0
    while offset + 24 <= len(value):
        kind, port = struct.unpack_from("<iI", value, offset)
        addr = value[offset + 8 + 12: offset + 8 + 16]
        if len(addr) < 4:
            break
        ip = ".".join(str(b) for b in addr[:4])
        out.append((kind, ip, port))
        offset += 24
    return out


def dump_pids(packets):
    """打印每个 SPDP 报文的参数表（PID/length/值），locator 直接解成 ip:port。

    为什么需要：判断"对端公告的 metatraffic 端口到底是多少"只能靠这里 ——
    计数器与日志都只能看到"学到的地址不对"，看不到报文里写的是什么。
    """
    seen = set()
    printed = 0
    for packet in packets:
        if len(packet) < 20 or packet[:4] != b"RTPS":
            continue
        for sid, _olen, pos, _reader, writer in walk_submessages(packet):
            if sid != 0x15 or writer != "SPDP_W":
                continue
            # DATA 体：readerId/writerId/SN 之后是 PL_CDR 封装 + 参数表。
            # 偏移同样用数据驱动判定（见 data_ids_offset 的说明）。
            body = data_ids_offset(packet, pos) + 4 + 4 + 8
            if body + 4 > len(packet):
                continue
            encap = packet[body:body + 4]
            if encap[1] not in (0x01, 0x03):     # CDR_LE / PL_CDR_LE
                continue
            q = body + 4
            pids = []
            while q + 4 <= len(packet):
                pid, plen = struct.unpack_from("<HH", packet, q)
                value = packet[q + 4: q + 4 + plen]
                pids.append((pid, plen, value))
                q += 4 + ((plen + 3) & ~3)
                if pid == 0x0001:
                    break
            key = tuple((pid, plen) for pid, plen, _ in pids)
            if key in seen:
                continue
            seen.add(key)
            printed += 1
            if printed > 3:
                continue
            print("\n=== SPDP 报文 #%d（encapsulation=%s）===" % (printed, encap.hex()))
            for pid, plen, value in pids:
                name = PID_NAMES.get(pid, "")
                if pid in (0x002F, 0x0031, 0x0032, 0x0030):
                    print("  PID=0x%04x %-20s len=%3d locators=%s"
                          % (pid, name, plen, pid_locators(value)))
                elif pid in (0x0050, 0x005A, 0x0070):
                    print("  PID=0x%04x %-20s len=%3d value=%s"
                          % (pid, name, plen, value[:16].hex()))
                else:
                    print("  PID=0x%04x %-20s len=%3d value=%s"
                          % (pid, name, plen, value[:16].hex()))
    if printed == 0:
        print("\n（没有捕获到 SPDP 报文）")


if __name__ == "__main__":
    sys.exit(main())
