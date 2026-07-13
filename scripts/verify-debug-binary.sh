#!/bin/bash
# ---- CRLF 自愈 ----
if grep -q $'\r' "$0" 2>/dev/null; then
  tmpf=$(mktemp 2>/dev/null) || tmpf="/tmp/.vdb.$$.clean"
  tr -d '\r' < "$0" > "$tmpf" && cat "$tmpf" > "$0" && rm -f "$tmpf"
  exec "$0" "$@"
fi

# =============================================================================
# 验证 UOS 上运行的 agent 是不是 debug 版
# =============================================================================

set -u
LOG="/tmp/verify-debug-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1

RED=$'\033[1;31m'; GRN=$'\033[1;32m'; YEL=$'\033[1;33m'; CYN=$'\033[1;36m'; DIM=$'\033[2m'; RST=$'\033[0m'
ok()   { echo "${GRN}[ OK ]${RST} $*"; }
fail() { echo "${RED}[FAIL]${RST} $*"; }
warn() { echo "${YEL}[WARN]${RST} $*"; }
hdr()  { echo; echo "${CYN}========== $* ==========${RST}"; }

echo "Agent debug 版验证"
echo "时间: $(date)"

# --- 1. 找到运行中的 agent 进程 ---
hdr "1. 运行中的 agent 进程"
AGENT_PID=$(ps -e -o pid,comm 2>/dev/null | grep -iE 'meshagent' | grep -v grep | head -1 | awk '{print $1}')
if [ -z "$AGENT_PID" ]; then
    fail "没有 meshagent 进程在运行!"
    fail "  -> agent 根本没起来, /tmp/slave 当然没有"
    fail "  -> 可能是 debug 二进制损坏导致启动失败"
    exit 1
fi
echo "PID: $AGENT_PID"
ps -p "$AGENT_PID" -o pid,user,comm,args 2>/dev/null

# --- 2. 找到二进制路径 ---
hdr "2. agent 二进制路径"
# 方法1: /proc/PID/exe (最准)
EXE_PATH=$(readlink /proc/$AGENT_PID/exe 2>/dev/null)
if [ -n "$EXE_PATH" ]; then
    ok "通过 /proc/$AGENT_PID/exe 找到: $EXE_PATH"
else
    warn "/proc/$AGENT_PID/exe 读不到, 尝试其他方法"
    # 方法2: 从 cmdline
    EXE_PATH=$(tr '\0' '\n' < /proc/$AGENT_PID/cmdline 2>/dev/null | head -1)
    [ -n "$EXE_PATH" ] && echo "从 cmdline: $EXE_PATH"
fi

if [ -z "$EXE_PATH" ]; then
    fail "找不到二进制路径"
    exit 1
fi

# --- 3. 检查二进制完整性 ---
hdr "3. 磁盘上二进制的完整性"
echo "${DIM}-- file 类型 --${RST}"
file "$EXE_PATH" 2>&1

# 关键: 是不是 ELF?
if file "$EXE_PATH" 2>/dev/null | grep -q 'ELF'; then
    ok "是有效的 ELF 可执行文件"
else
    fail "$EXE_PATH 不是 ELF!"
    fail "  -> 二进制在传输时被损坏了 (可能经过了 Windows 文本模式)"
    fail "  -> 必须用 zip/base64 方式重新传输"
    exit 1
fi

echo
echo "${DIM}-- 大小 (debug 版应为 3773840 字节 / 3.6M) --${RST}"
ls -la "$EXE_PATH"

echo
echo "${DIM}-- md5 (debug 版应为 80c9d4dece059c228e6d4b92f5881ef2) --${RST}"
if command -v md5sum >/dev/null 2>&1; then
    md5sum "$EXE_PATH"
elif command -v md5 >/dev/null 2>&1; then
    md5 "$EXE_PATH"
else
    warn "无 md5 命令"
fi

# --- 4. 关键: 检查二进制里有没有诊断字符串 ---
hdr "4. 诊断字符串检查 (决定性)"
echo "如果这是 debug 版, strings 里应该能找到诊断字符串:"
echo
for pattern in "SLAVE fork: requested uid" "SLAVE setuid" "Before XOpenDisplay" "XOpenDisplay returned" "SLAVE after setenv"; do
    if strings "$EXE_PATH" 2>/dev/null | grep -qF "$pattern"; then
        ok "找到: $pattern"
    else
        fail "缺失: $pattern"
    fi
done

echo
echo "${DIM}-- 诊断字符串总数 (debug 版应 ~7 个) --${RST}"
strings "$EXE_PATH" 2>/dev/null | grep -cE "SLAVE fork|SLAVE setuid|SLAVE after setenv|Before XOpenDisplay|XOpenDisplay returned|XOpenDisplay FAILED|dlopen\("

# --- 5. /tmp/slave 和 /tmp/master 检查 ---
hdr "5. 日志文件状态"
for f in /tmp/slave /tmp/master; do
    if [ -e "$f" ]; then
        ok "$f 存在"
        ls -la "$f"
        echo "${DIM}内容前 5 行:${RST}"
        head -5 "$f" 2>/dev/null || warn "(无权读)"
    else
        fail "$f 不存在"
    fi
done

# --- 6. 启动方式和时间 ---
hdr "6. agent 启动信息"
echo "${DIM}-- 进程启动时间 (判断服务有没有重启过) --${RST}"
ps -p "$AGENT_PID" -o pid,lstart,etime 2>/dev/null

echo
echo "${DIM}-- 进程用户 --${RST}"
AGENT_USER=$(ps -p "$AGENT_PID" -o user= 2>/dev/null)
echo "以 '$AGENT_USER' 运行"
[ "$AGENT_USER" = "root" ] && warn "root 运行 (这是预期的, 但意味着 /tmp/slave 属主会是 root)"

# =============================================================================
hdr "判定"
echo
DEBUG_STRINGS=$(strings "$EXE_PATH" 2>/dev/null | grep -cE "SLAVE fork|SLAVE setuid|SLAVE after setenv|Before XOpenDisplay|XOpenDisplay returned")
if [ "$DEBUG_STRINGS" -ge 5 ]; then
    echo "${GRN}>>> 这是 debug 版!${RST}"
    echo "    但 /tmp/slave 不存在, 说明:"
    echo "    - agent 跑了 debug 版, 但还没触发过连接 (去 web 连一次试试)"
    echo "    - 或触发连接后 slave 子进程在 fork 之前就出问题了"
    echo "    - 或 /tmp 不可写 (检查: ls -ld /tmp, 应是 1777)"
elif [ "$DEBUG_STRINGS" -gt 0 ]; then
    echo "${YEL}>>> 似乎是部分 debug 版? 字符串不全${RST}"
else
    echo "${RED}>>> 这不是 debug 版!${RST}"
    echo "    UOS 上跑的是旧二进制或被损坏的文件."
    echo "    可能原因:"
    echo "    1. debug 二进制传输到 UOS 时损坏 (经过 Windows 文本模式)"
    echo "    2. 管理员替换了错误的文件"
    echo "    3. 服务没重启, 还在跑旧二进制"
    echo "    4. 替换路径不对 (agent 不在你以为的位置)"
    echo
    echo "${CYN}解决方案: 用 base64 编码方式重新传输二进制 (见下一步)${RST}"
fi

echo
echo "${DIM}日志: $LOG${RST}"
