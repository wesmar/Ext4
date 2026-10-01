/**
 * status.c - translation between Linux errno values and NTSTATUS.
 *
 * Copyright (c) 2026 Marek Wesolowski (WESMAR)
 * Derived from Ext2Fsd (Matt Wu), Ext4Fsd (Bo Branten) and Linux ext4/jbd2.
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "ext4fs.h"

int Ext2LinuxError (NTSTATUS Status)
{
    switch (Status) {
    case STATUS_ACCESS_DENIED:
        return (-EACCES);

    case STATUS_ACCESS_VIOLATION:
        return (-EFAULT);

    case STATUS_BUFFER_TOO_SMALL:
        return (-ETOOSMALL);

    case STATUS_INVALID_PARAMETER:
        return (-EINVAL);

    case STATUS_NOT_IMPLEMENTED:
    case STATUS_NOT_SUPPORTED:
        return (-EOPNOTSUPP);

    case STATUS_INVALID_ADDRESS:
    case STATUS_INVALID_ADDRESS_COMPONENT:
        return (-EADDRNOTAVAIL);

    case STATUS_NO_SUCH_DEVICE:
    case STATUS_NO_SUCH_FILE:
    case STATUS_OBJECT_NAME_NOT_FOUND:
    case STATUS_OBJECT_PATH_NOT_FOUND:
    case STATUS_NETWORK_BUSY:
    case STATUS_INVALID_NETWORK_RESPONSE:
    case STATUS_UNEXPECTED_NETWORK_ERROR:
        return (-ENETDOWN);

    case STATUS_BAD_NETWORK_PATH:
    case STATUS_NETWORK_UNREACHABLE:
    case STATUS_PROTOCOL_UNREACHABLE:
        return (-ENETUNREACH);

    case STATUS_LOCAL_DISCONNECT:
    case STATUS_TRANSACTION_ABORTED:
    case STATUS_CONNECTION_ABORTED:
        return (-ECONNABORTED);

    case STATUS_REMOTE_DISCONNECT:
    case STATUS_LINK_FAILED:
    case STATUS_CONNECTION_DISCONNECTED:
    case STATUS_CONNECTION_RESET:
    case STATUS_PORT_UNREACHABLE:
        return (-ECONNRESET);

    case STATUS_INSUFFICIENT_RESOURCES:
        return (-ENOMEM);

    /* a full volume stays one on the way back (Ext2WinntError) */
    case STATUS_DISK_FULL:
        return (-ENOSPC);

    case STATUS_PAGEFILE_QUOTA:
    case STATUS_NO_MEMORY:
    case STATUS_CONFLICTING_ADDRESSES:
    case STATUS_QUOTA_EXCEEDED:
    case STATUS_TOO_MANY_PAGING_FILES:
    case STATUS_WORKING_SET_QUOTA:
    case STATUS_COMMITMENT_LIMIT:
    case STATUS_TOO_MANY_ADDRESSES:
    case STATUS_REMOTE_RESOURCES:
        return (-ENOBUFS);

    case STATUS_INVALID_CONNECTION:
        return (-ENOTCONN);

    case STATUS_PIPE_DISCONNECTED:
        return (-ESHUTDOWN);

    case STATUS_TIMEOUT:
    case STATUS_IO_TIMEOUT:
    case STATUS_LINK_TIMEOUT:
        return (-ETIMEDOUT);

    case STATUS_REMOTE_NOT_LISTENING:
    case STATUS_CONNECTION_REFUSED:
        return (-ECONNREFUSED);

    case STATUS_HOST_UNREACHABLE:
        return (-EHOSTUNREACH);

    case STATUS_CANT_WAIT:
    case STATUS_PENDING:
        return (-EAGAIN);

    case STATUS_DEVICE_NOT_READY:
        return (-EIO);

    case STATUS_CANCELLED:
    case STATUS_REQUEST_ABORTED:
        return (-EINTR);

    case STATUS_BUFFER_OVERFLOW:
    case STATUS_INVALID_BUFFER_SIZE:
        return (-EMSGSIZE);

    case STATUS_ADDRESS_ALREADY_EXISTS:
        return (-EADDRINUSE);
    }

    if (NT_SUCCESS (Status))
        return 0;

    return (-EINVAL);
}

NTSTATUS Ext2WinntError(int rc)
{
    switch (rc) {

    case 0:
        return STATUS_SUCCESS;

    case -EPERM:
    case -EACCES:
        return STATUS_ACCESS_DENIED;

    case -ENOENT:
        return  STATUS_OBJECT_NAME_NOT_FOUND;

    case -EFAULT:
        return STATUS_ACCESS_VIOLATION;

    case -ETOOSMALL:
        return STATUS_BUFFER_TOO_SMALL;

    case -EBADMSG:
    case -EBADF:
    case -EINVAL:
    case -EFBIG:
        return STATUS_INVALID_PARAMETER;

    case -EBUSY:
        return STATUS_DEVICE_BUSY;

/*    case -ENOSYS:
        return STATUS_NOT_IMPLEMENTED;*/

    case -ENOSPC:
        return STATUS_DISK_FULL;

    case -EOPNOTSUPP:
        return STATUS_NOT_SUPPORTED;

/*    case -EDEADLK:
        return STATUS_POSSIBLE_DEADLOCK;*/

    case -EEXIST:
        return STATUS_OBJECT_NAME_COLLISION;

    case -EIO:
        return STATUS_UNEXPECTED_IO_ERROR;

    case -ENOTDIR:
        return STATUS_NOT_A_DIRECTORY;

    case -EISDIR:
        return STATUS_FILE_IS_A_DIRECTORY;

/*    case -ENOTEMPTY:
        return STATUS_DIRECTORY_NOT_EMPTY;*/

    case -ENODEV:
        return STATUS_NO_SUCH_DEVICE;

    case -ENXIO:
        return STATUS_INVALID_ADDRESS;

    case -EADDRNOTAVAIL:
        return STATUS_INVALID_ADDRESS;

    case -ENETDOWN:
        return STATUS_UNEXPECTED_NETWORK_ERROR;

    case -ENETUNREACH:
        return STATUS_NETWORK_UNREACHABLE;

    case -ECONNABORTED:
        return STATUS_CONNECTION_ABORTED;

    case -ECONNRESET:
        return STATUS_CONNECTION_RESET;

    case -ENOMEM:
        return STATUS_INSUFFICIENT_RESOURCES;

    case -ENOBUFS:
        return STATUS_NO_MEMORY;

    case -ENOTCONN:
        return STATUS_INVALID_CONNECTION;

    case -ESHUTDOWN:
        return STATUS_CONNECTION_DISCONNECTED;

    case -ETIMEDOUT:
        return STATUS_TIMEOUT;

    case -ECONNREFUSED:
        return STATUS_CONNECTION_REFUSED;

    case -EHOSTUNREACH:
        return STATUS_HOST_UNREACHABLE;

    case -EAGAIN:
        return STATUS_CANT_WAIT;

    case -EINTR:
        return  STATUS_CANCELLED;

    case -EMSGSIZE:
        return STATUS_INVALID_BUFFER_SIZE;

    case -EADDRINUSE:
        return STATUS_ADDRESS_ALREADY_EXISTS;
    }

    return STATUS_UNSUCCESSFUL;
}
