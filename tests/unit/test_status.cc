// Copyright 2026 Stratum Project
// SPDX-License-Identifier: Apache-2.0
//
// **`Status` 的分类判定测试**（`docs/06` 缺口 #6）
//
// `Status` 有 12 个工厂方法，但只有 **5 个**判定方法：
//
//   工厂方法（12）：OK / NotFound / Corruption / NotSupported / InvalidArgument /
//                   IOError / OutOfSpace / AlreadyExists / SnapshotReleased /
//                   ShutdownInProgress / Timeout / Internal
//   判定方法（5） ：IsNotFound / IsCorruption / IsNotSupported / IsIOError / IsOutOfSpace
//
// 【这个不对称本身值得一条测试】
// 没有 `IsTimeout` / `IsAlreadyExists` / `IsInternal` 等 —— 这不是缺陷，
// 而是设计选择（只有"上层需要区分处理"的才给判定方法）。但**必须有测试记录这一点**，
// 否则下一个人会写 `if (s.IsTimeout())` 然后编译失败，或者更糟：以为它是 true。
//
// 【测试要证明的两件事】
//   1. 5 个判定方法**各自对应且只对应一个** code（一一对应，不能串）
//   2. 另外 7 个 code 只能通过 `code()` 比较 —— 且它们**不会**让任何判定方法为 true

// 标准库头文件必须最先包含
#include <cstdio>
#include <string>
#include <vector>

#include "stratum/core/status.h"
#include "test_framework.h"

using namespace stratum;

namespace {

// 一个 code 对应的 5 个判定方法的期望值（只有一个能是 true）
struct Expect {
  const char* name;
  Status s;
  bool is_notfound;
  bool is_corruption;
  bool is_notsupported;
  bool is_io;
  bool is_ospace;
};

std::vector<Expect> AllCodes() {
  return {
      {"OK", Status::OK(), false, false, false, false, false},
      {"NotFound", Status::NotFound("x"), true, false, false, false, false},
      {"Corruption", Status::Corruption("x"), false, true, false, false, false},
      {"NotSupported", Status::NotSupported("x"), false, false, true, false, false},
      {"InvalidArgument", Status::InvalidArgument("x"), false, false, false, false, false},
      {"IOError", Status::IOError("x"), false, false, false, true, false},
      {"OutOfSpace", Status::OutOfSpace("x"), false, false, false, false, true},
      {"AlreadyExists", Status::AlreadyExists("x"), false, false, false, false, false},
      {"SnapshotReleased", Status::SnapshotReleased(), false, false, false, false, false},
      {"ShutdownInProgress", Status::ShutdownInProgress(), false, false, false, false, false},
      {"Timeout", Status::Timeout(), false, false, false, false, false},
      {"Internal", Status::Internal("x"), false, false, false, false, false},
  };
}

}  // namespace

// ===========================================================================
// 一、★ 5 个判定方法与 12 个 code 的完整对应表
// ===========================================================================
//
// 这是本文件最重要的一条：它把"哪个 code 会让哪个判定为 true"钉死成一张表。
// 以后任何人改动 Status 的分类语义，这张表都会立刻红。

STRATUM_TEST(StatusPredicates, FullMappingTable) {
  const auto all = AllCodes();
  STRATUM_CHECK_EQ(all.size(), static_cast<size_t>(12));

  int checked = 0;
  for (const auto& e : all) {
    STRATUM_CHECK_EQ(e.s.IsNotFound(), e.is_notfound);
    STRATUM_CHECK_EQ(e.s.IsCorruption(), e.is_corruption);
    STRATUM_CHECK_EQ(e.s.IsNotSupported(), e.is_notsupported);
    STRATUM_CHECK_EQ(e.s.IsIOError(), e.is_io);
    STRATUM_CHECK_EQ(e.s.IsOutOfSpace(), e.is_ospace);
    ++checked;
  }
  STRATUM_CHECK_EQ(checked, 12);
  std::printf("      [对应表] 12 个 code × 5 个判定 = 60 个布尔值全部符合预期\n");
}

// ===========================================================================
// 二、`ok()` 只在 kOk 时为 true
// ===========================================================================

STRATUM_TEST(StatusPredicates, OkOnlyForSuccess) {
  STRATUM_CHECK_TRUE(Status::OK().ok());

  for (const auto& e : AllCodes()) {
    const bool should_ok = (std::string(e.name) == "OK");
    STRATUM_CHECK_EQ(e.s.ok(), should_ok);
  }
  std::printf("      [ok] 12 个 code 里只有 OK 的 ok() 为 true\n");
}

// ===========================================================================
// 三、★ 另外 7 个 code 没有判定方法（这个不对称要显式记录）
// ===========================================================================
//
// 这不是缺陷，是设计选择：只给"上层需要区分处理"的 code 提供判定方法。
// 其余 7 个必须用 `code() == StatusCode::kXxx` 比较。
//
// 为什么值得测：如果有人以为 `IsTimeout()` 存在，编译会失败（好）；
// 但如果有人以为"`Timeout` 会让 `IsIOError()` 为 true"（因为都是"底层出错"），
// 那是**运行期的逻辑错**，而且不会报错。

STRATUM_TEST(StatusPredicates, NonPredicateCodesAreNotAccidentallyMatched) {
  // 这 7 个 code 没有任何判定方法为 true —— 它们只能用 code() 比较
  const std::vector<Expect> no_pred = {
      {"InvalidArgument", Status::InvalidArgument("x"), false, false, false, false, false},
      {"AlreadyExists", Status::AlreadyExists("x"), false, false, false, false, false},
      {"SnapshotReleased", Status::SnapshotReleased(), false, false, false, false, false},
      {"ShutdownInProgress", Status::ShutdownInProgress(), false, false, false, false, false},
      {"Timeout", Status::Timeout(), false, false, false, false, false},
      {"Internal", Status::Internal("x"), false, false, false, false, false},
  };

  for (const auto& e : no_pred) {
    STRATUM_CHECK_FALSE(e.s.IsNotFound());
    STRATUM_CHECK_FALSE(e.s.IsCorruption());
    STRATUM_CHECK_FALSE(e.s.IsNotSupported());
    STRATUM_CHECK_FALSE(e.s.IsIOError());
    STRATUM_CHECK_FALSE(e.s.IsOutOfSpace());
  }

  // 但它们的 code() 必须各自可区分（否则上层没法用 code() 代替判定）
  STRATUM_CHECK_TRUE(Status::Timeout().code() != Status::Internal("x").code());
  STRATUM_CHECK_TRUE(Status::AlreadyExists("x").code() !=
                     Status::InvalidArgument("x").code());
  // 注意：SnapshotReleased 与 ShutdownInProgress 是两个不同 code，
  // 但默认消息都是空 —— 必须靠 code() 区分，不能靠 message()。
  STRATUM_CHECK_TRUE(Status::SnapshotReleased().code() !=
                     Status::ShutdownInProgress().code());

  std::printf("      [不对称] 6 个无判定的 code 不会误匹配任何判定；code() 仍可区分\n");
}

// ===========================================================================
// 四、`ToString()` 含 code 名（日志与排障依赖它）
// ===========================================================================

STRATUM_TEST(StatusPredicates, ToStringIncludesCodeName) {
  struct Case {
    const char* name;
    Status s;
  };
  const std::vector<Case> cases = {
      {"NotFound", Status::NotFound("detail")},
      {"Corruption", Status::Corruption("detail")},
      {"IOError", Status::IOError("detail")},
      {"Timeout", Status::Timeout("detail")},
  };

  for (const auto& c : cases) {
    const std::string txt = c.s.ToString();
    // 【必须含 code 名】日志里只看到 "detail" 而不知道是哪种错误，
    // 排障时就得回去看代码 —— 而 code 名是分类的唯一线索。
    STRATUM_CHECK_TRUE(txt.find(c.name) != std::string::npos);
    // 消息内容也要在
    STRATUM_CHECK_TRUE(txt.find("detail") != std::string::npos);
  }

  // OK 的 ToString 不应该包含任何错误名
  const std::string ok_txt = Status::OK().ToString();
  STRATUM_CHECK_TRUE(ok_txt.find("Corruption") == std::string::npos);

  std::printf("      [ToString] 4 种错误都含 code 名与消息内容\n");
}

// ===========================================================================
// 五、消息与 code 相互独立（同 code 不同消息、同消息不同 code）
// ===========================================================================

STRATUM_TEST(StatusPredicates, CodeAndMessageAreIndependent) {
  // 同 code、不同消息 → code 相同、message 不同
  const Status a = Status::IOError("disk full");
  const Status b = Status::IOError("permission denied");
  STRATUM_CHECK_EQ(static_cast<int>(a.code()), static_cast<int>(b.code()));
  STRATUM_CHECK_TRUE(a.message() != b.message());
  STRATUM_CHECK_TRUE(a.IsIOError());
  STRATUM_CHECK_TRUE(b.IsIOError());

  // 同消息、不同 code → 判定必须按 code 走，不能按消息
  const Status c = Status::IOError("same");
  const Status d = Status::OutOfSpace("same");
  STRATUM_CHECK_TRUE(c.message() == d.message());
  STRATUM_CHECK_TRUE(c.IsIOError());
  STRATUM_CHECK_FALSE(c.IsOutOfSpace());
  STRATUM_CHECK_TRUE(d.IsOutOfSpace());
  STRATUM_CHECK_FALSE(d.IsIOError());

  // 空消息也是合法构造（NotFound / SnapshotReleased 等的默认参数）
  const Status e = Status::NotFound();
  STRATUM_CHECK_TRUE(e.IsNotFound());
  STRATUM_CHECK_TRUE(e.message().empty());

  std::printf("      [独立] code 与 message 互不影响；空消息合法\n");
}

STRATUM_TEST_MAIN()
