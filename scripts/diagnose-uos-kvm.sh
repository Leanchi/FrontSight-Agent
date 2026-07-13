#!/bin/bash
# ---- CRLF 自愈: 传输若带入 \r, 就地清除并重新执行自己 ----
if grep -q $'\r' "$0" 2>/dev/null; then
  tmpf=$(mktemp 2>/dev/null) || tmpf="/tmp/.diag.$$.clean"
  tr -d '\r' < "$0" > "$tmpf" && cat "$tmpf" > "$0" && rm -f "$tmpf"
  exec "$0" "$@"
fi
# =============================================================================
# MeshAgent KVM 崩溃诊断脚本 (UOS / 非特权)
#
# 用途: 诊断 "KVM Child process has unexpectedly exited"
#       这里的 KVM 是 MeshCentral 桌面抓屏模块, 不是 Kernel Virtual Machine.
#
# 用法:
#   chmod +x diagnose-uos-kvm.sh
#   ./diagnose-uos-kvm.sh
#
# 全程不需要 sudo. 结果会同时输出到屏幕和日志文件.
# =============================================================================

set -u

LOG="/tmp/meshagent-diagnose-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$LOG") 2>&1

# --- 颜色 (终端可见, 写入文件时是 ANSI 码, 无害) ---
RED=$'\033[1;31m'
GRN=$'\033[1;32m'
YEL=$'\033[1;33m'
CYN=$'\033[1;36m'
DIM=$'\033[2m'
RST=$'\033[0m'

ok()   { echo "${GRN}[ OK ]${RST} $*"; }
fail() { echo "${RED}[FAIL]${RST} $*"; }
warn() { echo "${YEL}[WARN]${RST} $*"; }
hdr()  { echo; echo "${CYN}========== $* ==========${RST}"; }

echo "MeshAgent KVM 崩溃诊断"
echo "时间: $(date)"
echo "日志: $LOG"
echo "主机: $(hostname)  内核: $(uname -r)"
echo "当前用户: $(id 2>/dev/null)"

# 追踪命中哪些假设 (用于结尾判读)
HIT_LIB_MISSING=0
HIT_UID_PROBE_FAIL=0
HIT_XAUTH_MISSING=0

# -----------------------------------------------------------------------------
# A. 运行时库完整性 (假设①: dlopen 失败)
#    代码: meshcore/KVM/Linux/linux_kvm.c -> kvm_init() 用 dlopen 加载这些库
# -----------------------------------------------------------------------------
hdr "A. 运行时 X11 / 图像库检查"

LIBS="libX11.so.6 libXtst.so.6 libXext.so.6 libXfixes.so.3 libxkbfile.so.1 libjpeg.so.62"
for L in $LIBS; do
    hit=$(ldconfig -p 2>/dev/null | grep "$L" | head -1)
    if [ -z "$hit" ]; then
        fail "缺失运行库: $L"
        HIT_LIB_MISSING=1
    else
        ok "$L  ->  $(echo "$hit" | sed 's/.*=> //')"
    fi
done

# 找到 agent 二进制并 ldd
echo
echo "${DIM}-- 定位 agent 二进制 --${RST}"
AGENT_BIN=""
for cand in \
    "$(ps -eo pid,user,comm,args 2>/dev/null | grep -iE 'meshagent' | grep -v grep | head -1 | awk '{for(i=4;i<=NF;i++) printf "%s ", $i; print ""}' | sed 's/ *$//')" \
    "/opt/meshagent/meshagent_x86-64" \
    "/usr/local/meshagent/meshagent_x86-64"; do
    [ -n "${cand:-}" ] && [ -x "$cand" ] && AGENT_BIN="$cand" && break
done

if [ -n "$AGENT_BIN" ] && [ -r "$AGENT_BIN" ]; then
    ok "找到 agent: $AGENT_BIN"
    echo "${DIM}-- file --${RST}"
    file "$AGENT_BIN" 2>/dev/null
    echo "${DIM}-- ldd (只看异常/X11/jpeg) --${RST}"
    ldd "$AGENT_BIN" 2>/dev/null | grep -iE 'not found|X11|Xtst|Xext|Xfixes|xkb|jpeg|ssl|crypto' || echo "  (无相关动态依赖或无可读权限)"
    if ldd "$AGENT_BIN" 2>/dev/null | grep -q 'not found'; then
        fail "ldd 报告 'not found' 依赖 (见上)"
        HIT_LIB_MISSING=1
    fi
else
    warn "未自动定位到 agent 二进制 (可能没在运行, 或路径非标准)"
    warn "如果是手动指定路径, 请: ldd /你的/agent/路径"
fi

# -----------------------------------------------------------------------------
# B. uid 探测链路 (假设②: loginctl 在 UOS 安全策略下失效)
#    代码: modules/user-sessions.js consoleUid() 用 loginctl 找桌面用户
#          agentcore.c:1289 拿到 uid 传给 kvm_relay_setup
#          linux_kvm.c:1496  if(uid!=0) setuid(uid)  <- uid=0 时保持 root
# -----------------------------------------------------------------------------
hdr "B. 桌面用户 / uid 探测 (loginctl)"

echo "${DIM}-- loginctl 是否存在 --${RST}"
if command -v loginctl >/dev/null 2>&1; then
    ok "loginctl 存在: $(command -v loginctl)"
else
    fail "loginctl 不存在  ->  consoleUid() 无主路径"
    HIT_UID_PROBE_FAIL=1
fi

echo "${DIM}-- loginctl list-sessions (关键!) --${RST}"
SESSIONS_OUT=$(loginctl list-sessions --no-legend 2>&1)
echo "$SESSIONS_OUT"
if echo "$SESSIONS_OUT" | grep -qiE 'error|denied|failed|not registered|no session'; then
    fail "loginctl 被拒/报错  ->  安全策略拦截, uid 探测会失败"
    HIT_UID_PROBE_FAIL=1
elif [ -z "$(echo "$SESSIONS_OUT" | tr -d '[:space:]')" ]; then
    warn "loginctl 列不出任何会话  ->  deepin/UOS 会话可能未注册到 systemd-logind"
    HIT_UID_PROBE_FAIL=1
else
    ok "loginctl 能列出会话"
fi

echo "${DIM}-- 你的会话详情 --${RST}"
FIRST_SID=$(echo "$SESSIONS_OUT" 2>/dev/null | awk '{print $1}' | head -1)
if [ -n "$FIRST_SID" ]; then
    loginctl show-session "$FIRST_SID" 2>&1 | grep -E '^(Name|User|State|Display|Type|Remote|Service)=' || true
    if ! loginctl show-session "$FIRST_SID" 2>/dev/null | grep -q '^State=active'; then
        warn "当前会话 State 不是 active"
    fi
fi

echo
echo "${DIM}-- agent 进程实际以什么用户运行 (关键!) --${RST}"
ps -eo pid,user,comm,args 2>/dev/null | grep -iE 'meshagent' | grep -v grep || warn "没看到 meshagent 进程在运行"
AGENT_USER=$(ps -eo user,comm 2>/dev/null | grep -iE 'meshagent' | grep -v grep | awk '{print $1}' | head -1)
if [ -n "$AGENT_USER" ]; then
    if [ "$AGENT_USER" = "root" ]; then
        warn "agent 以 root 运行  ->  若 uid 探测失败, slave 子进程会保持 root 去 连 X, 必崩"
        [ "$HIT_UID_PROBE_FAIL" = "1" ] || warn "(虽然 loginctl 暂未报错, 但 root 启动 + X 授权是高危组合)"
    else
        ok "agent 以 '$AGENT_USER' 运行 (非 root, X 授权通常没问题)"
    fi
fi

# -----------------------------------------------------------------------------
# C. X 环境 (假设③: XAUTHORITY/DISPLAY 缺失或 root 无授权)
#    代码: linux_kvm.c:1512 setenv("XAUTHORITY"/"DISPLAY", ...)
#          linux_kvm.c:590  XOpenDisplay(CURRENT_XDISPLAY)
# -----------------------------------------------------------------------------
hdr "C. X 授权 / DISPLAY 环境"

echo "DISPLAY=${DISPLAY:-<空>}"
echo "XAUTHORITY=${XAUTHORITY:-<空>}"

if [ -z "${DISPLAY:-}" ]; then
    fail "当前 shell 没有 DISPLAY  (但这不一定代表 agent 没有, 仅参考)"
fi

echo "${DIM}-- /tmp/.X11-unix/ (X server 套接字) --${RST}"
if [ -d /tmp/.X11-unix ]; then
    ls -la /tmp/.X11-unix/ 2>/dev/null || warn "目录存在但无权列出"
else
    fail "/tmp/.X11-unix 不存在  ->  可能没在跑 Xorg (Wayland?)"
fi

echo "${DIM}-- xauth list (读自己的授权) --${RST}"
if command -v xauth >/dev/null 2>&1; then
    xauth list 2>&1 | head -5
    if ! xauth list 2>/dev/null | grep -q '\.' ; then
        warn "xauth 列不出条目  ->  当前用户可能没有 X 授权"
        HIT_XAUTH_MISSING=1
    fi
else
    warn "xauth 命令不存在"
fi

echo "${DIM}-- 尝试 XOpenDisplay 等价测试 (xdpyinfo) --${RST}"
if command -v xdpyinfo >/dev/null 2>&1 && [ -n "${DISPLAY:-}" ]; then
    if xdpyinfo >/dev/null 2>&1; then
        ok "当前用户能连上 X server ($DISPLAY)"
    else
        fail "当前用户连不上 X server ($DISPLAY)  ->  X 授权问题"
        HIT_XAUTH_MISSING=1
    fi
else
    warn "xdpyinfo 不存在或无 DISPLAY, 跳过"
fi

# -----------------------------------------------------------------------------
# D. 系统日志 (可能被 journalctl 权限拒, 试一下)
# -----------------------------------------------------------------------------
hdr "D. 系统日志 (尝试读取)"

if command -v journalctl >/dev/null 2>&1; then
    echo "${DIM}-- meshagent service 最近日志 --${RST}"
    journalctl -u meshagent.service --no-pager -n 40 2>&1 | tail -25 || \
        warn "读 meshagent.service 日志被拒 (非 root 常见)"
    echo
    echo "${DIM}-- 全局日志里和 mesh/kvm/slave 相关的行 --${RST}"
    journalctl --no-pager -n 300 2>/dev/null | grep -iE 'mesh|kvm|slave' | tail -15 || \
        warn "全局日志无权读或无相关行"
else
    warn "无 journalctl"
fi

# 尝试读 agent 自己的 slave 日志 (如果 SLAVELOG 开过)
echo "${DIM}-- agent slave 调试日志 (若存在) --${RST}"
for f in /tmp/slave /tmp/master; do
    if [ -r "$f" ]; then
        echo "== $f (最后 15 行) =="
        tail -15 "$f" 2>/dev/null
    fi
done

# =============================================================================
# 判读
# =============================================================================
hdr "诊断判读"

echo "命中标记: LIB_MISSING=$HIT_LIB_MISSING  UID_PROBE_FAIL=$HIT_UID_PROBE_FAIL  XAUTH_MISSING=$HIT_XAUTH_MISSING"
echo

if [ "$HIT_LIB_MISSING" = "1" ]; then
    echo "${RED}>>> 最可能: 假设① 运行库缺失${RST}"
    echo "    deb 没有声明 Depends, UOS 最小化安装缺少 libX11/libXtst/..."
    echo "    修复: 重新打包 deb, control 里加:"
    echo "      Depends: libx11-6, libxtst6, libxext6, libxfixes3, libxkbfile1, libjpeg62-turbo"
    echo "    这一步在开发机完成, 不需要 UOS 的 sudo (装包由管理员执行一次)."
    echo
fi

if [ "$HIT_UID_PROBE_FAIL" = "1" ]; then
    echo "${RED}>>> 最可能: 假设② uid 探测失败 (loginctl 被拦/会话未注册)${RST}"
    echo "    agent 以 root 跑, 但 consoleUid() 拿不到桌面用户 uid,"
    echo "    slave 子进程保持 root 身份去连 X server, 无授权 -> 崩溃."
    echo "    修复需要 UOS root (改 systemd unit 或 polkit), 自己做不了, 找管理员."
    echo
fi

if [ "$HIT_XAUTH_MISSING" = "1" ]; then
    echo "${RED}>>> 可能: 假设③ X 授权缺失${RST}"
    echo "    DISPLAY/XAUTHORITY 为空或 xauth 无条目."
    echo "    修复: 确认 agent 启动时能拿到桌面用户的 XAUTHORITY 路径."
    echo
fi

if [ "$HIT_LIB_MISSING" = "0" ] && [ "$HIT_UID_PROBE_FAIL" = "0" ] && [ "$HIT_XAUTH_MISSING" = "0" ]; then
    echo "${GRN}>>> 三大假设都未命中.${RST}"
    echo "    可能是更深的问题. 建议:"
    echo "    1. 把 $LOG 发回来"
    echo "    2. 若能拿到一次 root: strace -f -o /tmp/mesh.strace <启动agent>, 触发连接后看末尾信号"
    echo "    3. 临时打开 SLAVELOG 重编 (agentcore.c 里 extern int SLAVELOG; 改成 1)"
fi

echo
echo "${DIM}完整日志已保存: $LOG${RST}"
echo "${DIM}把这个文件发回来, 我帮你判读.${RST}"
