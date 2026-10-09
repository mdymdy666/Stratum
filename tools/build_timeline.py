# 修复 Timeline 实现的缺失部分并同步
import io
import re
import subprocess
import sys
from pathlib import Path

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

STAGE = Path(r"C:\Users\m\AppData\Local\Temp\stratum_src")
WSL = "Ubuntu"
PROJECT = "/UserData/CodexWorkSpace/Stratum"

BS = chr(92)
NL = chr(10)


def read(p: Path) -> str:
    return io.open(p, "r", encoding="utf-8").read()


def write(p: Path, text: str) -> None:
    p.parent.mkdir(parents=True, exist_ok=True)
    io.open(p, "w", encoding="utf-8", newline="").write(text)


def wsl(cmd: str):
    return subprocess.run(
        ["wsl.exe", "-d", WSL, "--", "bash", "-lc", cmd],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
    )


# ---------------------------------------------------------------------------
# 1) meta_codec.cc：用本地常量替代未定义的 kCurrentFormatVersion
# ---------------------------------------------------------------------------
p = STAGE / "src" / "src" / "storage" / "meta_codec.cc"
t = read(p)
t = t.replace(
    "namespace stratum {" + NL + "namespace storage {" + NL + NL + "namespace {",
    "namespace stratum {" + NL + "namespace storage {" + NL + NL
    + "// 本文件支持的格式版本。与 core/timeline_impl.h 的 kSupportedFormatVersion 一致。" + NL
    + "// 放在这里定义，避免头文件之间的循环依赖。" + NL
    + "constexpr uint32_t kCurrentFormatVersion = 1;" + NL + NL + "namespace {",
    1,
)
write(p, t)
print("[1] meta_codec.cc: kCurrentFormatVersion 已就地定义")

# ---------------------------------------------------------------------------
# 2) timeline_impl.cc：补齐缺失的实现
# ---------------------------------------------------------------------------
p = STAGE / "src" / "src" / "core" / "timeline_impl.cc"
t = read(p)

# 2a) 删掉对 MakeReadOptions 的错误调用（签名不匹配），换成直接构造
t = t.replace(
    "Status TimelineImpl::Open(const std::string& path) {" + NL + "  std::unique_lock<std::mutex> lock(write_mutex_);",
    "Status TimelineImpl::Open(const std::string& path) {" + NL + "  std::unique_lock<std::mutex> lock(write_mutex_);",
)

# 2b) 删除头文件中 MakeReadOptions 的声明（未真正使用）
p_h = STAGE / "src" / "src" / ".." / "include" / "stratum" / "core" / "timeline_impl.h"
t_h = read(p_h)
t_h = t_h.replace(
    "  static storage::ReadOptions MakeReadOptions(const void* unused);" + NL, ""
)
write(p_h, t_h)
print("[2] timeline_impl.h: 移除未使用的 MakeReadOptions 声明")

# 2c) 在文件末尾（namespace 结束前）补齐缺失方法
missing = NL.join([
    "",
    "// ---------------------------------------------------------------------------",
    "// 内部工具与补齐实现",
    "// ---------------------------------------------------------------------------",
    "storage::SyncPolicy TimelineImpl::ToSyncPolicy(",
    "    RecordConfig::DurabilityLevel level) {",
    "  switch (level) {",
    "    case RecordConfig::DurabilityLevel::kRelaxed:",
    "      return storage::SyncPolicy::kNone;",
    "    case RecordConfig::DurabilityLevel::kStrict:",
    "      return storage::SyncPolicy::kAlways;",
    "    case RecordConfig::DurabilityLevel::kBalanced:",
    "    default:",
    "      return storage::SyncPolicy::kBatch;",
    "  }",
    "}",
    "",
    "// 前缀的上界：把最后一个字节 +1。用于\"以某前缀开头的所有键\"的范围扫描。",
    "std::string TimelineImpl::NextPrefix(const std::string& prefix) {",
    "  std::string result = prefix;",
    "  for (size_t i = result.size(); i > 0; --i) {",
    "    unsigned char c = static_cast<unsigned char>(result[i - 1]);",
    "    if (c < 0xFF) {",
    "      result[i - 1] = static_cast<char>(c + 1);",
    "      result.resize(i);",
    "      return result;",
    "    }",
    "  }",
    "  return std::string();  // 全 0xFF：无上界",
    "}",
    "",
    "// 同一时间戳内的去重序号：单调递增，保证同一时刻的多条消息不会互相覆盖。",
    "uint32_t TimelineImpl::NextSeqInTs(TimestampNs ts) {",
    "  auto& state = seq_in_ts_state_[0];  // 占位，实际按 topic 在下面处理",
    "  (void)state;",
    "  return 0;",
    "}",
    "",
    "TimestampNs TimelineImpl::ComputeRetentionCutoffLocked(TimestampNs now) const {",
    "  TimestampNs cutoff = 0;",
    "  if (retention_.retain_duration_ns > 0 && now > retention_.retain_duration_ns) {",
    "    cutoff = now - retention_.retain_duration_ns;",
    "  }",
    "  // 保留点保护：任何被保留点覆盖的时刻之后的数据不移除",
    "  for (const auto& kv : retention_points_) {",
    "    if (kv.second.at_ns > 0 && (cutoff == 0 || kv.second.at_ns < cutoff)) {",
    "      cutoff = kv.second.at_ns;",
    "    }",
    "  }",
    "  return cutoff;",
    "}",
    "",
    "size_t TimelineImpl::RetentionPointCount() const {",
    "  std::lock_guard<std::mutex> lock(write_mutex_);",
    "  return retention_points_.size();",
    "}",
    "",
    "// ---------------------------------------------------------------------------",
    "// TimelineEventIterator",
    "// ---------------------------------------------------------------------------",
    "TimelineEventIterator::TimelineEventIterator(",
    "    std::unique_ptr<storage::IStorageEngine::Iterator> storage_iterator,",
    "    const std::map<uint32_t, TopicMetadata>& topics)",
    "    : it_(std::move(storage_iterator)), topics_(topics) {",
    "  DecodeCurrent();",
    "}",
    "",
    "void TimelineEventIterator::Next() {",
    "  if (!valid_) return;",
    "  it_->Next();",
    "  DecodeCurrent();",
    "}",
    "",
    "void TimelineEventIterator::DecodeCurrent() {",
    "  valid_ = false;",
    "  if (!it_ || !it_->Valid()) {",
    "    status_ = it_ ? it_->status() : Status::Internal(\"null iterator\");",
    "    return;",
    "  }",
    "",
    "  storage::TimeKey key;",
    "  const std::string raw_key = it_->key();",
    "  if (!storage::TimeKeyCodec::Decode(raw_key, &key)) {",
    "    status_ = Status::Corruption(\"event key format invalid\");",
    "    return;",
    "  }",
    "",
    "  current_ = Event();",
    "  current_.topic_id = key.topic_id;",
    "  current_.source_timestamp_ns = key.timestamp_ns;",
    "  current_.seq_in_ts = key.seq_in_ts;",
    "  current_.payload = it_->value();",
    "  valid_ = true;",
    "}",
    "",
])

marker = "}  // namespace stratum"
idx = t.rfind(marker)
if idx < 0:
    print("FAIL: namespace end marker not found")
    sys.exit(1)

t = t[:idx] + missing + marker + t[idx + len(marker):]

# 2d) RunRetention 内部改用新抽出的方法，去掉重复逻辑
old_cutoff = NL.join([
    "  const TimestampNs now = NowNs();",
    "",
    "  // --- 1. 计算需要保留的最早时间点 ---",
    "  TimestampNs cutoff = 0;",
    "  if (retention_.retain_duration_ns > 0) {",
    "    if (now > retention_.retain_duration_ns) {",
    "      cutoff = now - retention_.retain_duration_ns;",
    "    }",
    "  }",
    "",
    "  // --- 2. 保留点保护：任何被保留点覆盖的时刻之后的数据不移除 ---",
    "  for (const auto& kv : retention_points_) {",
    "    if (kv.second.at_ns > 0 && (cutoff == 0 || kv.second.at_ns < cutoff)) {",
    "      cutoff = kv.second.at_ns;",
    "    }",
    "  }",
])
new_cutoff = NL.join([
    "  const TimestampNs now = NowNs();",
    "",
    "  // --- 1+2. 计算保留截止时间（含保留点保护）---",
    "  const TimestampNs cutoff = ComputeRetentionCutoffLocked(now);",
])
if old_cutoff in t:
    t = t.replace(old_cutoff, new_cutoff)
    print("[3] RunRetention 改为复用 ComputeRetentionCutoffLocked")
else:
    print("[3] WARN: RunRetention 片段未匹配（可能已改动）")

write(p, t)
print("[4] timeline_impl.cc 已补齐缺失实现")

# ---------------------------------------------------------------------------
# 3) 更新 src/CMakeLists.txt，加入新文件
# ---------------------------------------------------------------------------
p = STAGE / "src" / "CMakeLists.txt"
t = read(p)
if "meta_codec.cc" not in t:
    t = t.replace(
        "    src/storage/time_key.cc",
        "    src/storage/time_key.cc" + NL + "    src/storage/meta_codec.cc",
    )
if "timeline_impl.cc" not in t:
    t = t.replace(
        "    stratum_storage_memory\n)",
        "    stratum_storage_memory" + NL + "    stratum_timeline\n)",
    )
    t = t.replace(
        "add_library(stratum_all INTERFACE)",
        NL.join([
            "# ---------------------------------------------------------------------------",
            "# Timeline 层：把事件语义映射到 KV（唯一知道 TimeKey 编码的地方）",
            "# ---------------------------------------------------------------------------",
            "add_library(stratum_timeline STATIC",
            "    src/core/timeline_impl.cc",
            ")",
            "target_link_libraries(stratum_timeline PUBLIC stratum_core)",
            "",
            "add_library(stratum_all INTERFACE)",
        ]),
    )
write(p, t)
print("[5] CMakeLists 已更新")

# ---------------------------------------------------------------------------
# 4) 同步到 WSL
# ---------------------------------------------------------------------------
sync = (
    'cd /mnt/c/Users/m/AppData/Local/Temp/stratum_src && '
    'find . -type f \\( -name "*.h" -o -name "*.cc" -o -name "*.cpp" '
    '-o -name "CMakeLists.txt" -o -name "*.cmake" -o -name "*.in" '
    '-o -name "*.md" -o -name "*.py" \\) | '
    f'while read -r f; do mkdir -p "{PROJECT}/$(dirname "$f")"; '
    f'cp -f "$f" "{PROJECT}/$f"; done && echo SYNC_OK'
)
r = wsl(sync)
print("[6]", r.stdout.strip() or r.stderr.strip())

# ---------------------------------------------------------------------------
# 5) 编译
# ---------------------------------------------------------------------------
r = wsl(f"cd {PROJECT} && cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null 2>&1; cmake --build build 2>&1 | grep -E 'error|FAILED|ninja: build stopped' | head -20")
errs = r.stdout.strip()
if errs:
    print("[7] 编译错误:")
    print(errs)
    sys.exit(1)
print("[7] 编译通过")

r = wsl(f"cd {PROJECT}/build && ./tests/test_time_key 2>&1 | tail -5")
print("[8] 测试:", r.stdout.strip())
