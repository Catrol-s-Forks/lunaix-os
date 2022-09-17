#include <lunaix/fs.h>
#include <lunaix/fs/devfs.h>
#include <lunaix/fs/ramfs.h>
#include <lunaix/fs/taskfs.h>
#include <lunaix/fs/twifs.h>

void
fsm_register_all()
{
    ramfs_init();
    twifs_init();
    devfs_init();
    taskfs_init();

    // ... more fs implementation
}