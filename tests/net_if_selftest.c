// net_if 自测（多网卡支持 v0.5.0 / P1 验收）
//
// 覆盖：接口枚举、HTTP 绑定地址、按对端选本机 IP、网段判定、LOCATION 重写。
// 独立可执行，不参与产品构建 —— 手工编译运行：
//   source tools/msvc_env.sh
//   cl /nologo /utf-8 /W3 /I src tests/net_if_selftest.c src/platform/net_if.c \
//      /Fe:build/net_if_selftest.exe /Fo:build/ ws2_32.lib iphlpapi.lib
//   ./build/net_if_selftest.exe
//
// 说明：接口相关断言只对「本机实际存在」的接口成立，换机器结果会变（属正常），
//       字符串重写与网段判定部分是与环境无关的硬断言。

#include "platform/net_if.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

static int g_fail = 0;
static int g_pass = 0;

static void check(int cond, const char* what) {
  if (cond) {
    ++g_pass;
  } else {
    ++g_fail;
    printf("  [FAIL] %s\n", what);
  }
}

static void check_str(const char* got, const char* want, const char* what) {
  int ok = got && want && strcmp(got, want) == 0;
  if (ok) {
    ++g_pass;
  } else {
    ++g_fail;
    printf("  [FAIL] %s\n         期望: %s\n         实际: %s\n", what,
           want ? want : "(null)", got ? got : "(null)");
  }
}

// 重写后应与期望文本一致，且必须是新指针（原包不许被动过）
static void check_rewrite(const char* in, const char* local, const char* want,
                          const char* what) {
  char* out = fr_net_if_rewrite_location(in, local);
  if (!out) {
    ++g_fail;
    printf("  [FAIL] %s（返回 NULL）\n", what);
    return;
  }
  check_str(out, want, what);
  free(out);
}

static void test_scan(void) {
  printf("\n== 1. 接口枚举 ==\n");
  int n = fr_net_if_scan();
  printf("  收录 %d 个接口\n", n);
  check(n > 0, "至少枚举到一个 IPv4 接口");
  check(n == fr_net_if_count(), "fr_net_if_count() 与 scan 返回值一致");
  for (int i = 0; i < n; ++i) {
    printf("  [%d] %-16s mask=%-16s idx=%-4u name=%s\n", i,
           fr_net_if_ip(i) ? fr_net_if_ip(i) : "?", fr_net_if_mask(i) ? fr_net_if_mask(i) : "?",
           fr_net_if_index(i), fr_net_if_name(i) ? fr_net_if_name(i) : "?");
    check(fr_net_if_ip(i) != NULL, "接口 IP 非空");
    check(strcmp(fr_net_if_ip(i), "0.0.0.0") != 0, "接口 IP 不是 0.0.0.0");
    check(strcmp(fr_net_if_ip(i), "127.0.0.1") != 0, "不含回环地址");
  }
  check(fr_net_if_ip(-1) == NULL, "越界下标返回 NULL");
  check(fr_net_if_ip(n) == NULL, "越界下标返回 NULL");

  printf("\n== 2. HTTP 绑定地址 ==\n");
  check_str(fr_net_if_http_bind(), "0.0.0.0", "HTTP 服务绑通配地址（全接口可达）");
}

static void test_local_for(void) {
  printf("\n== 3. 按对端选本机 IP ==\n");
  int n = fr_net_if_count();

  // 每个接口自己的 IP 必然落在自己的网段里 ⇒ 应原样返回自己
  // （若两个接口网段重叠，可能命中更靠前的那个，故按"返回值必须属于某接口"放宽）
  for (int i = 0; i < n; ++i) {
    const char* ip = fr_net_if_ip(i);
    const char* got = fr_net_if_local_for(ip);
    printf("  peer %-16s -> %s\n", ip, got ? got : "(null)");
    int found = 0;
    for (int j = 0; j < n; ++j) {
      if (got && strcmp(got, fr_net_if_ip(j)) == 0) found = 1;
    }
    check(found, "返回的必须是本机某个接口的 IP");
  }

  // 跨网段：回退主接口（列表首项）
  // 注意：变量名别用 far / near —— MSVC 里它们是 16 位内存模型遗留关键字
  const char* fallback = fr_net_if_local_for("8.8.8.8");
  printf("  peer 8.8.8.8（跨网段） -> %s\n", fallback ? fallback : "(null)");
  check_str(fallback, fr_net_if_ip(0), "跨网段回退到主接口（列表首项）");

  check(fr_net_if_local_for("not-an-ip") == NULL, "非法输入返回 NULL");
  check(fr_net_if_local_for(NULL) == NULL, "NULL 输入返回 NULL");
}

static void test_subnet_match(void) {
  printf("\n== 4. 网段判定（GENA 订阅校验用）==\n");
  int n = fr_net_if_count();

  for (int i = 0; i < n; ++i) {
    struct in_addr a;
    if (inet_pton(AF_INET, fr_net_if_ip(i), &a) != 1) continue;
    // 掩码为 0.0.0.0 的条目按设计不参与判定（valid_entry 过滤）
    int masked = strcmp(fr_net_if_mask(i), "0.0.0.0") != 0;
    int got = fr_net_if_subnet_match_nbo(a.s_addr, 0);
    if (masked) {
      check(got == 1, "接口自身 IP 必须命中本机网段");
    } else {
      printf("  （%s 掩码无效，跳过）\n", fr_net_if_ip(i));
    }
  }

  struct in_addr v;
  inet_pton(AF_INET, "8.8.8.8", &v);
  check(fr_net_if_subnet_match_nbo(v.s_addr, 0) == 0, "公网地址不命中本机网段");
  check(fr_net_if_subnet_match_nbo(v.s_addr, 1) == 0,
        "接口列表非空时忽略回退形参（回退只在枚举失败时生效）");

  inet_pton(AF_INET, "127.0.0.1", &v);
  check(fr_net_if_subnet_match_nbo(v.s_addr, 0) == 0, "回环地址不命中（回环不入列表）");

  printf("\n== 5. LOCATION 重写 ==\n");
  check_rewrite("NOTIFY * HTTP/1.1\r\n"
                "CACHE-CONTROL: max-age=1800\r\n"
                "LOCATION: http://192.0.2.2:49152/fr/device.xml\r\n"
                "NT: upnp:rootdevice\r\n\r\n",
                "192.0.2.1",
                "NOTIFY * HTTP/1.1\r\n"
                "CACHE-CONTROL: max-age=1800\r\n"
                "LOCATION: http://192.0.2.1:49152/fr/device.xml\r\n"
                "NT: upnp:rootdevice\r\n\r\n",
                "同长度替换：主机段换成对应网卡 IP");

  check_rewrite("LOCATION: http://1.2.3.4:1900/d.xml\r\n", "192.0.2.1",
                "LOCATION: http://192.0.2.1:1900/d.xml\r\n",
                "变长替换（短 -> 长）");

  check_rewrite("LOCATION: http://192.0.2.1:49152/fr/device.xml\r\n",
                "10.0.0.1", "LOCATION: http://10.0.0.1:49152/fr/device.xml\r\n",
                "变长替换（长 -> 短）");

  check_rewrite("location: http://192.0.2.2/x.xml\r\n", "192.0.2.1",
                "location: http://192.0.2.1/x.xml\r\n",
                "头名大小写不敏感，且不改动原大小写");

  check_rewrite("LOCATION: http://10.0.0.5/d.xml\r\n", "192.0.2.1",
                "LOCATION: http://192.0.2.1/d.xml\r\n",
                "无端口形式：主机段到 '/' 结束");

  check_rewrite("LOCATION: http://[::1]:1900/d.xml\r\n", "192.0.2.1",
                "LOCATION: http://[::1]:1900/d.xml\r\n",
                "IPv6 字面量不动（本补丁只管 IPv4 主机段）");

  check_rewrite("NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n\r\n",
                "192.0.2.1",
                "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n\r\n",
                "没有 LOCATION 时返回原样副本");

  check_rewrite("LOCATION: ftp://192.0.2.2/x\r\n", "192.0.2.1",
                "LOCATION: ftp://192.0.2.2/x\r\n",
                "非 http:// 形式不动");

  // 所有权：原包内容不许被改
  const char* orig = "LOCATION: http://192.0.2.2:49152/fr/device.xml\r\n";
  char* copy = strdup(orig);
  char* out = fr_net_if_rewrite_location(copy, "192.0.2.1");
  check(out != NULL && out != copy, "返回新指针，不原地修改");
  check_str(copy, orig, "原包内容保持不变（所有权在调用方）");
  free(copy);
  free(out);

  check(fr_net_if_rewrite_location(NULL, "1.2.3.4") == NULL, "NULL 包返回 NULL");
  check(fr_net_if_rewrite_location(orig, NULL) != NULL,
        "local_ip 为 NULL 时返回原样副本（不崩）");

  // 重复扫描必须幂等（S2/S3 会在每次响应时读列表，列表不能被翻动）
  int n2 = fr_net_if_scan();
  check(n2 == n, "重复 scan 结果稳定（幂等）");
}

static void test_doc_localize(void) {
  printf("\n== 6. 描述文档 URLBase 按本请求 Host 头重写 ==\n");
  static const char kDoc[] =
      "<?xml version=\"1.0\"?>\r\n"
      "<root>\r\n"
      "<URLBase>http://192.0.2.1:49152/</URLBase>\r\n"
      "<controlURL>/fr/control/AVTransport</controlURL>\r\n"
      "</root>\r\n";
  size_t len = 0;

  // ① Host 带端口：整个 authority 被替换，且**不会**出现 "ip:端口:端口"
  //    （d117 实测：只换主机段会拼出 http://ip:49152:49152/，SOAP 全挂）
  len = fr_net_if_doc_localize(kDoc, sizeof(kDoc) - 1, "192.0.2.2:49152", 18);
  check(len > 0, "Host 带端口时应重写");
  char* got = fr_net_if_doc_take(&len);
  check(got != NULL, "take() 取到替换结果");
  check(len == sizeof(kDoc) - 1, "Host 等长替换时长度不变");
  check_str(got,
            "<?xml version=\"1.0\"?>\r\n"
            "<root>\r\n"
            "<URLBase>http://192.0.2.2:49152/</URLBase>\r\n"
            "<controlURL>/fr/control/AVTransport</controlURL>\r\n"
            "</root>\r\n",
            "authority 整体替换（主机+端口），无重复端口");
  free(got);
  check(fr_net_if_doc_take(&len) == NULL, "take() 之后槽位已清空");

  // ② Host 不带端口：必须保留原 authority 的端口（d112 教训，否则按 80 发 SOAP）
  len = fr_net_if_doc_localize(kDoc, sizeof(kDoc) - 1, "10.0.0.5", 8);
  check(len > 0, "Host 不带端口时仍应重写");
  got = fr_net_if_doc_take(&len);
  check_str(got,
            "<?xml version=\"1.0\"?>\r\n"
            "<root>\r\n"
            "<URLBase>http://10.0.0.5:49152/</URLBase>\r\n"
            "<controlURL>/fr/control/AVTransport</controlURL>\r\n"
            "</root>\r\n",
            "Host 不带端口时保留原端口");
  free(got);

  // ③ 与现值相同：无需替换（常见路径 —— 客户端就在优选网卡网段上）
  check(fr_net_if_doc_localize(kDoc, sizeof(kDoc) - 1, "192.0.2.1:49152", 18) == 0,
        "authority 与现值相同 → 返回 0（不发替换版）");
  check(fr_net_if_doc_take(&len) == NULL, "返回 0 时槽位必须为空");

  // ④ 变长替换：长 -> 短 与 短 -> 长
  len = fr_net_if_doc_localize(kDoc, sizeof(kDoc) - 1, "10.0.0.5:49152", 14);
  got = fr_net_if_doc_take(&len);
  check(len == sizeof(kDoc) - 1 - 4, "长 -> 短：长度差 = 主机段差");
  free(got);

  // ⑤ Host 是客户端可控的输入：注入字符必须被拒绝
  check(fr_net_if_doc_localize(kDoc, sizeof(kDoc) - 1,
                               "192.0.2.1:49152</URLBase><x>", 30) == 0,
        "Host 含 '<' → 拒绝（防注入）");
  check(fr_net_if_doc_localize(kDoc, sizeof(kDoc) - 1, "a b", 3) == 0,
        "Host 含空格 → 拒绝");

  // ⑥ IPv6 字面量 / 无 URLBase / 非法入参
  static const char kDoc6[] = "<URLBase>http://[fe80::1]:49152/</URLBase>";
  check(fr_net_if_doc_localize(kDoc6, sizeof(kDoc6) - 1, "10.0.0.5", 8) == 0,
        "URLBase 是 IPv6 字面量 → 不动（本模块只管 IPv4）");
  static const char kDocNoBase[] = "<root><friendlyName>x</friendlyName></root>";
  check(fr_net_if_doc_localize(kDocNoBase, sizeof(kDocNoBase) - 1, "10.0.0.5", 8) == 0,
        "文档里没有 URLBase → 不动");
  check(fr_net_if_doc_localize(NULL, 0, "10.0.0.5", 8) == 0, "NULL 文档 → 0");
  check(fr_net_if_doc_localize(kDoc, sizeof(kDoc) - 1, NULL, 0) == 0, "NULL Host → 0");
  check(fr_net_if_doc_take(&len) == NULL, "异常路径后槽位保持为空");
}

int main(void) {
  printf("net_if 自测（v0.5.0 多网卡支持 P1）\n");
  test_scan();
  test_local_for();
  test_subnet_match();
  test_doc_localize();
  printf("\n------------------------------\n");
  printf("通过 %d 项，失败 %d 项\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
