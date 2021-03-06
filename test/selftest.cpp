#include <iostream>
#include <iomanip>
#include <csignal>
#include <cstdio>
#include <climits>
#include <algorithm>

#include "version.hpp"
#include "libporto.hpp"
#include "config.hpp"
#include "util/netlink.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/cred.hpp"
#include "util/idmap.hpp"
#include "test.hpp"
#include "rpc.hpp"

#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/coded_stream.h>

const TString TMPDIR = "/tmp/porto/selftest";

extern "C" {
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <grp.h>
#include <linux/capability.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
}

const TString oomMemoryLimit = "32M";
const TString oomCommand = "sort -S 1G /dev/urandom";
TString portoctl;
TString portoinit;

using TString;
using std::vector;
using std::map;
using std::pair;

namespace test {

static int expectedRespawns;
static int expectedErrors;
static int expectedWarns;

static vector<TString> subsystems = { "freezer", "memory", "cpu", "cpuacct", "devices", "net_cls" };
static vector<TString> namespaces = { "pid", "mnt", "ipc", "net", /*"user", */"uts" };

static int LeakConainersNr = 1000;

static TString StartWaitAndGetProperty(Porto::Connection &api, const TString &name, const TString &data) {
    TString v;
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, data, v));
    return v;
}

static void RemakeDir(Porto::Connection &api, const TPath &path) {
    if (path.Exists()) {
        bool drop = geteuid() != 0;
        if (drop)
            AsRoot(api);
        ExpectOk(path.RemoveAll());
        if (drop)
            AsAlice(api);
    }
    ExpectOk(path.MkdirAll(0755));
}

static void ExpectCorrectCgroups(const TString &pid, const TString &name, const TString &name2) {
    auto cgmap = GetCgroups(pid);

    for (auto &subsys : subsystems) {
        if (subsys == "freezer")
            ExpectEq(cgmap[subsys], "/porto/" + name);
        else if (subsys == "cpuacct" && cgmap["cpuacct"] != cgmap["cpu"])
            ExpectEq(cgmap[subsys], "/porto%" + name);
        else
            ExpectEq(cgmap[subsys], "/porto%" + name2);
    }
}

static void ShouldHaveOnlyRoot(Porto::Connection &api) {
    std::vector<TString> containers;

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 0);
}

static void TestDataMap(Porto::Connection &api, const TString &name, const TString &data, int zero) {
    TString full;
    int nr_nonzero = 0;

    Say() << "Test " << name << " data map " << data << " zero:" << zero << std::endl;

    ExpectApiSuccess(api.GetProperty(name, data, full));
    auto lines = SplitString(full, ';');

    if (!zero) {
        ExpectNeq(full, "");
        ExpectNeq(lines.size(), 0);
    }

    for (auto line: lines) {
        TString tmp;
        auto tokens = SplitString(line, ':');
        ExpectApiSuccess(api.GetProperty(name, data + "[" + StringTrim(tokens[0]) + "]", tmp));
        ExpectEq(tmp, StringTrim(tokens[1]));

        if (tmp != "0")
            nr_nonzero++;
    }

    if (zero == 1)
        ExpectEq(nr_nonzero, 0);
    if (zero == 0)
        ExpectNeq(nr_nonzero, 0);

    ExpectApiFailure(api.GetProperty(name, data + "[invalid]", full), EError::InvalidValue);
}

static void ShouldHaveValidProperties(Porto::Connection &api, const TString &name) {
    TString v;

    ExpectApiFailure(api.GetProperty(name, "command[1]", v), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "command[1]", "ls"), EError::InvalidValue);

    ExpectApiSuccess(api.GetProperty(name, "command", v));
    ExpectEq(v, TString(""));
    ExpectApiSuccess(api.GetProperty(name, "cwd", v));
    ExpectEq(v, TString(PORTO_WORKDIR) + "/" + name);
    ExpectApiSuccess(api.GetProperty(name, "root", v));
    ExpectEq(v, "/");
    ExpectApiSuccess(api.GetProperty(name, "user", v));
    ExpectEq(v, Alice.User());
    ExpectApiSuccess(api.GetProperty(name, "group", v));
    ExpectEq(v, Alice.Group());
    ExpectApiSuccess(api.GetProperty(name, "env", v));
    ExpectEq(v, TString(""));

    ExpectApiSuccess(api.GetProperty(name, "memory_limit", v));
    ExpectNeq(v, TString("0"));

    if (KernelSupports(KernelFeature::LOW_LIMIT)) {
        ExpectApiSuccess(api.GetProperty(name, "memory_guarantee", v));
        ExpectEq(v, TString("0"));
    }

    ExpectApiSuccess(api.GetProperty(name, "cpu_policy", v));
    ExpectEq(v, TString("normal"));
    ExpectApiSuccess(api.GetProperty(name, "cpu_limit", v));
    ExpectEq(v, "0c");
    ExpectApiSuccess(api.GetProperty(name, "cpu_guarantee", v));
    ExpectEq(v, "0c");
    ExpectApiSuccess(api.GetProperty(name, "io_policy", v));
    ExpectEq(v, "");
    if (KernelSupports(KernelFeature::FSIO)) {
        ExpectApiSuccess(api.GetProperty(name, "io_limit", v));
        ExpectEq(v, "");
        ExpectApiSuccess(api.GetProperty(name, "io_ops_limit", v));
        ExpectEq(v, "");
    }

    ExpectApiSuccess(api.GetProperty(name, "net", v));
    ExpectEq(v, "inherited");

    ExpectApiSuccess(api.GetProperty(name, "respawn", v));
    ExpectEq(v, TString("false"));
    ExpectApiSuccess(api.GetProperty(name, "stdin_path", v));
    ExpectEq(v, TString("/dev/null"));
    ExpectApiSuccess(api.GetProperty(name, "stdout_path", v));
    ExpectEq(v, "stdout");
    ExpectApiSuccess(api.GetProperty(name, "stderr_path", v));
    ExpectEq(v, "stderr");
    ExpectApiSuccess(api.GetProperty(name, "ulimit", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "hostname", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "bind_dns", v));
    ExpectEq(v, "false");
    ExpectApiSuccess(api.GetProperty(name, "devices", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "capabilities", v));
    ExpectEq(v, "CHOWN;DAC_OVERRIDE;FOWNER;FSETID;KILL;SETGID;SETUID;SETPCAP;LINUX_IMMUTABLE;NET_BIND_SERVICE;NET_ADMIN;NET_RAW;IPC_LOCK;SYS_CHROOT;SYS_PTRACE;SYS_ADMIN;SYS_BOOT;SYS_NICE;SYS_RESOURCE;MKNOD;AUDIT_WRITE;SETFCAP");
    if (KernelSupports(KernelFeature::RECHARGE_ON_PGFAULT)) {
        ExpectApiSuccess(api.GetProperty(name, "recharge_on_pgfault", v));
        ExpectEq(v, "false");
    }
    ExpectApiSuccess(api.GetProperty(name, "isolate", v));
    ExpectEq(v, "true");
    ExpectApiSuccess(api.GetProperty(name, "stdout_limit", v));
    ExpectEq(v, std::to_string(config().container().stdout_limit()));
    ExpectApiSuccess(api.GetProperty(name, "private", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "bind", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(name, "root_readonly", v));
    ExpectEq(v, "false");
    ExpectApiSuccess(api.GetProperty(name, "max_respawns", v));
    ExpectEq(v, "0");
    ExpectApiSuccess(api.GetProperty(name, "enable_porto", v));
    ExpectEq(v, "true");
}

static void ShouldHaveValidRunningData(Porto::Connection &api, const TString &name) {
    TString v;

    ExpectApiFailure(api.GetProperty(name, "__invalid_data__", v), EError::InvalidProperty);

    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, TString("running"));
    ExpectApiFailure(api.GetProperty(name, "exit_status", v), EError::InvalidState);

    ExpectApiSuccess(api.GetProperty(name, "root_pid", v));
    Expect(v != "" && v != "-1" && v != "0");

    ExpectApiSuccess(api.GetProperty(name, "stdout", v));
    ExpectApiSuccess(api.GetProperty(name, "stderr", v));
    ExpectApiSuccess(api.GetProperty(name, "cpu_usage", v));
    ExpectApiSuccess(api.GetProperty(name, "memory_usage", v));

    ExpectApiSuccess(api.GetProperty(name, "net_bytes", v));
    ExpectApiSuccess(api.GetProperty(name, "net_packets", v));
    ExpectApiSuccess(api.GetProperty(name, "net_drops", v));
    ExpectApiSuccess(api.GetProperty(name, "net_overlimits", v));

    ExpectApiSuccess(api.GetProperty(name, "net_rx_bytes", v));
    ExpectApiSuccess(api.GetProperty(name, "net_rx_packets", v));
    ExpectApiSuccess(api.GetProperty(name, "net_rx_drops", v));

    int intval;
    ExpectApiSuccess(api.GetProperty(name, "minor_faults", v));
    ExpectOk(StringToInt(v, intval));
    Expect(intval > 0);
    ExpectApiSuccess(api.GetProperty(name, "major_faults", v));
    ExpectOk(StringToInt(v, intval));
    Expect(intval >= 0);
    if (KernelSupports(KernelFeature::MAX_RSS)) {
        ExpectApiSuccess(api.GetProperty(name, "max_rss", v));
        ExpectOk(StringToInt(v, intval));
        Expect(intval >= 0);
    }

    ExpectApiFailure(api.GetProperty(name, "oom_killed", v), EError::InvalidState);
    ExpectApiSuccess(api.GetProperty(name, "respawn_count", v));
    ExpectEq(v, TString("0"));
    ExpectApiSuccess(api.GetProperty(name, "parent", v));
    ExpectEq(v, TString("/"));
    if (KernelSupports(KernelFeature::FSIO) ||
            KernelSupports(KernelFeature::CFQ)) {
        ExpectApiSuccess(api.GetProperty(name, "io_read", v));
        ExpectApiSuccess(api.GetProperty(name, "io_write", v));
        ExpectApiSuccess(api.GetProperty(name, "io_ops", v));
    }
}

static void ShouldHaveValidData(Porto::Connection &api, const TString &name) {
    TString v;

    ExpectApiFailure(api.GetProperty(name, "__invalid_data__", v), EError::InvalidProperty);

    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, TString("stopped"));
    ExpectApiFailure(api.GetProperty(name, "exit_status", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "root_pid", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "stdout", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "stderr", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "cpu_usage", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "memory_usage", v), EError::InvalidState);

    ExpectApiFailure(api.GetProperty(name, "net_bytes", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "net_packets", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "net_drops", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "net_overlimits", v), EError::InvalidState);

    ExpectApiFailure(api.GetProperty(name, "net_rx_bytes", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "net_rx_packets", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "net_rx_drops", v), EError::InvalidState);

    ExpectApiFailure(api.GetProperty(name, "minor_faults", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "major_faults", v), EError::InvalidState);
    if (KernelSupports(KernelFeature::MAX_RSS)) {
        ExpectApiFailure(api.GetProperty(name, "max_rss", v), EError::InvalidState);
    }

    ExpectApiFailure(api.GetProperty(name, "oom_killed", v), EError::InvalidState);
    ExpectApiSuccess(api.GetProperty(name, "respawn_count", v));
    ExpectApiSuccess(api.GetProperty(name, "parent", v));
    ExpectEq(v, TString("/"));
    if (KernelSupports(KernelFeature::FSIO) ||
            KernelSupports(KernelFeature::CFQ)) {
        ExpectApiFailure(api.GetProperty(name, "io_read", v), EError::InvalidState);
        ExpectApiFailure(api.GetProperty(name, "io_write", v), EError::InvalidState);
        ExpectApiFailure(api.GetProperty(name, "io_ops", v), EError::InvalidState);
    }
    ExpectApiSuccess(api.GetProperty(name, "max_respawns", v));
    ExpectEq(v, "0");
}

static void TestHolder(Porto::Connection &api) {
    ShouldHaveOnlyRoot(api);

    std::vector<TString> containers;

    Say() << "Create container A" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 1);
    ExpectEq(containers[0], TString("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Try to create existing container A" << std::endl;
    ExpectApiFailure(api.Create("a"), EError::ContainerAlreadyExists);
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 1);
    ExpectEq(containers[0], TString("a"));
    ShouldHaveValidProperties(api, "a");
    ShouldHaveValidData(api, "a");

    Say() << "Create container B" << std::endl;
    ExpectApiSuccess(api.Create("b"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], TString("a"));
    ExpectEq(containers[1], TString("b"));
    ShouldHaveValidProperties(api, "b");
    ShouldHaveValidData(api, "b");

    Say() << "Remove container A" << std::endl;
    ExpectApiSuccess(api.Destroy("a"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 1);
    ExpectEq(containers[0], TString("b"));

    Say() << "Remove container B" << std::endl;
    ExpectApiSuccess(api.Destroy("b"));

    Say() << "Try to execute operations on invalid container" << std::endl;
    ExpectApiFailure(api.Start("a"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Stop("a"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Pause("a"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Resume("a"), EError::ContainerDoesNotExist);

    TString name, value;
    ExpectApiFailure(api.GetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.SetProperty("a", "command", value), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.GetProperty("a", "root_pid", value), EError::ContainerDoesNotExist);

    name = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-@:.";
    Say() << "Try to create and destroy container " << name << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Try to create container with invalid name" << std::endl;

    name = "z$";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "\xD0\xAFndex";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "/invalid";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = TString(128, 'a');
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    name = TString(128, 'z');
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    name = TString(129, 'z');
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = TString(129, 'z') + "/z";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "z/" + TString(129, 'z');
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = "z/" + TString(129, 'z') + "/z";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    Say() << "Test hierarchy" << std::endl;

    TString parent = "a";
    TString child = "a/b";
    ExpectApiFailure(api.Create(child), EError::ContainerDoesNotExist);
    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.Destroy(parent));
    TString v;
    ExpectApiFailure(api.GetProperty(child, "state", v), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.GetProperty(parent, "state", v), EError::ContainerDoesNotExist);

    ExpectApiSuccess(api.Create("a"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 1);
    ExpectEq(containers[0], TString("a"));

    ExpectApiSuccess(api.Create("a/b"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], TString("a"));
    ExpectEq(containers[1], TString("a/b"));

    Say() << "Try to create long container path" << std::endl;

    name = TString(128, 'a');
    ExpectApiSuccess(api.Create(name));

    name += "/" + TString(200 - 128 - 1, 'a');
    ExpectEq(name.length(), 200);
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Destroy(name));

    name += "a";
    ExpectApiFailure(api.Create(name), EError::InvalidValue);

    name = TString(128, 'a');
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check meta soft limits" << std::endl;

    ExpectApiSuccess(api.Create("a/b/c"));
    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 3);
    ExpectEq(containers[0], TString("a"));
    ExpectEq(containers[1], TString("a/b"));
    ExpectEq(containers[2], TString("a/b/c"));

    ExpectApiSuccess(api.SetProperty("a/b/c", "command", "sleep 1000"));

    TString customLimit = std::to_string(1 * 1024 * 1024);

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectApiSuccess(api.GetProperty("a/b/c", "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetProperty("a/b", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.GetProperty("a", "state", v));
    ExpectEq(v, "meta");
    ExpectNeq(GetCgKnob("memory", "a/b/c", "memory.soft_limit_in_bytes"), customLimit);
    ExpectNeq(GetCgKnob("memory", "a", "memory.soft_limit_in_bytes"), customLimit);
    ExpectApiSuccess(api.Stop("a/b/c"));
    if (config().container().pressurize_on_death())
        ExpectEq(GetCgKnob("memory", "a", "memory.soft_limit_in_bytes"), customLimit);
    else
        ExpectNeq(GetCgKnob("memory", "a", "memory.soft_limit_in_bytes"), customLimit);

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectNeq(GetCgKnob("memory", "a/b/c", "memory.soft_limit_in_bytes"), customLimit);
    ExpectNeq(GetCgKnob("memory", "a/b", "memory.soft_limit_in_bytes"), customLimit);
    ExpectNeq(GetCgKnob("memory", "a", "memory.soft_limit_in_bytes"), customLimit);
    ExpectApiSuccess(api.Stop("a"));

    Say() << "Make sure parent gets valid state when child starts" << std::endl;

    ExpectApiSuccess(api.SetProperty("a", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectApiSuccess(api.GetProperty("a/b/c", "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetProperty("a/b", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.GetProperty("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a/b/c"));
    ExpectApiSuccess(api.GetProperty("a/b", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.GetProperty("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a"));

    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "true"));

    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.GetProperty("a/b/c", "state", v));
    ExpectEq(v, "stopped");
    ExpectApiSuccess(api.GetProperty("a/b", "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetProperty("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a/b"));
    ExpectApiSuccess(api.GetProperty("a", "state", v));
    ExpectEq(v, "meta");
    ExpectApiSuccess(api.Stop("a"));

    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a", "isolate", "true"));

    ExpectApiSuccess(api.Start("a"));
    ExpectApiSuccess(api.GetProperty("a/b/c", "state", v));
    ExpectEq(v, "stopped");
    ExpectApiSuccess(api.GetProperty("a/b", "state", v));
    ExpectEq(v, "stopped");
    ExpectApiSuccess(api.GetProperty("a", "state", v));
    ExpectEq(v, "running");
    ShouldHaveValidRunningData(api, "a");
    ExpectApiSuccess(api.Stop("a"));

    Say() << "Make sure we can have multiple meta parents" << std::endl;

    ExpectApiSuccess(api.Create("x"));
    ExpectApiSuccess(api.Create("x/y"));
    ExpectApiSuccess(api.Create("x/y/z"));
    ExpectApiSuccess(api.SetProperty("x/y/z", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("x/y/z"));
    ExpectApiSuccess(api.Destroy("x"));

    Say() << "Make sure when parent stops/dies children are stopped" << std::endl;

    TString state;

    ExpectApiSuccess(api.Start("a"));
    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.Start("a/b/c"));

    ExpectApiSuccess(api.GetProperty("a/b/c", "state", state));
    ExpectEq(state, "running");
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), false);
    ExpectEq(CgExists("memory", "a/b/c"), false);

    ExpectApiSuccess(api.Stop("a/b"));
    ExpectApiSuccess(api.GetProperty("a/b/c", "state", state));
    ExpectEq(state, "stopped");
    ExpectApiSuccess(api.GetProperty("a/b", "state", state));
    ExpectEq(state, "stopped");
    ExpectApiSuccess(api.GetProperty("a", "state", state));
    ExpectEq(state, "running");
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), false);
    ExpectEq(CgExists("memory", "a/b/c"), false);

    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1"));
    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), false);
    ExpectEq(CgExists("memory", "a/b/c"), false);

    WaitContainer(api, "a/b");
    ExpectApiSuccess(api.GetProperty("a/b", "state", state));
    ExpectEq(state, "dead");
    ExpectApiSuccess(api.GetProperty("a/b/c", "state", state));
    ExpectEq(state, "dead");
    ExpectEq(CgExists("memory", "a"), true);
    ExpectEq(CgExists("memory", "a/b"), false);
    ExpectEq(CgExists("memory", "a/b/c"), false);

    ExpectApiSuccess(api.Destroy("a/b/c"));
    ExpectApiSuccess(api.Destroy("a/b"));
    ExpectApiSuccess(api.Destroy("a"));

    Say() << "Make sure porto returns valid error code for destroy" << std::endl;
    ExpectApiFailure(api.Destroy("/"), EError::Permission);
    ExpectApiFailure(api.Destroy("doesntexist"), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.Destroy("z$"), EError::ContainerDoesNotExist);

    Say() << "Make sure we can't start child when parent is dead" << std::endl;

    ExpectApiSuccess(api.Create("parent"));
    ExpectApiSuccess(api.Create("parent/child"));
    ExpectApiSuccess(api.SetProperty("parent", "command", "sleep 1"));
    ExpectApiSuccess(api.SetProperty("parent/child", "command", "sleep 2"));
    ExpectApiSuccess(api.Start("parent"));
    ExpectApiSuccess(api.Start("parent/child"));
    ExpectApiSuccess(api.Stop("parent/child"));
    WaitContainer(api, "parent");
    ExpectApiFailure(api.Start("parent"), EError::InvalidState);
    ExpectApiSuccess(api.Destroy("parent"));

    Say() << "Make sure that dead child does not kill parent and siblings" << std::endl;

    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("a/b"));
    ExpectApiSuccess(api.GetProperty("a", "state", state));
    ExpectEq(state, "meta");
    ExpectApiSuccess(api.GetProperty("a/b", "state", state));
    ExpectEq(state, "running");
    ExpectApiSuccess(api.Create("a/c"));
    ExpectApiSuccess(api.SetProperty("a/c", "command", "__invalid_command__"));
    ExpectApiFailure(api.Start("a/c"), EError::InvalidCommand);
    ExpectApiSuccess(api.GetProperty("a", "state", state));
    ExpectEq(state, "meta");
    ExpectApiSuccess(api.GetProperty("a/b", "state", state));
    ExpectEq(state, "running");
    ExpectApiSuccess(api.GetProperty("a/c", "state", state));
    ExpectEq(state, "stopped");
    ExpectApiSuccess(api.Destroy("a"));

    ShouldHaveOnlyRoot(api);
}

static void TestMeta(Porto::Connection &api) {
    TString state;
    ShouldHaveOnlyRoot(api);

    std::vector<TString> isolateVals = { "true", "false" };

    for (auto isolate : isolateVals) {
        Say() << "Test meta state machine with isolate = " << isolate << std::endl;

        ExpectApiSuccess(api.Create("a"));
        ExpectApiSuccess(api.Create("a/b"));

        ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 2"));

        ExpectApiSuccess(api.SetProperty("a", "isolate", isolate));
        ExpectApiSuccess(api.SetProperty("a/b", "isolate", "true"));

        ExpectApiSuccess(api.GetProperty("a", "state", state));
        ExpectEq(state, "stopped");
        ExpectApiSuccess(api.GetProperty("a/b", "state", state));
        ExpectEq(state, "stopped");

        ExpectApiSuccess(api.Start("a/b"));
        ExpectApiSuccess(api.GetProperty("a", "state", state));
        ExpectEq(state, "meta");
        ExpectApiSuccess(api.GetProperty("a/b", "state", state));
        ExpectEq(state, "running");

        WaitContainer(api, "a/b");
        ExpectApiSuccess(api.GetProperty("a", "state", state));
        ExpectEq(state, "meta");
        ExpectApiSuccess(api.GetProperty("a/b", "state", state));
        ExpectEq(state, "dead");

        ExpectApiSuccess(api.Stop("a/b"));
        ExpectApiSuccess(api.GetProperty("a", "state", state));
        ExpectEq(state, "meta");
        ExpectApiSuccess(api.GetProperty("a/b", "state", state));
        ExpectEq(state, "stopped");

        ExpectApiSuccess(api.Stop("a"));
        ExpectApiSuccess(api.GetProperty("a", "state", state));
        ExpectEq(state, "stopped");

        ExpectApiSuccess(api.Destroy("a"));
    }
}

static void TestEmpty(Porto::Connection &api) {
    Say() << "Make sure we can start empty container" << std::endl;
    ExpectApiSuccess(api.Create("b"));
    ExpectApiSuccess(api.Start("b"));
    ExpectApiSuccess(api.Destroy("b"));
}

static bool TaskRunning(const TString &pid) {
    int p = stoi(pid);
    if (kill(p, 0))
        return false;
    auto state = GetState(pid);
    return state != "Z" && state != "X";
}

static bool TaskZombie(const TString &pid) {
    return GetState(pid) == "Z";
}

static void TestExitStatus(Porto::Connection &api) {
    TString pid;
    TString ret;

    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check exit status of 'false'" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "false"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", ret));
    ExpectEq(ret, TString("256"));
    ExpectApiSuccess(api.GetProperty(name, "oom_killed", ret));
    ExpectEq(ret, TString("false"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check exit status of 'true'" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "true"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", ret));
    ExpectEq(ret, TString("0"));
    ExpectApiSuccess(api.GetProperty(name, "oom_killed", ret));
    ExpectEq(ret, TString("false"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check exit status of invalid command" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "__invalid_command_name__"));
    ExpectApiSuccess(api.SetProperty(name, "cwd", "/"));
    ExpectApiFailure(api.Start(name), EError::InvalidCommand);
    ExpectApiFailure(api.GetProperty(name, "root_pid", ret), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "exit_status", ret), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "oom_killed", ret), EError::InvalidState);

    Say() << "Check exit status of invalid directory" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "cwd", "/__invalid__dir__"));
    ExpectApiFailure(api.Start(name), EError::InvalidPath);
    ExpectApiFailure(api.GetProperty(name, "root_pid", ret), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "exit_status", ret), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(name, "oom_killed", ret), EError::InvalidState);

    Say() << "Check exit status when killed by signal" << std::endl;
    ExpectApiSuccess(api.Destroy(name));
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    kill(stoi(pid), SIGKILL);
    WaitContainer(api, name);
    WaitProcessExit(pid);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", ret));
    ExpectEq(ret, TString("9"));
    ExpectApiSuccess(api.GetProperty(name, "oom_killed", ret));
    ExpectEq(ret, TString("false"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check oom_killed property" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", oomCommand));
    // limit is so small that we can't even set it
    ExpectApiFailure(api.SetProperty(name, "memory_limit", "10"), EError::InvalidValue);

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", oomMemoryLimit));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name, 60);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", ret));
    ExpectEq(ret, TString("9"));
    ExpectApiSuccess(api.GetProperty(name, "oom_killed", ret));
    ExpectEq(ret, TString("true"));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestStreams(Porto::Connection &api) {
    TString ret;

    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Make sure stdout works" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo out >&1'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    ExpectEq(ret, TString("out\n"));
    ExpectApiSuccess(api.GetProperty(name, "stderr", ret));
    ExpectEq(ret, TString(""));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure stderr works" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo err >&2'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    ExpectEq(ret, TString(""));
    ExpectApiSuccess(api.GetProperty(name, "stderr", ret));
    ExpectEq(ret, TString("err\n"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestNsCgTc(Porto::Connection &api) {
    TString pid;

    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Spawn long running task" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);

    AsRoot(api);
    Say() << "Check that portod doesn't leak fds" << std::endl;
    struct dirent **lst;
    TString path = "/proc/" + pid + "/fd/";
    int nr = scandir(path.c_str(), &lst, NULL, alphasort);
    PrintFds(path, lst, nr);
    ExpectEq(nr, 2 + 3);

    Say() << "Check that task namespaces are correct" << std::endl;
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(pid, "pid"));
    ExpectNeq(GetNamespace("self", "mnt"), GetNamespace(pid, "mnt"));
    ExpectNeq(GetNamespace("self", "ipc"), GetNamespace(pid, "ipc"));
    ExpectEq(GetNamespace("self", "net"), GetNamespace(pid, "net"));
    //ExpectEq(GetNamespace("self", "user"), GetNamespace(pid, "user"));
    ExpectNeq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));

    Say() << "Check that task cgroups are correct" << std::endl;
    ExpectCorrectCgroups(pid, name, name);
    AsAlice(api);

    ExpectApiSuccess(api.Stop(name));
    WaitProcessExit(pid);

    Say() << "Check that hierarchical task cgroups are correct" << std::endl;

    TString child = name + "/b";
    ExpectApiSuccess(api.Create(child));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectCorrectCgroups(pid, name, name);

    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));
    ExpectApiSuccess(api.GetProperty(child, "root_pid", pid));
    ExpectCorrectCgroups(pid, child, name);

    TString parent;
    ExpectApiSuccess(api.GetProperty(child, "parent", parent));
    ExpectEq(parent, "/porto/" + name);

    ExpectApiSuccess(api.Destroy(child));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestIsolateProperty(Porto::Connection &api) {
    TString ret;

    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Make sure PID isolation works" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "isolate", "false"));

    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    ExpectNeq(ret, TString("1\n"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "ps aux"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    ExpectNeq(std::count(ret.begin(), ret.end(), '\n'), 2);
    ExpectApiSuccess(api.Stop(name));


    ExpectApiSuccess(api.SetProperty(name, "isolate", "true"));
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'echo $BASHPID'"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    Expect(ret == "1\n" || ret == "2\n");
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "ps aux"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    Expect(std::count(ret.begin(), ret.end(), '\n') < 4);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure isolate works correctly with meta parent" << std::endl;
    TString pid;

    ExpectApiSuccess(api.Create("meta"));
    ExpectApiSuccess(api.SetProperty("meta", "isolate", "false"));

    ExpectApiSuccess(api.Create("meta/test"));
    ExpectApiSuccess(api.SetProperty("meta/test", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("meta/test", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("meta/test"));
    ExpectApiSuccess(api.GetProperty("meta/test", "root_pid", pid));
    AsRoot(api);
    ExpectEq(GetNamespace("self", "pid"), GetNamespace(pid, "pid"));
    AsAlice(api);
    ExpectApiSuccess(api.Stop("meta/test"));

    ExpectApiSuccess(api.SetProperty("meta/test", "isolate", "true"));
    ExpectApiSuccess(api.SetProperty("meta/test", "command", "sh -c 'ps aux; sleep 1000'"));
    ExpectApiSuccess(api.Start("meta/test"));
    ExpectApiSuccess(api.GetProperty("meta/test", "root_pid", pid));
    AsRoot(api);
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(pid, "pid"));
    AsAlice(api);
    ExpectApiSuccess(api.Stop("meta/test"));

    ExpectApiSuccess(api.Destroy("meta/test"));
    ExpectApiSuccess(api.Destroy("meta"));

    ExpectApiSuccess(api.Create("test"));
    ExpectApiSuccess(api.Create("test/meta"));
    ExpectApiSuccess(api.SetProperty("test/meta", "isolate", "false"));
    ExpectApiSuccess(api.Create("test/meta/test"));

    ExpectApiSuccess(api.SetProperty("test", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("test"));

    ExpectApiSuccess(api.SetProperty("test/meta/test", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("test/meta/test"));
    ExpectApiSuccess(api.GetProperty("test", "root_pid", pid));
    ExpectApiSuccess(api.GetProperty("test/meta/test", "root_pid", ret));
    AsRoot(api);
    ExpectNeq(GetNamespace(ret, "pid"), GetNamespace(pid, "pid"));
    AsAlice(api);
    ExpectApiSuccess(api.Stop("test/meta/test"));

    ExpectApiSuccess(api.SetProperty("test/meta/test", "isolate", "false"));
    ExpectApiSuccess(api.Start("test/meta/test"));
    ExpectApiSuccess(api.GetProperty("test", "root_pid", pid));
    ExpectApiSuccess(api.GetProperty("test/meta/test", "root_pid", ret));
    AsRoot(api);
    ExpectEq(GetNamespace(ret, "pid"), GetNamespace(pid, "pid"));
    AsAlice(api);
    ExpectApiSuccess(api.Stop("test/meta/test"));

    ExpectApiSuccess(api.Destroy("test/meta/test"));
    ExpectApiSuccess(api.Destroy("test/meta"));
    ExpectApiSuccess(api.Destroy("test"));

    Say() << "Make sure isolate works correctly with isolate=true in meta containers" << std::endl;
    ExpectApiSuccess(api.Create("iss"));
    ExpectApiSuccess(api.SetProperty("iss", "isolate", "false"));

    ExpectApiSuccess(api.Create("iss/container"));
    ExpectApiSuccess(api.SetProperty("iss/container", "isolate", "true"));

    ExpectApiSuccess(api.Create("iss/container/hook1"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook1", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook1", "command", "sleep 1000"));
    ExpectApiSuccess(api.Create("iss/container/hook2"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook2", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook2", "command", "sleep 1000"));

    ExpectApiSuccess(api.Start("iss/container/hook1"));
    ExpectApiSuccess(api.Start("iss/container/hook2"));

    TString rootPid, hook1Pid, hook2Pid;
    ExpectApiSuccess(api.GetProperty("iss/container/hook1", "root_pid", hook1Pid));
    ExpectApiSuccess(api.GetProperty("iss/container/hook2", "root_pid", hook2Pid));

    TString state;
    ExpectApiSuccess(api.GetProperty("iss/container", "state", state));
    ExpectEq(state, "meta");

    AsRoot(api);
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook1Pid, "pid"));
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook2Pid, "pid"));
    ExpectEq(GetNamespace(hook1Pid, "pid"), GetNamespace(hook2Pid, "pid"));
    AsAlice(api);

    ExpectApiSuccess(api.Stop("iss/container"));

    Say() << "Make sure isolate works correctly with isolate=true and chroot in meta containers" << std::endl;

    TPath path(TMPDIR + "/" + name);

    RemakeDir(api, path);
    AsRoot(api);
    BootstrapCommand("/bin/sleep", path.ToString());
    path.Chown(Alice);
    AsAlice(api);

    ExpectApiSuccess(api.SetProperty("iss/container", "root", path.ToString()));
    ExpectApiSuccess(api.SetProperty("iss/container/hook1", "command", "/sleep 1000"));
    ExpectApiSuccess(api.SetProperty("iss/container/hook2", "command", "/sleep 1000"));

    ExpectApiSuccess(api.Start("iss/container/hook1"));
    ExpectApiSuccess(api.Start("iss/container/hook2"));

    ExpectApiSuccess(api.GetProperty("iss/container/hook1", "root_pid", hook1Pid));
    ExpectApiSuccess(api.GetProperty("iss/container/hook2", "root_pid", hook2Pid));

    ExpectApiSuccess(api.GetProperty("iss/container", "state", state));
    ExpectEq(state, "meta");

    AsRoot(api);
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook1Pid, "pid"));
    ExpectNeq(GetNamespace("self", "pid"), GetNamespace(hook2Pid, "pid"));
    ExpectEq(GetNamespace(hook1Pid, "pid"), GetNamespace(hook2Pid, "pid"));
    AsAlice(api);

    ExpectApiSuccess(api.Destroy("iss"));

    Say() << "Make sure kill correctly works with isolate = false" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.SetProperty("a", "isolate", "true"));

    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.SetProperty("a/b", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));
    ExpectApiSuccess(api.Start("a/b"));

    ExpectApiSuccess(api.Create("a/c"));
    ExpectApiSuccess(api.SetProperty("a/c", "command", "bash -c 'nohup sleep 1000 & nohup sleep 1000 & sleep 1000'"));
    ExpectApiSuccess(api.SetProperty("a/c", "isolate", "false"));
    ExpectApiSuccess(api.Start("a/c"));

    ExpectApiSuccess(api.GetProperty("a/c", "root_pid", pid));
    ExpectApiSuccess(api.Kill("a/c", SIGKILL));
    WaitContainer(api, "a/c");

    WaitProcessExit(pid);
    kill(stoi(pid), 0);
    ExpectEq(errno, ESRCH);

    ExpectApiSuccess(api.GetProperty("a", "state", state));
    ExpectEq(state, "meta");
    ExpectApiSuccess(api.GetProperty("a/b", "state", state));
    ExpectEq(state, "running");
    ExpectApiSuccess(api.GetProperty("a/c", "state", state));
    ExpectEq(state, "dead");
    ExpectApiSuccess(api.Destroy("a"));
}

static void TestContainerNamespaces(Porto::Connection &api) {
    bool def = config().container().default_porto_namespace();
    TString val;

    Say() << "Test container namespaces" << std::endl;

    Say() << "Check default value" << std::endl;
    ExpectApiSuccess(api.Create("c"));
    ExpectApiSuccess(api.GetProperty("c", "porto_namespace", val));
    ExpectEq(val, def ? "c/" : "");

    Say() << "Check inheritance" << std::endl;
    ExpectApiSuccess(api.SetProperty("c", "porto_namespace", "my-prefix-"));
    ExpectApiSuccess(api.GetProperty("c", "porto_namespace", val));
    ExpectApiSuccess(api.Create("c/d"));
    ExpectApiSuccess(api.GetProperty("c/d", "porto_namespace", val));
    ExpectEq(val, def ? "d/" : "");
    ExpectApiSuccess(api.SetProperty("c/d", "porto_namespace", "second-prefix-"));
    ExpectApiSuccess(api.GetProperty("c/d", "porto_namespace", val));
    ExpectEq(val, "second-prefix-");

    Say() << "Check simple prefix" << std::endl;
    ExpectApiSuccess(api.SetProperty("c", "porto_namespace", "simple-prefix-"));
    ExpectApiSuccess(api.SetProperty("c/d", "command", portoctl + " create test"));

    ExpectApiSuccess(api.GetProperty("c", "absolute_namespace", val));
    ExpectEq(val, "/porto/simple-prefix-");

    ExpectApiSuccess(api.GetProperty("c/d", "absolute_namespace", val));
    ExpectEq(val, "/porto/simple-prefix-second-prefix-");

    ExpectApiSuccess(api.Start("c/d"));
    WaitContainer(api, "c/d");

    ExpectApiSuccess(api.Destroy("simple-prefix-second-prefix-test"));
    ExpectApiSuccess(api.Stop("c/d"));
    ExpectApiSuccess(api.Stop("c"));

    Say() << "Check container prefix" << std::endl;
    ExpectApiSuccess(api.SetProperty("c", "porto_namespace", "c/"));
    ExpectApiSuccess(api.SetProperty("c/d", "command", portoctl + " create test"));
    ExpectApiSuccess(api.Start("c/d"));
    WaitContainer(api, "c/d");
    ExpectApiSuccess(api.Destroy("c/second-prefix-test"));
    ExpectApiSuccess(api.Stop("c/d"));

    Say() << "Check absolute name" << std::endl;
    ExpectApiSuccess(api.Start("c/d"));
    WaitContainer(api, "c/d");
    ExpectApiSuccess(api.GetProperty("c/second-prefix-test", "absolute_name", val));
    ExpectEq(val, "/porto/c/second-prefix-test");
    ExpectApiSuccess(api.Stop("c/d"));
    ExpectApiSuccess(api.Destroy("c/d"));
    ExpectApiSuccess(api.Destroy("c"));
}

static void TestEnvTrim(Porto::Connection &api) {
    TString val;
    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check property trimming" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "env", ""));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "");

    ExpectApiSuccess(api.SetProperty(name, "env", " "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "");

    ExpectApiSuccess(api.SetProperty(name, "env", "    "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "");

    ExpectApiSuccess(api.SetProperty(name, "env", " a"));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "a");

    ExpectApiSuccess(api.SetProperty(name, "env", "b "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "b");

    ExpectApiSuccess(api.SetProperty(name, "env", " c "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "c");

    ExpectApiSuccess(api.SetProperty(name, "env", "     d     "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "d");

    ExpectApiSuccess(api.SetProperty(name, "env", "    e"));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "e");

    ExpectApiSuccess(api.SetProperty(name, "env", "f    "));
    ExpectApiSuccess(api.GetProperty(name, "env", val));
    ExpectEq(val, "f");

    TString longProperty = TString(10 * 1024, 'x');
    ExpectApiSuccess(api.SetProperty(name, "env", longProperty));
    ExpectApiSuccess(api.GetProperty(name, "env", val));

    ExpectApiSuccess(api.Destroy(name));
}

static TString EnvSep(1, '\0');

static void ExpectEnv(Porto::Connection &api,
                      const TString &name,
                      const TString &env,
                      const TString expected) {
    TString pid;

    ExpectApiSuccess(api.SetProperty(name, "env", env));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    TString ret = GetEnv(pid);

    Expect(ret == expected);
    ExpectApiSuccess(api.Stop(name));
}

static void TestEnvProperty(Porto::Connection &api) {
    TString name = "a";
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    AsRoot(api);

    Say() << "Check default environment" << std::endl;

    static const TString empty_env =
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" + EnvSep +
        "HOME=/place/porto/a" + EnvSep +
        "USER=porto-alice" + EnvSep +
        "container=lxc" + EnvSep +
        "PORTO_NAME=a" + EnvSep +
        "PORTO_HOST=" + GetHostName() + EnvSep +
        "PORTO_USER=porto-alice" + EnvSep;
    ExpectEnv(api, name, "", empty_env);

    Say() << "Check user-defined environment" << std::endl;
    static const TString ab_env =
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" + EnvSep +
        "HOME=/place/porto/a" + EnvSep +
        "USER=porto-alice" + EnvSep +
        "container=lxc" + EnvSep +
        "PORTO_NAME=a" + EnvSep +
        "PORTO_HOST=" + GetHostName() + EnvSep +
        "PORTO_USER=porto-alice" + EnvSep +
        "a=b" + EnvSep +
        "c=d" + EnvSep;

    ExpectEnv(api, name, "a=b;c=d;", ab_env);
    ExpectEnv(api, name, "a=b;;c=d;", ab_env);

    static const TString asb_env =
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin" + EnvSep +
        "HOME=/place/porto/a" + EnvSep +
        "USER=porto-alice" + EnvSep +
        "container=lxc" + EnvSep +
        "PORTO_NAME=a" + EnvSep +
        "PORTO_HOST=" + GetHostName() + EnvSep +
        "PORTO_USER=porto-alice" + EnvSep +
        "a=e;b" + EnvSep +
        "c=d" + EnvSep;
    ExpectEnv(api, name, "a=e\\;b;c=d;", asb_env);

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep $N"));
    ExpectApiSuccess(api.SetProperty(name, "env", "N=1"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestUserGroupProperty(Porto::Connection &api) {
    int uid, gid;
    TString pid;

    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check default user & group" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    ExpectEq(uid, Alice.Uid);
    ExpectEq(gid, Alice.Gid);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check custom user & group" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    ExpectApiSuccess(api.SetProperty(name, "user", "root"));
    ExpectApiFailure(api.Start(name), EError::Permission);

    ExpectApiSuccess(api.SetProperty(name, "user", Bob.User()));
    ExpectApiFailure(api.Start(name), EError::Permission);

    ExpectApiSuccess(api.SetProperty(name, "user", Alice.User()));

    ExpectApiSuccess(api.SetProperty(name, "group", Bob.Group()));
    ExpectApiFailure(api.Start(name), EError::Permission);
    ExpectApiSuccess(api.SetProperty(name, "group", Alice.Group()));

    ExpectApiFailure(api.SetProperty(name, "owner_user", Bob.User()), EError::Permission);
    ExpectApiFailure(api.SetProperty(name, "owner_group", Bob.Group()), EError::Permission);

    TString user, group;
    ExpectApiSuccess(api.GetProperty(name, "user", user));
    ExpectApiSuccess(api.GetProperty(name, "group", group));
    ExpectEq(user, Alice.User());
    ExpectEq(group, Alice.Group());
    ExpectApiSuccess(api.SetProperty(name, "user", user));
    ExpectApiSuccess(api.SetProperty(name, "group", group));

    AsRoot(api);
    ExpectApiSuccess(api.SetProperty(name, "user", Bob.User()));
    ExpectApiSuccess(api.SetProperty(name, "group", Bob.Group()));
    ExpectApiSuccess(api.SetProperty(name, "owner_user", Bob.User()));
    ExpectApiSuccess(api.SetProperty(name, "owner_group", Bob.Group()));
    AsAlice(api);

    ExpectApiFailure(api.Start(name), EError::Permission);

    AsRoot(api);
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    GetUidGid(pid, uid, gid);

    ExpectEq(uid, Bob.Uid);
    ExpectEq(gid, Bob.Gid);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check integer user & group" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "user", "9999"));
    ExpectApiSuccess(api.SetProperty(name, "group", "9999"));
    ExpectApiSuccess(api.GetProperty(name, "user", user));
    ExpectApiSuccess(api.GetProperty(name, "group", group));
    ExpectEq(user, "9999");
    ExpectEq(group, "9999");

    ExpectApiSuccess(api.Destroy(name));
    AsAlice(api);
}

static void TestCwdProperty(Porto::Connection &api) {
    TString pid;
    TString cwd;
    TString portodPid, portodCwd;

    AsRoot(api);

    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    (void)TPath(PORTO_PIDFILE).ReadAll(portodPid);
    portodCwd = GetCwd(portodPid);

    Say() << "Check default working directory" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    cwd = GetCwd(pid);

    TString prefix = PORTO_WORKDIR;

    ExpectNeq(cwd, portodCwd);
    ExpectEq(cwd, prefix + "/" + name);

    ExpectEq(access(cwd.c_str(), F_OK), 0);
    ExpectApiSuccess(api.Stop(name));
    ExpectNeq(access(cwd.c_str(), F_OK), 0);
    ExpectApiSuccess(api.Destroy(name));

    ExpectApiSuccess(api.Create("b"));
    ExpectApiSuccess(api.SetProperty("b", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("b"));
    ExpectApiSuccess(api.GetProperty("b", "root_pid", pid));
    TString bcwd = GetCwd(pid);
    ExpectApiSuccess(api.Destroy("b"));

    ExpectNeq(bcwd, portodCwd);
    ExpectEq(bcwd, prefix + "/b");
    ExpectNeq(bcwd, cwd);

    Say() << "Check user defined working directory" << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "cwd", "/tmp"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    cwd = GetCwd(pid);

    ExpectEq(cwd, "/tmp");
    ExpectEq(access("/tmp", F_OK), 0);
    ExpectApiSuccess(api.Stop(name));
    ExpectEq(access("/tmp", F_OK), 0);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check working directory of meta parent/child" << std::endl;
    TString parent = "parent", child = "parent/child";

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(child, "cwd", "/tmp"));
    ExpectApiSuccess(api.SetProperty(child, "command", "pwd"));
    ExpectApiSuccess(api.SetProperty(child, "isolate", "false"));
    auto s = StartWaitAndGetProperty(api, child, "stdout");
    ExpectEq(StringTrim(s), "/tmp");
    ExpectApiSuccess(api.Destroy(parent));

    AsAlice(api);
}

static void TestStdPathProperty(Porto::Connection &api) {
    TString pid;
    TString name = "a";
    TString cwd, stdinName, stdoutName, stderrName;
    TPath stdinPath, stdoutPath, stderrPath;

    AsRoot(api); // FIXME
    ExpectApiSuccess(api.Create(name));

    Say() << "Check default stdin/stdout/stderr" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.GetProperty(name, "cwd", cwd));

    ExpectApiSuccess(api.GetProperty(name, "stdin_path", stdinName));
    ExpectEq(stdinName, "/dev/null");

    ExpectApiSuccess(api.GetProperty(name, "stdout_path", stdoutName));
    ExpectEq(stdoutName, "stdout");

    ExpectApiSuccess(api.GetProperty(name, "stderr_path", stderrName));
    ExpectEq(stderrName, "stderr");

    stdoutPath = TPath(cwd) / stdoutName;
    stderrPath = TPath(cwd) / stderrName;

    Expect(!stdoutPath.Exists());
    Expect(!stderrPath.Exists());
    ExpectApiSuccess(api.Start(name));
    Expect(stdoutPath.Exists());
    Expect(stderrPath.Exists());

    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(ReadLink("/proc/" + pid + "/fd/0"), "/dev/null");
    ExpectEq(ReadLink("/proc/" + pid + "/fd/1"), stdoutPath.ToString());
    ExpectEq(ReadLink("/proc/" + pid + "/fd/2"), stderrPath.ToString());
    ExpectApiSuccess(api.Stop(name));

    Expect(!stdoutPath.Exists());
    Expect(!stderrPath.Exists());

    Say() << "Check custom stdin/stdout/stderr" << std::endl;
    stdinPath = "/tmp/a_stdin";
    stdoutPath = "/tmp/a_stdout";
    stderrPath = "/tmp/a_stderr";

    (void)stdinPath.Unlink();
    (void)stdoutPath.Unlink();
    (void)stderrPath.Unlink();

    ExpectOk(stdinPath.Mkfile(0600));
    ExpectOk(stdinPath.WriteAll("hi"));

    ExpectApiSuccess(api.SetProperty(name, "stdin_path", "/tmp/a_stdin"));
    ExpectApiSuccess(api.SetProperty(name, "stdout_path", "/tmp/a_stdout"));
    ExpectApiSuccess(api.SetProperty(name, "stderr_path", "/tmp/a_stderr"));
    Expect(!stdoutPath.Exists());
    Expect(!stderrPath.Exists());
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(ReadLink("/proc/" + pid + "/fd/0"), "/tmp/a_stdin");
    ExpectEq(ReadLink("/proc/" + pid + "/fd/1"), "/tmp/a_stdout");
    ExpectEq(ReadLink("/proc/" + pid + "/fd/2"), "/tmp/a_stderr");
    ExpectApiSuccess(api.Stop(name));
    Expect(stdinPath.Exists());
    Expect(stdoutPath.Exists());
    Expect(stderrPath.Exists());

    Say() << "Make sure custom stdin is not removed" << std::endl;
    TString ret;
    ExpectApiSuccess(api.SetProperty(name, "command", "cat"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    ExpectEq(ret, TString("hi"));

    ExpectApiSuccess(api.Destroy(name));

    Expect(stdinPath.Exists());
    Expect(stdoutPath.Exists());
    Expect(stderrPath.Exists());

    ExpectOk(stdinPath.Unlink());
    ExpectOk(stdoutPath.Unlink());
    ExpectOk(stderrPath.Unlink());
}

struct TMountInfo {
    TString flags;
    TString source;
};

static map<TString, TMountInfo> ParseMountinfo(std::vector<TString> lines) {
    map<TString, TMountInfo> m;

    for (auto &line : lines) {
        auto tok = SplitString(line, ' ');
        ExpectOp(tok.size(), >, 5);

        TMountInfo i;
        i.flags = tok[5];

        int sep = 6;
        while (tok[sep] != "-")
            sep++;

        i.source = tok[sep + 2];

        m[tok[4]] = i;
    }

    return m;
}

static void TestRootRdOnlyProperty(Porto::Connection &api) {
    TString name = "a";
    TPath path(TMPDIR + "/" + name);
    TString ROnly;
    TString ret;

    RemakeDir(api, path);

    Say() << "Check root read only property" << std::endl;
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.GetProperty(name, "root_readonly", ROnly));
    ExpectEq(ROnly, "false");

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));
    AsRoot(api);
    BootstrapCommand("/usr/bin/touch", path.ToString());
    BootstrapCommand("/bin/cat", path.ToString(), false);
    path.Chown(Alice);
    AsAlice(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/touch test"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", ret));
    ExpectEq(ret, TString("0"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "root_readonly", "true"));
    ExpectApiSuccess(api.SetProperty(name, "command", "/touch test2"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", ret));
    ExpectNeq(ret, TString("0"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure pivot_root works and we don't leak host mount points" << std::endl;
    std::set<TString> expected = {
        "/run/lock",
        // restricted proc
        "/proc/sysrq-trigger",
        "/proc/irq",
        "/proc/bus",
        "/proc/sys",
        "/proc/kcore",

        // dev
        "/dev",
        "/dev/pts",
        "/dev/shm",

        "/etc/resolv.conf",

        "/proc",
        "/run",
        "/sys",
        "/",
    };

    if (config().container().enable_tracefs()) {
        expected.insert("/sys/kernel/debug");
        expected.insert("/sys/kernel/debug/tracing");
        if (TPath("/sys/kernel/tracing").Exists())
            expected.insert("/sys/kernel/tracing");
    }

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));
    ExpectApiSuccess(api.SetProperty(name, "root_readonly", "true"));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
    ExpectApiSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    auto v = StartWaitAndGetProperty(api, name, "stdout");
    auto lines = SplitString(v, '\n');
    auto m = ParseMountinfo(lines);

    if (m.count("/dev/hugepages"))
        expected.insert("/dev/hugepages");

    ExpectEq(m.size(), expected.size());
    for (auto pair : m)
        Expect(expected.find(pair.first) != expected.end());

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

unsigned long GetInode(const TPath &path) {
    struct stat st;
    ExpectEq(stat(path.ToString().c_str(), &st), 0);
    return st.st_ino;
}

static void TestRootProperty(Porto::Connection &api) {
    TString pid;
    TString v;

    TString name = "a";
    TString path = TMPDIR + "/" + name;

    Say() << "Make sure root is empty" << std::endl;

    ExpectApiSuccess(api.Create(name));
    RemakeDir(api, path);

    ExpectApiSuccess(api.SetProperty(name, "command", "ls"));
    ExpectApiSuccess(api.SetProperty(name, "root", path));
    ExpectApiFailure(api.Start(name), EError::InvalidCommand);
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check filesystem isolation" << std::endl;

    ExpectApiSuccess(api.Create(name));

    RemakeDir(api, path);

    AsRoot(api);
    BootstrapCommand("/bin/sleep", path, false);
    BootstrapCommand("/bin/pwd", path, false);
    BootstrapCommand("/bin/ls", path, false);
    AsAlice(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    TString bindDns;

    ExpectApiSuccess(api.SetProperty(name, "root", path));

    TString cwd;
    ExpectApiSuccess(api.GetProperty(name, "cwd", cwd));
    ExpectEq(cwd, "/");

    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    // root or cwd may have / but it actually points to correct path,
    // test inodes instead
    AsRoot(api);
    ExpectEq(GetInode("/proc/" + pid + "/cwd"), GetInode(path));
    ExpectEq(GetInode("/proc/" + pid + "/root"), GetInode(path));
    AsAlice(api);

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/pwd"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);

    ExpectApiSuccess(api.GetProperty(name, "stdout", v));
    ExpectEq(v, TString("/\n"));
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check /dev layout" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "command", "/ls -1 /dev"));
    v = StartWaitAndGetProperty(api, name, "stdout");

    vector<TString> devs = { "null", "zero", "full", "urandom",
                            "random", "console", "tty", "stdin", "stdout",
                            "stderr", "ptmx", "pts", "shm", "fd" };
    auto tokens = SplitString(v, '\n');

    if (std::find(tokens.begin(), tokens.end(), "hugepages") != tokens.end())
        devs.push_back("hugepages");

    ExpectEq(devs.size(), tokens.size());
    for (auto &dev : devs)
        Expect(std::find(tokens.begin(), tokens.end(), dev) != tokens.end());

    ExpectApiSuccess(api.Stop(name));

    Say() << "Check /proc restrictions" << std::endl;

    RemakeDir(api, path);
    AsRoot(api);
    BootstrapCommand("/bin/cat", path, false);
    AsAlice(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "/cat /proc/self/mountinfo"));
    v = StartWaitAndGetProperty(api, name, "stdout");

    auto lines = SplitString(v, '\n');
    auto m = ParseMountinfo(lines);
    ExpectNeq(m["/sys"].flags.find("ro,"), TString::npos);
    ExpectNeq(m["/proc/sys"].flags.find("ro,"), TString::npos);
    ExpectNeq(m["/proc/sysrq-trigger"].flags.find("ro,"), TString::npos);
    ExpectNeq(m["/proc/irq"].flags.find("ro,"), TString::npos);
    ExpectNeq(m["/proc/bus"].flags.find("ro,"), TString::npos);

    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure /dev /sys /proc are not mounted when root is not isolated " << std::endl;

    cwd = TString(PORTO_WORKDIR) + "/" + name;

    TPath f(cwd);
    AsRoot(api);
    if (f.Exists()) {
        TError error = f.RemoveAll();
        ExpectOk(error);
    }
    AsAlice(api);

    ExpectApiSuccess(api.SetProperty(name, "root", "/"));
    ExpectApiSuccess(api.SetProperty(name, "command", "ls -1 " + cwd));

    v = StartWaitAndGetProperty(api, name, "stdout");
    ExpectEq(v, "stderr\nstdout\n");

    ExpectApiSuccess(api.Destroy(name));
}

static bool TestPathsHelper(Porto::Connection &api,
                            const TString &cmd,
                            const TString &root,
                            const TString &cwd,
                            const TString &bind,
                            const TString &cout_path,
                            const TString &cerr_path) {
    static const TString name = "paths_test_container";
    TString state;
    TString log = "Paths test: cmd=" + cmd;

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", cmd));

    if (root.length() > 0) {
        ExpectApiSuccess(api.SetProperty(name, "root", root));
        log += " root=" + root;
    }
    if (cwd.length() > 0) {
        ExpectApiSuccess(api.SetProperty(name, "cwd", cwd));
        log += " cwd=" + cwd;
    }
    if (bind.length() > 0) {
        ExpectApiSuccess(api.SetProperty(name, "bind", bind));
        log += " bind=" + bind;
    }
    if (cout_path.length() > 0) {
        log += " cout_path=" + cout_path;
        ExpectApiSuccess(api.SetProperty(name, "stdout_path", cout_path));
    }
    if (cerr_path.length() > 0) {
        log += " cerr_path=" + cerr_path;
        ExpectApiSuccess(api.SetProperty(name, "stderr_path", cerr_path));
    }

    Say() << log << std::endl;

    TString ret;
    ExpectApiSuccess(api.SetProperty(name, "isolate", "true"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.WaitContainer(name, state, -1));
    ExpectEq(state, "dead");
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    ExpectApiSuccess(api.GetProperty(name, "stderr", ret));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "isolate", "false"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.WaitContainer(name, state, -1));
    ExpectEq(state, "dead");
    ExpectApiSuccess(api.GetProperty(name, "stdout", ret));
    ExpectApiSuccess(api.GetProperty(name, "stderr", ret));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));

    return true;
}

static void TestPaths(Porto::Connection &api) {
    AsRoot(api);
    ExpectEq(system("mkdir /myroot && chmod 777 /myroot"), 0);
    AsAlice(api);
    ExpectEq(system(("mkdir /myroot/bin && cp " + portoinit + " /myroot/bin/test2").c_str()), 0);

    /* isolate, root, cwd, bind, cout_path, cerr_path */
    TestPathsHelper(api, "/myroot/bin/test2 -v", "", "", "", "", "");
    TestPathsHelper(api, "/bin/test2 -v", "/myroot", "", "", "", "");
    TestPathsHelper(api, "test2 -v", "/myroot", "/bin", "", "", "");
    TestPathsHelper(api, "sbin/test2 -v", "/myroot", "/bin", "/myroot/bin sbin ro",
                    "", "");
    TestPathsHelper(api, "/myroot/sbin/test2 -v", "", "", "/myroot/bin /myroot/sbin ro", "", "");
    AsRoot(api);
    TestPathsHelper(api, "/myroot/bin/test2 -v", "", "", "", "my.stdout", "my.stderr");
    TestPathsHelper(api, "/bin/test2 -v", "/myroot", "", "", "/my.stdout", "/my.stderr");
    TestPathsHelper(api, "test2 -v", "/myroot", "/bin", "", "my.stdout", "my.stderr");
    AsAlice(api);

    AsRoot(api);
    ExpectEq(system("rm -rf /myroot"), 0);
    AsAlice(api);
}

static TString GetHostname() {
    char buf[1024];
    ExpectEq(gethostname(buf, sizeof(buf)), 0);
    return buf;
}

static void TestHostnameProperty(Porto::Connection &api) {
    TString pid, v;
    TString name = "a";
    TString host = "porto_" + name;
    TPath path(TMPDIR + "/" + name);

    ExpectApiSuccess(api.Create(name));


    Say() << "Check non-isolated hostname" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "/bin/sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "isolate", "false"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    AsRoot(api);
    ExpectEq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));
    AsAlice(api);
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/bin/hostname"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", v));
    ExpectEq(v, GetHostname() + "\n");
    ExpectApiSuccess(api.Stop(name));

    RemakeDir(api, path);

    AsRoot(api);
    ExpectOk(path.Mount(name, "tmpfs", 0, {"size=32m"}));
    ExpectOk(TPath(path + "/etc").Mkdir(0755));
    ExpectOk(TPath(path + "/etc/hostname").Mkfile(0644));
    BootstrapCommand("/bin/hostname", path, false);
    BootstrapCommand("/bin/sleep", path, false);
    BootstrapCommand("/bin/cat", path, false);
    AsAlice(api);

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));

    Say() << "Check default isolated hostname" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "isolate", "true"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    AsRoot(api);
    ExpectNeq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));
    AsAlice(api);
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/hostname"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", v));
    ExpectEq(v, GetHostname() + "\n");
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check custom hostname" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "hostname", host));

    ExpectApiSuccess(api.SetProperty(name, "command", "/sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    AsRoot(api);
    ExpectNeq(GetNamespace("self", "uts"), GetNamespace(pid, "uts"));
    AsAlice(api);
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "/hostname"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", v));
    ExpectNeq(v, GetHostname() + "\n");
    ExpectEq(v, host + "\n");
    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Check /etc/hostname" << std::endl;

    AsBob(api);
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "virt_mode", "os"));
    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));
    ExpectApiSuccess(api.SetProperty(name, "hostname", host));
    ExpectApiSuccess(api.SetProperty(name, "command", "/cat /etc/hostname"));
    ExpectApiSuccess(api.SetProperty(name, "stdout_path", "stdout"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "stdout", v));
    ExpectNeq(v, GetHostname() + "\n");
    ExpectEq(v, host + "\n");

    ExpectApiSuccess(api.Destroy(name));

    AsRoot(api);
    ExpectOk(path.Umount(0));
    AsAlice(api);
}

static void TestCapabilitiesProperty(Porto::Connection &api) {
    TString name = "a";
    TString pid;

    int lastCap;
    TError error = TPath("/proc/sys/kernel/cap_last_cap").ReadInt(lastCap);
    ExpectOk(error);

    //uint64_t allCap = (1ULL << (lastCap + 1)) - 1;

    uint64_t defaultCap = 0x00000000a9ec77fb;

    Say() << "Check default capabilities for non-root container" << std::endl;

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(GetCap(pid, "CapInh"), 0);
    ExpectEq(GetCap(pid, "CapPrm"), 0);
    ExpectEq(GetCap(pid, "CapEff"), 0);
    ExpectEq(GetCap(pid, "CapBnd"), defaultCap);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Checking custom capabilities" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "capabilities", "CHOWN"));

    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(GetCap(pid, "CapInh"), 0);
    ExpectEq(GetCap(pid, "CapPrm"), 0);
    ExpectEq(GetCap(pid, "CapEff"), 0);
    ExpectEq(GetCap(pid, "CapBnd"), 1);
    ExpectApiSuccess(api.Destroy(name));

    AsRoot(api);

    Say() << "Checking default capabilities for root container" << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    ExpectEq(GetCap(pid, "CapInh"), 0);
    ExpectEq(GetCap(pid, "CapPrm"), defaultCap);
    ExpectEq(GetCap(pid, "CapEff"), defaultCap);
    ExpectEq(GetCap(pid, "CapBnd"), defaultCap);
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check limiting root capabilities" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "capabilities", "CHOWN"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(GetCap(pid, "CapInh"), 0);
    ExpectEq(GetCap(pid, "CapPrm"), 1);
    ExpectEq(GetCap(pid, "CapEff"), 1);
    ExpectEq(GetCap(pid, "CapBnd"), 1);

    ExpectApiSuccess(api.Destroy(name));
}

static void CheckConnectivity(Porto::Connection &api, const TString &name,
                              bool enabled, bool disabled) {
    TString v;

    if (disabled) {
        ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
        ExpectApiSuccess(api.Start(name));
        WaitContainer(api, name);
        ExpectApiSuccess(api.GetProperty(name, "exit_status", v));
        ExpectNeq(v, "0");
        ExpectApiSuccess(api.Stop(name));
    }

    if (enabled) {
        ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));
        ExpectApiSuccess(api.Start(name));
        WaitContainer(api, name);
        ExpectApiSuccess(api.GetProperty(name, "exit_status", v));
        ExpectEq(v, "0");
        ExpectApiSuccess(api.Stop(name));
    }
}

static void TestEnablePortoProperty(Porto::Connection &api) {
    TString name = "a";
    TString name2 = "a/b";
    TPath path(TMPDIR + "/" + name);

    RemakeDir(api, path);
    AsRoot(api);
    BootstrapCommand(program_invocation_name, path.ToString());
    path.Chown(Alice);
    AsAlice(api);

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.Create(name2));

    ExpectApiSuccess(api.SetProperty(name, "command", "/portotest connectivity"));

    Say() << "Non-isolated" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
    ExpectApiSuccess(api.SetProperty(name2, "enable_porto", "false"));
    ExpectApiFailure(api.SetProperty(name2, "enable_porto", "true"), EError::Permission);

    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));
    ExpectApiSuccess(api.SetProperty(name2, "enable_porto", "false"));
    ExpectApiSuccess(api.SetProperty(name2, "enable_porto", "true"));

    Say() << "Root-isolated" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));

    Say() << "Namespace-isolated" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "root", "/"));
    ExpectApiSuccess(api.SetProperty(name, "porto_namespace", "a/"));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "false"));
    ExpectApiSuccess(api.SetProperty(name, "enable_porto", "true"));

    Say() << "Isolated" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "root", path.ToString()));

    CheckConnectivity(api, name, true, true);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Isolated hierarchy" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("a/b"));

    ExpectApiSuccess(api.SetProperty("a", "porto_namespace", ""));
    ExpectApiSuccess(api.SetProperty("a/b", "command", "/portotest connectivity"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "true"));
    ExpectApiSuccess(api.SetProperty("a/b", "porto_namespace", "a/"));
    ExpectApiSuccess(api.SetProperty("a/b", "root", path.ToString()));

    CheckConnectivity(api, "a/b", true, true);

    ExpectApiSuccess(api.Stop("a"));
    ExpectApiSuccess(api.SetProperty("a/b", "root", "/"));
    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("a/b", "porto_namespace", ""));
    ExpectApiSuccess(api.SetProperty("a", "porto_namespace", "a/"));
    ExpectApiSuccess(api.SetProperty("a", "root", path.ToString()));

    CheckConnectivity(api, "a/b", true, true);

    ExpectApiSuccess(api.Destroy("a"));
}

static void TestStateMachine(Porto::Connection &api) {
    TString name = "a";
    TString pid;
    TString v;

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "stopped");

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "running");

    ExpectApiFailure(api.Start(name), EError::InvalidState);

    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    WaitProcessExit(pid);
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    Expect(v == "running" || v == "dead");

    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "dead");

    ExpectApiFailure(api.Start(name), EError::InvalidState);

    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "stopped");

    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "stopped");

    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'while :; do :; done'"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    v = GetState(pid);
    ExpectEq(v, "R");

    ExpectApiSuccess(api.Pause(name));
    v = GetState(pid);
    ExpectEq(v, "D");

    ExpectApiFailure(api.Pause(name), EError::InvalidState);

    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "paused");

    ExpectApiSuccess(api.Resume(name));
    v = GetState(pid);
    ExpectEq(v, "R");

    ExpectApiFailure(api.Resume(name), EError::InvalidState);

    ExpectApiSuccess(api.Stop(name));
    WaitProcessExit(pid);

    Say() << "Make sure we can stop unintentionally frozen container " << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    v = GetFreezer(name);
    ExpectEq(v, "THAWED\n");

    AsRoot(api);
    SetFreezer(name, "FROZEN");
    AsAlice(api);

    v = GetFreezer(name);
    ExpectEq(v, "FROZEN\n");

    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure we can remove paused container " << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Pause(name));
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure kill SIGTERM works" << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);
    ExpectApiSuccess(api.Kill(name, SIGTERM));
    WaitContainer(api, name);
    ExpectEq(TaskRunning(pid), false);
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "dead");
    ExpectApiSuccess(api.GetProperty(name, "exit_status", v));
    ExpectEq(v, TString("15"));
    ExpectApiSuccess(api.Destroy(name));

    // if container init process doesn't have custom handler for a signal
    // it's ignored
    Say() << "Make sure init in container ignores SIGTERM but dies after SIGKILL" << std::endl;
    AsRoot(api);
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "virt_mode", "os"));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);
    ExpectApiSuccess(api.Kill(name, SIGTERM));
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "running");
    ExpectEq(TaskRunning(pid), true);
    ExpectApiSuccess(api.Kill(name, SIGKILL));
    WaitContainer(api, name);
    ExpectEq(TaskRunning(pid), false);
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "dead");
    ExpectApiSuccess(api.GetProperty(name, "exit_status", v));
    ExpectEq(v, TString("9"));

    // we can't kill root or non-running container
    ExpectApiFailure(api.Kill(name, SIGKILL), EError::InvalidState);
    ExpectApiFailure(api.Kill("/", SIGKILL), EError::Permission);

    ExpectApiSuccess(api.Destroy(name));
    AsAlice(api);
}

static void TestPath(Porto::Connection &) {
    vector<pair<TString, TString>> normalize = {
        { "",   "" },
        { ".",  "." },
        { "..", ".." },
        { "a",  "a" },
        { "/a",   "/a" },
        { "/a/b/c",   "/a/b/c" },
        { "////a//",   "/a" },
        { "/././.",   "/" },
        { "/a/..",   "/" },
        { "a/..",   "." },
        { "../a/../..",   "../.." },
        { "/a/../..",   "/" },
        { "/abc/cde/../..",   "/" },
        { "/abc/../cde/.././../abc",   "/abc" },

        /* Stolen from golang src/path/filepath/path_test.go */

        // Already clean
        {"abc", "abc"},
        {"abc/def", "abc/def"},
        {"a/b/c", "a/b/c"},
        {".", "."},
        {"..", ".."},
        {"../..", "../.."},
        {"../../abc", "../../abc"},
        {"/abc", "/abc"},
        {"/", "/"},

        // Remove trailing slash
        {"abc/", "abc"},
        {"abc/def/", "abc/def"},
        {"a/b/c/", "a/b/c"},
        {"./", "."},
        {"../", ".."},
        {"../../", "../.."},
        {"/abc/", "/abc"},

        // Remove doubled slash
        {"abc//def//ghi", "abc/def/ghi"},
        {"//abc", "/abc"},
        {"///abc", "/abc"},
        {"//abc//", "/abc"},
        {"abc//", "abc"},

        // Remove . elements
        {"abc/./def", "abc/def"},
        {"/./abc/def", "/abc/def"},
        {"abc/.", "abc"},

        // Remove .. elements
        {"abc/def/ghi/../jkl", "abc/def/jkl"},
        {"abc/def/../ghi/../jkl", "abc/jkl"},
        {"abc/def/..", "abc"},
        {"abc/def/../..", "."},
        {"/abc/def/../..", "/"},
        {"abc/def/../../..", ".."},
        {"/abc/def/../../..", "/"},
        {"abc/def/../../../ghi/jkl/../../../mno", "../../mno"},
        {"/../abc", "/abc"},

        // Combinations
        {"abc/./../def", "def"},
        {"abc//./../def", "def"},
        {"abc/../../././../def", "../../def"},
    };

    vector<vector<TString>> inner = {
        { "/", "/", ".", "/" },
        { "/", "a", "", "" },
        { "a", "/", "", "" },
        { "/", "", "", "" },
        { "", "/", "", "" },
        { "/", "/abc", "abc", "/abc" },
        { "/", "/abc/def", "abc/def", "/abc/def" },
        { "/abc", "/abc", ".", "/" },
        { "/abc", "/abc/def", "def", "/def" },
        { "/abc", "/abcdef", "", "" },
        { "/abcdef", "/abc", "", "" },
        { "/abc/def", "/abc", "", "" },
        { "abc", "abc", ".", "/" },
        { "abc", "abc/def", "def", "/def" },
        { "abc", "abcdef", "", "" },
    };

    /* path, dirname, basename */
    std::vector<std::vector<TString>> split = {
        { "/usr/lib", "/usr", "lib" },
        { "/usr/", "/", "usr" },
        { "usr", ".", "usr" },
        { "/", "/", "/" },
        { ".", ".", "." },
        { "..", ".", ".." },

        { "//usr//lib", "/usr", "lib" },
        { "//usr//", "/", "usr" },
        { "usr//", ".", "usr" },
        { "//", "/", "/" },
        { "///", "/", "/" },
        { ".//", ".", "." },
        { "..//", ".", ".." },

        { "", "", "" },
        { "/.", "/", "/" },
        { "/..", "/", "/" },
        { "/a/..", "/", "/" },
        { "/../a", "/", "a" },
        { "/../a/../b/c", "/b", "c" },
        { "a/..", ".", "." },
        { "../a", "..", "a" },
        { "../..", "..", ".."},
        { "../../..", "../..", ".."},
    };

    /* base, path, relative */
    std::vector<std::vector<TString>> relative = {
        { "/a", "/a/b", "b" },
        { "/a", "/a", "." },
        { "/a/b", "/a", ".." },
        { "/a/b", "/a/b/c", "c" },
        { "/a/b", "/a/b/c/d", "c/d" },
        { "/a/b", "/c", "../../c" },
        { "/a/b", "/c/d", "../../c/d" },
        { "/a/b/c", "/a/c/d", "../../c/d" },
        { "/a/b/c", "/a", "../.." },
    };

    for (auto n: normalize)
        ExpectEq(TPath(n.first).NormalPath().ToString(), n.second);

    for (auto n: inner) {
        ExpectEq(TPath(n[0]).InnerPath(n[1], false).ToString(), n[2]);
        ExpectEq(TPath(n[0]).InnerPath(n[1], true).ToString(), n[3]);
        if (n[3] != "")
            ExpectEq((TPath(n[0]) / n[3]).ToString(), n[1]);
    }

    for (auto n: split) {
        ExpectEq(TPath(n[0]).DirName().ToString(), n[1]);
        ExpectEq(TPath(n[0]).BaseName(), n[2]);
    }

    for (auto n: relative)
        ExpectEq(TPath(n[1]).RelativePath(n[0]).ToString(), n[2]);

    std::vector<std::pair<TString, std::vector<TString>>> components = {
        { "", {}},
        { ".", { "." }},
        { "..", { ".." }},
        { "/", { "/" }},
        { "a", { "a" }},
        { "/a", { "/", "a" }},
        { "a/", { "a" }},
        { "a/b", { "a", "b" }},
        { "a//b", { "a", "b" }},
        { "a///b", { "a", "b" }},
        { "/a/b", { "/", "a", "b" }},
        { "/a/../c", { "/", "a", "..", "c" }},
    };

    for (auto n: components)
        Expect(TPath(n.first).Components() == n.second);
}

static void TestIdmap(Porto::Connection &) {
    TIdMap idmap(1, 99);
    int id;

    for (int i = 1; i <= 99; i++) {
        ExpectOk(idmap.Get(id));
        ExpectEq(id, i);
    }

    ExpectEq(idmap.Get(id).Error, EError::ResourceNotAvailable);

    for (int i = 1; i <= 99; i++)
        ExpectOk(idmap.Put(i));

    ExpectOk(idmap.Get(id));
    ExpectEq(id, 1);

    ExpectOk(idmap.Put(1));

    ExpectOk(idmap.Get(id));
    ExpectEq(id, 2);
}

static void TestFormat(Porto::Connection &) {
    uint64_t v;

    ExpectEq(StringFormat("%s %d", "a", 1), "a 1");
    ExpectEq(StringFormatSize(1), "1B");
    ExpectEq(StringFormatSize(1<<20), "1M");
    ExpectOk(StringToSize("1", v));
    ExpectEq(v, 1);
    ExpectOk(StringToSize("1kb", v));
    ExpectEq(v, 1<<10);
    ExpectOk(StringToSize("1M", v));
    ExpectEq(v, 1<<20);
    ExpectOk(StringToSize("1 Gb", v));
    ExpectEq(v, 1ull<<30);
    ExpectOk(StringToSize("1TiB", v));
    ExpectEq(v, 1ull<<40);
    ExpectOk(StringToSize("\t1\tPB\t", v));
    ExpectEq(v, 1ull<<50);
    Expect(!!StringToSize("", v));
    Expect(!!StringToSize("z", v));
    Expect(!!StringToSize("1z", v));
}

static void TestRoot(Porto::Connection &api) {
    TString v;
    TString root = "/";
    TString porto_root = "/porto";
    vector<TString> properties = {
        "command",
        "user",
        "group",
        "env",
        "cwd",
        "memory_limit",
        "cpu_policy",
        "cpu_limit",
        "cpu_guarantee",
        "devices",
        "io_policy",
        "respawn",
        "respawn_count",
        "isolate",
        "stdin_path",
        "stdout_path",
        "stderr_path",
        "stdout_limit",
        "private",
        "ulimit",
        "hostname",
        "root",
        "max_respawns",
        "bind",
        "root_readonly",
        "virt_mode",
        "aging_time",
        "porto_namespace",
        "enable_porto",
        "resolv_conf",
        "weak",
        "anon_usage",
        "absolute_name",
        "absolute_namespace",
        "state",
        "stdout_offset",
        "stderr_offset",
        "cpu_usage",
        "cpu_usage_system",
        "memory_usage",
        "minor_faults",
        "major_faults",
        "io_read",
        "io_write",
        "io_ops",
        "time",
        "net",
        "ip",
        "default_gw",
        "net_guarantee",
        "net_limit",
        "net_rx_limit",
        "net_bytes",
        "net_packets",
        "net_drops",
        "net_overlimits",
        "net_tx_bytes",
        "net_tx_packets",
        "net_tx_drops",
        "net_rx_bytes",
        "net_rx_packets",
        "net_rx_drops",
        "net_tos",
    };

    if (KernelSupports(KernelFeature::LOW_LIMIT))
        properties.push_back("memory_guarantee");

    if (KernelSupports(KernelFeature::RECHARGE_ON_PGFAULT))
        properties.push_back("recharge_on_pgfault");

    if (KernelSupports(KernelFeature::FSIO)) {
        properties.push_back("io_limit");
        properties.push_back("io_ops_limit");
        properties.push_back("dirty_limit");
    }

    if (KernelSupports(KernelFeature::MAX_RSS))
        properties.push_back("max_rss");

    std::vector<TString> plist;

    ExpectApiSuccess(api.ListProperties(plist));

    for (auto name: properties) {
        bool found = false;
        for (auto p: plist)
            found |= p == name;
        Expect(found);
    }

#if 0
    Say() << "Check root cpu_usage & memory_usage" << std::endl;
    ExpectApiSuccess(api.GetProperty(porto_root, "cpu_usage", v));
    ExpectApiSuccess(api.GetProperty(porto_root, "memory_usage", v));

    if (KernelSupports(KernelFeature::FSIO) ||
            KernelSupports(KernelFeature::CFQ)) {
        TestDataMap(api, porto_root, "io_write", 2);
        TestDataMap(api, porto_root, "io_read", 2);
        TestDataMap(api, porto_root, "io_ops", 2);
    }
#endif
    Say() << "Check root properties & data" << std::endl;
    for (auto p : properties)
        ExpectApiSuccess(api.GetProperty(root, p, v));

    ExpectApiSuccess(api.GetProperty(root, "state", v));
    ExpectEq(v, TString("meta"));

    ExpectApiFailure(api.GetProperty(root, "exit_status", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(root, "oom_killed", v), EError::InvalidState);
    ExpectApiFailure(api.GetProperty(root, "stdout", v), EError::InvalidData);
    ExpectApiFailure(api.GetProperty(root, "stderr", v), EError::InvalidData);
    ExpectApiSuccess(api.GetProperty(root, "parent", v));
    ExpectEq(v, "");
    ExpectApiSuccess(api.GetProperty(root, "time", v));

    Say() << "Check that stop on root stops all children" << std::endl;

    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("b"));
    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("b", "command", "sleep 1000"));
    ExpectApiSuccess(api.Start("a"));
    ExpectApiSuccess(api.Start("b"));

    ExpectApiFailure(api.Destroy(root), EError::Permission);
    ExpectApiSuccess(api.Destroy("a"));
    ExpectApiSuccess(api.Destroy("b"));

#if 0
    Say() << "Check cpu_limit/cpu_guarantee" << std::endl;
    if (KernelSupports(KernelFeature::CFS_BANDWIDTH))
        ExpectEq(GetCgKnob("cpu", "", "cpu.cfs_quota_us"), "-1");
    if (KernelSupports(KernelFeature::CFS_RESERVE))
        ExpectEq(GetCgKnob("cpu", "", "cpu.cfs_reserve_us"), "0");
    if (KernelSupports(KernelFeature::CFS_GROUPSCHED))
        ExpectEq(GetCgKnob("cpu", "", "cpu.shares"), "1024");
    if (KernelSupports(KernelFeature::CFQ))
        ExpectEq(GetCgKnob("blkio", "", "blkio.weight"),
                 std::to_string(config().container().normal_io_weight()));
#endif
}

static void TestData(Porto::Connection &api) {
    // should be executed right after TestRoot because assumes empty statistics

    TString root = "/";
    TString wget = "wget";
    TString noop = "noop";

    ExpectApiSuccess(api.Create(noop));
    // this will cause io read and noop will not have io_read
    ExpectEq(system("/bin/true"), 0);
    ExpectApiSuccess(api.SetProperty(noop, "command", "/bin/true"));
    ExpectApiSuccess(api.SetProperty(noop, "stdout_path", "/dev/null"));
    ExpectApiSuccess(api.SetProperty(noop, "stderr_path", "/dev/null"));
    ExpectApiSuccess(api.Start(noop));
    WaitContainer(api, noop);

    ExpectApiSuccess(api.Create(wget));
    ExpectApiSuccess(api.SetProperty(wget, "command", "bash -c 'dd if=/dev/urandom bs=4k count=1 of=index.html oflag=direct'"));
    ExpectApiSuccess(api.Start(wget));
    WaitContainer(api, wget, 60);

    TString v, rv;
    ExpectApiSuccess(api.GetProperty(wget, "exit_status", v));
    ExpectEq(v, "0");

    ExpectApiSuccess(api.GetProperty(root, "cpu_usage", v));
    ExpectNeq(v, "0");
    ExpectNeq(v, "-1");

    ExpectApiSuccess(api.GetProperty(root, "memory_usage", v));
    ExpectNeq(v, "0");
    ExpectNeq(v, "-1");

    if (KernelSupports(KernelFeature::FSIO) ||
            KernelSupports(KernelFeature::CFQ)) {
        TestDataMap(api, root, "io_write", 0);
        TestDataMap(api, root, "io_read", 0);
        TestDataMap(api, root, "io_ops", 0);
    }

    ExpectApiSuccess(api.GetProperty(wget, "cpu_usage", v));
    ExpectNeq(v, "0");
    ExpectNeq(v, "-1");

    ExpectApiSuccess(api.GetProperty(wget, "memory_usage", v));
    ExpectNeq(v, "0");
    ExpectNeq(v, "-1");

    if (KernelSupports(KernelFeature::FSIO) ||
            KernelSupports(KernelFeature::CFQ)) {
        TestDataMap(api, wget, "io_write", 0);
        TestDataMap(api, wget, "io_ops", 0);
    }

    ExpectApiSuccess(api.GetProperty(noop, "cpu_usage", v));
    ExpectNeq(v, "0");
    ExpectNeq(v, "-1");

    ExpectApiSuccess(api.GetProperty(noop, "memory_usage", v));
    ExpectNeq(v, "-1");

    uint64_t val;
    ExpectOk(StringToUint64(v, val));
    ExpectOp(val, <, 1024 * 1024);

    if (KernelSupports(KernelFeature::FSIO) ||
            KernelSupports(KernelFeature::CFQ)) {
        TestDataMap(api, noop, "io_write", 1);
        TestDataMap(api, noop, "io_read", 1);
        TestDataMap(api, noop, "io_ops", 1);
    }

    ExpectApiSuccess(api.Destroy(wget));
    ExpectApiSuccess(api.Destroy(noop));
}

static bool CanTestLimits() {
    if (!KernelSupports(KernelFeature::LOW_LIMIT))
        return false;

    if (!KernelSupports(KernelFeature::RECHARGE_ON_PGFAULT))
        return false;

    return true;
}

static void TestCoresConvertion(Porto::Connection &api, const TString &name, const TString &property) {
    auto cores = GetNumCores();
    TString v;

    ExpectApiSuccess(api.SetProperty(name, property, "100"));
    ExpectApiSuccess(api.GetProperty(name, property, v));
    ExpectEq(v, StringFormat("%dc", cores));

    ExpectApiSuccess(api.SetProperty(name, property, "50"));
    ExpectApiSuccess(api.GetProperty(name, property, v));
    ExpectEq(v, StringFormat("%gc", 0.5 * cores));
}

static void TestLimits(Porto::Connection &api) {
    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check default limits" << std::endl;
    TString current;

#if 0
    current = GetCgKnob("memory", "", "memory.use_hierarchy");
    ExpectEq(current, "1");
#endif

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.use_hierarchy");
    ExpectEq(current, "1");

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) ||
           current == std::to_string(ULLONG_MAX) ||
           current == std::to_string(LLONG_MAX - 4095));

    if (KernelSupports(KernelFeature::LOW_LIMIT)) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        ExpectEq(current, "0");
    }
    ExpectApiSuccess(api.Stop(name));

    Say() << "Check custom limits" << std::endl;
    TString exp_limit = "134217728";
    TString exp_guar = "16384";
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "1g"));
    ExpectApiSuccess(api.GetProperty(name, "memory_limit", current));
    ExpectEq(current, "1073741824");

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    if (KernelSupports(KernelFeature::LOW_LIMIT))
        ExpectApiSuccess(api.SetProperty(name, "memory_guarantee", exp_guar));
    ExpectApiSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);
    if (KernelSupports(KernelFeature::LOW_LIMIT)) {
        current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
        ExpectEq(current, exp_guar);
    }

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "2g"));
    ExpectApiFailure(api.SetProperty(name, "memory_limit", "10k"), EError::InvalidValue);

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "0"));

    Say() << "Check cpu_limit and cpu_guarantee range" << std::endl;
    if (KernelSupports(KernelFeature::CFS_BANDWIDTH)) {
        ExpectApiFailure(api.SetProperty(name, "cpu_limit", "test"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "cpu_limit", "101"), EError::InvalidValue);
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "0"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "0.5"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "1"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "1.5"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "100"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "1c"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "1.5c"));
    }

    if (KernelSupports(KernelFeature::CFS_GROUPSCHED) ||
            KernelSupports(KernelFeature::CFS_RESERVE)) {
        ExpectApiFailure(api.SetProperty(name, "cpu_guarantee", "test"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "cpu_guarantee", "-1"), EError::InvalidValue);
        ExpectApiFailure(api.SetProperty(name, "cpu_guarantee", "101"), EError::InvalidValue);
        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "0"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "1.5"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "100"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "1c"));
        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "1.5c"));
    }

    Say() << "Check cpu_policy" << std::endl;

    ExpectApiFailure(api.SetProperty(name, "cpu_policy", "somecrap"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "cpu_policy", "idle"));

    if (KernelSupports(KernelFeature::CFS_BANDWIDTH)) {
        Say() << "Check cpu_limit" << std::endl;
        ExpectApiSuccess(api.SetProperty(name, "cpu_policy", "normal"));

        uint64_t period, quota;
        ExpectOk(StringToUint64(GetCgKnob("cpu", "/", "cpu.cfs_period_us"), period));
        long ncores = sysconf(_SC_NPROCESSORS_CONF);

        const uint64_t minQuota = 1 * 1000;
        uint64_t half = ncores * period / 2;
        if (half < minQuota)
            half = minQuota;

        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "20"));
        ExpectApiSuccess(api.Start(name));
        ExpectOk(StringToUint64(GetCgKnob("cpu", name, "cpu.cfs_quota_us"), quota));
        Say() << "quota=" << quota << " half="<< half << " min=" << minQuota << std::endl;

        Expect(quota < half);
        Expect(quota > minQuota);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "80"));
        ExpectApiSuccess(api.Start(name));
        ExpectOk(StringToUint64(GetCgKnob("cpu", name, "cpu.cfs_quota_us"), quota));
        Say() << "quota=" << quota << " half="<< half << " min=" << minQuota << std::endl;
        Expect(quota > half);
        Expect(quota > minQuota);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_limit", "100"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("cpu", name, "cpu.cfs_quota_us"), "-1");
        ExpectApiSuccess(api.Stop(name));

        TestCoresConvertion(api, name, "cpu_limit");
    }

    if (KernelSupports(KernelFeature::CFS_RESERVE)) {

    } else if (KernelSupports(KernelFeature::CFS_GROUPSCHED)) {
        Say() << "Check cpu_guarantee" << std::endl;
        uint64_t shares;

        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "0"));
        ExpectApiSuccess(api.Start(name));
        ExpectOk(StringToUint64(GetCgKnob("cpu", name, "cpu.shares"), shares));
        ExpectEq(shares, 1024);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "1c"));
        ExpectApiSuccess(api.Start(name));
        ExpectOk(StringToUint64(GetCgKnob("cpu", name, "cpu.shares"), shares));
        ExpectEq(shares, 1024 + 1024);
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "cpu_guarantee", "0.5c"));
        ExpectApiSuccess(api.Start(name));
        ExpectOk(StringToUint64(GetCgKnob("cpu", name, "cpu.shares"), shares));
        ExpectEq(shares, 1024 + 512);
        ExpectApiSuccess(api.Stop(name));

        TestCoresConvertion(api, name, "cpu_guarantee");
    }

    Say() << "Check io_policy" << std::endl;

    ExpectApiFailure(api.SetProperty(name, "io_policy", "invalid"), EError::InvalidValue);

    ExpectApiSuccess(api.SetProperty(name, "io_policy", "normal"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "io_policy", "batch"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.Stop(name));

    if (KernelSupports(KernelFeature::FSIO)) {
        Say() << "Check io_limit" << std::endl;

        ExpectApiSuccess(api.SetProperty(name, "io_limit", "0"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("memory", name, "memory.fs_bps_limit"), "0");
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "io_limit", "1000"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("memory", name, "memory.fs_bps_limit"), "1000");
        ExpectApiSuccess(api.Stop(name));

        Say() << "Check io_ops_limit" << std::endl;

        ExpectApiSuccess(api.SetProperty(name, "io_ops_limit", "0"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("memory", name, "memory.fs_iops_limit"), "0");
        ExpectApiSuccess(api.Stop(name));

        ExpectApiSuccess(api.SetProperty(name, "io_ops_limit", "1000"));
        ExpectApiSuccess(api.Start(name));
        ExpectEq(GetCgKnob("memory", name, "memory.fs_iops_limit"), "1000");
        ExpectApiSuccess(api.Stop(name));
    }

    Say() << "Make sure we have a cap for stdout_limit property" << std::endl;

    ExpectApiFailure(api.SetProperty(name, "stdout_limit",
                     std::to_string(config().container().stdout_limit_max() + 1)),
                     EError::Permission);

    Say() << "Make sure we have a cap for private property" << std::endl;
    TString tooLong = TString(PRIVATE_VALUE_MAX + 1, 'a');
    ExpectApiFailure(api.SetProperty(name, "private", tooLong), EError::InvalidValue);

    ExpectApiSuccess(api.Destroy(name));
}

static void TestUlimitProperty(Porto::Connection &api) {
    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    Say() << "Check rlimits parsing" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "ulimit", ""));
    ExpectApiSuccess(api.SetProperty(name, "ulimit", ";;;"));
    ExpectApiSuccess(api.SetProperty(name, "ulimit", " ; ; ; "));
    ExpectApiFailure(api.SetProperty(name, "ulimit", "qwe"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "qwe: 123"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "qwe: 123 456"), EError::InvalidValue);
    ExpectApiSuccess(api.SetProperty(name, "ulimit", "as: 123"));
    ExpectApiFailure(api.SetProperty(name, "ulimit", "as 123 456"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "as: 123 456 789"), EError::InvalidValue);
    ExpectApiFailure(api.SetProperty(name, "ulimit", "as: 123 :456"), EError::InvalidValue);

    Say() << "Check rlimits" << std::endl;

    map<TString, pair<TString, TString>> rlim = {
        { "nproc", { "20480", "30720" } },
        { "nofile", { "819200", "1024000" } },
        /* RLIMIT_DATA breaks asan build for kernels >= 4.6 */
        //{ "data", { "8388608000", "10485760000" } },
        { "memlock", { "41943040000", "41943040000" } },
    };

    TString ulimit;
    for (auto &lim : rlim)
        ulimit += lim.first + ": " + lim.second.first + " " + lim.second.second + "; ";

    ExpectApiSuccess(api.SetProperty(name, "ulimit", ulimit));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    TString pid;
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    AsRoot(api);

    for (auto &lim : rlim) {
        ExpectEq(GetRlimit(pid, lim.first, true), lim.second.first);
        ExpectEq(GetRlimit(pid, lim.first, false), lim.second.second);
    }

    ExpectApiSuccess(api.Stop(name));

    Say() << "Make sure we can set limit to unlimited" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "ulimit", "data: unlim unlimited"));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestAlias(Porto::Connection &api) {
    if (!KernelSupports(KernelFeature::LOW_LIMIT))
        return;
    if (!KernelSupports(KernelFeature::RECHARGE_ON_PGFAULT))
        return;

    TString name = "a", current, alias, real;

    ExpectApiSuccess(api.Create(name));

    Say() << "Check default limits" << std::endl;

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX));

    current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
    ExpectEq(current, "0");

    current = GetCgKnob("memory", name, "memory.recharge_on_pgfault");
    ExpectEq(current, "0");

    Say() << "Check custom limits" << std::endl;
    TString exp_limit = "52428800";
    TString exp_guar = "16384";

    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", "12m"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectEq(alias, "12582912\n");
    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", "123g"));
    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectEq(alias, "132070244352\n");

    ExpectApiSuccess(api.SetProperty(name, "memory.limit_in_bytes", exp_limit));
    ExpectApiSuccess(api.SetProperty(name, "memory.low_limit_in_bytes", exp_guar));
    ExpectApiSuccess(api.SetProperty(name, "memory.recharge_on_pgfault", "1"));
    ExpectApiSuccess(api.SetProperty(name, "cpu.smart", "1"));

    ExpectApiSuccess(api.GetProperty(name, "memory.limit_in_bytes", alias));
    ExpectApiSuccess(api.GetProperty(name, "memory_limit", real));
    ExpectEq(alias, real+"\n");
    ExpectApiSuccess(api.GetProperty(name, "memory.low_limit_in_bytes", alias));
    ExpectApiSuccess(api.GetProperty(name, "memory_guarantee", real));
    ExpectEq(alias, real+"\n");
    ExpectApiSuccess(api.GetProperty(name, "memory.recharge_on_pgfault", alias));
    ExpectApiSuccess(api.GetProperty(name, "recharge_on_pgfault", real));
    ExpectEq(alias, "1\n");
    ExpectEq(real, "true");
    ExpectApiSuccess(api.GetProperty(name, "cpu.smart", alias));
    ExpectApiSuccess(api.GetProperty(name, "cpu_policy", real));
    ExpectEq(alias, "1\n");
    ExpectEq(real, "rt");

    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);
    current = GetCgKnob("memory", name, "memory.low_limit_in_bytes");
    ExpectEq(current, exp_guar);

    current = GetCgKnob("memory", name, "memory.recharge_on_pgfault");
    ExpectEq(current, "1");

    current = GetCgKnob("cpu", name, "cpu.smart");
    ExpectEq(current, "1");
    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.Destroy(name));
}

static void TestDynamic(Porto::Connection &api) {
    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    TString current;
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    Expect(current == std::to_string(LLONG_MAX) || current == std::to_string(ULLONG_MAX) ||
           current == std::to_string(LLONG_MAX - 4095));

    TString exp_limit = "268435456";
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);

    ExpectApiSuccess(api.Pause(name));

    exp_limit = "536870912";
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", exp_limit));
    current = GetCgKnob("memory", name, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);

    ExpectApiSuccess(api.Resume(name));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));
}

static void TestLimitsHierarchy(Porto::Connection &api) {
    if (!KernelSupports(KernelFeature::LOW_LIMIT))
        return;

    //
    // box +-- monitoring
    //     |
    //     +-- system
    //     |
    //     +-- production +-- slot1
    //                    |
    //                    +-- slot2
    //

    TString box = "box";
    TString prod = "box/production";
    TString slot1 = "box/production/slot1";
    TString slot2 = "box/production/slot2";
    TString system = "box/system";
    TString monit = "box/monitoring";

    ExpectApiSuccess(api.Create(box));
    ExpectApiSuccess(api.Create(prod));
    ExpectApiSuccess(api.Create(slot1));
    ExpectApiSuccess(api.Create(slot2));
    ExpectApiSuccess(api.Create(system));
    ExpectApiSuccess(api.Create(monit));

    auto total = GetTotalMemory();

    Say() << "Single container can't go over reserve" << std::endl;
    ExpectApiFailure(api.SetProperty(system, "memory_guarantee", std::to_string(total)), EError::ResourceNotAvailable);
    ExpectApiSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(total - config().daemon().memory_guarantee_reserve())));

    Say() << "Distributed guarantee can't go over reserve" << std::endl;
    auto chunk = (total - config().daemon().memory_guarantee_reserve()) / 4;

    ExpectApiSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(chunk)));
    ExpectApiSuccess(api.SetProperty(monit, "memory_guarantee", std::to_string(chunk)));
    ExpectApiSuccess(api.SetProperty(slot1, "memory_guarantee", std::to_string(chunk)));
    ExpectApiFailure(api.SetProperty(slot2, "memory_guarantee", std::to_string(chunk + 1)), EError::ResourceNotAvailable);
    ExpectApiSuccess(api.SetProperty(slot2, "memory_guarantee", std::to_string(chunk)));

    ExpectApiSuccess(api.SetProperty(monit, "memory_guarantee", std::to_string(0)));
    ExpectApiSuccess(api.SetProperty(system, "memory_guarantee", std::to_string(0)));

    ExpectApiSuccess(api.Destroy(monit));
    ExpectApiSuccess(api.Destroy(system));
    ExpectApiSuccess(api.Destroy(slot2));
    ExpectApiSuccess(api.Destroy(slot1));
    ExpectApiSuccess(api.Destroy(prod));
    ExpectApiSuccess(api.Destroy(box));

    Say() << "Test child-parent isolation" << std::endl;

    TString parent = "parent";
    TString child = "parent/child";

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.SetProperty(parent, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(parent));

    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(child, "isolate", "false"));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));

    TString exp_limit = "268435456";
    ExpectApiSuccess(api.SetProperty(child, "memory_limit", exp_limit));
    ExpectApiSuccess(api.SetProperty(child, "cpu_limit", "10"));
    ExpectApiSuccess(api.SetProperty(child, "cpu_guarantee", "10"));
    ExpectApiSuccess(api.SetProperty(child, "respawn", "true"));

    ExpectApiSuccess(api.Start(child));

    TString v;
    ExpectApiSuccess(api.GetProperty(parent, "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetProperty(child, "state", v));
    ExpectEq(v, "running");

    TString current = GetCgKnob("memory", child, "memory.limit_in_bytes");
    ExpectEq(current, exp_limit);
    current = GetCgKnob("memory", parent, "memory.limit_in_bytes");
    ExpectNeq(current, exp_limit);

    TString parentProperty, childProperty;
    ExpectApiSuccess(api.GetProperty(parent, "stdout_path", parentProperty));
    ExpectApiSuccess(api.GetProperty(child, "stdout_path", childProperty));
    ExpectEq(parentProperty, childProperty);
    ExpectApiSuccess(api.GetProperty(parent, "stderr_path", parentProperty));
    ExpectApiSuccess(api.GetProperty(child, "stderr_path", childProperty));
    ExpectEq(parentProperty, childProperty);

    TString parentPid, childPid;

    ExpectApiSuccess(api.GetProperty(parent, "root_pid", parentPid));
    ExpectApiSuccess(api.GetProperty(child, "root_pid", childPid));

    AsRoot(api);

    auto parentCgmap = GetCgroups(parentPid);
    auto childCgmap = GetCgroups(childPid);

    ExpectNeq(parentCgmap["freezer"], childCgmap["freezer"]);
    ExpectNeq(parentCgmap["memory"], childCgmap["memory"]);
    ExpectNeq(parentCgmap["net_cls"], childCgmap["net_cls"]);
    ExpectNeq(parentCgmap["cpu"], childCgmap["cpu"]);
    ExpectNeq(parentCgmap["cpuacct"], childCgmap["cpuacct"]);

    ExpectNeq(GetCwd(parentPid), GetCwd(childPid));

    for (auto &ns : namespaces)
        ExpectEq(GetNamespace(parentPid, ns), GetNamespace(childPid, ns));

    ExpectApiSuccess(api.Destroy(child));
    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Test resume/pause propagation" << std::endl;
    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.SetProperty(parent, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(parent));

    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));

    TString parentState, childState;
    ExpectApiSuccess(api.Pause(parent));
    ExpectApiSuccess(api.GetProperty(parent, "state", parentState));
    ExpectApiSuccess(api.GetProperty(child, "state", childState));
    ExpectEq(parentState, "paused");
    ExpectEq(childState, "paused");

    ExpectApiSuccess(api.Resume(parent));
    ExpectApiSuccess(api.GetProperty(parent, "state", parentState));
    ExpectApiSuccess(api.GetProperty(child, "state", childState));
    ExpectEq(parentState, "running");
    ExpectEq(childState, "running");

    ExpectApiSuccess(api.Pause(parent));
    ExpectApiFailure(api.Resume(child), EError::InvalidState);

    ExpectApiFailure(api.Destroy(child), EError::InvalidState);
    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Test mixed tree resume/pause" << std::endl;
    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.Create("a/b/c"));
    ExpectApiSuccess(api.Create("a/b/d"));

    ExpectApiSuccess(api.SetProperty("a", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b/c", "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty("a/b/d", "command", "true"));

    ExpectApiSuccess(api.Start("a/b/c"));
    ExpectState(api, "a", "running");
    ExpectState(api, "a/b", "meta");
    ExpectState(api, "a/b/c", "running");
    ExpectState(api, "a/b/d", "stopped");

    ExpectApiSuccess(api.Pause("a"));
    ExpectState(api, "a", "paused");
    ExpectState(api, "a/b", "paused");
    ExpectState(api, "a/b/c", "paused");
    ExpectState(api, "a/b/d", "stopped");

    ExpectApiFailure(api.Resume("a/b/c"), EError::InvalidState);
    ExpectApiFailure(api.Destroy("a/b/c"), EError::InvalidState);
    ExpectApiFailure(api.Start("a/b/d"), EError::InvalidState);

    ExpectApiSuccess(api.Resume("a"));
    ExpectState(api, "a", "running");
    ExpectState(api, "a/b", "meta");
    ExpectState(api, "a/b/c", "running");
    ExpectState(api, "a/b/d", "stopped");

    ExpectApiSuccess(api.Pause("a"));
    ExpectApiSuccess(api.Destroy("a"));

    Say() << "Test property propagation" << std::endl;
    TString val;

    ExpectApiSuccess(api.Create("a"));
    ExpectApiSuccess(api.Create("a/b"));
    ExpectApiSuccess(api.Create("a/b/c"));
    ExpectApiSuccess(api.SetProperty("a", "root", "/tmp"));

    ExpectApiSuccess(api.SetProperty("a/b", "isolate", "false"));
    ExpectApiSuccess(api.SetProperty("a/b/c", "isolate", "false"));

    ExpectApiSuccess(api.GetProperty("a/b", "root", val));
    ExpectEq(val, "/");
    ExpectApiSuccess(api.GetProperty("a/b/c", "root", val));
    ExpectEq(val, "/");

    ExpectApiSuccess(api.SetProperty("a", "memory_limit", "12345"));
    ExpectApiSuccess(api.GetProperty("a/b", "memory_limit", val));
    ExpectNeq(val, "12345");
    ExpectApiSuccess(api.GetProperty("a/b/c", "memory_limit", val));
    ExpectNeq(val, "12345");

    ExpectApiSuccess(api.Destroy("a"));
}

static void TestPermissions(Porto::Connection &api) {
    struct stat st;
    TString path;

    TString name = "a";
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    path = CgRoot("memory", name);
    ExpectEq(lstat(path.c_str(), &st), 0);
    ExpectEq(st.st_mode, (0755 | S_IFDIR));

    path = path + "/tasks";
    ExpectEq(lstat(path.c_str(), &st), 0);
    ExpectEq(st.st_mode, (0644 | S_IFREG));

    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Only user that created container can start/stop/destroy/etc it" << std::endl;

    TString s;

    AsAlice(api);

    ExpectApiSuccess(api.Create(name));

    AsBob(api);

    ExpectApiFailure(api.Start(name), EError::Permission);
    ExpectApiFailure(api.Destroy(name), EError::Permission);
    ExpectApiFailure(api.SetProperty(name, "command", "sleep 1000"), EError::Permission);
    ExpectApiSuccess(api.GetProperty(name, "command", s));

    AsAlice(api);

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.SetProperty(name, "user", Bob.User()));
    ExpectApiSuccess(api.SetProperty(name, "group", Bob.Group()));
    ExpectApiFailure(api.Start(name), EError::Permission);

    ExpectApiSuccess(api.GetProperty(name, "command", s));

    ExpectApiSuccess(api.SetProperty(name, "user", Alice.User()));
    ExpectApiSuccess(api.SetProperty(name, "group", Alice.Group()));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", s));

    AsBob(api);

    ExpectApiSuccess(api.GetProperty(name, "root_pid", s));
    ExpectApiFailure(api.Stop(name), EError::Permission);
    ExpectApiFailure(api.Pause(name), EError::Permission);

    AsAlice(api);

    ExpectApiSuccess(api.Pause(name));

    AsBob(api);

    ExpectApiFailure(api.Destroy(name), EError::Permission);
    ExpectApiFailure(api.Resume(name), EError::Permission);

    AsRoot(api);

    ExpectApiSuccess(api.Destroy(name));

    AsAlice(api);

    Say() << "Make sure we can't create child for parent with different uid/gid " << std::endl;

    AsAlice(api);

    ExpectApiSuccess(api.Create("a"));

    AsBob(api);

    ExpectApiFailure(api.Create("a/b"), EError::Permission);

    AsAlice(api);

    ExpectApiSuccess(api.Destroy("a"));
}

static void WaitRespawn(Porto::Connection &api, const TString &name, int expected, int maxTries = 10) {
    TString respawnCount;
    int successRespawns = 0;
    for(int i = 0; i < maxTries; i++) {
        sleep(config().container().respawn_delay_ms() / 1000);
        ExpectApiSuccess(api.GetProperty(name, "respawn_count", respawnCount));
        if (respawnCount == std::to_string(expected))
            successRespawns++;
        if (successRespawns == 2)
            break;
        Say() << "Respawned " << i << " times" << std::endl;
    }
    ExpectEq(std::to_string(expected), respawnCount);
}

static void TestRespawnProperty(Porto::Connection &api) {
    TString pid, respawnPid;
    TString ret;

    TString name = "a";
    ExpectApiSuccess(api.Create(name));
    ExpectApiFailure(api.SetProperty(name, "max_respawns", "true"), EError::InvalidValue);

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1"));

    ExpectApiSuccess(api.SetProperty(name, "respawn", "false"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "respawn_count", ret));
    ExpectEq(ret, TString("0"));
    WaitContainer(api, name);
    sleep(config().container().respawn_delay_ms() / 1000);
    ExpectApiSuccess(api.GetProperty(name, "respawn_count", ret));
    ExpectEq(ret, TString("0"));
    ExpectApiSuccess(api.Stop(name));

    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    WaitContainer(api, name);
    WaitState(api, name, "running");
    ExpectApiSuccess(api.GetProperty(name, "root_pid", respawnPid));
    ExpectNeq(pid, respawnPid);
    ExpectApiSuccess(api.GetProperty(name, "respawn_count", ret));
    Expect(ret != "0" && ret != "");
    ExpectApiSuccess(api.Stop(name));

    int expected = 3;
    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.SetProperty(name, "max_respawns", std::to_string(expected)));
    ExpectApiSuccess(api.SetProperty(name, "command", "echo test"));
    ExpectApiSuccess(api.Start(name));

    WaitRespawn(api, name, expected);

    ExpectApiSuccess(api.Destroy(name));
}

static void ReadPropsAndData(Porto::Connection &api, const TString &name) {
    std::vector<TString> plist;
    ExpectApiSuccess(api.ListProperties(plist));
    TString v;

    for (auto p : plist)
        (void)api.GetProperty(name, p, v);
}

static void TestLeaks(Porto::Connection &api) {
    TString slavePid, masterPid;
    TString name;
    int slack = 4096 * 2;
    int perct = 64;
    uint64_t time;

    ExpectOk(TPath(PORTO_PIDFILE).ReadAll(slavePid));
    ExpectOk(TPath(PORTO_MASTER_PIDFILE).ReadAll(masterPid));

    int initSlave = GetVmRss(slavePid);
    int initMaster = GetVmRss(masterPid);

    int createDestroyNr = 50000;

    time = GetCurrentTimeMs();
    Say() << "Create and destroy single container " << createDestroyNr << " times" << std::endl;
    name = "a";
    for (int i = 0; i < createDestroyNr; i++) {
        ExpectApiSuccess(api.Create(name));
        api.Close();
        ExpectApiSuccess(api.Destroy(name));
        api.Close();
    }

    int nowSlave = GetVmRss(slavePid);
    int nowMaster = GetVmRss(masterPid);

    int expSlave = initSlave + slack;
    int expMaster = initMaster + slack;

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);

    time = GetCurrentTimeMs();
    Say() << "Create " << LeakConainersNr << " containers" << std::endl;
    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "true"));
    }

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    expSlave = initSlave + slack + perct * LeakConainersNr;

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);

    time = GetCurrentTimeMs();
    Say() << "Start " << LeakConainersNr << " containers" << std::endl;

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ExpectApiSuccess(api.Start(name));
    }

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);

    time = GetCurrentTimeMs();
    Say() << "Read properties of " << LeakConainersNr << " containers" << std::endl;

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ReadPropsAndData(api, name);
    }

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);

    time = GetCurrentTimeMs();
    Say() << "Destroy " << LeakConainersNr << " containers" << std::endl;

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "a" + std::to_string(i);
        ExpectApiSuccess(api.Destroy(name));
    }

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);

    time = GetCurrentTimeMs();
    Say() << "Create and start " << LeakConainersNr << " containers" << std::endl;

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "b" + std::to_string(i);
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "true"));
        ExpectApiSuccess(api.Start(name));
        ReadPropsAndData(api, name);
        api.Close();
    }

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);

    time = GetCurrentTimeMs();
    Say() << "Read properties of " << LeakConainersNr << " containers" << std::endl;

    name = "b0";
    for (int i = 0; i < LeakConainersNr; i++)
        ReadPropsAndData(api, name);

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);

    time = GetCurrentTimeMs();
    Say() << "Destroy " << LeakConainersNr << " containers" << std::endl;

    for (int i = 0; i < LeakConainersNr; i++) {
        name = "b" + std::to_string(i);
        ExpectApiSuccess(api.Destroy(name));
        api.Close();
    }

    nowSlave = GetVmRss(slavePid);
    nowMaster = GetVmRss(masterPid);

    time = GetCurrentTimeMs() - time;
    Say() << time << " ms " << "Master " << nowMaster << " kb Slave " << nowSlave << " kb" << std::endl;

    ExpectLessEq(nowSlave, expSlave);
    ExpectLessEq(nowMaster, expMaster);
}

bool WriteDelimitedTo(
                      const google::protobuf::MessageLite& message,
                      google::protobuf::io::ZeroCopyOutputStream* rawOutput) {
    // We create a new coded stream for each message.  Don't worry, this is fast.
    google::protobuf::io::CodedOutputStream output(rawOutput);

    // Write the size.
    const int size = message.ByteSize();
    output.WriteVarint32(size);
    if (output.HadError())
        return false;

    uint8_t* buffer = output.GetDirectBufferForNBytesAndAdvance(size);
    if (buffer != NULL) {
        // Optimization:  The message fits in one buffer, so use the faster
        // direct-to-array serialization path.
        message.SerializeWithCachedSizesToArray(buffer);
    } else {
        // Slightly-slower path when the message is multiple buffers.
        message.SerializeWithCachedSizes(&output);
        if (output.HadError())
            return false;
    }

    return true;
}

TError ConnectToRpcServer(const TString& path, int &fd)
{
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;

    memset(&peer_addr, 0, sizeof(struct sockaddr_un));

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return TError::System("socket()");

    peer_addr.sun_family = AF_UNIX;
    strncpy(peer_addr.sun_path, path.c_str(), sizeof(peer_addr.sun_path) - 1);

    peer_addr_size = sizeof(struct sockaddr_un);
    if (connect(fd, (struct sockaddr *) &peer_addr, peer_addr_size) < 0) {
        close(fd);
        fd = -1;
        return TError::System("connect(" + path + ")");
    }

    return OK;
}

static void TestSigPipe(Porto::Connection &api) {
    TString before;
    ExpectApiSuccess(api.GetProperty("/", "porto_stat[spawned]", before));

    int fd;
    ExpectOk(ConnectToRpcServer(PORTO_SOCKET_PATH, fd));

    rpc::TPortoRequest req;
    req.mutable_list();

    google::protobuf::io::FileOutputStream post(fd);
    WriteDelimitedTo(req, &post);
    post.Flush();

    close(fd);
    WaitPortod(api);

    TString after;
    ExpectApiSuccess(api.GetProperty("/", "porto_stat[spawned]", after));
    ExpectEq(before, after);
}

static void InitErrorCounters(Porto::Connection &api) {
    TString v;

    ExpectApiSuccess(api.GetProperty("/", "porto_stat[spawned]", v));
    StringToInt(v, expectedRespawns);

    ExpectApiSuccess(api.GetProperty("/", "porto_stat[errors]", v));
    StringToInt(v, expectedErrors);

    ExpectApiSuccess(api.GetProperty("/", "porto_stat[warnings]", v));
    StringToInt(v, expectedWarns);
}

static void CheckErrorCounters(Porto::Connection &api) {
    TString v;

    ExpectApiSuccess(api.GetProperty("/", "porto_stat[spawned]", v));
    ExpectEq(v, std::to_string(expectedRespawns));

    ExpectApiSuccess(api.GetProperty("/", "porto_stat[errors]", v));
    ExpectEq(v, std::to_string(expectedErrors));

    ExpectApiSuccess(api.GetProperty("/", "porto_stat[warnings]", v));
    ExpectEq(v, std::to_string(expectedWarns));
}

static void KillMaster(Porto::Connection &api, int sig, int times = 10) {
    int pid = ReadPid(PORTO_MASTER_PIDFILE);
    if (kill(pid, sig))
        Fail("Cannot kill portod-master");
    WaitProcessExit(std::to_string(pid));
    WaitPortod(api, times);

    expectedRespawns++;
    CheckErrorCounters(api);
}

static void KillSlave(Porto::Connection &api, int sig, int times = 10) {
    int portodPid = ReadPid(PORTO_PIDFILE);
    if (kill(portodPid, sig))
        Fail("Cannot kill portod");
    WaitProcessExit(std::to_string(portodPid));
    WaitPortod(api, times);
    expectedRespawns++;
    CheckErrorCounters(api);
}

static bool RespawnTicks(Porto::Connection &api, const TString &name, int maxTries = 3) {
    TString respawnCount, v;
    ExpectApiSuccess(api.GetProperty(name, "respawn_count", respawnCount));
    for(int i = 0; i < maxTries; i++) {
        sleep(config().container().respawn_delay_ms() / 1000);
        ExpectApiSuccess(api.GetProperty(name, "respawn_count", v));

        if (v != respawnCount)
            return true;
    }
    return false;
}

static void TestWait(Porto::Connection &api) {
    TString c = "aaa";
    TString d = "aaa/bbb";
    TString tmp, tmp_state;

    Say() << "Check wait for / container" << std::endl;
    ExpectApiSuccess(api.WaitContainer("/", tmp, 0));
    ExpectNeq("timeout", tmp);

    Say() << "Check wait for non-existing and invalid containers" << std::endl;
    ExpectApiFailure(api.WaitContainers({c}, tmp, tmp_state, 0), EError::ContainerDoesNotExist);
    ExpectApiFailure(api.WaitContainers({}, tmp, tmp_state, 0), EError::InvalidValue);

    Say() << "Check wait for stopped container" << std::endl;
    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, 0));
    ExpectEq(c, tmp);

    Say() << "Check wait for running/dead container" << std::endl;
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 1"));
    ExpectApiSuccess(api.Start(c));
    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, 5));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetProperty(c, "state", tmp));
    ExpectEq(tmp, "dead");

    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, 5));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetProperty(c, "state", tmp));
    ExpectEq(tmp, "dead");
    ExpectApiSuccess(api.Stop(c));
    ExpectApiSuccess(api.Destroy(c));

    Say() << "Check wait for containers in meta-state" << std::endl;
    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.Create(d));

    ExpectApiSuccess(api.SetProperty(d, "command", "sleep 1"));
    ExpectApiSuccess(api.Start(d));
    ExpectApiSuccess(api.GetProperty(c, "state", tmp));
    ExpectEq(tmp, "meta");
    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, 5));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.Stop(d));
    ExpectApiSuccess(api.Destroy(d));
    ExpectApiSuccess(api.Stop(c));
    ExpectApiSuccess(api.Destroy(c));

    Say() << "Check wait for large number of containers" << std::endl;
    std::vector<TString> containers;
    for (int i = 0; i < 100; i++)
        containers.push_back(c + std::to_string(i));
    for (auto &name : containers) {
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectApiSuccess(api.Start(name));
        ExpectApiSuccess(api.GetProperty(name, "state", tmp));
        ExpectEq(tmp, "running");
    }

    ExpectApiSuccess(api.Kill(containers[50], 9));
    ExpectApiSuccess(api.WaitContainers(containers, tmp, tmp_state, 5));
    ExpectEq(tmp, containers[50]);
    ExpectApiSuccess(api.GetProperty(containers[50], "state", tmp));
    ExpectEq(tmp, "dead");

    for (auto &name : containers)
        ExpectApiSuccess(api.Destroy(name));

    Say() << "Check wait timeout" << std::endl;
    uint64_t begin, end;

    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(c));

    begin = GetCurrentTimeMs();
    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, 0));
    end = GetCurrentTimeMs();
    ExpectEq(tmp, "");
    Expect(end - begin < 100);

    begin = GetCurrentTimeMs();
    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, 1));
    end = GetCurrentTimeMs();
    ExpectEq(tmp, "");
    Expect(end - begin >= 1000);

    ExpectApiSuccess(api.Destroy(c));
}

static void TestWaitRecovery(Porto::Connection &api) {
    TString c = "aaa";
    TString d = "aaa/bbb";
    TString tmp, tmp_state;

    Say() << "Check wait for restored container" << std::endl;

    ExpectApiSuccess(api.Create(c));
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 3"));
    ExpectApiSuccess(api.Start(c));

    KillSlave(api, SIGKILL);

    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, -1));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetProperty(c, "state", tmp));
    ExpectEq(tmp, "dead");
    ExpectApiSuccess(api.Stop(c));

    Say() << "Check wait for lost and restored container" << std::endl;
    ExpectApiSuccess(api.SetProperty(c, "command", "sleep 3"));
    ExpectApiSuccess(api.Start(c));

    KillMaster(api, SIGKILL);

    ExpectApiSuccess(api.WaitContainers({c}, tmp, tmp_state, -1));
    ExpectEq(c, tmp);
    ExpectApiSuccess(api.GetProperty(c, "state", tmp));
    ExpectEq(tmp, "dead");
    ExpectApiSuccess(api.Stop(c));
    ExpectApiSuccess(api.Destroy(c));
}

static void TestRecovery(Porto::Connection &api) {
    TString pid, v;
    TString name = "a:b";
    std::vector<TString> containers;

    map<TString, TString> props = {
        { "command", "sleep 1000" },
        { "user", Alice.User() },
        { "group", Bob.Group() },
        { "env", "a=a;b=b" },
    };

    Say() << "Make sure we can restore stopped child when parent is dead" << std::endl;

    ExpectApiSuccess(api.Create("parent"));
    ExpectApiSuccess(api.Create("parent/child"));
    ExpectApiSuccess(api.SetProperty("parent", "command", "sleep 1"));
    ExpectApiSuccess(api.SetProperty("parent/child", "command", "sleep 2"));
    ExpectApiSuccess(api.Start("parent"));
    ExpectApiSuccess(api.Start("parent/child"));
    ExpectApiSuccess(api.Stop("parent/child"));
    WaitContainer(api, "parent");

    KillMaster(api, SIGKILL);

    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], TString("parent"));
    ExpectEq(containers[1], TString("parent/child"));

    ExpectApiSuccess(api.Destroy("parent"));

    Say() << "Make sure we can figure out that containers are dead even if master dies" << std::endl;

    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 3"));
    ExpectApiSuccess(api.Start(name));

    KillMaster(api, SIGKILL);
    WaitContainer(api, name);

    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure we don't kill containers when doing recovery" << std::endl;

    AsRoot(api);
    ExpectApiSuccess(api.Create(name));

    for (auto &pair : props)
        ExpectApiSuccess(api.SetProperty(name, pair.first, pair.second));
    ExpectApiSuccess(api.Start(name));
    ExpectApiSuccess(api.SetProperty(name, "private", "ISS-AGENT"));

    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectEq(TaskRunning(pid), true);
    ExpectEq(TaskZombie(pid), false);

    KillSlave(api, SIGKILL);

    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.GetProperty(name, "root_pid", v));
    ExpectEq(v, pid);

    ExpectEq(TaskRunning(pid), true);
    ExpectEq(TaskZombie(pid), false);

    for (auto &pair : props) {
        TString v;
        ExpectApiSuccess(api.GetProperty(name, pair.first, v));
        ExpectEq(v, pair.second);
    }

    ExpectApiSuccess(api.Destroy(name));
    AsAlice(api);

    Say() << "Make sure meta gets correct state upon recovery" << std::endl;
    TString parent = "a";
    TString child = "a/b";

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(parent, "isolate", "true"));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));

    AsRoot(api);
    KillSlave(api, SIGKILL);
    AsAlice(api);

    ExpectApiSuccess(api.GetProperty(parent, "state", v));
    ExpectEq(v, "meta");

    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Make sure hierarchical recovery works" << std::endl;

    ExpectApiSuccess(api.Create(parent));
    ExpectApiSuccess(api.Create(child));
    ExpectApiSuccess(api.SetProperty(parent, "isolate", "false"));
    ExpectApiSuccess(api.SetProperty(child, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(child));

    AsRoot(api);
    KillSlave(api, SIGKILL);
    AsAlice(api);

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), 2);
    ExpectEq(containers[0], TString("a"));
    ExpectEq(containers[1], TString("a/b"));
    ExpectApiSuccess(api.GetProperty(parent, "state", v));
    ExpectEq(v, "meta");

    if (KernelSupports(KernelFeature::RECHARGE_ON_PGFAULT))
        ExpectApiSuccess(api.SetProperty(parent, "recharge_on_pgfault", "true"));
    ExpectApiFailure(api.SetProperty(parent, "env", "a=b"), EError::InvalidState);

    ExpectApiSuccess(api.GetProperty(child, "state", v));
    ExpectEq(v, "running");
    ExpectApiSuccess(api.Destroy(child));
    ExpectApiSuccess(api.Destroy(parent));

    Say() << "Make sure task is moved to correct cgroup on recovery" << std::endl;
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));

    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));

    AsRoot(api);
    ExpectOk(TPath(CgRoot("memory", "/") + "cgroup.procs").WriteAll(pid));
    auto cgmap = GetCgroups(pid);
    ExpectEq(cgmap["memory"], "/");
    KillSlave(api, SIGKILL);
    AsAlice(api);

    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    ExpectCorrectCgroups(pid, name, name);
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure some data is persistent" << std::endl;
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.SetProperty(name, "command", oomCommand));
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", oomMemoryLimit));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", v));
    ExpectEq(v, TString("9"));
    ExpectApiSuccess(api.GetProperty(name, "oom_killed", v));
    ExpectEq(v, TString("true"));
    KillSlave(api, SIGKILL);
    ExpectApiSuccess(api.GetProperty(name, "exit_status", v));
    ExpectEq(v, TString("9"));
    ExpectApiSuccess(api.GetProperty(name, "oom_killed", v));
    ExpectEq(v, TString("true"));
    ExpectApiSuccess(api.Stop(name));

    int expected = 1;
    ExpectApiSuccess(api.SetProperty(name, "command", "false"));
    ExpectApiSuccess(api.SetProperty(name, "memory_limit", "0"));
    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.SetProperty(name, "max_respawns", std::to_string(expected)));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);
    KillSlave(api, SIGKILL);
    WaitRespawn(api, name, expected);
    ExpectApiSuccess(api.GetProperty(name, "respawn_count", v));
    ExpectEq(v, std::to_string(expected));

    Say() << "Make sure stopped state is persistent" << std::endl;
    ExpectApiSuccess(api.Destroy(name));
    ExpectApiSuccess(api.Create(name));
    ShouldHaveValidProperties(api, name);
    ShouldHaveValidData(api, name);
    KillSlave(api, SIGKILL);
    ExpectApiSuccess(api.GetProperty(name, "state", v));
    ExpectEq(v, "stopped");
    ShouldHaveValidProperties(api, name);
    ShouldHaveValidData(api, name);

    Say() << "Make sure paused state is persistent" << std::endl;
    ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
    ExpectApiSuccess(api.Start(name));
    ShouldHaveValidRunningData(api, name);
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    v = GetState(pid);
    Expect(v == "S" || v == "R");
    ExpectApiSuccess(api.Pause(name));
    v = GetState(pid);
    //ExpectEq(v, "D");
    KillSlave(api, SIGKILL);
    ExpectApiSuccess(api.GetProperty(name, "root_pid", pid));
    v = GetState(pid);
    //ExpectEq(v, "D");
    ExpectApiSuccess(api.Resume(name));
    ShouldHaveValidRunningData(api, name);
    v = GetState(pid);
    Expect(v == "S" || v == "R");
    ExpectApiSuccess(api.GetProperty(name, "time", v));
    ExpectNeq(v, "0");
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure respawn_count ticks after recovery " << std::endl;
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "true"));
    ExpectApiSuccess(api.SetProperty(name, "respawn", "true"));
    ExpectApiSuccess(api.Start(name));
    ExpectEq(RespawnTicks(api, name), true);
    KillSlave(api, SIGKILL);
    ExpectEq(RespawnTicks(api, name), true);
    ExpectApiSuccess(api.Destroy(name));

    Say() << "Make sure we can recover huge number of containers " << std::endl;
    const size_t nr = config().container().max_total();

    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectApiSuccess(api.Create(name));
        ExpectApiSuccess(api.SetProperty(name, "command", "sleep 1000"));
        ExpectApiSuccess(api.Start(name));
    }

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), nr);

    ExpectApiFailure(api.Create("max_plus_one"), EError::ResourceNotAvailable);

    KillSlave(api, SIGKILL, 5 * 60);

    containers.clear();
    ExpectApiSuccess(api.List(containers));
    ExpectEq(containers.size(), nr);

    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectApiSuccess(api.Kill(name, SIGKILL));
    }
    for (size_t i = 0; i < nr; i++) {
        name = "recover" + std::to_string(i);
        ExpectApiSuccess(api.Destroy(name));
    }
}

static void TestCgroups(Porto::Connection &api) {
    AsRoot(api);

    Say() << "Make sure we don't remove non-porto cgroups" << std::endl;

    TPath freezerCg = "/sys/fs/cgroup/freezer/qwerty/asdfg";

    ExpectOk(freezerCg.MkdirAll(0755));

    KillSlave(api, SIGINT);

    ExpectEq(freezerCg.Exists(), true);
    ExpectOk(freezerCg.Rmdir());

    Say() << "Make sure we can remove freezed cgroups" << std::endl;

    freezerCg = "/sys/fs/cgroup/freezer/porto/asdf";
    TPath memoryCg = "/sys/fs/cgroup/memory/porto/asdf";
    TPath cpuCg = "/sys/fs/cgroup/cpu/porto/asdf";

    ExpectOk(freezerCg.MkdirAll(0755));
    ExpectOk(memoryCg.MkdirAll(0755));
    ExpectOk(cpuCg.MkdirAll(0755));

    int pid = fork();
    if (pid == 0) {
        ExpectOk(TPath(freezerCg + "/cgroup.procs").WriteAll(std::to_string(getpid())));
        ExpectOk(TPath(memoryCg + "/cgroup.procs").WriteAll(std::to_string(getpid())));
        ExpectOk(TPath(cpuCg + "/cgroup.procs").WriteAll(std::to_string(getpid())));
        execlp("sleep", "sleep", "1000", nullptr);
        abort();
    }

    KillSlave(api, SIGKILL);

    ExpectEq(freezerCg.Exists(), false);
    ExpectEq(memoryCg.Exists(), false);
    ExpectEq(cpuCg.Exists(), false);
}

static void TestVersion(Porto::Connection &api) {
    TString version, revision;
    ExpectApiSuccess(api.GetVersion(version, revision));

    ExpectEq(version, PORTO_VERSION);
    ExpectEq(revision, PORTO_REVISION);
}

static void TestBadClient(Porto::Connection &api) {
    std::vector<TString> clist;
    int sec = 120;

    //FIXME lol
#if 0
    Say() << "Check client that doesn't read responses" << std::endl;

    ExpectApiSuccess(api.List(clist)); // connect to porto

    alarm(sec);
    size_t nr = 1000000;
    while (nr--) {
        rpc::TPortoRequest req;
        req.mutable_propertylist();
        api.Send(req);

        if (nr && nr % 100000 == 0)
            Say() << nr << " left" << std::endl;
    }
    alarm(0);
#endif

    Say() << "Check client that does partial write" << std::endl;

    int fd;
    TString buf = "xyz";
    alarm(sec);
    ExpectOk(ConnectToRpcServer(PORTO_SOCKET_PATH, fd));
    int ret = write(fd, buf.c_str(), buf.length());

    Expect(ret > 0);
    ExpectEq(ret, buf.length());

    Porto::Connection api2;
    ExpectApiSuccess(api2.List(clist));
    close(fd);
    alarm(0);
}

static void TestRemoveDead(Porto::Connection &api) {
    TString v;
    ExpectApiSuccess(api.GetProperty("/", "porto_stat[remove_dead]", v));
    ExpectEq(v, std::to_string(0));

    TString name = "dead";
    ExpectApiSuccess(api.Create(name));
    ExpectApiSuccess(api.SetProperty(name, "command", "true"));
    ExpectApiSuccess(api.SetProperty(name, "aging_time", "1"));
    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);

    usleep((config().daemon().log_rotate_ms() + 1000) * 1000);
    TString state;
    ExpectApiFailure(api.GetProperty(name, "state", state), EError::ContainerDoesNotExist);

    ExpectApiSuccess(api.GetProperty("/", "porto_stat[remove_dead]", v));
    ExpectEq(v, std::to_string(1));
}

static void TestStdoutLimit(Porto::Connection &api) {
    TString v, cwd, limitStr;
    struct stat st;
    uint64_t limit;

    TString name = "biglog";
    ExpectApiSuccess(api.Create(name));

    ExpectApiSuccess(api.GetProperty(name, "cwd", cwd));
    ExpectApiSuccess(api.GetProperty(name, "stdout_path", v));
    ExpectApiSuccess(api.GetProperty(name, "stdout_limit", limitStr));
    ExpectOk(StringToUint64(limitStr, limit));

    ExpectApiSuccess(api.SetProperty(name, "command", "dd if=/dev/zero bs=" + limitStr + " count=2"));

    ExpectApiSuccess(api.Start(name));
    WaitContainer(api, name);

    usleep((config().daemon().log_rotate_ms() + 1000) * 1000);

    TPath stdoutPath(cwd + "/" + v);
    ExpectOk(stdoutPath.StatFollow(st));
    ExpectLessEq(st.st_size, limit);

    ExpectApiSuccess(api.Stop(name));
    ExpectApiSuccess(api.SetProperty(name, "stdout_limit", "1m"));
    ExpectApiSuccess(api.SetProperty(name, "command", "bash -c 'dd if=/dev/zero bs=1M count=2; sleep 1000'"));
    ExpectApiSuccess(api.Start(name));

    usleep((config().daemon().log_rotate_ms() + 1000) * 1000);
    ExpectOk(stdoutPath.StatFollow(st));
    ExpectLessEq(st.st_size, 1<<20);

    ExpectApiSuccess(api.Destroy(name));
}

static void TestConvertPath(Porto::Connection &api) {
    ExpectApiSuccess(api.Create("abc"));
    ExpectApiSuccess(api.SetProperty("abc", "root", "/root_abc"));

    ExpectApiSuccess(api.Create("abc/def"));
    ExpectApiSuccess(api.SetProperty("abc/def", "root", "/root_def"));

    ExpectApiSuccess(api.Create("abc/def/gik"));
    ExpectApiSuccess(api.SetProperty("abc/def/gik", "root", "/root_gik"));

    TString res;

    ExpectApiSuccess(api.ConvertPath("/", "/", "", res));
    ExpectEq(res, "/");
    ExpectApiSuccess(api.ConvertPath("/", "",  "/", res));
    ExpectEq(res, "/");
    ExpectApiSuccess(api.ConvertPath("/", "/", "/", res));
    ExpectEq(res, "/");

    ExpectApiSuccess(api.ConvertPath("/", "abc", "", res));
    ExpectEq(res, "/root_abc");
    ExpectApiSuccess(api.ConvertPath("/", "abc/def", "", res));
    ExpectEq(res, "/root_abc/root_def");
    ExpectApiSuccess(api.ConvertPath("/", "abc/def/gik", "", res));
    ExpectEq(res, "/root_abc/root_def/root_gik");

    ExpectApiFailure(api.ConvertPath("/", "", "abc", res), EError::InvalidValue);
    ExpectApiFailure(api.ConvertPath("/", "", "abc/def", res), EError::InvalidValue);
    ExpectApiFailure(api.ConvertPath("/", "", "abc/def/gik", res), EError::InvalidValue);
    ExpectApiFailure(api.ConvertPath("/", "abc", "abc/def", res), EError::InvalidValue);

    ExpectApiSuccess(api.ConvertPath("/", "abc/def", "abc", res));
    ExpectEq(res, "/root_def");
    ExpectApiSuccess(api.ConvertPath("/", "abc/def/gik", "abc", res));
    ExpectEq(res, "/root_def/root_gik");
    ExpectApiSuccess(api.ConvertPath("/", "abc/def/gik", "abc/def", res));
    ExpectEq(res, "/root_gik");

    ExpectApiSuccess(api.Destroy("abc"));
}

int SelfTest(std::vector<TString> args) {
    pair<TString, std::function<void(Porto::Connection &)>> tests[] = {
        { "path", TestPath },
        { "idmap", TestIdmap },
        { "format", TestFormat },
        { "root", TestRoot },
        { "data", TestData },
        { "holder", TestHolder },
        { "meta", TestMeta },
        { "empty", TestEmpty },
        { "state_machine", TestStateMachine },
        { "wait", TestWait },
        { "exit_status", TestExitStatus },
        { "streams", TestStreams },
        { "ns_cg_tc", TestNsCgTc },
        { "isolate_property", TestIsolateProperty },
        { "container_namespaces", TestContainerNamespaces },
        { "env_trim", TestEnvTrim },
        { "env_property", TestEnvProperty },
        { "user_group_property", TestUserGroupProperty },
        { "paths", TestPaths },
        { "cwd_property", TestCwdProperty },
        { "stdpath_property", TestStdPathProperty },
        { "stdout_limit", TestStdoutLimit },
        { "root_property", TestRootProperty },
        { "root_readonly", TestRootRdOnlyProperty },
        { "hostname_property", TestHostnameProperty },
        { "capabilities_property", TestCapabilitiesProperty },
        { "enable_porto_property", TestEnablePortoProperty },
        { "limits", TestLimits },
        { "ulimit_property", TestUlimitProperty },
        { "alias", TestAlias },
        { "dynamic", TestDynamic },
        { "permissions", TestPermissions },
        { "respawn_property", TestRespawnProperty },
        { "hierarchy", TestLimitsHierarchy },
        { "sigpipe", TestSigPipe },
        { "stats", CheckErrorCounters },
        { "daemon", TestDaemon },
        { "convert", TestConvertPath },
        { "leaks", TestLeaks },

        // the following tests will restart porto several times
        { "bad_client", TestBadClient },
        { "recovery", TestRecovery },
        { "wait_recovery", TestWaitRecovery },
        { "cgroups", TestCgroups },
        { "version", TestVersion },
        { "remove_dead", TestRemoveDead },
        { "stats", CheckErrorCounters },
    };

    int ret = EXIT_SUCCESS;
    bool except = args.size() == 0 || args[0] == "--except";

    TPath exe("/proc/self/exe"), path;
    exe.ReadLink(path);
    portoctl = (path.DirName() / "portoctl").ToString();
    portoinit = (path.DirName() / "portoinit").ToString();

    ReadConfigs();
    Porto::Connection api;

    InitUsersAndGroups();

    InitErrorCounters(api);

    for (auto t : tests) {
        if (except ^ (std::find(args.begin(), args.end(), t.first) == args.end()))
            continue;

        std::cerr << ">>> Testing " << t.first << "..." << std::endl;
        AsAlice(api);

        t.second(api);

        CheckErrorCounters(api);
    }

    AsRoot(api);

    std::cerr << "SUCCESS: All tests successfully passed!" << std::endl;
    if (!CanTestLimits())
        std::cerr << "WARNING: Due to missing kernel support, memory_guarantee/cpu_policy has not been tested!" << std::endl;
    if (!KernelSupports(KernelFeature::CFS_BANDWIDTH))
        std::cerr << "WARNING: CFS bandwidth is not enabled, skipping cpu_limit tests" << std::endl;
    if (!KernelSupports(KernelFeature::CFS_GROUPSCHED))
        std::cerr << "WARNING: CFS group scheduling is not enabled, skipping cpu_guarantee tests" << std::endl;
    if (!KernelSupports(KernelFeature::CFQ))
        std::cerr << "WARNING: CFQ is not enabled for one of your block devices, skipping io_read and io_write tests" << std::endl;
    if (!KernelSupports(KernelFeature::MAX_RSS))
        std::cerr << "WARNING: max_rss is not tested" << std::endl;
    if (!KernelSupports(KernelFeature::FSIO))
        std::cerr << "WARNING: io_limit is not tested" << std::endl;

    AsRoot(api);
    if (system("hostname -F /etc/hostname") != 0)
        std::cerr << "WARNING: can't restore hostname" << std::endl;
    return ret;
}
}
