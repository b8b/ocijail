#pragma once

namespace ocijail {

class main_app;

// Remove the things a container jail leaves behind that jail_remove(2) cannot
// deal with: child jails (whose vnets hand interfaces back when they go away),
// interfaces in the jail's own vnet and bhyve VMs created by the jail.
//
// Must be called before the jail is removed; nothing here throws.
void cleanup_jail(main_app& app, int jid);

}  // namespace ocijail
