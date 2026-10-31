#include <sys/param.h>

#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <sys/ioctl.h>
#include <sys/jail.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "ocijail/cleanup.h"
#include "ocijail/main.h"

namespace fs = std::filesystem;

namespace ocijail {

namespace {

std::vector<int> child_jids(int parent) {
    std::vector<int> result;
    int32_t lastjid = 0;
    for (;;) {
        int32_t last = lastjid;
        int32_t jid = 0;
        int32_t parent_out = 0;
        iovec jiov[] = {
            {const_cast<char*>("lastjid"), sizeof("lastjid")},
            {&last, sizeof(last)},
            {const_cast<char*>("jid"), sizeof("jid")},
            {&jid, sizeof(jid)},
            {const_cast<char*>("parent"), sizeof("parent")},
            {&parent_out, sizeof(parent_out)},
        };
        if (jail_get(jiov, std::size(jiov), 0) < 0) {
            // ENOENT: no more jails to look at.
            break;
        }
        lastjid = jid;
        if (parent_out == parent) {
            result.push_back(jid);
        }
    }
    return result;
}

bool exists(int jid) {
    int32_t jid_in = jid;
    iovec jiov[] = {
        {const_cast<char*>("jid"), sizeof("jid")},
        {&jid_in, sizeof(jid_in)},
    };
    return jail_get(jiov, std::size(jiov), 0) >= 0;
}

bool owns_vnet(int jid) {
    int32_t jid_in = jid;
    int32_t vnet = 0;
    iovec jiov[] = {
        {const_cast<char*>("jid"), sizeof("jid")},
        {&jid_in, sizeof(jid_in)},
        {const_cast<char*>("vnet"), sizeof("vnet")},
        {&vnet, sizeof(vnet)},
    };
    if (jail_get(jiov, std::size(jiov), 0) < 0) {
        return false;
    }
    return vnet == JAIL_SYS_NEW;
}

// The driver name behind an interface, e.g. "epair0b" or "em0". Interfaces the
// CNI plugins rename (the host side to vnetXXXX, the container side to eth0)
// keep their epair driver, so this - and not the name - is what tells them
// apart.
std::string interface_driver(const char* name) {
    auto ifindex = ::if_nametoindex(name);
    if (ifindex == 0) {
        return {};
    }
    int mib[6] = {CTL_NET,
                  PF_LINK,
                  NETLINK_GENERIC,
                  IFMIB_IFDATA,
                  static_cast<int>(ifindex),
                  IFDATA_DRIVERNAME};
    std::size_t len = 0;
    if (::sysctl(mib, std::size(mib), nullptr, &len, nullptr, 0) < 0) {
        return {};
    }
    std::string driver(len, '\0');
    if (::sysctl(mib, std::size(mib), driver.data(), &len, nullptr, 0) < 0) {
        return {};
    }
    driver.resize(strnlen(driver.c_str(), driver.size()));
    return driver;
}

// Bring "name" down and rename it to "new_name". This runs inside the jail,
// which owns its vnet, so the network privileges (PRIV_NET_SETIFFLAGS,
// PRIV_NET_SETIFNAME) are granted and plain ioctls are enough - no ifconfig.
void rename_interface(int socket,
                      main_app& app,
                      const std::string& name,
                      const std::string& new_name) {
    struct ifreq ifr{};
    strlcpy(ifr.ifr_name, name.c_str(), sizeof(ifr.ifr_name));
    if (::ioctl(socket, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags &= ~IFF_UP;
        ::ioctl(socket, SIOCSIFFLAGS, &ifr);
    }

    char newname[IFNAMSIZ];
    strlcpy(newname, new_name.c_str(), sizeof(newname));
    struct ifreq rename_ifr{};
    strlcpy(rename_ifr.ifr_name, name.c_str(), sizeof(rename_ifr.ifr_name));
    rename_ifr.ifr_data = newname;
    if (::ioctl(socket, SIOCSIFNAME, &rename_ifr) != 0) {
        if (errno == ENXIO) {
            app.log_debug() << "interface " << name << " is gone";
            return;
        }
        app.log() << "renaming interface " << name << " to " << new_name
                  << ": " << std::strerror(errno);
        return;
    }
    app.log_debug() << "renamed interface " << name << " to " << new_name;
}

// Interfaces created in the parent vnet and moved into this jail (which is how
// the CNI plugins set up container networking) are handed back to the parent
// when the jail's vnet is destroyed, keeping their names. An interface called
// "eth0" arriving next to the parent's own "eth0" breaks everything that keys
// on interface names - packet filters in particular. So destroy any interface
// still carrying its epair name (left over from an incomplete setup) and give
// everything that is left a name no other vnet uses.
void cleanup_interfaces(main_app& app, int jid) {
    auto socket = ::socket(AF_LOCAL, SOCK_DGRAM, 0);
    if (socket < 0) {
        app.log() << "socket: " << std::strerror(errno);
        return;
    }

    // Classify while we still have all of them: the driver of an interface we
    // just destroyed cannot be looked up any more, and it must not be renamed
    // afterwards.
    struct netif {
        std::string name;
        bool epair;
    };
    std::vector<netif> interfaces;
    struct ifaddrs* ifap = nullptr;
    if (::getifaddrs(&ifap) != 0) {
        app.log() << "getifaddrs: " << std::strerror(errno);
        ::close(socket);
        return;
    }
    for (auto* ifa = ifap; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_name == nullptr) {
            continue;
        }
        std::string name{ifa->ifa_name};
        if (name == "lo0" || name.starts_with("rip")) {
            continue;
        }
        if (std::any_of(interfaces.begin(),
                        interfaces.end(),
                        [&](const netif& n) { return n.name == name; })) {
            continue;
        }
        interfaces.push_back(
            {name, interface_driver(name.c_str()).starts_with("epair")});
    }
    ::freeifaddrs(ifap);

    for (auto& netif : interfaces) {
        if (!netif.epair) {
            continue;
        }
        struct ifreq ifr{};
        strlcpy(ifr.ifr_name, netif.name.c_str(), sizeof(ifr.ifr_name));
        if (::ioctl(socket, SIOCIFDESTROY, &ifr) == 0) {
            app.log_debug() << "destroyed interface " << netif.name;
        } else if (errno == ENXIO) {
            app.log_debug() << "interface " << netif.name << " is gone";
        } else {
            app.log() << "destroying interface " << netif.name << ": "
                      << std::strerror(errno);
        }
    }

    int counter = 1;
    for (auto& netif : interfaces) {
        if (netif.epair) {
            continue;
        }
        rename_interface(socket,
                         app,
                         netif.name,
                         "rip" + std::to_string(jid) + "." +
                             std::to_string(counter++));
    }

    ::close(socket);
}

// The VMs of a jail are only visible through the nodes devfs creates for them,
// and a container can unlink those: devfs keeps one dirent per mount, so the
// unlink takes the name out of the container's own /dev/vmm and leaves the VM
// running. The softc, on the other hand, lives in vmm's list until it is
// destroyed, so the names have to be collected from a devfs mount the container
// never touched, which is the caller's.
std::vector<std::string> list_vmm_devices() {
    std::vector<std::string> names;
    std::error_code ec;
    for (auto it = fs::directory_iterator{"/dev/vmm", ec};
         !ec && it != fs::directory_iterator{}; it.increment(ec)) {
        std::error_code type_error;
        if (it->is_directory(type_error) || it->is_symlink(type_error)) {
            continue;
        }
        names.push_back(it->path().filename().native());
    }
    return names;
}

// hw.vmm.destroy only matches VMs of the caller's prison and, for other prisons,
// returns EINVAL (vmmdev_lookup() filters by cr_cansee()) or EPERM, so the
// ownership check is the kernel's and a name that belongs to somebody else is
// simply skipped. It has to run inside the jail: the sysctl checks the caller's
// prison, both for allow.vmm and for the owner of the VM.
//
// The sysctl is the legacy interface next to VMMCTL_VM_DESTROY on /dev/vmmctl,
// but both end up in the same vmmdev_lookup_and_destroy() and the same softc
// list, so it does not matter which one created the VM - only that the name is
// looked up by the jail that owns it. Up to 15.1 nothing destroys a VM when the
// process that created it dies, which is exactly what this is for; kernels with
// VMMCTL_CREATE_DESTROY_ON_CLOSE destroy such VMs when the descriptor is closed,
// and the lookup then fails with EINVAL/ENOENT here.
void destroy_vmm_devices(main_app& app,
                         const std::vector<std::string>& names) {
    for (auto& vm : names) {
        if (::sysctlbyname("hw.vmm.destroy", nullptr, nullptr, vm.c_str(),
                           vm.size() + 1) < 0) {
            // EPERM/EINVAL: not ours. ENOENT: already gone.
            if (errno == EPERM || errno == EINVAL || errno == ENOENT) {
                continue;
            }
            app.log() << "destroying vmm device " << vm << ": "
                      << std::strerror(errno);
        } else {
            app.log_debug() << "destroyed vmm device " << vm;
        }
    }
}

}  // namespace

void cleanup_jail(main_app& app, int jid) {
    // Nothing to do for a jail that is already gone; delete tolerates that.
    if (!exists(jid)) {
        return;
    }

    // Child jails first: a child only hands the interfaces of its vnet back to
    // this jail when it goes away, and jail_remove() below removes this jail
    // right after, so they would arrive too late to be cleaned up. Destroying
    // the children ourselves, while we are still attached to the parent, puts
    // them where cleanup_interfaces() can see them - the tree would come down
    // either way.
    for (auto child : child_jids(jid)) {
        app.log_debug() << "removing child jail " << child;
        if (::jail_remove(child) < 0 && errno != EINVAL) {
            app.log() << "removing child jail " << child << ": "
                      << std::strerror(errno);
        }
    }

    // Collected before attaching, see list_vmm_devices().
    auto vmm_devices = list_vmm_devices();

    // jail_attach is a one way trip and the caller still has to remove the
    // jail, so the work that needs to happen inside it gets its own process.
    auto vnet = owns_vnet(jid);
    auto pid = ::fork();
    if (pid < 0) {
        app.log() << "fork: " << std::strerror(errno);
        return;
    }
    if (pid == 0) {
        int rc = 0;
        if (::jail_attach(jid) < 0) {
            ::_exit(1);
        }
        if (vnet) {
            cleanup_interfaces(app, jid);
        }
        // Look again from inside, in case the caller's devfs ruleset hides the
        // nodes; anything not ours is rejected by the sysctl.
        auto names = vmm_devices;
        for (auto& name : list_vmm_devices()) {
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                names.push_back(name);
            }
        }
        destroy_vmm_devices(app, names);
        ::_exit(rc);
    }

    int status;
    if (::waitpid(pid, &status, 0) < 0) {
        app.log() << "waitpid: " << std::strerror(errno);
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        app.log() << "failed to clean up jail " << jid;
    }
}

}  // namespace ocijail
