#pragma once
#include <sys/stat.h>
#include <string>

namespace goblin {
long GuestReadlink(int fd, char* buffer, size_t size);
int SetSymlinkMetadata(int fd, const std::string& target);
// Metadata stays on the host inode, including across rename and virtual links.
// Only the broker can access this xattr; no host UID or capability is changed.
int InitializeRootMetadata(int fd);
int GuestMetadata(int fd, struct stat* value);
int SetGuestMetadata(int fd, unsigned mode, unsigned uid, unsigned gid);
int SetSocketMetadata(int fd);
int AllowBrokerAccess(int fd, const struct stat& value);
int ClearWritePrivilegeBits(int fd);
int SyncDirectory(int fd);
}
