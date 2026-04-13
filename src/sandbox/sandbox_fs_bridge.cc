// 🎯 Strict alignment: OpenClaw agents/sandbox/fs-bridge.ts (308) + fs-bridge.types.ts (39)
//   + fs-paths.ts (259) + fs-bridge-path-safety.ts (280) + fs-bridge-mutation-helper.ts (403)
//   + fs-bridge-shell-command-plans.ts (24) + fs-bridge-rename-targets.ts (40)
//   + path-utils.ts (16) + bind-spec.ts (35) + host-paths.ts (44)
// Sandbox filesystem bridge: path resolution, mount mapping, pinned mutations, safety guards.

#include "hunterclaw/sandbox/sandbox.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <regex>
#include <set>
#include <sstream>

namespace hunterclaw {
namespace sandbox {
namespace fs_bridge {

namespace fs = std::filesystem;

// ─── Types (对齐 fs-bridge.types.ts) ─────────────────────────────────────

struct ResolvedPath {
    std::string host_path;
    std::string relative_path;
    std::string container_path;
};

struct FsStat {
    std::string type;  // "file"|"directory"|"other"
    size_t size = 0;
    int64_t mtime_ms = 0;
};

// ─── path-utils.ts ───────────────────────────────────────────────────────

// 🎯 normalizeContainerPath (对齐 path-utils.ts:3-6)
static std::string normalize_container_path(const std::string& v) {
    auto p = fs::path(v).lexically_normal().generic_string();
    return p == "." ? "/" : p;
}

// 🎯 isPathInsideContainerRoot (对齐 path-utils.ts:8-15)
static bool is_inside_container(const std::string& root, const std::string& target) {
    auto nr = normalize_container_path(root);
    auto nt = normalize_container_path(target);
    if (nr == "/") return true;
    return nt == nr || nt.substr(0, nr.size() + 1) == nr + "/";
}

// ─── bind-spec.ts ────────────────────────────────────────────────────────

struct BindParts { std::string host, container, options; };

// 🎯 splitSandboxBindSpec (对齐 bind-spec.ts:7-24)
static std::optional<BindParts> split_bind_spec(const std::string& spec) {
    // Handle Windows drive letter prefix
    bool has_drive = spec.size() >= 3 && std::isalpha(spec[0]) &&
                     spec[1] == ':' && (spec[2] == '\\' || spec[2] == '/');
    size_t start = has_drive ? 2 : 0;
    auto sep = spec.find(':', start);
    if (sep == std::string::npos) return std::nullopt;
    auto host = spec.substr(0, sep);
    auto rest = spec.substr(sep + 1);
    auto opt_sep = rest.find(':');
    if (opt_sep == std::string::npos) return BindParts{host, rest, ""};
    return BindParts{host, rest.substr(0, opt_sep), rest.substr(opt_sep + 1)};
}

// ─── host-paths.ts ───────────────────────────────────────────────────────

// 🎯 normalizeSandboxHostPath (对齐 host-paths.ts:25-32)
static std::string normalize_host_path(const std::string& raw) {
    auto trimmed = raw;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(trimmed.begin());
    while (!trimmed.empty() && trimmed.back() == ' ') trimmed.pop_back();
    if (trimmed.empty()) return "/";
    // Replace backslashes
    for (auto& c : trimmed) if (c == '\\') c = '/';
    auto p = fs::path(trimmed).lexically_normal().generic_string();
    // Strip trailing /
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    return p.empty() ? "/" : p;
}

// ─── fs-paths.ts: Mount Types ────────────────────────────────────────────

struct FsMount {
    std::string host_root;
    std::string container_root;
    bool writable = false;
    std::string source;  // "workspace"|"agent"|"bind"
};

struct ResolvedFsPath {
    std::string host_path;
    std::string relative_path;
    std::string container_path;
    bool writable = false;
};

// 🎯 parseSandboxBindMount (对齐 fs-paths.ts:30-59)
static std::optional<FsMount> parse_bind_mount(const std::string& spec) {
    auto parts = split_bind_spec(spec);
    if (!parts) return std::nullopt;
    auto host = parts->host; auto container = parts->container;
    while (!host.empty() && host.back() == ' ') host.pop_back();
    while (!container.empty() && container.back() == ' ') container.pop_back();
    if (host.empty() || container.empty() || container[0] != '/') return std::nullopt;
    // Check options for "ro"
    std::string opts = parts->options;
    for (auto& c : opts) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    bool writable = opts.find("ro") == std::string::npos;
    return FsMount{fs::path(host).lexically_normal().string(),
                   normalize_container_path(container), writable, "bind"};
}

// 🎯 buildSandboxFsMounts (对齐 fs-paths.ts:61-97)
static std::vector<FsMount> build_mounts(
    const std::string& workspace_dir, const std::string& agent_workspace_dir,
    const std::string& container_workdir, const std::string& workspace_access,
    const std::vector<std::string>& binds)
{
    std::vector<FsMount> mounts;
    mounts.push_back({fs::path(workspace_dir).lexically_normal().string(),
                      normalize_container_path(container_workdir),
                      workspace_access == "rw", "workspace"});
    auto wd = fs::path(workspace_dir).lexically_normal().string();
    auto awd = fs::path(agent_workspace_dir).lexically_normal().string();
    if (workspace_access != "none" && wd != awd) {
        mounts.push_back({awd, "/agent", workspace_access == "rw", "agent"});
    }
    for (const auto& b : binds) {
        auto m = parse_bind_mount(b);
        if (m) mounts.push_back(*m);
    }
    // Dedup
    std::set<std::string> seen;
    std::vector<FsMount> deduped;
    for (auto& m : mounts) {
        auto key = m.host_root + "=>" + m.container_root;
        if (seen.insert(key).second) deduped.push_back(std::move(m));
    }
    return deduped;
}

// 🎯 resolveSandboxFsPathWithMounts (对齐 fs-paths.ts:99-160)
static ResolvedFsPath resolve_fs_path(
    const std::string& file_path, const std::string& cwd,
    const std::string& default_workspace, const std::string& default_container,
    const std::vector<FsMount>& mounts)
{
    // Sort by container root length desc
    auto sorted = mounts;
    std::sort(sorted.begin(), sorted.end(), [](const FsMount& a, const FsMount& b) {
        return a.container_root.size() > b.container_root.size();
    });
    
    std::string input = file_path;
    for (auto& c : input) if (c == '\\') c = '/';
    while (!input.empty() && input.front() == ' ') input.erase(input.begin());
    
    // Absolute container path check
    if (!input.empty() && input[0] == '/') {
        auto nc = normalize_container_path(input);
        for (const auto& m : sorted) {
            if (is_inside_container(m.container_root, nc)) {
                auto rel = fs::path(nc).lexically_relative(m.container_root).generic_string();
                if (rel == ".") rel = "";
                auto hp = rel.empty() ? m.host_root : (fs::path(m.host_root) / rel).string();
                auto cp = rel.empty() ? m.container_root : m.container_root + "/" + rel;
                return {hp, rel, cp, m.writable};
            }
        }
    }
    
    // Host path resolution
    auto resolved = fs::path(cwd) / input;
    auto host_resolved = resolved.lexically_normal().string();
    // Sort mounts by host root length desc
    auto host_sorted = mounts;
    std::sort(host_sorted.begin(), host_sorted.end(), [](const FsMount& a, const FsMount& b) {
        return a.host_root.size() > b.host_root.size();
    });
    for (const auto& m : host_sorted) {
        auto rel = fs::path(host_resolved).lexically_relative(m.host_root).generic_string();
        if (!rel.empty() && rel[0] != '.' && rel[0] != '/') {
            auto cp = rel == "." ? m.container_root : m.container_root + "/" + rel;
            return {host_resolved, rel == "." ? "" : rel, cp, m.writable};
        }
    }
    
    throw std::runtime_error("Path escapes sandbox root (" + default_workspace + "): " + file_path);
}

// ─── fs-bridge-mutation-helper.ts: Pinned Mutation Python ────────────────

// 🎯 SANDBOX_PINNED_MUTATION_PYTHON (对齐 fs-bridge-mutation-helper.ts:16-289)
static const char* PINNED_MUTATION_PYTHON = R"PY(
import errno, os, secrets, stat, sys
operation = sys.argv[1]
DIR_FLAGS = os.O_RDONLY
if hasattr(os,'O_DIRECTORY'): DIR_FLAGS |= os.O_DIRECTORY
if hasattr(os,'O_NOFOLLOW'): DIR_FLAGS |= os.O_NOFOLLOW
READ_FLAGS = os.O_RDONLY
if hasattr(os,'O_NOFOLLOW'): READ_FLAGS |= os.O_NOFOLLOW
WRITE_FLAGS = os.O_WRONLY | os.O_CREAT | os.O_EXCL
if hasattr(os,'O_NOFOLLOW'): WRITE_FLAGS |= os.O_NOFOLLOW
def split_relative(p):
    segs=[]
    for s in p.split('/'):
        if not s or s=='.': continue
        if s=='..': raise OSError(errno.EPERM,'path traversal',s)
        segs.append(s)
    return segs
def open_dir(p,dir_fd=None): return os.open(p,DIR_FLAGS,dir_fd=dir_fd)
def walk_dir(root_fd,rel,mkdir_en):
    cur=os.dup(root_fd)
    try:
        for seg in split_relative(rel):
            try: nxt=open_dir(seg,dir_fd=cur)
            except FileNotFoundError:
                if not mkdir_en: raise
                os.mkdir(seg,0o777,dir_fd=cur); nxt=open_dir(seg,dir_fd=cur)
            os.close(cur); cur=nxt
        return cur
    except: os.close(cur); raise
def create_temp_file(pfd,bn):
    pfx='.hunterclaw-write-'+bn+'.'
    for _ in range(128):
        c=pfx+secrets.token_hex(6)
        try: fd=os.open(c,WRITE_FLAGS,0o600,dir_fd=pfd); return c,fd
        except FileExistsError: continue
    raise RuntimeError('temp file alloc failed')
def write_atomic(pfd,bn,stdin_buf):
    tf=tn=None
    try:
        tn,tf=create_temp_file(pfd,bn)
        while True:
            chunk=stdin_buf.read(65536)
            if not chunk: break
            os.write(tf,chunk)
        os.fsync(tf); os.close(tf); tf=None
        os.replace(tn,bn,src_dir_fd=pfd,dst_dir_fd=pfd); tn=None
        os.fsync(pfd)
    finally:
        if tf is not None: os.close(tf)
        if tn is not None:
            try: os.unlink(tn,dir_fd=pfd)
            except FileNotFoundError: pass
def read_file(pfd,bn):
    fd=os.open(bn,READ_FLAGS,dir_fd=pfd)
    try:
        st=os.fstat(fd)
        if not stat.S_ISREG(st.st_mode): raise OSError(errno.EPERM,'not regular file',bn)
        if st.st_nlink>1: raise OSError(errno.EPERM,'hardlinked',bn)
        while True:
            chunk=os.read(fd,65536)
            if not chunk: break
            os.write(1,chunk)
    finally: os.close(fd)
if operation=='write':
    r=open_dir(sys.argv[2]); p=None
    try: p=walk_dir(r,sys.argv[3],sys.argv[5]=='1'); write_atomic(p,sys.argv[4],sys.stdin.buffer)
    finally:
        if p is not None: os.close(p)
        os.close(r)
elif operation=='read':
    r=open_dir(sys.argv[2]); p=None
    try: p=walk_dir(r,sys.argv[3],False); read_file(p,sys.argv[4])
    finally:
        if p is not None: os.close(p)
        os.close(r)
elif operation=='mkdirp':
    r=open_dir(sys.argv[2]); t=None
    try: t=walk_dir(r,sys.argv[3],True); os.fsync(t)
    finally:
        if t is not None: os.close(t)
        os.close(r)
elif operation=='remove':
    r=open_dir(sys.argv[2]); p=None
    try:
        p=walk_dir(r,sys.argv[3],False)
        try:
            if sys.argv[5]=='1':
                st=os.lstat(sys.argv[4],dir_fd=p)
                if stat.S_ISDIR(st.st_mode) and not stat.S_ISLNK(st.st_mode):
                    import shutil; shutil.rmtree(os.path.join('/proc/self/fd/'+str(p),sys.argv[4]))
                else: os.unlink(sys.argv[4],dir_fd=p)
            else: os.unlink(sys.argv[4],dir_fd=p)
            os.fsync(p)
        except FileNotFoundError:
            if sys.argv[6]!='1': raise
    finally:
        if p is not None: os.close(p)
        os.close(r)
)PY";

// 🎯 Build pinned mutation shell script (对齐 fs-bridge-mutation-helper.ts:293-318)
static std::string build_mutation_script(const std::vector<std::string>& args) {
    std::string script = "set -eu\npython3 -c '";
    // Escape Python for shell
    std::string py = PINNED_MUTATION_PYTHON;
    size_t p = 0;
    while ((p = py.find('\'', p)) != std::string::npos) {
        py.replace(p, 1, "'\"'\"'"); p += 5;
    }
    script += py + "' --";
    for (const auto& a : args) {
        script += " '";
        std::string ea = a;
        p = 0;
        while ((p = ea.find('\'', p)) != std::string::npos) {
            ea.replace(p, 1, "'\"'\"'"); p += 5;
        }
        script += ea + "'";
    }
    return script;
}

// ─── SandboxFsBridge Implementation (对齐 fs-bridge.ts:33-289) ──────────

// 🎯 createSandboxFsBridge (对齐 fs-bridge.ts:33-37)
ResolvedPath resolve_path(
    const std::string& file_path, const std::string& cwd,
    const std::string& workspace_dir, const std::string& container_workdir,
    const std::string& agent_workspace_dir, const std::string& workspace_access,
    const std::vector<std::string>& binds)
{
    auto mounts = build_mounts(workspace_dir, agent_workspace_dir, container_workdir,
                                workspace_access, binds);
    auto resolved = resolve_fs_path(file_path, cwd, workspace_dir, container_workdir, mounts);
    return {resolved.host_path, resolved.relative_path, resolved.container_path};
}

// 🎯 coerceStatType (对齐 fs-bridge.ts:295-307)
static std::string coerce_stat_type(const std::string& raw) {
    std::string lower;
    for (char c : raw) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("directory") != std::string::npos) return "directory";
    if (lower.find("file") != std::string::npos) return "file";
    return "other";
}

// 🎯 buildStatPlan (对齐 fs-bridge-shell-command-plans.ts:13-23)
static std::string build_stat_script(const std::string& parent, const std::string& basename) {
    return "set -eu\ncd -- '" + parent + "'\nstat -c '%F|%s|%Y' -- '" + basename + "'";
}

}  // namespace fs_bridge
}  // namespace sandbox
}  // namespace hunterclaw
